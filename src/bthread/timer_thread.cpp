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


#include <queue>                           // heap functions
#include "butil/scoped_lock.h"
#include "butil/logging.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"   // fmix64
#include "butil/resource_pool.h"
#include "butil/threading/platform_thread.h"
#include "bvar/bvar.h"
#include "bthread/sys_futex.h"
#include "bthread/timer_thread.h"
#include "bthread/log.h"

namespace bthread {

// Defined in task_control.cpp
void run_worker_startfn();

const TimerThread::TaskId TimerThread::INVALID_TASK_ID = 0;

TimerThreadOptions::TimerThreadOptions()
    : num_buckets(13) { // 默认13 个桶
}

// A task contains the necessary information for running fn(arg).
// Tasks are created in Bucket::schedule and destroyed in TimerThread::run
// 时间任务结构体
struct BAIDU_CACHELINE_ALIGNMENT TimerThread::Task {
    Task* next;                 // For linking tasks in a Bucket. 链表下一个节点
    int64_t run_time;           // run the task at this realtime  运行实际时间  
    void (*fn)(void*);          // the fn(arg) to run   执行函数
    void* arg;                  // 执行函数参数
    // Current TaskId, checked against version in TimerThread::run to test
    // if this task is unscheduled.
    TaskId task_id;             // 包含 初始version + ResourcePool slot

    // initial_version:     not run yet
    // initial_version + 1: running
    // initial_version + 2: removed (also the version of next Task reused
    //                      this struct)
    butil::atomic<uint32_t> version;    // 当前version

    Task() : version(2/*skip 0*/) {}    // 默认初始版本为2

    // Run this task and delete this struct.
    // Returns true if fn(arg) did run.
    bool run_and_delete();

    // Delete this struct if this task was unscheduled.
    // Returns true on deletion.
    bool try_delete();
};

// Timer tasks are sharded into different Buckets to reduce contentions.
class BAIDU_CACHELINE_ALIGNMENT TimerThread::Bucket {
public:
    Bucket()
        : _nearest_run_time(std::numeric_limits<int64_t>::max())
        , _task_head(NULL) {
    }

    ~Bucket() {}

    struct ScheduleResult {
        TimerThread::TaskId task_id;
        bool earlier;
    };
    
    // Schedule a task into this bucket.
    // Returns the TaskId and if it has the nearest run time.
    ScheduleResult schedule(void (*fn)(void*), void* arg,
                            const timespec& abstime);

    // Pull all scheduled tasks.
    // This function is called in timer thread.
    Task* consume_tasks();

private:
    FastPthreadMutex _mutex;    // 限制对 _task_head 以及 _nearest_run_time 的访问
    int64_t _nearest_run_time;  // 任务链表中最早执行任务的时间
    Task* _task_head;           // 任务链表
};

// Utilies for making and extracting TaskId.
// 创建taskId，高32位为版本（默认为2）低32位为 ResoucePool slot 下标
inline TimerThread::TaskId make_task_id(
    butil::ResourceId<TimerThread::Task> slot, uint32_t version) {
    return TimerThread::TaskId((((uint64_t)version) << 32) | slot.value);
}

inline
butil::ResourceId<TimerThread::Task> slot_of_task_id(TimerThread::TaskId id) {
    butil::ResourceId<TimerThread::Task> slot = { (id & 0xFFFFFFFFul) };
    return slot;
}

inline uint32_t version_of_task_id(TimerThread::TaskId id) {
    return (uint32_t)(id >> 32);
}

inline bool task_greater(const TimerThread::Task* a, const TimerThread::Task* b) {
    return a->run_time > b->run_time;
}

void* TimerThread::run_this(void* arg) {
    butil::PlatformThread::SetName("brpc_timer");
    static_cast<TimerThread*>(arg)->run();
    return NULL;
}

TimerThread::TimerThread()
    : _started(false)
    , _stop(false)
    , _buckets(NULL)
    , _nearest_run_time(std::numeric_limits<int64_t>::max())
    , _nsignals(0)
    , _thread(0) {
}

TimerThread::~TimerThread() {
    stop_and_join();
    delete [] _buckets;
    _buckets = NULL;
}

int TimerThread::start(const TimerThreadOptions* options_in) {
    if (_started) {
        return 0;
    }
    if (options_in) {
        _options = *options_in;
    }
    if (_options.num_buckets == 0) {
        LOG(ERROR) << "num_buckets can't be 0";
        return EINVAL;
    }
    if (_options.num_buckets > 1024) {
        LOG(ERROR) << "num_buckets=" << _options.num_buckets << " is too big";
        return EINVAL;
    }
    _buckets = new (std::nothrow) Bucket[_options.num_buckets];
    if (NULL == _buckets) {
        LOG(ERROR) << "Fail to new _buckets";
        return ENOMEM;
    }        
    const int ret = pthread_create(&_thread, NULL, TimerThread::run_this, this);
    if (ret) {
        return ret;
    }
    _started = true;
    return 0;
}

TimerThread::Task* TimerThread::Bucket::consume_tasks() {
    Task* head = NULL;
    if (_task_head) { // NOTE: schedule() and consume_tasks() are sequenced
        // by TimerThread._nearest_run_time and fenced by TimerThread._mutex.
        // We can avoid touching the mutex and related cacheline when the
        // bucket is actually empty.
        BAIDU_SCOPED_LOCK(_mutex);
        if (_task_head) {
            head = _task_head;
            _task_head = NULL;
            _nearest_run_time = std::numeric_limits<int64_t>::max();
        }
    }
    return head;
}

// bucket 注册任务
TimerThread::Bucket::ScheduleResult
TimerThread::Bucket::schedule(void (*fn)(void*), void* arg,
                              const timespec& abstime) {
    // 申请一个Task 对象空间
    butil::ResourceId<Task> slot_id;
    Task* task = butil::get_resource<Task>(&slot_id);
    if (task == NULL) {
        ScheduleResult result = { INVALID_TASK_ID, false };
        return result;
    }
    task->next = NULL;
    task->fn = fn;
    task->arg = arg;
    task->run_time = butil::timespec_to_microseconds(abstime);
    uint32_t version = task->version.load(butil::memory_order_relaxed);
    if (version == 0) {  // skip 0.
        task->version.fetch_add(2, butil::memory_order_relaxed);
        version = 2;
    }

    // 创建taskId
    const TaskId id = make_task_id(slot_id, version);
    task->task_id = id;
    bool earlier = false;
    {
        // 上锁操作bucket 的链表，头插法
        BAIDU_SCOPED_LOCK(_mutex);
        task->next = _task_head;
        _task_head = task;
        if (task->run_time < _nearest_run_time) {
            _nearest_run_time = task->run_time;
            earlier = true;
        }
    }
    ScheduleResult result = { id, earlier };
    return result;
}

// TimerThread 注册任务
TimerThread::TaskId TimerThread::schedule(
    void (*fn)(void*), void* arg, const timespec& abstime) {
    if (_stop.load(butil::memory_order_relaxed) || !_started) {
        // Not add tasks when TimerThread is about to stop.
        return INVALID_TASK_ID;
    }
    // Hashing by pthread id is better for cache locality.
    // Hash 一个桶用于添加任务
    const Bucket::ScheduleResult result = 
        _buckets[butil::fmix64(pthread_numeric_id()) % _options.num_buckets]
        .schedule(fn, arg, abstime);
    // 如果添加的任务 更新了 bucket的 _nearest_run_time
    // 进一步检测是否更新 全局 _nearest_run_time，若有更新，则唤醒 timer thread
    if (result.earlier) {
        bool earlier = false;
        const int64_t run_time = butil::timespec_to_microseconds(abstime);
        {
            BAIDU_SCOPED_LOCK(_mutex);
            if (run_time < _nearest_run_time) {
                _nearest_run_time = run_time;
                ++_nsignals;        // 更改 nsignals，用于futex_wake_private
                earlier = true;
            }
        }
        if (earlier) {
            futex_wake_private(&_nsignals, 1);
        }
    }
    return result.task_id;
}

// Notice that we don't recycle the Task in this function, let TimerThread::run
// do it. The side effect（副作用) is that we may allocate many unscheduled tasks before
// TimerThread wakes up. The number is approximately qps * timeout_s.

// Under the precondition that ResourcePool<Task> caches 128K for each thread, with some
// further calculations, we can conclude that in a RPC scenario:
//   when timeout / latency < 2730 (128K / sizeof(Task))
// unscheduled tasks do not occupy additional memory. 2730 is a large ratio
// between timeout and latency in most RPC scenarios, this is why we don't
// try to reuse tasks right now inside unschedule() with more complicated code.

// 在 ResourcePool<Task> 缓存 128K 的情况下，当 timeout / latency < 2730 时，未调度的任务不会占用额外的内存
int TimerThread::unschedule(TaskId task_id) {
    const butil::ResourceId<Task> slot_id = slot_of_task_id(task_id);
    Task* const task = butil::address_resource(slot_id);
    if (task == NULL) {
        LOG(ERROR) << "Invalid task_id=" << task_id;
        return -1;
    }
    // 从task_id 中获取初始版本号
    const uint32_t id_version = version_of_task_id(task_id);
    uint32_t expected_version = id_version;
    // This CAS is rarely contended, should be fast.
    // The acquire fence is paired with release fence in Task::run_and_delete
    // to make sure that we see all changes brought by fn(arg).
    // 通过CAS操作，将版本号从 expected_version  更新为 id_version + 2，将其标记为任务取消
    if (task->version.compare_exchange_strong(
            expected_version, id_version + 2,
            butil::memory_order_acquire)) {
        return 0;
    }
    // id_version + 1 means the task is running. We can't unschedule it.
    return (expected_version == id_version + 1) ? 1 : -1;
}

// Task 执行并且删除（若执行成功）
bool TimerThread::Task::run_and_delete() {
    const uint32_t id_version = version_of_task_id(task_id);
    uint32_t expected_version = id_version;
    // This CAS is rarely contended, should be fast.
    if (version.compare_exchange_strong(
            expected_version, id_version + 1, butil::memory_order_relaxed)) {
        // CAS 从 expected_version 更新为 id_version + 1，标记任务正在执行
        fn(arg);
        // The release fence is paired with acquire fence in
        // TimerThread::unschedule to make changes of fn(arg) visible.
        // 更新为 id_version + 2，标记任务已经执行，可删除
        version.store(id_version + 2, butil::memory_order_release);
        butil::return_resource(slot_of_task_id(task_id));
        return true;
    } else if (expected_version == id_version + 2) {
        // already unscheduled.
        butil::return_resource(slot_of_task_id(task_id));
        return false;
    } else {
        // Impossible.
        LOG(ERROR) << "Invalid version=" << expected_version
                   << ", expecting " << id_version + 2;
        return false;
    }
}

// 删除任务，释放ResoucePool 空间
bool TimerThread::Task::try_delete() {
    const uint32_t id_version = version_of_task_id(task_id);
    if (version.load(butil::memory_order_relaxed) != id_version) {
        CHECK_EQ(version.load(butil::memory_order_relaxed), id_version + 2);
        butil::return_resource(slot_of_task_id(task_id));
        return true;
    }
    return false;
}

template <typename T>
static T deref_value(void* arg) {
    return *(T*)arg;
}

void TimerThread::run() {
    run_worker_startfn();
#ifdef BAIDU_INTERNAL
    logging::ComlogInitializer comlog_initializer;
#endif

    int64_t last_sleep_time = butil::gettimeofday_us();
    BT_VLOG << "Started TimerThread=" << pthread_self();

    // min heap of tasks (ordered by run_time)
    std::vector<Task*> tasks;
    tasks.reserve(4096);

    // vars
    size_t nscheduled = 0;
    bvar::PassiveStatus<size_t> nscheduled_var(deref_value<size_t>, &nscheduled);
    bvar::PerSecond<bvar::PassiveStatus<size_t> > nscheduled_second(&nscheduled_var);
    size_t ntriggered = 0;
    bvar::PassiveStatus<size_t> ntriggered_var(deref_value<size_t>, &ntriggered);
    bvar::PerSecond<bvar::PassiveStatus<size_t> > ntriggered_second(&ntriggered_var);
    double busy_seconds = 0;
    bvar::PassiveStatus<double> busy_seconds_var(deref_value<double>, &busy_seconds);
    bvar::PerSecond<bvar::PassiveStatus<double> > busy_seconds_second(&busy_seconds_var);
    if (!_options.bvar_prefix.empty()) {
        nscheduled_second.expose_as(_options.bvar_prefix, "scheduled_second");
        ntriggered_second.expose_as(_options.bvar_prefix, "triggered_second");
        busy_seconds_second.expose_as(_options.bvar_prefix, "usage");
    }
    
    // mian loop
    while (!_stop.load(butil::memory_order_relaxed)) {
        // Clear _nearest_run_time before consuming tasks from buckets.
        // This helps us to be aware of earliest task of the new tasks before we
        // would run the consumed tasks.
        {
            // 锁1，重置 _nearest_run_time
            BAIDU_SCOPED_LOCK(_mutex);
            _nearest_run_time = std::numeric_limits<int64_t>::max();
        }
        
        // Pull tasks from buckets.
        // 从buckets 中取出任务
        for (size_t i = 0; i < _options.num_buckets; ++i) {
            Bucket& bucket = _buckets[i];
            for (Task* p = bucket.consume_tasks(); p != nullptr; ++nscheduled) {
                // 遍历bucket的任务
                // p->next should be kept first
                // in case of the deletion of Task p which is unscheduled
                Task* next_task = p->next;

                if (!p->try_delete()) { // remove the task if it's unscheduled
                //  如果任务 p 未被取消，则加入到 tasks 中
                    tasks.push_back(p);
                    std::push_heap(tasks.begin(), tasks.end(), task_greater);
                }
                p = next_task;
            }
        }

        bool pull_again = false;
        while (!tasks.empty()) {
            Task* task1 = tasks[0];  // the about-to-run task
            if (butil::gettimeofday_us() < task1->run_time) {  // not ready yet.
                break;
            }
            // Each time before we run the earliest task (that we think), 
            // check the globally shared _nearest_run_time. If a task earlier
            // than task1 was scheduled during pulling from buckets, we'll
            // know. In RPC scenarios, _nearest_run_time is not often changed by
            // threads because the task needs to be the earliest in its bucket,
            // since run_time of scheduled tasks are often in ascending order,
            // most tasks are unlikely to be "earliest". (If run_time of tasks
            // are in descending orders, all tasks are "earliest" after every
            // insertion, and they'll grab _mutex and change _nearest_run_time
            // frequently, fortunately this is not true at most of time).
            {
                // 锁2，检查 _nearest_run_time 是否可能有变化
                // 因为从 锁1 - 锁2 从buckets 中取出任务， 或者 锁2 外部这个循环 可能有新任务加入，导致 _nearest_run_time 变小
                // 所以需要重新拉取
                BAIDU_SCOPED_LOCK(_mutex);
                if (task1->run_time > _nearest_run_time) {
                    // a task is earlier than task1. We need to check buckets.
                    pull_again = true;
                    break;
                }
            }
            // 堆排序，将最早的任务放到最后
            std::pop_heap(tasks.begin(), tasks.end(), task_greater);
            tasks.pop_back();
            if (task1->run_and_delete()) {
                ++ntriggered;
            }
        }
        if (pull_again) {
            BT_VLOG << "pull again, tasks=" << tasks.size();
            continue;
        }

        // The realtime to wait for.
        // 计算 time thread wait 时间
        int64_t next_run_time = std::numeric_limits<int64_t>::max();
        if (!tasks.empty()) {
            next_run_time = tasks[0]->run_time;
        }
        // Similarly with the situation before running tasks, we check
        // _nearest_run_time to prevent us from waiting on a non-earliest
        // task. We also use the _nsignal to make sure that if new task 
        // is earlier than the realtime that we wait for, we'll wake up.
        int expected_nsignals = 0;
        {
            BAIDU_SCOPED_LOCK(_mutex);
            if (next_run_time > _nearest_run_time) {
                // a task is earlier than what we would wait for.
                // We need to check the buckets.
                // 在计算 next_run_time 到此临界区之间存在新任务加入，且修改了 _nearest_run_time
                continue;
            } else {
                _nearest_run_time = next_run_time;
                expected_nsignals = _nsignals;
            }
        }
        timespec* ptimeout = NULL;
        timespec next_timeout = { 0, 0 };
        const int64_t now = butil::gettimeofday_us();
        // 存在下一个任务，计算出 wait时间，反之永久等待
        if (next_run_time != std::numeric_limits<int64_t>::max()) {
            next_timeout = butil::microseconds_to_timespec(next_run_time - now);
            ptimeout = &next_timeout;
        }
        busy_seconds += (now - last_sleep_time) / 1000000.0;
        // 挂起
        futex_wait_private(&_nsignals, expected_nsignals, ptimeout);
        last_sleep_time = butil::gettimeofday_us();
    }
    BT_VLOG << "Ended TimerThread=" << pthread_self();
}

void TimerThread::stop_and_join() {
    _stop.store(true, butil::memory_order_relaxed);
    if (_started) {
        {
            BAIDU_SCOPED_LOCK(_mutex);
             // trigger pull_again and wakeup TimerThread
            _nearest_run_time = 0;
            ++_nsignals;
        }
        if (pthread_self() != _thread) {
            // stop_and_join was not called from a running task.
            // wake up the timer thread in case it is sleeping.
            futex_wake_private(&_nsignals, 1);
            pthread_join(_thread, NULL);
        }
    }
}

static pthread_once_t g_timer_thread_once = PTHREAD_ONCE_INIT;
static TimerThread* g_timer_thread = NULL;
static void init_global_timer_thread() {
    g_timer_thread = new (std::nothrow) TimerThread;
    if (g_timer_thread == NULL) {
        LOG(FATAL) << "Fail to new g_timer_thread";
        return;
    }
    TimerThreadOptions options;
    options.bvar_prefix = "bthread_timer";
    const int rc = g_timer_thread->start(&options);
    if (rc != 0) {
        LOG(FATAL) << "Fail to start timer_thread, " << berror(rc);
        delete g_timer_thread;
        g_timer_thread = NULL;
        return;
    }
}
TimerThread* get_or_create_global_timer_thread() {
    pthread_once(&g_timer_thread_once, init_global_timer_thread);
    return g_timer_thread;
}
TimerThread* get_global_timer_thread() {
    return g_timer_thread;
}

}  // end namespace bthread
