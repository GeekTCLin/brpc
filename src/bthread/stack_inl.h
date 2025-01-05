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

// Date: Sun Sep  7 22:37:39 CST 2014

// stack_inl.h 主要提供函数栈的内存申请 有 mmap 以及 malloc 两种
// 以及 context 上下文创建（MainStack 没有 上下文）
// 和 Stack的资源回收，MainStack 直接delete，其余有大小（有context和storage的）使用ObjectPool维护
#ifndef BTHREAD_ALLOCATE_STACK_INL_H
#define BTHREAD_ALLOCATE_STACK_INL_H

DECLARE_int32(guard_page_size);
DECLARE_int32(tc_stack_small);
DECLARE_int32(tc_stack_normal);

namespace bthread {

struct MainStackClass {};

struct SmallStackClass {
    static int* stack_size_flag;
    // Older gcc does not allow static const enum, use int instead.
    static const int stacktype = (int)STACK_TYPE_SMALL;
};

struct NormalStackClass {
    static int* stack_size_flag;
    static const int stacktype = (int)STACK_TYPE_NORMAL;
};

struct LargeStackClass {
    static int* stack_size_flag;
    static const int stacktype = (int)STACK_TYPE_LARGE;
};

template <typename StackClass> struct StackFactory {
    struct Wrapper : public ContextualStack {
        // void (*entry)(intptr_t) 函数指针，方法栈执行的 函数
        explicit Wrapper(void (*entry)(intptr_t)) {
            if (allocate_stack_storage(&storage, *StackClass::stack_size_flag,
                                       FLAGS_guard_page_size) != 0) {
                storage.zeroize();
                context = NULL;
                return;
            }
            // 在分配好的栈空间初始化协程上下文
            context = bthread_make_fcontext(storage.bottom, storage.stacksize, entry);
            stacktype = (StackType)StackClass::stacktype;
        }
        ~Wrapper() {
            if (context) {
                context = NULL;
                deallocate_stack_storage(&storage);
                storage.zeroize();
            }
        }
    };
    
    // Wrapper 由 ObjectPool 进行资源管理
    static ContextualStack* get_stack(void (*entry)(intptr_t)) {
        return butil::get_object<Wrapper>(entry);
    }
    
    static void return_stack(ContextualStack* sc) {
        butil::return_object(static_cast<Wrapper*>(sc));
    }
};

// 主栈偏特化
template <> struct StackFactory<MainStackClass> {
    static ContextualStack* get_stack(void (*)(intptr_t)) {
        ContextualStack* s = new (std::nothrow) ContextualStack;
        if (NULL == s) {
            return NULL;
        }
        s->context = NULL;      // 主栈没有设置上下文？
        s->stacktype = STACK_TYPE_MAIN;
        s->storage.zeroize();
        return s;
    }
    
    static void return_stack(ContextualStack* s) {
        // 主栈对象 内存资源不受ObjectPool 维护，直接delete释放空间
        delete s;
    }
};

inline ContextualStack* get_stack(StackType type, void (*entry)(intptr_t)) {
    switch (type) {
    case STACK_TYPE_PTHREAD:
        return NULL;
    case STACK_TYPE_SMALL:
        return StackFactory<SmallStackClass>::get_stack(entry);
    case STACK_TYPE_NORMAL: // 默认使用normal
        return StackFactory<NormalStackClass>::get_stack(entry);
    case STACK_TYPE_LARGE:
        return StackFactory<LargeStackClass>::get_stack(entry);
    case STACK_TYPE_MAIN:
        return StackFactory<MainStackClass>::get_stack(entry);
    }
    return NULL;
}

inline void return_stack(ContextualStack* s) {
    if (NULL == s) {
        return;
    }
    switch (s->stacktype) {
    case STACK_TYPE_PTHREAD:
        assert(false);
        return;
    case STACK_TYPE_SMALL:
        return StackFactory<SmallStackClass>::return_stack(s);
    case STACK_TYPE_NORMAL:
        return StackFactory<NormalStackClass>::return_stack(s);
    case STACK_TYPE_LARGE:
        return StackFactory<LargeStackClass>::return_stack(s);
    case STACK_TYPE_MAIN:
        return StackFactory<MainStackClass>::return_stack(s);
    }
}

// 切换函数栈
inline void jump_stack(ContextualStack* from, ContextualStack* to) {
    bthread_jump_fcontext(&from->context, to->context, 0/*not skip remained*/);
}

}  // namespace bthread

namespace butil {

template <> struct ObjectPoolBlockMaxItem<
    bthread::StackFactory<bthread::LargeStackClass>::Wrapper> {
    static const size_t value = 64;
};
template <> struct ObjectPoolBlockMaxItem<
    bthread::StackFactory<bthread::NormalStackClass>::Wrapper> {
    static const size_t value = 64;
};

template <> struct ObjectPoolBlockMaxItem<
    bthread::StackFactory<bthread::SmallStackClass>::Wrapper> {
    static const size_t value = 64;
};

template <> struct ObjectPoolFreeChunkMaxItem<
    bthread::StackFactory<bthread::SmallStackClass>::Wrapper> {
    inline static size_t value() {
        return (FLAGS_tc_stack_small <= 0 ? 0 : FLAGS_tc_stack_small);
    }
};

template <> struct ObjectPoolFreeChunkMaxItem<
    bthread::StackFactory<bthread::NormalStackClass>::Wrapper> {
    inline static size_t value() {
        return (FLAGS_tc_stack_normal <= 0 ? 0 : FLAGS_tc_stack_normal);
    }
};

template <> struct ObjectPoolFreeChunkMaxItem<
    bthread::StackFactory<bthread::LargeStackClass>::Wrapper> {
    inline static size_t value() { return 1UL; }
};

template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::LargeStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::LargeStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};

template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::NormalStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::NormalStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};

template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::SmallStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::SmallStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};
    
}  // namespace butil

#endif  // BTHREAD_ALLOCATE_STACK_INL_H
