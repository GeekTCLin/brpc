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

// Date: Tue Jul 10 17:40:58 CST 2012

#ifndef BTHREAD_WORK_STEALING_QUEUE_H
#define BTHREAD_WORK_STEALING_QUEUE_H

#include "butil/macros.h"
#include "butil/atomicops.h"
#include "butil/logging.h"

namespace bthread {

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue()
        : _bottom(1)
        , _capacity(0)
        , _buffer(NULL)
        , _top(1) {
    }

    ~WorkStealingQueue() {
        delete [] _buffer;
        _buffer = NULL;
    }

    int init(size_t capacity) {
        if (_capacity != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (capacity == 0) {
            LOG(ERROR) << "Invalid capacity=" << capacity;
            return -1;
        }
        if (capacity & (capacity - 1)) {
            LOG(ERROR) << "Invalid capacity=" << capacity
                       << " which must be power of 2";
            return -1;
        }
        _buffer = new(std::nothrow) T[capacity];
        if (NULL == _buffer) {
            return -1;
        }
        _capacity = capacity;
        return 0;
    }

    // Push an item into the queue.
    // Returns true on pushed.
    // May run in parallel with steal().
    // Never run in parallel with pop() or another push().
    bool push(const T& x) {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        const size_t t = _top.load(butil::memory_order_acquire);
        if (b >= t + _capacity) { // Full queue.
            return false;
        }
        _buffer[b & (_capacity - 1)] = x;
        // memory_order_release 保证 _buffer[b & (_capacity - 1)] = x; 不会在 后面执行
        // 保证其他线程 获取 b 不会先取到 b+1 但 buffer[b & (_capacity - 1)] 未存储 x
        _bottom.store(b + 1, butil::memory_order_release);
        return true;
    }

    // Pop an item from the queue.
    // Returns true on popped and the item is written to `val'.
    // May run in parallel with steal().
    // Never run in parallel with push() or another pop().
    bool pop(T* val) {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        size_t t = _top.load(butil::memory_order_relaxed);
        if (t >= b) {
            // fast check since we call pop() in each sched.
            // Stale _top which is smaller should not enter this branch.
            return false;
        }
        const size_t newb = b - 1;
        _bottom.store(newb, butil::memory_order_relaxed);
        butil::atomic_thread_fence(butil::memory_order_seq_cst);
        t = _top.load(butil::memory_order_relaxed);
        if (t > newb) {
            // steal() 太快，导致 newb位置的Item被steal，所以pop 失败？
            _bottom.store(b, butil::memory_order_relaxed);
            return false;
        }
        *val = _buffer[newb & (_capacity - 1)];
        if (t != newb) {
            // steal() 没那么快，成功了 哈哈哈
            return true;
        }
        // t == newb 只有最后一个Item，和 steal 竞争
        // 如果 CAS _top 从 t 到 t+1 成功，则pop成功，反之被 steal了
        // Single last element, compete with steal()
        const bool popped = _top.compare_exchange_strong(
            t, t + 1, butil::memory_order_seq_cst, butil::memory_order_relaxed);
        _bottom.store(b, butil::memory_order_relaxed);
        return popped;
    }

    // Steal one item from the queue.
    // Returns true on stolen.
    // May run in parallel with push() pop() or another steal().
    // steal() 和 push() 、 pop() 以及 steal() 可并行
    bool steal(T* val) {
        size_t t = _top.load(butil::memory_order_acquire);
        size_t b = _bottom.load(butil::memory_order_acquire);
        if (t >= b) {
            // Permit false negative for performance considerations.
            return false;
        }
        do {
            butil::atomic_thread_fence(butil::memory_order_seq_cst);
            b = _bottom.load(butil::memory_order_acquire);
            if (t >= b) {
                return false;
            }
            *val = _buffer[t & (_capacity - 1)];
            // 若 CAS 检测 _top 从 t 变化 到 t+1 失败，重复尝试 steal
        } while (!_top.compare_exchange_strong(t, t + 1,
                                               butil::memory_order_seq_cst,
                                               butil::memory_order_relaxed));
        return true;
    }

    size_t volatile_size() const {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        const size_t t = _top.load(butil::memory_order_relaxed);
        return (b <= t ? 0 : (b - t));
    }

    size_t capacity() const { return _capacity; }

private:
    // Copying a concurrent structure makes no sense.
    DISALLOW_COPY_AND_ASSIGN(WorkStealingQueue);

    butil::atomic<size_t> _bottom;          // 末尾Item 下标 + 1
    size_t _capacity;                       // 容量
    T* _buffer;                             // Item数组
    // BAIDU_CACHELINE_ALIGNMENT __attribute__((aligned(64)))  应该是用于防止伪共享吧
    BAIDU_CACHELINE_ALIGNMENT butil::atomic<size_t> _top;       // 首个Item 下标
};

}  // namespace bthread

#endif  // BTHREAD_WORK_STEALING_QUEUE_H
