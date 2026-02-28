#pragma once
#include <memory>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <string>
#include <algorithm>
#include <sys/uio.h>
#include "Socket.h"
#include "Reactor.h"
#include "Future.h"
#include "Packet.h"

class TcpConnection:public std::enable_shared_from_this<TcpConnection>{
private:
    Socket socket_;
    Reactor* reactor_;

    struct PrivateKey{};

    std::vector<char> input_buffer_;
    size_t read_index_=0;

public:
    TcpConnection(PrivateKey,Socket&& socket,Reactor* reactor):socket_(std::move(socket)),reactor_(reactor) {
        input_buffer_.reserve(4096);
    }

    static std::shared_ptr<TcpConnection> create(Socket&& socket,Reactor* reactor){
        return std::make_shared<TcpConnection>(PrivateKey{},std::move(socket),reactor);
    }

    TcpConnection(const TcpConnection&)=delete;
    TcpConnection operator=(const TcpConnection&)=delete;
    TcpConnection(TcpConnection&&)=delete;
    TcpConnection& operator=(TcpConnection&&)=delete;

    ~TcpConnection(){
        std::cout<<"[Mini-Seastar] Connection closed. FD: "<<socket_.fd()<<std::endl;
    }

    Future<Packet> read(){
        auto promise=std::make_shared<Promise<Packet>>();

        if(readable_bytes()>0){
            Packet pkt(peek(),readable_bytes());
            retrieve_all();
            promise->set_value(std::move(pkt));
            return promise->get_future();
        }
        
        do_read_from_socket([self=shared_from_this(),promise](bool success){
            if(success && self->readable_bytes()>0){
                Packet pkt(self->peek(),self->readable_bytes());
                self->retrieve_all();
                promise->set_value(std::move(pkt));
            }else{
                promise->set_value(Packet());
            }
        });
        
        return promise->get_future();
    }

    Future<std::string> read_until(std::string delimiter){
        auto promise=std::make_shared<Promise<std::string>>();
        check_buffer_for_line(promise,delimiter);
        return promise->get_future();
    }

    Future<ssize_t> write(std::vector<Packet> packets){
        auto promise=std::make_shared<Promise<ssize_t>>();
        int fd=socket_.fd();

        std::vector<struct iovec> iovs;
        iovs.reserve(packets.size());
        size_t total_size=0;

        for(auto& p:packets){
            if(p.size()>0){
                struct iovec iov;
                iov.iov_base=(void*)p.data();
                iov.iov_len=p.size();
                iovs.push_back(iov);
                total_size+=p.size();
            }
        }

        if(iovs.empty()){
            promise->set_value(0);
            return promise->get_future();
        }

        ssize_t n=::writev(fd,iovs.data(),iovs.size());

        if(n<0){
            if(errno!=EAGAIN && errno!=EWOULDBLOCK){
                promise->set_value(-1);
                return promise->get_future();
            }
            n=0;
        }

        if(static_cast<size_t>(n)==total_size){
            promise->set_value(n);
            return promise->get_future();
        }

        reactor_->add(fd,EPOLLOUT,[self=shared_from_this(),promise,packets=std::move(packets),total_written=n,total_size,fd]() mutable{
            std::vector<struct iovec> current_iovs;
            size_t bytes_to_skip=total_written;

            for(auto& p:packets){
                if(bytes_to_skip>=p.size()){
                    bytes_to_skip-=p.size();
                }else{
                    struct iovec iov;
                    iov.iov_base=(void*)(p.data()+bytes_to_skip);
                    iov.iov_len=p.size()-bytes_to_skip;
                    current_iovs.push_back(iov);

                    bytes_to_skip=0;
                }
            }

            ssize_t written=::writev(fd,current_iovs.data(),current_iovs.size());

            if(written<0){
                if(errno==EAGAIN || errno==EWOULDBLOCK) return;
                self->reactor_->remove(fd);
                promise->set_value(-1);
                return;
            }

            total_written+=written;

            if(total_written==total_size){
                self->reactor_->remove(fd);
                promise->set_value(total_written);
            }
        });

        return promise->get_future();
    }

    Future<ssize_t> write(Packet p){
        return write(std::vector<Packet>{std::move(p)});
    }

    int fd() const {return socket_.fd();}

private:
    char* peek() {return input_buffer_.data()+read_index_;}
    const char* peek() const {return input_buffer_.data()+read_index_;}

    size_t readable_bytes() const {return input_buffer_.size()-read_index_;}

    void retrieve(size_t len){
        if(len>=readable_bytes()){
            retrieve_all();
            return;
        }
        read_index_+=len;
        if(read_index_>=1024 && read_index_>readable_bytes()){
            make_room();
        }
    }

    void retrieve_all(){
        input_buffer_.clear();
        read_index_=0;
    }

    void make_room(){
        if(readable_bytes()>0){
            std::copy(input_buffer_.begin()+read_index_,input_buffer_.end(),input_buffer_.begin());
        }
        input_buffer_.resize(readable_bytes());
        read_index_=0;
    }

    template<typename Callback>
    void do_read_from_socket(Callback cb){
        int fd=socket_.fd();
        reactor_->add(fd,EPOLLIN,[self=shared_from_this(),fd,cb](){
            if(self->input_buffer_.capacity()-self->input_buffer_.size()<4096){
                self->input_buffer_.reserve(self->input_buffer_.capacity()+4096);
            }

            size_t old_size=self->input_buffer_.size();

            self->input_buffer_.resize(old_size+4096);

            ssize_t n=::read(fd,&self->input_buffer_[old_size],4096);

            if(n>0){
                self->input_buffer_.resize(old_size+n);
                self->reactor_->remove(fd);
                cb(true);
            }else if(n==0){
                self->input_buffer_.resize(old_size);
                self->reactor_->remove(fd);
                cb(false);
            }else{
                self->input_buffer_.resize(old_size);
                if(errno==EAGAIN || errno==EWOULDBLOCK) return;
                self->reactor_->remove(fd);
                cb(false);
            }
        });
    }

    void check_buffer_for_line(std::shared_ptr<Promise<std::string>> promise,std::string delimiter){
        auto start_iter=input_buffer_.begin()+read_index_;
        auto it=std::search(start_iter,input_buffer_.end(),
        delimiter.begin(),delimiter.end());

        if(it!=input_buffer_.end()){
            size_t len=std::distance(start_iter,it)+delimiter.length();
            std::string line(start_iter,start_iter+len);

            retrieve(len);
            promise->set_value(std::move(line));
            return;
        }

        do_read_from_socket([self=shared_from_this(),promise,delimiter](bool success){
            if(success){
                self->check_buffer_for_line(promise,delimiter);
            }else{
                promise->set_value("");
            }
        });
    }
};