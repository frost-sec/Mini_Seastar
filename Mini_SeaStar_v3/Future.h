#pragma once
#include <stdexcept>
#include <functional>
#include <type_traits>
#include "IntrusivePtr.h"
#include "Poolable.h"

// 声明外部任务调度函数
void schedule_task(std::function<void()> task);

/**
 * 1. State 的改造
 * State 是 Future 和 Promise 共享的核心，必须接入内存池并支持侵入式计数
 */
template<typename T>
struct State : public RefCounted<State<T>>, public Poolable<State<T>>
{
    T value;
    bool ready = false;
    std::function<void(T)> callback;
};

template<>
struct State<void> : public RefCounted<State<void>>, public Poolable<State<void>>
{
    bool ready = false;
    std::function<void()> callback;
};

template<typename T>
class Future;

/**
 * 2. Promise 的改造
 * 因为 TcpConnection 会持有 Promise 的 LocalPtr，所以 Promise 也要池化
 */
template<typename T>
class Promise : public RefCounted<Promise<T>>, public Poolable<Promise<T>>
{
    LocalPtr<State<T>> state; // 替换 std::shared_ptr
    bool future_retrieved_ = false;

public:
    // 使用 make_local 触发内存池分配
    Promise() : state(make_local<State<T>>()) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    Promise(Promise&& other) noexcept : 
        state(std::move(other.state)), 
        future_retrieved_(other.future_retrieved_) {}

    Promise& operator=(Promise&& other) noexcept {
        if (this != &other) {
            state = std::move(other.state);
            future_retrieved_ = other.future_retrieved_;
        }
        return *this;
    }

    Future<T> get_future();

    void set_value(T val) {
        if (!state) return;
        if (state->ready) throw std::runtime_error("Promise already satisfied");

        state->value = std::move(val); // 尽量使用移动语义
        state->ready = true;

        if (state->callback) {
            // 捕获 LocalPtr，增加引用计数（无锁）
            auto bound_task = [s = state]() {
                s->callback(s->value);
            };
            schedule_task(std::move(bound_task));
        }
    }
};

/**
 * 3. Future 的改造
 */
template <typename T>
class Future
{
    LocalPtr<State<T>> state; // 替换 std::shared_ptr

public:
    Future(LocalPtr<State<T>> s) : state(std::move(s)) {}

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    Future(Future&& other) noexcept : state(std::move(other.state)) {}
    Future& operator=(Future&& other) noexcept {
        if (this != &other) state = std::move(other.state);
        return *this;
    }

    template<typename Func>
    auto then(Func func) {
        using U = std::invoke_result_t<Func, T>;

        // 使用 make_local 创建下一阶段的 Promise
        auto next_promise = make_local<Promise<U>>();
        auto next_future = next_promise->get_future();
        
        auto task = [p = next_promise, f = std::move(func)](T value) mutable {
            if constexpr(std::is_void_v<U>) {
                f(std::move(value));
                p->set_value();
            } else {           
                p->set_value(f(std::move(value)));
            }
        };

        if (!state) throw std::runtime_error("No state");

        if (state->ready) {
            task(std::move(state->value));
        } else {
            state->callback = std::move(task);
        }

        return next_future;
    }
    
    // 静态辅助方法：快速创建一个已完成的 Future
    static Future<T> make_ready(T val) {
        auto s = make_local<State<T>>();
        s->value = std::move(val);
        s->ready = true;
        return Future<T>(std::move(s));
    }
};

// --- Promise<void> 特化版本 ---
template<>
class Promise<void> : public RefCounted<Promise<void>>, public Poolable<Promise<void>>
{
    LocalPtr<State<void>> state;
    bool future_retrieved_ = false;

public:
    Promise() : state(make_local<State<void>>()) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    Promise(Promise&& other) noexcept : 
        state(std::move(other.state)), 
        future_retrieved_(other.future_retrieved_) {}

    Promise& operator=(Promise&& other) noexcept {
        if (this != &other) {
            state = std::move(other.state);
            future_retrieved_ = other.future_retrieved_;
        }
        return *this;
    }

    Future<void> get_future();

    void set_value() {
        if (!state) return;
        if (state->ready) throw std::runtime_error("Promise already satisfied");

        state->ready = true;

        if (state->callback) {
            auto bound_task = [s = state]() {
                s->callback();
            };
            schedule_task(std::move(bound_task));
        }
    }
};

// --- Future<void> 特化版本 ---
template <>
class Future<void> {
    LocalPtr<State<void>> state;

public:
    Future(LocalPtr<State<void>> s) : state(std::move(s)) {}

    Future(Future&& other) noexcept : state(std::move(other.state)) {}
    Future& operator=(Future&& other) noexcept {
         if (this != &other) state = std::move(other.state);
         return *this;
    }

    template<typename Func>
    auto then(Func func) {
        using U = std::invoke_result_t<Func>;

        auto next_promise = make_local<Promise<U>>();
        auto next_future = next_promise->get_future();

        auto task = [p = next_promise, f = std::move(func)]() mutable {
            if constexpr(std::is_void_v<U>) {
                f();
                p->set_value();
            } else {
                p->set_value(f());
            }
        };

        if (!state) throw std::runtime_error("No state");

        if (state->ready) {
            task();
        } else {
            state->callback = std::move(task);
        }

        return next_future;
    }

    static Future<void> make_ready() {
        auto s = make_local<State<void>>();
        s->ready = true;
        return Future<void>(std::move(s));
    }
};

// --- 最终的 get_future 实现 ---
template<typename T>
inline Future<T> Promise<T>::get_future() {
    if (future_retrieved_) throw std::logic_error("Future already retrieved");
    if (!state) throw std::runtime_error("No state");
    future_retrieved_ = true;
    return Future<T>(state);
};

inline Future<void> Promise<void>::get_future() {
    if (future_retrieved_) throw std::logic_error("Future already retrieved");
    if (!state) throw std::runtime_error("No state");
    future_retrieved_ = true;
    return Future<void>(state);
};