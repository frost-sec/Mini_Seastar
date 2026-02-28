#pragma once
#include <memory>
#include <cstring>
#include <string>
#include <iostream>

class Packet {
private:
    std::shared_ptr<char[]> data_;
    
    size_t offset_;
    size_t size_;

public:
    Packet() : data_(nullptr), offset_(0), size_(0) {}

    explicit Packet(size_t size) : offset_(0), size_(size) {
        if (size > 0) {
            data_ = std::shared_ptr<char[]>(new char[size]);
        }
    }

    Packet(const char* data, size_t size) : Packet(size) {
        if (size_ > 0 && data) {
            std::memcpy(data_.get(), data, size);
        }
    }

    static Packet from_string(const std::string& str) {
        return Packet(str.data(), str.size());
    }

    // 返回一个新的 Packet，指向同一块内存，引用计数 +1
    Packet share() const {
        Packet other;
        other.data_ = data_; // shared_ptr 赋值，ref_count++
        other.offset_ = offset_;
        other.size_ = size_;
        return other;
    }

    // 返回一个新的 Packet，指向同一块内存，但只看一部分
    Packet slice(size_t start, size_t length) const {
        if (start >= size_) return Packet(); // 越界返回空
        if (start + length > size_) length = size_ - start; // 截断

        Packet other;
        other.data_ = data_; // 共享内存
        other.offset_ = offset_ + start; // 调整视图偏移
        other.size_ = length; // 调整视图大小
        return other;
    }

    Packet drop_front(size_t n) const {
        return slice(n, size_ - n);
    }

    char* data() { 
        if (!data_) return nullptr;
        return data_.get() + offset_; 
    }
    
    const char* data() const { 
        if (!data_) return nullptr;
        return data_.get() + offset_; 
    }

    size_t size() const { return size_; }

    std::string to_string() const {
        if (!data_) return "";
        return std::string(data(), size_);
    }

    // 调试用：查看引用计数
    long use_count() const {
        return data_.use_count();
    }
};