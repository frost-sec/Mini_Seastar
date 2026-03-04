#pragma once
#include <memory>
#include <vector>
#include <deque>
#include <iostream>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include "Socket.h"
#include "Reactor.h"
#include "Future.h"
#include "Packet.h"
#include "Poolable.h"
#include "IntrusivePtr.h"
#include "NetBuffer.h"

class TcpConnection : public Poolable<TcpConnection>, public RefCounted<TcpConnection>
{                    
private:
    Socket socket_;
    Reactor* reactor_;

    // ── 读状态 ──
    std::deque<NetBuffer*> input_buffers_;
    LocalPtr<Promise<Packet>> pending_read_;

    // ── 写状态 ──
    std::deque<NetBuffer*> output_buffers_;
    LocalPtr<Promise<ssize_t>> pending_write_;
    ssize_t total_write_size_ = 0;

    // ── 连接状态 ──
    bool closed_ = false;
    uint32_t current_events_ = 0;  // 当前 epoll 注册的事件掩码

    struct PrivateKey {};

public:

    TcpConnection(PrivateKey, Socket&& socket, Reactor* reactor)
        : socket_(std::move(socket)), reactor_(reactor)
    {
    }

    // 工厂方法：创建后立即注册到 epoll（一生一次的 ADD）
    static LocalPtr<TcpConnection> create(Socket&& socket, Reactor* reactor) {
        auto conn = make_local<TcpConnection>(
            PrivateKey{}, std::move(socket), reactor);
        conn->register_to_reactor();
        return conn;
    }

    // 禁用拷贝和移动
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) = delete;
    TcpConnection& operator=(TcpConnection&&) = delete;

    ~TcpConnection() {
        if (!closed_) {
            reactor_->remove(socket_.fd());
        }
        // 清理由于断开连接残留在队列中的 NetBuffer，防止内存池泄漏
        for (auto buf : input_buffers_) delete buf;
        for (auto buf : output_buffers_) delete buf;
    }

    Future<Packet> read() {
        auto promise = LocalPtr<Promise<Packet>>(new Promise<Packet>());

        if (closed_) {
            promise->set_value(Packet());
            return promise->get_future();
        }

        // 步骤 1: 检查缓冲区
        if (readable_bytes() > 0) {
            Packet pkt = extract_packet(readable_bytes());
            promise->set_value(std::move(pkt));
            return promise->get_future();
        }

        // 步骤 2: 直接挂起，等待 handle_readable() 来 fulfill
        pending_read_ = promise;
        return promise->get_future();
    }

    Future<ssize_t> write(Packet p) {
        auto promise = LocalPtr<Promise<ssize_t>>(new Promise<ssize_t>());

        if (closed_) {
            promise->set_value(-1);
            return promise->get_future();
        }

        if (p.size() == 0) {
            promise->set_value(0);
            return promise->get_future();
        }

        int fd = socket_.fd();
        const char* data = p.data();
        size_t remaining = p.size();
        ssize_t total = static_cast<ssize_t>(p.size());

        // ★ 步骤 1: 尝试立即写入（热路径）
        while (remaining > 0) {
            ssize_t n = ::write(fd, data, remaining);
            if (n > 0) {
                data += n;
                remaining -= n;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 发送缓冲区满了，走步骤 3
                }
                promise->set_value(-1);
                return promise->get_future();
            }
        }

        // ★ 步骤 2: 全部写完 → 直接返回
        if (remaining == 0) {
            promise->set_value(total);
            return promise->get_future();
        }

        // ★ 步骤 3: 部分写入 → 分配 NetBuffer 缓存剩余数据
        while (remaining > 0) {
            NetBuffer* buf = new NetBuffer(); // 触发 O(1) 分配
            size_t to_copy = std::min(remaining, buf->writable_bytes());
            std::memcpy(buf->write_ptr(), data, to_copy);
            buf->append(to_copy);
            output_buffers_.push_back(buf);
            
            data += to_copy;
            remaining -= to_copy;
        }

        total_write_size_ = total;
        pending_write_ = promise;
        enable_write();

        return promise->get_future();
    }

    int fd() const { return socket_.fd(); }

private:

    LocalPtr<TcpConnection> local_from_this() {
        return LocalPtr<TcpConnection>(this);
    }

    void register_to_reactor() {
        current_events_ = EPOLLIN;
        reactor_->add(socket_.fd(), current_events_,
            [self = local_from_this()](uint32_t events) {
                self->handle_events(events);
            });
    }

    void handle_events(uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP)) {
            handle_close();
            return;
        }

        if (events & EPOLLIN)  handle_readable();
        if (events & EPOLLOUT) handle_writable();
    }

    void handle_readable() {
        drain_socket();

        if (pending_read_ && readable_bytes() > 0) {
            auto p = std::move(pending_read_);
            Packet pkt = extract_packet(readable_bytes());
            p->set_value(std::move(pkt));
        }
    }

    void handle_writable() {
        flush_output();
    }

    //  drain_socket() — 直接读入裸内存，告别 vector memset 开销
    void drain_socket() {
        int fd = socket_.fd();

        while (true) {
            // 如果队尾 buffer 写满，或者队列为空，则申请一个新 buffer
            if (input_buffers_.empty() || input_buffers_.back()->writable_bytes() == 0) {
                input_buffers_.push_back(new NetBuffer());
            }

            NetBuffer* buf = input_buffers_.back();
            size_t requested = buf->writable_bytes();
            
            // 直接传递指向未初始化内存的指针，免去零初始化
            ssize_t n = ::read(fd, buf->write_ptr(), requested);

            if (n > 0) {
                buf->append(n);

                // 内核已空，不必再读
                if (static_cast<size_t>(n) < requested) {
                    return;
                }
                continue;
            }

            if (n == 0) {
                handle_close();
                return;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            handle_close();
            return;
        }
    }

    //  flush_output() — 消费输出队列
    void flush_output() {
        int fd = socket_.fd();

        while (!output_buffers_.empty()) {
            NetBuffer* buf = output_buffers_.front();
            ssize_t n = ::write(fd, buf->read_ptr(), buf->readable_bytes());

            if (n > 0) {
                buf->retrieve(n);
                // 当前 buffer 写空了，归还给对象池
                if (buf->readable_bytes() == 0) {
                    delete buf; 
                    output_buffers_.pop_front();
                }
                continue;
            }

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; // 等待下次 EPOLLOUT
                }
                
                disable_write();
                if (pending_write_) {
                    auto p = std::move(pending_write_);
                    p->set_value(-1);
                }
                return;
            }
        }

        disable_write();

        if (pending_write_) {
            auto p = std::move(pending_write_);
            p->set_value(total_write_size_);
        }
    }

    void enable_write() {
        if (!(current_events_ & EPOLLOUT)) {
            current_events_ |= EPOLLOUT;
            reactor_->modify_events(socket_.fd(), current_events_);
        }
    }

    void disable_write() {
        if (current_events_ & EPOLLOUT) {
            current_events_ &= ~EPOLLOUT;
            reactor_->modify_events(socket_.fd(), current_events_);
        }
    }

    void handle_close() {
        if (closed_) return;
        closed_ = true;

        reactor_->remove(socket_.fd());

        if (pending_read_) {
            auto p = std::move(pending_read_);
            p->set_value(Packet());
        }
        if (pending_write_) {
            auto p = std::move(pending_write_);
            p->set_value(-1);
        }
    }

    // ── Buffer 辅助提取逻辑 ──

    // 获取当前所有 buffer 加起来的可读总长度
    size_t readable_bytes() const {
        size_t total = 0;
        for (auto buf : input_buffers_) {
            total += buf->readable_bytes();
        }
        return total;
    }

    // 组装并提取跨越多个 NetBuffer 的离散数据为一个连续的 Packet
    Packet extract_packet(size_t len) {
        if (len == 0) return Packet();
        
        Packet pkt(len); // 底层只会 new char[size]，不会 memset
        char* dest = pkt.data();
        size_t remaining = len;

        while (remaining > 0 && !input_buffers_.empty()) {
            NetBuffer* buf = input_buffers_.front();
            size_t to_copy = std::min(remaining, buf->readable_bytes());
            
            std::memcpy(dest, buf->read_ptr(), to_copy);
            buf->retrieve(to_copy);
            
            dest += to_copy;
            remaining -= to_copy;

            // 块数据被读完，安全回收
            if (buf->readable_bytes() == 0) {
                delete buf;
                input_buffers_.pop_front();
            }
        }
        return pkt;
    }
};