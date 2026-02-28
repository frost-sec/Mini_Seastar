#pragma once
#include "Reactor.h"
#include "Socket.h"
#include "Future.h"
#include <iostream>
#include <memory>

class TcpServer
{
private:
    std::unique_ptr<Socket> listen_sock_;
    Reactor* reactor_;

    std::function<void(Socket)> new_connection_callback_;
public:
    TcpServer(Reactor* reactor):reactor_(reactor){}

    void set_connection_handler(std::function<void(Socket)> cb){
        new_connection_callback_=std::move(cb);
    }

    void listen(int port){
        listen_sock_=std::make_unique<Socket>(Socket::create_tcp());
        listen_sock_->set_reuse_addr(true);
        listen_sock_->set_reuse_port(true);
        listen_sock_->bind(port);
        listen_sock_->listen();

        std::cout<<"Server listening on port "<<port<<"..."<<std::endl;

        int fd=listen_sock_->fd();
        reactor_->add(fd,[this](){
            this->handle_accept();
        });
    }

private:
    void handle_accept(){
        while(true){
            Socket client_sock=listen_sock_->accept();
            if(client_sock.fd()<0){
                break;
            }
            if(new_connection_callback_){
                new_connection_callback_(std::move(client_sock));
            }else{
                std::cout<<"Warning: New connection dropped (no handle)."<<std::endl;
            }
        }
    }
};