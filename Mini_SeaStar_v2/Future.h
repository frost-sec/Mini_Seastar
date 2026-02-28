#pragma once
#include <memory>
#include <stdexcept>
#include <functional>
#include <iostream>
#include <type_traits>

void schedule_task(std::function<void()> task);

template<typename T>
struct State
{
    T value;
    bool ready=false;
    std::function<void(T)> callback;
};

template<>
struct State<void>
{
    bool ready=false;
    std::function<void()> callback;
};

template<typename T>
class Future;

template<typename T>
class Promise
{
    std::shared_ptr<State<T>> state;
    bool future_retrieved_ =false;

public:

    Promise():state(std::make_shared<State<T>>()) {}

    Promise(const Promise&)=delete;
    Promise operator=(const Promise&)=delete;

    Promise(Promise&& other) noexcept{
        state=std::move(other.state);
        future_retrieved_=other.future_retrieved_;
    }
    Promise& operator=(Promise&& other) noexcept{
        if(this != &other){
            state=std::move(other.state);
            future_retrieved_=other.future_retrieved_;
        }
        return *this;
    }

    Future<T> get_future();

    void set_value(T val)
    {
        if(!state) return;

        if(state->ready)
        {
            throw std::runtime_error("Promise already satisfied");
        }

        state->value=val;
        state->ready=true;

        if(state->callback)
        {
            auto bound_task=[s=state](){
                s->callback(s->value);
            };

            schedule_task(bound_task);
        }
    }
};

template<>
class Promise<void> 
{
    std::shared_ptr<State<void>> state;
    bool future_retrieved_=false;

public:
    Promise():state(std::make_shared<State<void>>()){}

    Promise(const Promise&)=delete;
    Promise& operator=(const Promise&)=delete;

    Promise(Promise&& other) noexcept{
        state=std::move(other.state);
        future_retrieved_=other.future_retrieved_;
    }
    Promise& operator=(Promise&& other) noexcept{
        if(this!=&other){
            state=std::move(other.state);
            future_retrieved_=other.future_retrieved_;
        }

        return *this;
    }

    Future<void> get_future();

    void set_value(){
        if (!state) return;
        if (state->ready) throw std::runtime_error("Promise already satisfied");

        state->ready = true;

        if (state->callback) {
            auto bound_task=[s=state]() {
                s->callback();
            };
            schedule_task(bound_task);
        }
    }
};

template <typename T>
class Future
{
    std::shared_ptr<State<T>> state;

public:

    Future(std::shared_ptr<State<T>> s):state(s) {}

    Future(const Future&)=delete;
    Future operator=(const Future&)=delete;

    Future(Future&& other) noexcept{
        state=std::move(other.state);
    }
    Future& operator=(Future&& other) noexcept{
        if(this != &other)
        {
            state=std::move(other.state);
        }
        return *this;
    }

    template<typename Func>
    auto then(Func func)
    {

        using U=std::invoke_result_t<Func,T>;

        auto next_promise=std::make_shared<Promise<U>>();

        Future<U> next_future=next_promise->get_future();
        
        auto task=[p=next_promise,f=std::move(func)](T value){
            if constexpr(std::is_void_v<U>)
            {
                f(value);
                p->set_value();
            }
            else
            {           
                U result=f(value);
                p->set_value(result);
            }
        };

        if(!state) throw std::runtime_error("No state");

        if(state->ready)
        {
            task(state->value);
        }
        else
        {
            state->callback=task;
        }

        return next_future;
    }
};

template<typename T>
Future<T> Promise<T>::get_future(){
    if(future_retrieved_){
        throw std::logic_error("Future already retrieved");
    }
    if(!state) throw std::runtime_error("No state");

    future_retrieved_=true;
    return Future<T>(state);
};

template <>
class Future<void> {
    std::shared_ptr<State<void>> state;

public:
    Future(std::shared_ptr<State<void>> s) : state(s) {}

    Future(Future&& other) noexcept : state(std::move(other.state)) {}
    Future& operator=(Future&& other) noexcept {
         if (this != &other) state = std::move(other.state);
         return *this;
    }

    template<typename Func>
    auto then(Func func) {
        using U = std::invoke_result_t<Func>;

        auto next_promise = std::make_shared<Promise<U>>();
        auto next_future = next_promise->get_future();

        auto task = [p = next_promise, f = std::move(func)]() {
            if constexpr(std::is_void_v<U>) {
                f();
                p->set_value();
            } else {
                U res = f();  
                p->set_value(res);
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
};

inline Future<void> Promise<void>::get_future() {
    if (future_retrieved_) throw std::logic_error("Future already retrieved");
    future_retrieved_ = true;
    return Future<void>(state);
};