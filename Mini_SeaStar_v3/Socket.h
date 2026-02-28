#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/tcp.h>

class Socket{
private:
    int fd_;
    
public:

    explicit Socket(int fd):fd_(fd){}
    Socket():fd_(-1){}
    ~Socket(){
        if(fd_>=0){
            ::close(fd_);
        }
    }

    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;

    Socket(Socket&& other) noexcept:fd_(other.fd_){
        other.fd_=-1;
    }

    Socket& operator=(Socket&& other) noexcept{
        if(this != &other){
            if(fd_>=0) ::close(fd_);
            fd_=other.fd_;
            other.fd_=-1;
        }
        return *this;
    }

    int fd() const {return fd_;}

    static Socket create_tcp(){
        int fd = ::socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(fd<0) throw std::runtime_error("socket failed");
        return Socket(fd);
    }

    void bind(int port){
        struct sockaddr_in addr;
        std::memset(&addr,0,sizeof(addr));

        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_ANY);
        addr.sin_port=htons(static_cast<uint16_t>(port));

        if(::bind(fd_,reinterpret_cast<struct sockaddr*>(&addr),sizeof(addr))<0){
            throw std::runtime_error("bind failed");
        }
    }

    void listen(){
        if(::listen(fd_,SOMAXCONN)<0){
            throw std::runtime_error("listen failed");
        }
    }

    Socket accept(){
        struct sockaddr_in addr;
        socklen_t len=sizeof(addr);
        std::memset(&addr,0,sizeof(addr));

        int connfd=::accept4(fd_,reinterpret_cast<struct sockaddr*>(&addr),&len,SOCK_NONBLOCK|SOCK_CLOEXEC);

        if(connfd<0){
            int saved_errno=errno;
            if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK){
                return Socket(-1);
            }
            if(saved_errno==EINTR){
                return Socket(-1);
            }
            if(saved_errno==EMFILE){
                thread_local static int idle_fd=::open("/dev/null",O_RDONLY|O_CLOEXEC);
                if(idle_fd>=0){
                    ::close(idle_fd);
                    int temp_fd=::accept4(fd_,nullptr,nullptr,0);
                    ::close(temp_fd);
                    idle_fd=::open("/dev/null",O_RDONLY|O_CLOEXEC);
                }

                return Socket(-1);
            }
            throw std::runtime_error("accept failed");
        }
        return Socket(connfd);
    }

    bool connect(const char* ip,int port){
        struct sockaddr_in addr;
        std::memset(&addr,0,sizeof(addr));
        addr.sin_family=AF_INET;
        addr.sin_port=htons(static_cast<uint16_t>(port));
        if(::inet_pton(AF_INET,ip,&addr.sin_addr)<=0){
            throw std::runtime_error("invalid ip");
        }

        int ret=::connect(fd_,reinterpret_cast<struct sockaddr*>(&addr),sizeof(addr));

        if(ret==0){
            return true;
        }

        int saved_errno=errno;
        if(ret<0&&saved_errno==EINPROGRESS){
            return false;
        }

        throw std::runtime_error("connect failed");
    }

    void set_tcp_no_delay(bool on){
        int opt=on?1:0;
        ::setsockopt(fd_,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt));
    }

    void set_reuse_addr(bool on){
        int opt=on?1:0;
        ::setsockopt(fd_,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    }

    void set_reuse_port(bool on){
        int opt=on?1:0;
        ::setsockopt(fd_,SOL_SOCKET,SO_REUSEPORT,&opt,sizeof(opt));
    }

    void set_keep_alive(bool on){
        int opt=on?1:0;
        ::setsockopt(fd_,SOL_SOCKET,SO_KEEPALIVE,&opt,sizeof(opt));
    }
};