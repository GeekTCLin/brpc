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

#include "bthread/types.h"
#include "bthread/butex.h"

bthread_once_t::bthread_once_t()
    : _butex(bthread::butex_create_checked<butil::atomic<int>>())  {
    _butex->store(UNINITIALIZED, butil::memory_order_relaxed);
}

bthread_once_t::~bthread_once_t() {
    bthread::butex_destroy(_butex);
}

namespace bthread {

// 保证 init_routine 只会被调用一次
int bthread_once_impl(bthread_once_t* once_control, void (*init_routine)()) {
    butil::atomic<int>* butex = once_control->_butex;
    // We need acquire memory order for this load because if the value
    // signals that initialization has finished, we need to see any
    // data modifications done during initialization.
    int val = butex->load(butil::memory_order_acquire);
    if (BAIDU_LIKELY(val == bthread_once_t::INITIALIZED)) {
        // The initialization has already been done.
        return 0;
    }
    val = bthread_once_t::UNINITIALIZED;
    if (butex->compare_exchange_strong(val, bthread_once_t::INPROGRESS,
                                       butil::memory_order_relaxed,
                                       butil::memory_order_relaxed)) {
        // This (b)thread is the first and the Only one here. Do the initialization.
        // 抢占成功，执行初始化函数
        init_routine();
        // Mark *once_control as having finished the initialization. We need
        // release memory order here because we need to synchronize with other
        // (b)threads that want to use the initialized data.
        butex->store(bthread_once_t::INITIALIZED, butil::memory_order_release);
        // Wake up all other (b)threads.
        // 唤醒所有等待的线程
        bthread::butex_wake_all(butex);
        return 0;
    }

    while (true) {
        // Same as above, we need acquire memory order.
        val = butex->load(butil::memory_order_acquire);
        if (BAIDU_LIKELY(val == bthread_once_t::INITIALIZED)) {
            // The initialization has already been done.
            // 已经初始化完成，返回
            return 0;
        }
        // Unless your constructor can be very time consuming, it is very unlikely o hit
        // this race. When it does, we just wait the thread until the object has been created.
        // 忽略 EINTR（信号中断）和 EWOULDBLOCK（虚假唤醒），其他错误直接返回
        // 反过来理解应该是 EINTR 和 EWOULDBLOCK 会导致唤醒，如果还是没有初始化完成，则会继续等待
        if (bthread::butex_wait(butex, val, NULL) < 0 &&
            errno != EWOULDBLOCK && errno != EINTR/*note*/) {
            return errno;
        }
    }
}

} // namespace bthread

__BEGIN_DECLS

int bthread_once(bthread_once_t* once_control, void (*init_routine)()) {
    return bthread::bthread_once_impl(once_control, init_routine);
}

__END_DECLS