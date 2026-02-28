#pragma once
#include <atomic>
#include <cstddef>
#include <cassert>
#include <new>
#include <utility>
#include <type_traits>

#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE=std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE=64;
#endif

template<typename T,size_t Capacity=64>
class SpscQueue
{
    static_assert((Capacity&(Capacity-1))==0,"Capacity must be a power of 2");
    static_assert(sizeof(T)*Capacity<=1024*1024,"Queue size too large for inline allocation");
    static_assert(std::is_nothrow_destructible_v<T>,"T must be nothrow destructible");

    static constexpr size_t Mask=Capacity-1;

public:
    SpscQueue()=default;

    ~SpscQueue(){
        size_t head=head_.load(std::memory_order_relaxed);
        size_t tail=tail_.load(std::memory_order_relaxed);
        while(tail!=head){
            T* item_ptr=std::launder(reinterpret_cast<T*>(&ring_data_[tail*sizeof(T)]));
            item_ptr->~T();
            tail=(tail+1)&Mask;
        }
    }

    SpscQueue(const SpscQueue&)=delete;
    SpscQueue& operator=(const SpscQueue&)=delete;
    SpscQueue(SpscQueue&& other)=delete;
    SpscQueue& operator=(SpscQueue&& other)=delete;

    template<typename... Args>
    bool emplace(Args&&... args){
        size_t head=head_.load(std::memory_order_relaxed);
        size_t next_head=(head+1)&Mask;

        if(next_head==cached_tail_){
            cached_tail_=tail_.load(std::memory_order_acquire);
            if(next_head==cached_tail_){
                return false;
            }
        }

        T* dest=reinterpret_cast<T*>(&ring_data_[head*sizeof(T)]);
        new(dest) T(std::forward<Args>(args)...);

        head_.store(next_head,std::memory_order_release);
        return true;
    }

    bool push(const T& item){return emplace(item);}
    bool push(T&& item){return emplace(std::move(item));}

    bool pop(T& item){
        size_t tail=tail_.load(std::memory_order_relaxed);
        if(tail==cached_head_){
            cached_head_=head_.load(std::memory_order_acquire);
            if(tail==cached_head_){
                return false;
            }
        }

        T* src=std::launder(reinterpret_cast<T*>(&ring_data_[tail*sizeof(T)]));
        item=std::move(*src);
        src->~T();

        tail_.store((tail+1)&Mask,std::memory_order_release);
        return true;
    }

private:

    //生产者
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};

    //消费者
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};

    //数据存放
    alignas(CACHE_LINE_SIZE) alignas(T) std::byte ring_data_[Capacity*sizeof(T)];
};