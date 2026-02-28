#pragma once
#include <memory>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include "Socket.h"
#include "Reactor.h"
#include "Future.h"
#include "Packet.h"
#include "Poolable.h"

class TcpConnection : public std::enable_shared_from_this<TcpConnection>,
                    public Poolable<TcpConnection>
{                    
private:
    Socket socket_;
    Reactor* reactor_;

    // ── 读状态 ──
    std::vector<char> input_buffer_;
    size_t read_index_ = 0;
    std::shared_ptr<Promise<Packet>> pending_read_;

    // ── 写状态 ──
    std::vector<char> output_buffer_;
    size_t write_index_ = 0;
    std::shared_ptr<Promise<ssize_t>> pending_write_;
    ssize_t total_write_size_ = 0;

    // ── 连接状态 ──
    bool closed_ = false;
    uint32_t current_events_ = 0;  // 当前 epoll 注册的事件掩码

    struct PrivateKey {};

public:

    TcpConnection(PrivateKey, Socket&& socket, Reactor* reactor)
        : socket_(std::move(socket)), reactor_(reactor)
    {
        input_buffer_.reserve(8192);
        output_buffer_.reserve(4096);
    }

    // 工厂方法：创建后立即注册到 epoll（一生一次的 ADD）
    static std::shared_ptr<TcpConnection> create(Socket&& socket, Reactor* reactor) {
        auto conn = std::make_shared<TcpConnection>(
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
        // 如果还没关闭，在析构时从 epoll 移除
        if (!closed_) {
            reactor_->remove(socket_.fd());
        }
        std::cout << "[Mini-Seastar] Connection closed. FD: "
                  << socket_.fd() << std::endl;
    }


    Future<Packet> read() {
        auto promise = std::make_shared<Promise<Packet>>();

        if (closed_) {
            promise->set_value(Packet());
            return promise->get_future();
        }

        // 步骤 1: 检查缓冲区（上次 drain 可能已经读到了数据）
        if (readable_bytes() > 0) {
            Packet pkt(peek(), readable_bytes());
            retrieve_all();
            promise->set_value(std::move(pkt));
            return promise->get_future();
        }

        // 步骤 2: 直接挂起，等待 handle_readable() 来 fulfill
        //
        // 不再调 drain_socket()！原因：
        //   - 在这个 HTTP bench 场景中，read() 总是在 write 完成后调用
        //   - write 完成后 wrk 才会发下一个请求，此时内核缓冲区必然为空
        //   - drain_socket() 几乎 100% 返回 EAGAIN，白白浪费一次 syscall
        //
        // 安全性：
        //   - handle_readable() drain 后如果发现 pending_read_，立即 fulfill
        //   - 如果 EPOLLIN 在 drain 返回 EAGAIN 之后、read() 之前触发，
        //     EPOLLIN edge 会在下次 epoll_wait 被捕获 → handle_readable 处理
        //   - 如果 handle_readable 在 pending_read_ 设置之前运行，
        //     数据留在 input_buffer_，read() 在步骤 1 就会取走
        pending_read_ = promise;
        return promise->get_future();
    }

    Future<ssize_t> write(Packet p) {
        auto promise = std::make_shared<Promise<ssize_t>>();

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
        // 对于小响应（~100字节），内核发送缓冲区（~200KB）几乎不可能满
        // 所以大多数情况下这里一次就写完，整个 write 零次 epoll_ctl
        while (remaining > 0) {
            ssize_t n = ::write(fd, data, remaining);
            if (n > 0) {
                data += n;
                remaining -= n;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // 发送缓冲区满了，走步骤 3
                }
                // 真正的写错误
                promise->set_value(-1);
                return promise->get_future();
            }
        }

        // ★ 步骤 2: 全部写完 → 直接返回
        // 这是 99% 的情况，没有任何 epoll_ctl 调用
        if (remaining == 0) {
            promise->set_value(total);
            return promise->get_future();
        }

        // ★ 步骤 3: 部分写入 → 缓存剩余数据，开启 EPOLLOUT
        // 仅在发送缓冲区满时才走到这里（高负载场景）
        output_buffer_.assign(data, data + remaining);
        write_index_ = 0;
        total_write_size_ = total;
        pending_write_ = promise;
        enable_write();  // epoll_ctl(MOD) 开启 EPOLLOUT

        return promise->get_future();
    }

    int fd() const { return socket_.fd(); }

private:
    //  epoll 注册（一生一次）
    void register_to_reactor() {
        current_events_ = EPOLLIN;  // 初始只关心可读
        // Reactor::add 会自动附加 EPOLLET
        reactor_->add(socket_.fd(), current_events_,
            [self = shared_from_this()](uint32_t events) {
                self->handle_events(events);
            });
    }

    //  统一事件分发（固定 handler，一生不换）

    void handle_events(uint32_t events) {
        // 错误和挂起优先处理
        // ET 模式下 EPOLLERR 和 EPOLLHUP 总会被报告，不管有没有注册
        if (events & (EPOLLERR | EPOLLHUP)) {
            handle_close();
            return;
        }

        if (events & EPOLLIN)  handle_readable();
        if (events & EPOLLOUT) handle_writable();
    }

    //  EPOLLIN 处理

    void handle_readable() {
        // ET 模式：必须 drain 到 EAGAIN
        drain_socket();

        // 如果有人在等数据（pending_read_ 非空），fulfill 它
        if (pending_read_ && readable_bytes() > 0) {
            // 必须先 move 走再 set_value
            // 因为 set_value → .then() 回调 → 可能再次调 read()
            // → 再次设置 pending_read_
            // 如果不 move，新设的值会被后续代码覆盖
            auto p = std::move(pending_read_);
            Packet pkt(peek(), readable_bytes());
            retrieve_all();
            p->set_value(std::move(pkt));
        }

        // 如果 pending_read_ == nullptr:
        // 数据留在 input_buffer_ 里，下次 read() 调用时直接取走
        // 这不会造成问题，因为 read() 一开始就会检查 buffer
    }

    //  EPOLLOUT 处理

    void handle_writable() {
        flush_output();
    }

    static constexpr size_t kReadBufSize = 16384;

    //  drain_socket() — ET 模式核心：循环读到 EAGAIN
    //
    //  为什么必须循环？
    //  ET 只在状态变化时通知一次：
    //    内核缓冲区 空→有数据 : 触发 EPOLLIN（一次）
    //    只读了一部分，有数据→还有数据 : 不会再触发
    //    读到 EAGAIN（缓冲区变空）后，下次来数据才会再触发
    //
    //  如果不循环读完，剩余数据会卡在内核直到下次恰好有新数据到达

    void drain_socket() {
        int fd = socket_.fd();

        while (true) {
            // 确保 buffer 有足够空间
            size_t cap = input_buffer_.capacity();
            size_t sz  = input_buffer_.size();
            if (cap - sz < kReadBufSize) {
                input_buffer_.reserve(cap + kReadBufSize * 2);
            }

            size_t old_size = input_buffer_.size();
            input_buffer_.resize(old_size + kReadBufSize);

            ssize_t n = ::read(fd, &input_buffer_[old_size], kReadBufSize);

            if (n > 0) {
                input_buffer_.resize(old_size + n);

                // ★ 关键优化：读到的比请求的少 → 内核已空，不必再读
                if (static_cast<size_t>(n) < kReadBufSize) {
                    return;  // 省掉一次返回 EAGAIN 的 read syscall
                }
                continue;  // 读满了，可能还有更多数据
            }

            // 读取失败或结束，恢复 buffer size
            input_buffer_.resize(old_size);

            if (n == 0) {
                handle_close();
                return;
            }

            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            handle_close();
            return;
        }
    }


    //  flush_output() — ET 模式核心：循环写到 EAGAIN 或写完

    void flush_output() {
        int fd = socket_.fd();

        while (write_index_ < output_buffer_.size()) {
            size_t remaining = output_buffer_.size() - write_index_;
            ssize_t n = ::write(fd,
                                output_buffer_.data() + write_index_,
                                remaining);

            if (n > 0) {
                write_index_ += n;
                continue;  // 继续写
            }

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 发送缓冲区又满了，等下次 EPOLLOUT
                    return;
                }
                // 写错误
                disable_write();
                if (pending_write_) {
                    auto p = std::move(pending_write_);
                    p->set_value(-1);
                }
                return;
            }
        }

        // 全部写完
        output_buffer_.clear();
        write_index_ = 0;

        // 关掉 EPOLLOUT，否则会一直触发白耗 CPU
        disable_write();

        if (pending_write_) {
            auto p = std::move(pending_write_);
            p->set_value(total_write_size_);
        }
    }

    //  事件掩码管理（enable/disable EPOLLOUT）
    //
    //  为什么需要幂等检查？
    //  避免重复调用 epoll_ctl(MOD)，如果状态已经是期望的就什么都不做

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

    //  连接关闭（一生一次的 DEL）

    void handle_close() {
        if (closed_) return;  // 防止重复关闭
        closed_ = true;

        reactor_->remove(socket_.fd());

        // 清理所有 pending promises，让上层的 .then() 链正常终止
        if (pending_read_) {
            auto p = std::move(pending_read_);
            p->set_value(Packet());  // 空包 → 上层判断为连接关闭
        }
        if (pending_write_) {
            auto p = std::move(pending_write_);
            p->set_value(-1);  // -1 → 上层判断为写入失败
        }
    }

    //  Buffer 辅助方法

    char* peek() {
        return input_buffer_.data() + read_index_;
    }

    const char* peek() const {
        return input_buffer_.data() + read_index_;
    }

    size_t readable_bytes() const {
        return input_buffer_.size() - read_index_;
    }

    void retrieve_all() {
        input_buffer_.clear();
        read_index_ = 0;
    }
};