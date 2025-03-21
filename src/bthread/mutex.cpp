// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - An M:N threading library to make applications more concurrent.

// Date: Sun Aug  3 12:46:15 CST 2014

#include <sys/cdefs.h>
#include <pthread.h>
#include <dlfcn.h>                               // dlsym
#include <fcntl.h>                               // O_RDONLY
#include "butil/atomicops.h"
#include "bvar/bvar.h"
#include "bvar/collector.h"
#include "butil/macros.h"                         // BAIDU_CASSERT
#include "butil/containers/flat_map.h"
#include "butil/iobuf.h"
#include "butil/fd_guard.h"
#include "butil/files/file.h"
#include "butil/files/file_path.h"
#include "butil/file_util.h"
#include "butil/unique_ptr.h"
#include "butil/memory/scope_guard.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"
#include "butil/third_party/symbolize/symbolize.h"
#include "butil/logging.h"
#include "butil/object_pool.h"
#include "butil/debug/stack_trace.h"
#include "butil/thread_local.h"
#include "bthread/butex.h"                       // butex_*
#include "bthread/mutex.h"                       // bthread_mutex_t
#include "bthread/sys_futex.h"
#include "bthread/log.h"
#include "bthread/processor.h"
#include "bthread/task_group.h"

__BEGIN_DECLS
extern void* BAIDU_WEAK _dl_sym(void* handle, const char* symbol, void* caller);
__END_DECLS

namespace bthread {

EXTERN_BAIDU_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group);

// Warm up backtrace before main().
const butil::debug::StackTrace ALLOW_UNUSED dummy_bt;

// For controlling contentions collected per second.
bvar::CollectorSpeedLimit g_cp_sl = BVAR_COLLECTOR_SPEED_LIMIT_INITIALIZER;

const size_t MAX_CACHED_CONTENTIONS = 512;
// Skip frames which are always same: the unlock function and submit_contention()
const int SKIPPED_STACK_FRAMES = 2;

struct SampledContention : public bvar::Collected {
    // time taken by lock and unlock, normalized according to sampling_range
    int64_t duration_ns;
    // number of samples, normalized according to to sampling_range
    double count;
    void* stack[26];      // backtrace.
    int nframes;          // #elements in stack

    // Implement bvar::Collected
    void dump_and_destroy(size_t round) override;
    void destroy() override;
    bvar::CollectorSpeedLimit* speed_limit() override { return &g_cp_sl; }

    size_t hash_code() const {
        if (nframes == 0) {
            return 0;
        }
        if (_hash_code == 0) {
            _hash_code = 1;
            uint32_t seed = nframes;
            butil::MurmurHash3_x86_32(stack, sizeof(void*) * nframes, seed, &_hash_code);
        }
        return _hash_code;
    }
private:
friend butil::ObjectPool<SampledContention>;
    SampledContention()
        : duration_ns(0), count(0), stack{NULL}, nframes(0), _hash_code(0) {}
    ~SampledContention() override = default;

    mutable uint32_t _hash_code; // For combining samples with hashmap.
};

BAIDU_CASSERT(sizeof(SampledContention) == 256, be_friendly_to_allocator);

// Functor to compare contentions.
struct ContentionEqual {
    bool operator()(const SampledContention* c1,
                    const SampledContention* c2) const {
        return c1->hash_code() == c2->hash_code() &&
            c1->nframes == c2->nframes &&
            memcmp(c1->stack, c2->stack, sizeof(void*) * c1->nframes) == 0;
    }
};

// Functor to hash contentions.
struct ContentionHash {
    size_t operator()(const SampledContention* c) const {
        return c->hash_code();
    }
};

// The global context for contention profiler.
class ContentionProfiler {
public:
    typedef butil::FlatMap<SampledContention*, SampledContention*,
                          ContentionHash, ContentionEqual> ContentionMap;

    explicit ContentionProfiler(const char* name);
    ~ContentionProfiler();
    
    void dump_and_destroy(SampledContention* c);

    // Write buffered data into resulting file. If `ending' is true, append
    // content of /proc/self/maps and retry writing until buffer is empty.
    void flush_to_disk(bool ending);

    void init_if_needed();
private:
    bool _init;  // false before first dump_and_destroy is called
    bool _first_write;      // true if buffer was not written to file yet.
    std::string _filename;  // the file storing profiling result.
    butil::IOBuf _disk_buf;  // temp buf before saving the file.
    ContentionMap _dedup_map; // combining same samples to make result smaller.
};

ContentionProfiler::ContentionProfiler(const char* name)
    : _init(false)
    , _first_write(true)
    , _filename(name) {
}

ContentionProfiler::~ContentionProfiler() {
    if (!_init) {
        // Don't write file if dump_and_destroy was never called. We may create
        // such instances in ContentionProfilerStart.
        return;
    }
    flush_to_disk(true);
}

void ContentionProfiler::init_if_needed() {
    if (!_init) {
        // Already output nanoseconds, always set cycles/second to 1000000000.
        _disk_buf.append("--- contention\ncycles/second=1000000000\n");
        if (_dedup_map.init(1024, 60) != 0) {
            LOG(WARNING) << "Fail to initialize dedup_map";
        }
        _init = true;
    }
}
    
void ContentionProfiler::dump_and_destroy(SampledContention* c) {
    init_if_needed();
    // Categorize the contention.
    SampledContention** p_c2 = _dedup_map.seek(c);
    if (p_c2) {
        // Most contentions are caused by several hotspots, this should be
        // the common branch.
        SampledContention* c2 = *p_c2;
        c2->duration_ns += c->duration_ns;
        c2->count += c->count;
        c->destroy();
    } else {
        _dedup_map.insert(c, c);
    }
    if (_dedup_map.size() > MAX_CACHED_CONTENTIONS) {
        flush_to_disk(false);
    }
}

void ContentionProfiler::flush_to_disk(bool ending) {
    BT_VLOG << "flush_to_disk(ending=" << ending << ")";
    
    // Serialize contentions in _dedup_map into _disk_buf.
    if (!_dedup_map.empty()) {
        BT_VLOG << "dedup_map=" << _dedup_map.size();
        butil::IOBufBuilder os;
        for (ContentionMap::const_iterator
                 it = _dedup_map.begin(); it != _dedup_map.end(); ++it) {
            SampledContention* c = it->second;
            os << c->duration_ns << ' ' << (size_t)ceil(c->count) << " @";
            for (int i = SKIPPED_STACK_FRAMES; i < c->nframes; ++i) {
                os << ' ' << (void*)c->stack[i];
            }
            os << '\n';
            c->destroy();
        }
        _dedup_map.clear();
        _disk_buf.append(os.buf());
    }

    // Append /proc/self/maps to the end of the contention file, required by
    // pprof.pl, otherwise the functions in sys libs are not interpreted.
    if (ending) {
        BT_VLOG << "Append /proc/self/maps";
        // Failures are not critical, don't return directly.
        butil::IOPortal mem_maps;
        const butil::fd_guard fd(open("/proc/self/maps", O_RDONLY));
        if (fd >= 0) {
            while (true) {
                ssize_t nr = mem_maps.append_from_file_descriptor(fd, 8192);
                if (nr < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    PLOG(ERROR) << "Fail to read /proc/self/maps";
                    break;
                }
                if (nr == 0) {
                    _disk_buf.append(mem_maps);
                    break;
                }
            }
        } else {
            PLOG(ERROR) << "Fail to open /proc/self/maps";
        }
    }
    // Write _disk_buf into _filename
    butil::File::Error error;
    butil::FilePath path(_filename);
    butil::FilePath dir = path.DirName();
    if (!butil::CreateDirectoryAndGetError(dir, &error)) {
        LOG(ERROR) << "Fail to create directory=`" << dir.value()
                   << "', " << error;
        return;
    }
    // Truncate on first write, append on later writes.
    int flag = O_APPEND;
    if (_first_write) {
        _first_write = false;
        flag = O_TRUNC;
    }
    butil::fd_guard fd(open(_filename.c_str(), O_WRONLY|O_CREAT|flag, 0666));
    if (fd < 0) {
        PLOG(ERROR) << "Fail to open " << _filename;
        return;
    }
    // Write once normally, write until empty in the end.
    do {
        ssize_t nw = _disk_buf.cut_into_file_descriptor(fd);
        if (nw < 0) {
            if (errno == EINTR) {
                continue;
            }
            PLOG(ERROR) << "Fail to write into " << _filename;
            return;
        }
        BT_VLOG << "Write " << nw << " bytes into " << _filename;
    } while (!_disk_buf.empty() && ending);
}

// If contention profiler is on, this variable will be set with a valid
// instance. NULL otherwise.
BAIDU_CACHELINE_ALIGNMENT ContentionProfiler* g_cp = NULL;
// Need this version to solve an issue that non-empty entries left by
// previous contention profilers should be detected and overwritten.
static uint64_t g_cp_version = 0;
// Protecting accesses to g_cp.
static pthread_mutex_t g_cp_mutex = PTHREAD_MUTEX_INITIALIZER;

// The map storing information for profiling pthread_mutex. Different from
// bthread_mutex, we can't save stuff into pthread_mutex, we neither can
// save the info in TLS reliably, since a mutex can be unlocked in a different
// thread from the one locked (although rare, undefined behavior)
// This map must be very fast, since it's accessed inside the lock.
// Layout of the map:
//  * Align each entry by cacheline so that different threads do not collide.
//  * Hash the mutex into the map by its address. If the entry is occupied,
//    cancel sampling.
// The canceling rate should be small provided that programs are unlikely to
// lock a lot of mutexes simultaneously.
const size_t MUTEX_MAP_SIZE = 1024;
BAIDU_CASSERT((MUTEX_MAP_SIZE & (MUTEX_MAP_SIZE - 1)) == 0, must_be_power_of_2);
struct BAIDU_CACHELINE_ALIGNMENT MutexMapEntry {
    butil::static_atomic<uint64_t> versioned_mutex;
    bthread_contention_site_t csite;
};
static MutexMapEntry g_mutex_map[MUTEX_MAP_SIZE] = {}; // zero-initialize

void SampledContention::dump_and_destroy(size_t /*round*/) {
    if (g_cp) {
        // Must be protected with mutex to avoid race with deletion of ctx.
        // dump_and_destroy is called from dumping thread only so this mutex
        // is not contended at most of time.
        BAIDU_SCOPED_LOCK(g_cp_mutex);
        if (g_cp) {
            g_cp->dump_and_destroy(this);
            return;
        }
    }
    destroy();
}

void SampledContention::destroy() {
    _hash_code = 0;
    butil::return_object(this);
}

// Remember the conflict hashes for troubleshooting, should be 0 at most of time.
static butil::static_atomic<int64_t> g_nconflicthash = BUTIL_STATIC_ATOMIC_INIT(0);
static int64_t get_nconflicthash(void*) {
    return g_nconflicthash.load(butil::memory_order_relaxed);
}

// Start profiling contention.
bool ContentionProfilerStart(const char* filename) {
    if (filename == NULL) {
        LOG(ERROR) << "Parameter [filename] is NULL";
        return false;
    }
    // g_cp is also the flag marking start/stop.
    if (g_cp) {
        return false;
    }

    // Create related global bvar lazily.
    static bvar::PassiveStatus<int64_t> g_nconflicthash_var
        ("contention_profiler_conflict_hash", get_nconflicthash, NULL);
    static bvar::DisplaySamplingRatio g_sampling_ratio_var(
        "contention_profiler_sampling_ratio", &g_cp_sl);
    
    // Optimistic locking. A not-used ContentionProfiler does not write file.
    std::unique_ptr<ContentionProfiler> ctx(new ContentionProfiler(filename));
    {
        BAIDU_SCOPED_LOCK(g_cp_mutex);
        if (g_cp) {
            return false;
        }
        g_cp = ctx.release();
        ++g_cp_version;  // invalidate non-empty entries that may exist.
    }
    return true;
}

// Stop contention profiler.
void ContentionProfilerStop() {
    ContentionProfiler* ctx = NULL;
    if (g_cp) {
        std::unique_lock<pthread_mutex_t> mu(g_cp_mutex);
        if (g_cp) {
            ctx = g_cp;
            g_cp = NULL;
            mu.unlock();

            // make sure it's initialiazed in case no sample was gathered,
            // otherwise nothing will be written and succeeding pprof will fail.
            ctx->init_if_needed();
            // Deletion is safe because usages of g_cp are inside g_cp_mutex.
            delete ctx;
            return;
        }
    }
    LOG(ERROR) << "Contention profiler is not started!";
}

bool is_contention_site_valid(const bthread_contention_site_t& cs) {
    return bvar::is_sampling_range_valid(cs.sampling_range);
}

void make_contention_site_invalid(bthread_contention_site_t* cs) {
    cs->sampling_range = 0;
}

#ifndef NO_PTHREAD_MUTEX_HOOK
// Replace pthread_mutex_lock and pthread_mutex_unlock:
// First call to sys_pthread_mutex_lock sets sys_pthread_mutex_lock to the
// real function so that next calls go to the real function directly. This
// technique avoids calling pthread_once each time.
typedef int (*MutexInitOp)(pthread_mutex_t*, const pthread_mutexattr_t*);
typedef int (*MutexOp)(pthread_mutex_t*);
int first_sys_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* mutexattr);
int first_sys_pthread_mutex_destroy(pthread_mutex_t* mutex);
int first_sys_pthread_mutex_lock(pthread_mutex_t* mutex);
int first_sys_pthread_mutex_trylock(pthread_mutex_t* mutex);
int first_sys_pthread_mutex_unlock(pthread_mutex_t* mutex);
static MutexInitOp sys_pthread_mutex_init = first_sys_pthread_mutex_init;
static MutexOp sys_pthread_mutex_destroy = first_sys_pthread_mutex_destroy;
static MutexOp sys_pthread_mutex_lock = first_sys_pthread_mutex_lock;
static MutexOp sys_pthread_mutex_trylock = first_sys_pthread_mutex_trylock;
static MutexOp sys_pthread_mutex_unlock = first_sys_pthread_mutex_unlock;
#if HAS_PTHREAD_MUTEX_TIMEDLOCK
typedef int (*TimedMutexOp)(pthread_mutex_t*, const struct timespec*);
int first_sys_pthread_mutex_timedlock(pthread_mutex_t* mutex,
                                      const struct timespec* __abstime);
static TimedMutexOp sys_pthread_mutex_timedlock = first_sys_pthread_mutex_timedlock;
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK

static pthread_once_t init_sys_mutex_lock_once = PTHREAD_ONCE_INIT;

// dlsym may call malloc to allocate space for dlerror and causes contention
// profiler to deadlock at boostraping when the program is linked with
// libunwind. The deadlock bt:
//   #0  0x00007effddc99b80 in __nanosleep_nocancel () at ../sysdeps/unix/syscall-template.S:81
//   #1  0x00000000004b4df7 in butil::internal::SpinLockDelay(int volatile*, int, int) ()
//   #2  0x00000000004b4d57 in SpinLock::SlowLock() ()
//   #3  0x00000000004b4a63 in tcmalloc::ThreadCache::InitModule() ()
//   #4  0x00000000004aa2b5 in tcmalloc::ThreadCache::GetCache() ()
//   #5  0x000000000040c6c5 in (anonymous namespace)::do_malloc_no_errno(unsigned long) [clone.part.16] ()
//   #6  0x00000000006fc125 in tc_calloc ()
//   #7  0x00007effdd245690 in _dlerror_run (operate=operate@entry=0x7effdd245130 <dlsym_doit>, args=args@entry=0x7fff483dedf0) at dlerror.c:141
//   #8  0x00007effdd245198 in __dlsym (handle=<optimized out>, name=<optimized out>) at dlsym.c:70
//   #9  0x0000000000666517 in bthread::init_sys_mutex_lock () at bthread/mutex.cpp:358
//   #10 0x00007effddc97a90 in pthread_once () at ../nptl/sysdeps/unix/sysv/linux/x86_64/pthread_once.S:103
//   #11 0x000000000066649f in bthread::first_sys_pthread_mutex_lock (mutex=0xbaf880 <_ULx86_64_lock>) at bthread/mutex.cpp:366
//   #12 0x00000000006678bc in pthread_mutex_lock_impl (mutex=0xbaf880 <_ULx86_64_lock>) at bthread/mutex.cpp:489
//   #13 pthread_mutex_lock (__mutex=__mutex@entry=0xbaf880 <_ULx86_64_lock>) at bthread/mutex.cpp:751
//   #14 0x00000000004c6ea1 in _ULx86_64_init () at x86_64/Gglobal.c:83
//   #15 0x00000000004c44fb in _ULx86_64_init_local (cursor=0x7fff483df340, uc=0x7fff483def90) at x86_64/Ginit_local.c:47
//   #16 0x00000000004b5012 in GetStackTrace(void**, int, int) ()
//   #17 0x00000000004b2095 in tcmalloc::PageHeap::GrowHeap(unsigned long) ()
//   #18 0x00000000004b23a3 in tcmalloc::PageHeap::New(unsigned long) ()
//   #19 0x00000000004ad457 in tcmalloc::CentralFreeList::Populate() ()
//   #20 0x00000000004ad628 in tcmalloc::CentralFreeList::FetchFromSpansSafe() ()
//   #21 0x00000000004ad6a3 in tcmalloc::CentralFreeList::RemoveRange(void**, void**, int) ()
//   #22 0x00000000004b3ed3 in tcmalloc::ThreadCache::FetchFromCentralCache(unsigned long, unsigned long) ()
//   #23 0x00000000006fbb9a in tc_malloc ()
// Call _dl_sym which is a private function in glibc to workaround the malloc
// causing deadlock temporarily. This fix is hardly portable.

static void init_sys_mutex_lock() {
// When bRPC library is linked as a shared library, need to make sure bRPC
// shared library is loaded before the pthread shared library. Otherwise,
// it may cause runtime error: undefined symbol: pthread_mutex_xxx.
// Alternatively, static linking can also avoid this problem.
#if defined(OS_LINUX)
    // TODO: may need dlvsym when GLIBC has multiple versions of a same symbol.
    // http://blog.fesnel.com/blog/2009/08/25/preloading-with-multiple-symbol-versions
    if (_dl_sym) {
        sys_pthread_mutex_init = (MutexInitOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_init", (void*)init_sys_mutex_lock);
        sys_pthread_mutex_destroy = (MutexOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_destroy", (void*)init_sys_mutex_lock);
        sys_pthread_mutex_lock = (MutexOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_lock", (void*)init_sys_mutex_lock);
        sys_pthread_mutex_unlock = (MutexOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_unlock", (void*)init_sys_mutex_lock);
        sys_pthread_mutex_trylock = (MutexOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_trylock", (void*)init_sys_mutex_lock);
#if HAS_PTHREAD_MUTEX_TIMEDLOCK
        sys_pthread_mutex_timedlock = (TimedMutexOp)_dl_sym(
            RTLD_NEXT, "pthread_mutex_timedlock", (void*)init_sys_mutex_lock);
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK
    } else {
        // _dl_sym may be undefined reference in some system, fallback to dlsym
        sys_pthread_mutex_init = (MutexInitOp)dlsym(RTLD_NEXT, "pthread_mutex_init");
        sys_pthread_mutex_destroy = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_destroy");
        sys_pthread_mutex_lock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_lock");
        sys_pthread_mutex_unlock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
        sys_pthread_mutex_trylock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
#if HAS_PTHREAD_MUTEX_TIMEDLOCK
        sys_pthread_mutex_timedlock = (TimedMutexOp)dlsym(RTLD_NEXT, "pthread_mutex_timedlock");
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK
    }
#elif defined(OS_MACOSX)
    // TODO: look workaround for dlsym on mac
    sys_pthread_mutex_init = (MutexInitOp)dlsym(RTLD_NEXT, "pthread_mutex_init");
    sys_pthread_mutex_destroy = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    sys_pthread_mutex_lock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_lock");
    sys_pthread_mutex_trylock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    sys_pthread_mutex_unlock = (MutexOp)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
#endif
}

// Make sure pthread functions are ready before main().
const int ALLOW_UNUSED dummy = pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);

int first_sys_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* mutexattr) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_init(mutex, mutexattr);
}

int first_sys_pthread_mutex_destroy(pthread_mutex_t* mutex) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_destroy(mutex);
}

int first_sys_pthread_mutex_lock(pthread_mutex_t* mutex) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_lock(mutex);
}

int first_sys_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_trylock(mutex);
}

#if HAS_PTHREAD_MUTEX_TIMEDLOCK
int first_sys_pthread_mutex_timedlock(pthread_mutex_t* mutex,
                                      const struct timespec* abstime) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_timedlock(mutex, abstime);
}
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK

int first_sys_pthread_mutex_unlock(pthread_mutex_t* mutex) {
    pthread_once(&init_sys_mutex_lock_once, init_sys_mutex_lock);
    return sys_pthread_mutex_unlock(mutex);
}
#endif

template <typename Mutex>
inline uint64_t hash_mutex_ptr(const Mutex* m) {
    return butil::fmix64((uint64_t)m);
}

// Mark being inside locking so that pthread_mutex calls inside collecting
// code are never sampled, otherwise deadlock may occur.
static __thread bool tls_inside_lock = false;

// Warn up some singleton objects used in contention profiler
// to avoid deadlock in malloc call stack.
static __thread bool tls_warn_up = false;

#if BRPC_DEBUG_BTHREAD_SCHE_SAFETY
// ++tls_pthread_lock_count when pthread locking,
// --tls_pthread_lock_count when pthread unlocking.
// Only when it is equal to 0, it is safe for the bthread to be scheduled.
// Note: If a mutex is locked/unlocked in different thread,
// `tls_pthread_lock_count' is inaccurate, so this feature cannot be used.
static __thread int tls_pthread_lock_count = 0;

#define ADD_TLS_PTHREAD_LOCK_COUNT ++tls_pthread_lock_count
#define SUB_TLS_PTHREAD_LOCK_COUNT --tls_pthread_lock_count

void CheckBthreadScheSafety() {
    if (BAIDU_LIKELY(0 == tls_pthread_lock_count)) {
        return;
    }

    // It can only be checked once because the counter is messed up.
    LOG_BACKTRACE_ONCE(ERROR) << "bthread is suspended while holding "
                              << tls_pthread_lock_count << " pthread locks.";
}
#else
#define ADD_TLS_PTHREAD_LOCK_COUNT ((void)0)
#define SUB_TLS_PTHREAD_LOCK_COUNT ((void)0)
void CheckBthreadScheSafety() {}
#endif // BRPC_DEBUG_BTHREAD_SCHE_SAFETY

// Speed up with TLS:
//   Most pthread_mutex are locked and unlocked in the same thread. Putting
//   contention information in TLS avoids collisions that may occur in
//   g_mutex_map. However when user unlocks in another thread, the info cached
//   in the locking thread is not removed, making the space bloated. We use a
//   simple strategy to solve the issue: If a thread has enough thread-local
//   space to store the info, save it, otherwise save it in g_mutex_map. For
//   a program that locks and unlocks in the same thread and does not lock a
//   lot of mutexes simulateneously, this strategy always uses the TLS.
#ifndef DONT_SPEEDUP_PTHREAD_CONTENTION_PROFILER_WITH_TLS
const int TLS_MAX_COUNT = 3;
struct MutexAndContentionSite {
    void* mutex;
    bthread_contention_site_t csite;
};
struct TLSPthreadContentionSites {
    int count;
    uint64_t cp_version;
    MutexAndContentionSite list[TLS_MAX_COUNT];
};
static __thread TLSPthreadContentionSites tls_csites = {0,0,{}};
#endif  // DONT_SPEEDUP_PTHREAD_CONTENTION_PROFILER_WITH_TLS

// Guaranteed in linux/win.
const int PTR_BITS = 48;

template <typename Mutex>
inline bthread_contention_site_t*
add_pthread_contention_site(const Mutex* mutex) {
    MutexMapEntry& entry = g_mutex_map[hash_mutex_ptr(mutex) & (MUTEX_MAP_SIZE - 1)];
    butil::static_atomic<uint64_t>& m = entry.versioned_mutex;
    uint64_t expected = m.load(butil::memory_order_relaxed);
    // If the entry is not used or used by previous profiler, try to CAS it.
    if (expected == 0 ||
        (expected >> PTR_BITS) != (g_cp_version & ((1 << (64 - PTR_BITS)) - 1))) {
        uint64_t desired = (g_cp_version << PTR_BITS) | (uint64_t)mutex;
        if (m.compare_exchange_strong(
                expected, desired, butil::memory_order_acquire)) {
            return &entry.csite;
        }
    }
    g_nconflicthash.fetch_add(1, butil::memory_order_relaxed);
    return NULL;
}

template <typename Mutex>
inline bool remove_pthread_contention_site(const Mutex* mutex,
                                           bthread_contention_site_t* saved_csite) {
    MutexMapEntry& entry = g_mutex_map[hash_mutex_ptr(mutex) & (MUTEX_MAP_SIZE - 1)];
    butil::static_atomic<uint64_t>& m = entry.versioned_mutex;
    if ((m.load(butil::memory_order_relaxed) & ((((uint64_t)1) << PTR_BITS) - 1))
        != (uint64_t)mutex) {
        // This branch should be the most common case since most locks are
        // neither contended nor sampled. We have one memory indirection and
        // several bitwise operations here, the cost should be ~ 5-50ns
        return false;
    }
    // Although this branch is inside a contended lock, we should also make it
    // as simple as possible because altering the critical section too much
    // may make unpredictable impact to thread interleaving status, which
    // makes profiling result less accurate.
    *saved_csite = entry.csite;
    make_contention_site_invalid(&entry.csite);
    m.store(0, butil::memory_order_release);
    return true;
}

// Submit the contention along with the callsite('s stacktrace)
void submit_contention(const bthread_contention_site_t& csite, int64_t now_ns) {
    tls_inside_lock = true;
    BRPC_SCOPE_EXIT {
        tls_inside_lock = false;
    };

    butil::debug::StackTrace stack(true); // May lock.
    if (0 == stack.FrameCount()) {
        return;
    }
    // There are two situations where we need to check whether in the
    // malloc call stack:
    // 1. Warn up some singleton objects used in `submit_contention'
    // to avoid deadlock in malloc call stack.
    // 2. LocalPool is empty, GlobalPool may allocate memory by malloc.
    if (!tls_warn_up || butil::local_pool_free_empty<SampledContention>()) {
        // In malloc call stack, can not submit contention.
        if (stack.FindSymbol((void*)malloc)) {
            return;
        }
    }

    auto sc = butil::get_object<SampledContention>();
    // Normalize duration_us and count so that they're addable in later
    // processings. Notice that sampling_range is adjusted periodically by
    // collecting thread.
    sc->duration_ns = csite.duration_ns * bvar::COLLECTOR_SAMPLING_BASE
        / csite.sampling_range;
    sc->count = bvar::COLLECTOR_SAMPLING_BASE / (double)csite.sampling_range;
    sc->nframes = stack.CopyAddressTo(sc->stack, arraysize(sc->stack));
    sc->submit(now_ns / 1000);  // may lock
    // Once submit a contention, complete warn up.
    tls_warn_up = true;
}

#if BRPC_DEBUG_LOCK
#define MUTEX_RESET_OWNER_COMMON(owner)                                              \
    ((butil::atomic<bool>*)&(owner).hold)                                            \
        ->store(false, butil::memory_order_relaxed)

#define PTHREAD_MUTEX_SET_OWNER(owner)                                               \
    owner.id = pthread_numeric_id();                                                 \
    ((butil::atomic<bool>*)&(owner).hold)                                            \
        ->store(true, butil::memory_order_release)

// Check if the mutex has been locked by the current thread.
// Double lock on the same thread will cause deadlock.
#define PTHREAD_MUTEX_CHECK_OWNER(owner)                                             \
    bool hold = ((butil::atomic<bool>*)&(owner).hold)                                \
        ->load(butil::memory_order_acquire);                                         \
    if (hold && (owner).id == pthread_numeric_id()) {                                \
        butil::debug::StackTrace trace(true);                                        \
        LOG(ERROR) << "Detected deadlock caused by double lock of FastPthreadMutex:" \
                   << std::endl << trace.ToString();                                 \
    }
#else
#define MUTEX_RESET_OWNER_COMMON(owner) ((void)owner)
#define PTHREAD_MUTEX_SET_OWNER(owner) ((void)owner)
#define PTHREAD_MUTEX_CHECK_OWNER(owner) ((void)owner)
#endif // BRPC_DEBUG_LOCK

namespace internal {

#ifndef NO_PTHREAD_MUTEX_HOOK

#if BRPC_DEBUG_LOCK
struct BAIDU_CACHELINE_ALIGNMENT MutexOwnerMapEntry {
    butil::static_atomic<bool> valid;
    pthread_mutex_t* mutex;
    mutex_owner_t owner;
};

// The map storing owner information for pthread_mutex. Different from
// bthread_mutex, we can't save stuff into pthread_mutex, we neither can
// save the info in TLS reliably, since a mutex can be unlocked in a different
// thread from the one locked (although rare).
static MutexOwnerMapEntry g_mutex_owner_map[MUTEX_MAP_SIZE] = {}; // zero-initialize

static void InitMutexOwnerMapEntry(pthread_mutex_t* mutex,
                                   const pthread_mutexattr_t* mutexattr) {
    int type = PTHREAD_MUTEX_DEFAULT;
    if (NULL != mutexattr) {
        pthread_mutexattr_gettype(mutexattr, &type);
    }
    // Only normal mutexes are tracked.
    if (type != PTHREAD_MUTEX_NORMAL) {
        return;
    }

    // Fast path: If the hash entry is not used, use it.
    MutexOwnerMapEntry& hash_entry =
        g_mutex_owner_map[hash_mutex_ptr(mutex) & (MUTEX_MAP_SIZE - 1)];
    if (!hash_entry.valid.exchange(true, butil::memory_order_relaxed)) {
        MUTEX_RESET_OWNER_COMMON(hash_entry.owner);
        return;
    }

    // Slow path: Find an unused entry.
    for (auto& entry : g_mutex_owner_map) {
        if (!entry.valid.exchange(true, butil::memory_order_relaxed)) {
            MUTEX_RESET_OWNER_COMMON(entry.owner);
            return;
        }
    }
}

static BUTIL_FORCE_INLINE
MutexOwnerMapEntry* FindMutexOwnerMapEntry(pthread_mutex_t* mutex) {
    if (NULL == mutex) {
        return NULL;
    }

    // Fast path.
    MutexOwnerMapEntry* hash_entry =
        &g_mutex_owner_map[hash_mutex_ptr(mutex) & (MUTEX_MAP_SIZE - 1)];
    if (hash_entry->valid.load(butil::memory_order_relaxed) && hash_entry->mutex == mutex) {
        return hash_entry;
    }
    // Slow path.
    for (auto& entry : g_mutex_owner_map) {
        if (entry.valid.load(butil::memory_order_relaxed) && entry.mutex == mutex) {
            return &entry;
        }
    }
    return NULL;
}

static void DestroyMutexOwnerMapEntry(pthread_mutex_t* mutex) {
    MutexOwnerMapEntry* entry = FindMutexOwnerMapEntry(mutex);
    if (NULL != entry) {
        entry->valid.store(false, butil::memory_order_relaxed);
    }
}

#define INIT_MUTEX_OWNER_MAP_ENTRY(mutex, mutexattr) \
    ::bthread::internal::InitMutexOwnerMapEntry(mutex, mutexattr)

#define DESTROY_MUTEX_OWNER_MAP_ENTRY(mutex) \
    ::bthread::internal::DestroyMutexOwnerMapEntry(mutex)

#define FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex) \
    MutexOwnerMapEntry* entry = ::bthread::internal::FindMutexOwnerMapEntry(mutex)

#define SYS_PTHREAD_MUTEX_CHECK_OWNER              \
    if (NULL != entry) {                           \
        PTHREAD_MUTEX_CHECK_OWNER(entry->owner);   \
    }

#define SYS_PTHREAD_MUTEX_SET_OWNER                \
    if (NULL != entry) {                           \
        PTHREAD_MUTEX_SET_OWNER(entry->owner);     \
    }

#define SYS_PTHREAD_MUTEX_RESET_OWNER(mutex)       \
    FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex); \
    if (NULL != entry) {                           \
        MUTEX_RESET_OWNER_COMMON(entry->owner);           \
    }

#else
#define INIT_MUTEX_OWNER_MAP_ENTRY(mutex, mutexattr) ((void)0)
#define DESTROY_MUTEX_OWNER_MAP_ENTRY(mutex) ((void)0)
#define FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex) ((void)0)
#define SYS_PTHREAD_MUTEX_CHECK_OWNER ((void)0)
#define SYS_PTHREAD_MUTEX_SET_OWNER ((void)0)
#define SYS_PTHREAD_MUTEX_RESET_OWNER(mutex) ((void)0)
#endif // BRPC_DEBUG_LOCK


#if HAS_PTHREAD_MUTEX_TIMEDLOCK
BUTIL_FORCE_INLINE int pthread_mutex_lock_internal(pthread_mutex_t* mutex,
                                                   const struct timespec* abstime) {
    int rc = 0;
    if (NULL == abstime) {
        FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex);
        SYS_PTHREAD_MUTEX_CHECK_OWNER;
        rc = sys_pthread_mutex_lock(mutex);
        if (0 == rc) {
            SYS_PTHREAD_MUTEX_SET_OWNER;
        }
    } else {
        rc = sys_pthread_mutex_timedlock(mutex, abstime);
        if (0 == rc) {
            FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex);
            SYS_PTHREAD_MUTEX_SET_OWNER;
        }
    }
    if (0 == rc) {
        ADD_TLS_PTHREAD_LOCK_COUNT;
    }
    return rc;
}
#else
BUTIL_FORCE_INLINE int pthread_mutex_lock_internal(pthread_mutex_t* mutex,
                                                   const struct timespec*/* Not supported */) {
    FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex);
    SYS_PTHREAD_MUTEX_CHECK_OWNER;
    int rc = sys_pthread_mutex_lock(mutex);
    if (0 == rc) {
        SYS_PTHREAD_MUTEX_SET_OWNER;
        ADD_TLS_PTHREAD_LOCK_COUNT;
    }
    return rc;
}
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK

BUTIL_FORCE_INLINE int pthread_mutex_trylock_internal(pthread_mutex_t* mutex) {
    int rc = sys_pthread_mutex_trylock(mutex);
    if (0 == rc) {
        FIND_SYS_PTHREAD_MUTEX_OWNER_MAP_ENTRY(mutex);
        SYS_PTHREAD_MUTEX_SET_OWNER;
        ADD_TLS_PTHREAD_LOCK_COUNT;
    }
    return rc;
}

BUTIL_FORCE_INLINE int pthread_mutex_unlock_internal(pthread_mutex_t* mutex) {
    SYS_PTHREAD_MUTEX_RESET_OWNER(mutex);
    SUB_TLS_PTHREAD_LOCK_COUNT;
    return sys_pthread_mutex_unlock(mutex);
}
#endif // NO_PTHREAD_MUTEX_HOOK

BUTIL_FORCE_INLINE int pthread_mutex_lock_internal(FastPthreadMutex* mutex,
                                                   const struct timespec* abstime) {
    if (NULL == abstime) {
        mutex->lock();
        return 0;
    } else {
        return mutex->timed_lock(abstime) ? 0 : errno;
    }
}

BUTIL_FORCE_INLINE int pthread_mutex_trylock_internal(FastPthreadMutex* mutex) {
    return mutex->try_lock() ? 0 : EBUSY;
}

BUTIL_FORCE_INLINE int pthread_mutex_unlock_internal(FastPthreadMutex* mutex) {
    mutex->unlock();
    return 0;
}

template <typename Mutex>
BUTIL_FORCE_INLINE int pthread_mutex_lock_impl(Mutex* mutex, const struct timespec* abstime) {
    // Don't change behavior of lock when profiler is off.
    if (!g_cp ||
        // collecting code including backtrace() and submit() may call
        // pthread_mutex_lock and cause deadlock. Don't sample.
        tls_inside_lock) {
        return pthread_mutex_lock_internal(mutex, abstime);
    }
    // Don't slow down non-contended locks.
    int rc = pthread_mutex_trylock_internal(mutex);
    if (rc != EBUSY) {
        return rc;
    }
    // Ask bvar::Collector if this (contended) locking should be sampled
    const size_t sampling_range = bvar::is_collectable(&g_cp_sl);

    bthread_contention_site_t* csite = NULL;
#ifndef DONT_SPEEDUP_PTHREAD_CONTENTION_PROFILER_WITH_TLS
    TLSPthreadContentionSites& fast_alt = tls_csites;
    if (fast_alt.cp_version != g_cp_version) {
        fast_alt.cp_version = g_cp_version;
        fast_alt.count = 0;
    }
    if (fast_alt.count < TLS_MAX_COUNT) {
        MutexAndContentionSite& entry = fast_alt.list[fast_alt.count++];
        entry.mutex = mutex;
        csite = &entry.csite;
        if (!bvar::is_sampling_range_valid(sampling_range)) {
            make_contention_site_invalid(&entry.csite);
            return pthread_mutex_lock_internal(mutex, abstime);
        }
    }
#endif
    if (!bvar::is_sampling_range_valid(sampling_range)) {  // don't sample
        return pthread_mutex_lock_internal(mutex, abstime);
    }
    // Lock and monitor the waiting time.
    const int64_t start_ns = butil::cpuwide_time_ns();
    rc = pthread_mutex_lock_internal(mutex, abstime);
    if (!rc) { // Inside lock
        if (!csite) {
            csite = add_pthread_contention_site(mutex);
            if (csite == NULL) {
                return rc;
            }
        }
        csite->duration_ns = butil::cpuwide_time_ns() - start_ns;
        csite->sampling_range = sampling_range;
    } // else rare
    return rc;
}

template <typename Mutex>
BUTIL_FORCE_INLINE int pthread_mutex_trylock_impl(Mutex* mutex) {
    return pthread_mutex_trylock_internal(mutex);
}

template <typename Mutex>
BUTIL_FORCE_INLINE int pthread_mutex_unlock_impl(Mutex* mutex) {
    // Don't change behavior of unlock when profiler is off.
    if (!g_cp || tls_inside_lock) {
        // This branch brings an issue that an entry created by
        // add_pthread_contention_site may not be cleared. Thus we add a
        // 16-bit rolling version in the entry to find out such entry.
        return pthread_mutex_unlock_internal(mutex);
    }
    int64_t unlock_start_ns = 0;
    bool miss_in_tls = true;
    bthread_contention_site_t saved_csite = {0,0};
#ifndef DONT_SPEEDUP_PTHREAD_CONTENTION_PROFILER_WITH_TLS
    TLSPthreadContentionSites& fast_alt = tls_csites;
    for (int i = fast_alt.count - 1; i >= 0; --i) {
        if (fast_alt.list[i].mutex == mutex) {
            if (is_contention_site_valid(fast_alt.list[i].csite)) {
                saved_csite = fast_alt.list[i].csite;
                unlock_start_ns = butil::cpuwide_time_ns();
            }
            fast_alt.list[i] = fast_alt.list[--fast_alt.count];
            miss_in_tls = false;
            break;
        }
    }
#endif
    // Check the map to see if the lock is sampled. Notice that we're still
    // inside critical section.
    if (miss_in_tls) {
        if (remove_pthread_contention_site(mutex, &saved_csite)) {
            unlock_start_ns = butil::cpuwide_time_ns();
        }
    }
    const int rc = pthread_mutex_unlock_internal(mutex);
    // [Outside lock]
    if (unlock_start_ns) {
        const int64_t unlock_end_ns = butil::cpuwide_time_ns();
        saved_csite.duration_ns += unlock_end_ns - unlock_start_ns;
        submit_contention(saved_csite, unlock_end_ns);
    }
    return rc;
}

}

#ifndef NO_PTHREAD_MUTEX_HOOK
BUTIL_FORCE_INLINE int pthread_mutex_lock_impl(pthread_mutex_t* mutex) {
    return internal::pthread_mutex_lock_impl(mutex, NULL);
}

BUTIL_FORCE_INLINE int pthread_mutex_trylock_impl(pthread_mutex_t* mutex) {
    return internal::pthread_mutex_trylock_impl(mutex);
}

#if HAS_PTHREAD_MUTEX_TIMEDLOCK
BUTIL_FORCE_INLINE int pthread_mutex_timedlock_impl(pthread_mutex_t* mutex,
                                                    const struct timespec* abstime) {
    return internal::pthread_mutex_lock_impl(mutex, abstime);
}
#endif // HAS_PTHREAD_MUTEX_TIMEDLOCK

BUTIL_FORCE_INLINE int pthread_mutex_unlock_impl(pthread_mutex_t* mutex) {
    return internal::pthread_mutex_unlock_impl(mutex);
}
#endif

// Implement bthread_mutex_t related functions
// MutexInternal 占用 4 字节
struct MutexInternal {
    butil::static_atomic<unsigned char> locked;
    butil::static_atomic<unsigned char> contended;      // 标记是否有竞争，当释放锁时，如果 contended 为1，则需要进行线程唤醒
    unsigned short padding;
};

const MutexInternal MUTEX_CONTENDED_RAW = {{1},{1},0};
const MutexInternal MUTEX_LOCKED_RAW = {{1},{0},0};
// Define as macros rather than constants which can't be put in read-only
// section and affected by initialization-order fiasco.
// 取出 MutexInternal 中的 locked 字段
#define BTHREAD_MUTEX_CONTENDED (*(const unsigned*)&bthread::MUTEX_CONTENDED_RAW)
#define BTHREAD_MUTEX_LOCKED (*(const unsigned*)&bthread::MUTEX_LOCKED_RAW)

BAIDU_CASSERT(sizeof(unsigned) == sizeof(MutexInternal),
              sizeof_mutex_internal_must_equal_unsigned);

#if BRPC_DEBUG_LOCK

#define BTHREAD_MUTEX_SET_OWNER                                                             \
    do {                                                                                    \
        TaskGroup* task_group = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);            \
        if (NULL != task_group && !task_group->is_current_main_task()) {                    \
            m->owner.id = bthread_self();                                                   \
        } else {                                                                            \
            m->owner.id = pthread_numeric_id();                                             \
        }                                                                                   \
        ((butil::atomic<bool>*)&m->owner.hold)                                              \
            ->store(true, butil::memory_order_release);                                     \
    } while(false)

// Check if the mutex has been locked by the current thread.
// Double lock on the same thread will cause deadlock.
#define BTHREAD_MUTEX_CHECK_OWNER                                                            \
        bool hold = ((butil::atomic<bool>*)&m->owner.hold)                                   \
            ->load(butil::memory_order_acquire);                                             \
        bool double_lock =                                                                   \
            hold && (m->owner.id == bthread_self() || m->owner.id == pthread_numeric_id());  \
        if (double_lock) {                                                                   \
            butil::debug::StackTrace trace(true);                                            \
            LOG(ERROR) << "Detected deadlock caused by double lock of bthread_mutex_t:"      \
                       << std::endl << trace.ToString();                                     \
       }
#else
#define BTHREAD_MUTEX_SET_OWNER ((void)0)
#define BTHREAD_MUTEX_CHECK_OWNER ((void)0)
#endif // BRPC_DEBUG_LOCK

inline int mutex_trylock_impl(bthread_mutex_t* m) {
    MutexInternal* split = (MutexInternal*)m->butex;
    if (!split->locked.exchange(1, butil::memory_order_acquire)) {
        BTHREAD_MUTEX_SET_OWNER;
        return 0;
    }
    return EBUSY;
}

const int MAX_SPIN_ITER = 4;

inline int mutex_lock_contended_impl(bthread_mutex_t* __restrict m,
                                     const struct timespec* __restrict abstime) {
    BTHREAD_MUTEX_CHECK_OWNER;
    // When a bthread first contends for a lock, active spinning makes sense.
    // Spin only few times and only if local `rq' is empty.
    TaskGroup* g = BAIDU_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
    if (BAIDU_UNLIKELY(NULL == g || g->rq_size() == 0)) {
        for (int i = 0; i < MAX_SPIN_ITER; ++i) {
            cpu_relax();
        }
    }

    bool queue_lifo = false;
    bool first_wait = true;
    auto whole = (butil::atomic<unsigned>*)m->butex;
    while (whole->exchange(BTHREAD_MUTEX_CONTENDED) & BTHREAD_MUTEX_LOCKED) {
        if (bthread::butex_wait(whole, BTHREAD_MUTEX_CONTENDED, abstime, queue_lifo) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR/*note*/) {
            // A mutex lock should ignore interruptions in general since
            // user code is unlikely to check the return value.
            return errno;
        }
        // Ignore EWOULDBLOCK and EINTR.
        if (first_wait && 0 == errno) {
            first_wait = false;
        }
        if (!first_wait) {
            // Normally, bthreads are queued in FIFO order. But competing with new
            // arriving bthreads over the ownership of mutex, a woken up bthread
            // has good chances of losing. Because new arriving bthreads are already
            // running on CPU and there can be lots of them. In such case, for fairness,
            // to avoid starvation, it is queued at the head of the waiter queue.
            queue_lifo = true;
        }
    }
    BTHREAD_MUTEX_SET_OWNER;
    return 0;
}

#ifdef BTHREAD_USE_FAST_PTHREAD_MUTEX
namespace internal {

FastPthreadMutex::FastPthreadMutex() : _futex(0) {
    MUTEX_RESET_OWNER_COMMON(_owner);
}

int FastPthreadMutex::lock_contended(const struct timespec* abstime) {
    int64_t abstime_us = 0;
    if (NULL != abstime) {
        abstime_us = butil::timespec_to_microseconds(*abstime);
    }
    // whole 为 MutexInternal == unsigned 同占用 4 字节
    auto whole = (butil::atomic<unsigned>*)&_futex;
    // 第一个调用whole->exchange(BTHREAD_MUTEX_CONTENDED)将返回BTHREAD_MUTEX_LOCKED
    // 后续调用的，将返回BTHREAD_MUTEX_CONTENDED
    while (whole->exchange(BTHREAD_MUTEX_CONTENDED) & BTHREAD_MUTEX_LOCKED) {
        // 如果上锁失败，且没有超时
        timespec* ptimeout = NULL;
        timespec timeout{};
        if (NULL != abstime) {
            timeout = butil::microseconds_to_timespec(
                abstime_us - butil::gettimeofday_us());
            ptimeout = &timeout;
        }
        if (NULL == abstime  || abstime_us > MIN_SLEEP_US) {
            // 进入系统调用，挂起当前线程
            if (futex_wait_private(whole, BTHREAD_MUTEX_CONTENDED, ptimeout) < 0
                && errno != EWOULDBLOCK && errno != EINTR/*note*/) {
                // A mutex lock should ignore interruptions in general since
                // user code is unlikely to check the return value.
                return errno;
            }
        } else {
            errno = ETIMEDOUT;
            return errno;
        }
    }
    // 这里上锁成功
    PTHREAD_MUTEX_SET_OWNER(_owner);
    ADD_TLS_PTHREAD_LOCK_COUNT;
    return 0;
}

void FastPthreadMutex::lock() {
    if (try_lock()) {
        // 上锁成功直接返回
        return;
    }

    // 未能上锁
    PTHREAD_MUTEX_CHECK_OWNER(_owner);
    (void)lock_contended(NULL);
}

bool FastPthreadMutex::try_lock() {
    auto split = (bthread::MutexInternal*)&_futex;
    // 目前看下来 返回0 代表上锁成功
    // 取 !0 标记为 上锁成功，lock = true
    bool lock = !split->locked.exchange(1, butil::memory_order_acquire);
    if (lock) {
        PTHREAD_MUTEX_SET_OWNER(_owner);
        ADD_TLS_PTHREAD_LOCK_COUNT;
    }
    return lock;
}

bool FastPthreadMutex::timed_lock(const struct timespec* abstime) {
    if (try_lock()) {
        return true;
    }
    return 0 == lock_contended(abstime);
}

void FastPthreadMutex::unlock() {
    SUB_TLS_PTHREAD_LOCK_COUNT;
    MUTEX_RESET_OWNER_COMMON(_owner);
    auto whole = (butil::atomic<unsigned>*)&_futex;
    const unsigned prev = whole->exchange(0, butil::memory_order_release);
    // CAUTION: the mutex may be destroyed, check comments before butex_create
    if (prev != BTHREAD_MUTEX_LOCKED) {
        futex_wake_private(whole, 1);
    }
}

} // namespace internal
#endif // BTHREAD_USE_FAST_PTHREAD_MUTEX

void FastPthreadMutex::lock() {
    internal::pthread_mutex_lock_impl(&_mutex, NULL);
}

void FastPthreadMutex::unlock() {
    internal::pthread_mutex_unlock_impl(&_mutex);
}

#if defined(BTHREAD_USE_FAST_PTHREAD_MUTEX) || HAS_PTHREAD_MUTEX_TIMEDLOCK
bool FastPthreadMutex::timed_lock(const struct timespec* abstime) {
    return internal::pthread_mutex_lock_impl(&_mutex, abstime) == 0;
}
#endif // BTHREAD_USE_FAST_PTHREAD_MUTEX HAS_PTHREAD_MUTEX_TIMEDLOCK

} // namespace bthread

__BEGIN_DECLS

int bthread_mutex_init(bthread_mutex_t* __restrict m,
                       const bthread_mutexattr_t* __restrict attr) {
    bthread::make_contention_site_invalid(&m->csite);
    MUTEX_RESET_OWNER_COMMON(m->owner);
    m->butex = bthread::butex_create_checked<unsigned>();
    if (!m->butex) {
        return ENOMEM;
    }
    *m->butex = 0;
    m->enable_csite = NULL == attr ? true : attr->enable_csite;
    return 0;
}

int bthread_mutex_destroy(bthread_mutex_t* m) {
    bthread::butex_destroy(m->butex);
    return 0;
}

int bthread_mutex_trylock(bthread_mutex_t* m) {
    return bthread::mutex_trylock_impl(m);
}

int bthread_mutex_lock_contended(bthread_mutex_t* m) {
    return bthread::mutex_lock_contended_impl(m, NULL);
}

static int bthread_mutex_lock_impl(bthread_mutex_t* __restrict m,
                                   const struct timespec* __restrict abstime) {
    if (0 == bthread::mutex_trylock_impl(m)) {
        return 0;
    }
    // Don't sample when contention profiler is off.
    if (!bthread::g_cp) {
        return bthread::mutex_lock_contended_impl(m, abstime);
    }
    // Ask Collector if this (contended) locking should be sampled.
    const size_t sampling_range =
        m->enable_csite ? bvar::is_collectable(&bthread::g_cp_sl) : bvar::INVALID_SAMPLING_RANGE;
    if (!bvar::is_sampling_range_valid(sampling_range)) { // Don't sample
        return bthread::mutex_lock_contended_impl(m, abstime);
    }
    // Start sampling.
    const int64_t start_ns = butil::cpuwide_time_ns();
    // NOTE: Don't modify m->csite outside lock since multiple threads are
    // still contending with each other.
    const int rc = bthread::mutex_lock_contended_impl(m, abstime);
    if (!rc) { // Inside lock
        m->csite.duration_ns = butil::cpuwide_time_ns() - start_ns;
        m->csite.sampling_range = sampling_range;
    } else if (rc == ETIMEDOUT) {
        // Failed to lock due to ETIMEDOUT, submit the elapse directly.
        const int64_t end_ns = butil::cpuwide_time_ns();
        const bthread_contention_site_t csite = {end_ns - start_ns, sampling_range};
        bthread::submit_contention(csite, end_ns);
    }
    return rc;
}

int bthread_mutex_lock(bthread_mutex_t* m) {
    return bthread_mutex_lock_impl(m, NULL);
}

int bthread_mutex_timedlock(bthread_mutex_t* __restrict m,
                            const struct timespec* __restrict abstime) {
    return bthread_mutex_lock_impl(m, abstime);
}

int bthread_mutex_unlock(bthread_mutex_t* m) {
    auto whole = (butil::atomic<unsigned>*)m->butex;
    bthread_contention_site_t saved_csite = {0, 0};
    bool is_valid = bthread::is_contention_site_valid(m->csite);
    if (is_valid) {
        saved_csite = m->csite;
        bthread::make_contention_site_invalid(&m->csite);
    }
    MUTEX_RESET_OWNER_COMMON(m->owner);
    const unsigned prev = whole->exchange(0, butil::memory_order_release);
    // CAUTION: the mutex may be destroyed, check comments before butex_create
    if (prev == BTHREAD_MUTEX_LOCKED) {
        return 0;
    }
    // Wakeup one waiter
    if (!is_valid) {
        bthread::butex_wake(whole);
        return 0;
    }
    const int64_t unlock_start_ns = butil::cpuwide_time_ns();
    bthread::butex_wake(whole);
    const int64_t unlock_end_ns = butil::cpuwide_time_ns();
    saved_csite.duration_ns += unlock_end_ns - unlock_start_ns;
    bthread::submit_contention(saved_csite, unlock_end_ns);
    return 0;
}

int bthread_mutexattr_init(bthread_mutexattr_t* attr) {
    attr->enable_csite = true;
    return 0;
}

int bthread_mutexattr_disable_csite(bthread_mutexattr_t* attr) {
    attr->enable_csite = false;
    return 0;
}

int bthread_mutexattr_destroy(bthread_mutexattr_t* attr) {
    attr->enable_csite = true;
    return 0;
}

#ifndef NO_PTHREAD_MUTEX_HOOK

int pthread_mutex_init(pthread_mutex_t * __restrict mutex,
                       const pthread_mutexattr_t* __restrict mutexattr) {
    INIT_MUTEX_OWNER_MAP_ENTRY(mutex, mutexattr);
    return bthread::sys_pthread_mutex_init(mutex, mutexattr);
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    DESTROY_MUTEX_OWNER_MAP_ENTRY(mutex);
    return bthread::sys_pthread_mutex_destroy(mutex);
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    return bthread::pthread_mutex_lock_impl(mutex);
}

#if defined(OS_LINUX) && defined(OS_POSIX) && defined(__USE_XOPEN2K)
int pthread_mutex_timedlock(pthread_mutex_t *__restrict __mutex,
				            const struct timespec *__restrict __abstime) {
    return bthread::pthread_mutex_timedlock_impl(__mutex, __abstime);
}
#endif // OS_POSIX __USE_XOPEN2K

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    return bthread::pthread_mutex_trylock_impl(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    return bthread::pthread_mutex_unlock_impl(mutex);
}
#endif // NO_PTHREAD_MUTEX_HOOK


__END_DECLS
