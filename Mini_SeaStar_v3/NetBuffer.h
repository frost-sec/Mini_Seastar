#pragma once
#include "Poolable.h"

// 继承自 Poolable，享受对象池的 O(1) 极速分配
class NetBuffer : public Poolable<NetBuffer> {
public:
    static constexpr size_t kBufferSize = 16384; 
    
    char data_[kBufferSize];
    size_t read_index_ = 0;
    size_t write_index_ = 0;

    // 获取可读和可写字节数
    size_t readable_bytes() const { return write_index_ - read_index_; }
    size_t writable_bytes() const { return kBufferSize - write_index_; }

    // 获取读写指针
    char* read_ptr() { return data_ + read_index_; }
    char* write_ptr() { return data_ + write_index_; }

    // 移动指针
    void retrieve(size_t len) { read_index_ += len; }
    void append(size_t len) { write_index_ += len; }
};