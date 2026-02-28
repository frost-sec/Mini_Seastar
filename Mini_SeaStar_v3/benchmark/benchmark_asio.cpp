#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <thread>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() { do_read(); }

private:
    void do_read() {
        auto self(shared_from_this());
        socket_.async_read_some(boost::asio::buffer(data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) do_write(length);
            });
    }

    void do_write(std::size_t length) {
        auto self(shared_from_this());
        // 静态响应，避免每次分配内存
        static const std::string response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 12\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "Hello World!";
            
        boost::asio::async_write(socket_, boost::asio::buffer(response),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) do_read(); 
            });
    }

    tcp::socket socket_;
    enum { max_length = 1024 };
    char data_[max_length];
};

class Server {
public:
    Server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context) {
        
        tcp::endpoint endpoint(tcp::v4(), port);
        acceptor_.open(endpoint.protocol());
        
        // 允许地址复用
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        
        // 核心魔法：开启 Linux SO_REUSEPORT，允许多个线程绑定同一端口，内核负责负载均衡
        int one = 1;
        setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

        acceptor_.bind(endpoint);
        acceptor_.listen(boost::asio::socket_base::max_listen_connections); // 拉满 Backlog

        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    // 关闭 Nagle 算法，降低延迟
                    socket.set_option(tcp::no_delay(true));
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept();
            });
    }
    tcp::acceptor acceptor_;
};

int main() {
    try {
        const int num_threads = 16; // 设定为 16 线程
        std::vector<std::shared_ptr<boost::asio::io_context>> io_contexts;
        std::vector<std::shared_ptr<Server>> servers;
        std::vector<std::thread> threads;

        std::cout << "Starting Asio server with " << num_threads << " threads using SO_REUSEPORT..." << std::endl;

        // 1. 为每个核心创建独立的 io_context 和 Server
        for (int i = 0; i < num_threads; ++i) {
            auto ioc = std::make_shared<boost::asio::io_context>(1); // 提示 Asio 只有 1 个线程访问此 ioc
            io_contexts.push_back(ioc);
            servers.push_back(std::make_shared<Server>(*ioc, 8080));
        }

        // 2. 启动 16 个 Worker 线程，各自跑自己的 Event Loop
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([ioc = io_contexts[i]]() {
                ioc->run();
            });
        }

        // 3. 等待所有线程结束（通常服务端会一直阻塞在这里）
        for (auto& t : threads) {
            t.join();
        }

    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}