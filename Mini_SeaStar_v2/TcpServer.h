
#pragma once
#include "Reactor.h"
#include "Socket.h"
#include "Future.h"
#include <iostream>
#include <memory>

class TcpServer {
private:
    std::unique_ptr<Socket> listen_sock_;
    Reactor* reactor_;

    std::function<void(Socket)> new_connection_callback_;

public:
    TcpServer(Reactor* reactor) : reactor_(reactor) {}

    void set_connection_handler(std::function<void(Socket)> cb) {
        new_connection_callback_ = std::move(cb);
    }

    void listen(int port) {
        listen_sock_ = std::make_unique<Socket>(Socket::create_tcp());
        listen_sock_->set_reuse_addr(true);
        listen_sock_->set_reuse_port(true);
        listen_sock_->bind(port);
        listen_sock_->listen();

        std::cout << "Server listening on port " << port << "..." << std::endl;

        int fd = listen_sock_->fd();

        // 用新的 add 接口：handler 接收 uint32_t events 参数
        // Reactor::add 会自动附加 EPOLLET
        // handle_accept() 内部已有 while 循环，满足 ET 的 drain 要求
        reactor_->add(fd, EPOLLIN, [this](uint32_t) {
            this->handle_accept();
        });
    }

private:
    void handle_accept() {
        // 循环 accept 直到 EAGAIN（ET 模式要求 drain）
        while (true) {
            Socket client_sock = listen_sock_->accept();
            if (client_sock.fd() < 0) {
                break;  // EAGAIN，没有更多连接了
            }
            if (new_connection_callback_) {
                new_connection_callback_(std::move(client_sock));
            } else {
                std::cout << "Warning: New connection dropped (no handler)."
                          << std::endl;
            }
        }
    }
};