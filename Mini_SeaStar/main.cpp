#include <iostream>
#include <memory>
#include <string>
#include "Seastar.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include "Packet.h"

using namespace seastar;

// 1. 定义标准的 HTTP 响应
// Content-Length: 12 对应 "Hello World!"
// Connection: keep-alive 让 wrk 保持连接，复用 TCP 通道
const std::string HTTP_RESPONSE_STR = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 12\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello World!";

// 2. 处理连接的回调函数
void start_http_bench(std::shared_ptr<TcpConnection> conn) {
    // 递归读取：这是 Keep-Alive 的关键
    conn->read().then([conn](Packet p) {
        // 如果读到 0 字节，说明对端关闭了连接
        if (p.size() == 0) {
            return; 
        }

        // 3. 性能优化：静态全局 Packet
        // 我们不需要每次都 new char[]，直接复用这一块内存
        // Packet::share() 只增加引用计数，不拷贝内存 (Zero-Copy)
        static Packet response_packet = Packet::from_string(HTTP_RESPONSE_STR);

        // 发送响应
        conn->write(response_packet.share()).then([conn](ssize_t n) {
            if (n < 0) return; // 写入出错
            
            // 4. 发送成功后，继续读取下一个请求
            start_http_bench(conn);
        });
    });
}

int main() {
    // 初始化引擎（自动根据核数创建线程）
    Engine engine;

    engine.run([] {
        // static thread_local 保证 server 对象在 lambda 执行完后不被销毁
        // 每个线程都有一个独立的 TcpServer 实例
        static thread_local std::unique_ptr<TcpServer> server;
        
        Reactor* r = Reactor::instance();
        
        server = std::make_unique<TcpServer>(r);
        
        // 设置连接建立后的回调
        server->set_connection_handler([r](Socket sock) {
            auto conn = TcpConnection::create(std::move(sock), r);
            start_http_bench(conn);
        });

        // 监听端口
        // 由于 Socket.h 中设置了 SO_REUSEPORT，所有线程都可以 bind 到同一个端口
        // 内核会自动进行负载均衡
        try {
            server->listen(8080);
            std::cout << "Core " << cpu_id() << " is ready (HTTP Bench Mode)." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Core " << cpu_id() << " listen failed: " << e.what() << std::endl;
        }
    });

    return 0;
}