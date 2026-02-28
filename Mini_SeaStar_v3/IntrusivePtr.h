#pragma once
#include <cstdint>
#include <utility>
#include <new>

// 1. 基类：负责引用计数和提供 from_this 功能
template<typename Derived>
class RefCounted {
protected:
    mutable uint32_t ref_count_ = 0;
    ~RefCounted() = default;

public:
    void add_ref() const { ++ref_count_; }

    void release() const {
        if (--ref_count_ == 0) {
            delete static_cast<const Derived*>(this);
        }
    }

    uint32_t use_count() const { return ref_count_; }

    // ★ 封装在这里！这样子类 TcpConnection 就能直接调用此方法
    // 注意：LocalPtr 的定义必须在它之前，或者使用前向声明
};

template<typename T>
class LocalPtr {
private:
    T* ptr_;

public:
    LocalPtr() : ptr_(nullptr) {}
    explicit LocalPtr(T* p) : ptr_(p) { if (ptr_) ptr_->add_ref(); }
    ~LocalPtr() { if (ptr_) ptr_->release(); }

    // 拷贝与移动逻辑保持你写好的不变...
    LocalPtr(const LocalPtr& other) : ptr_(other.ptr_) { if (ptr_) ptr_->add_ref(); }
    LocalPtr& operator=(const LocalPtr& other) {
        LocalPtr temp(other);
        std::swap(ptr_, temp.ptr_);
        return *this;
    }
    LocalPtr(LocalPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    LocalPtr& operator=(LocalPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_) ptr_->release();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
};

// ★ make_local 应该放在全局，模仿 std::make_shared
template<typename T, typename... Args>
LocalPtr<T> make_local(Args&&... args) {
    return LocalPtr<T>(new T(std::forward<Args>(args)...));
}