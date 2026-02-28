#include <iostream>
#include <memory>
#include <string>
#include "Seastar.h"
#include "TcpServer.h"
#include "TcpConnection.h"
#include "Packet.h"

using namespace seastar;

const std::string HTTP_RESPONSE_STR =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 12\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello World!";

void start_http_bench(std::shared_ptr<TcpConnection> conn) {
    // 接口完全不变，read()/write() 返回的 Future 兼容
    conn->read().then([conn](Packet p) {
        if (p.size() == 0) {
            return;
        }

        // thread_local 避免多线程对 shared_ptr 引用计数的原子竞争
        static thread_local Packet response_packet = Packet::from_string(HTTP_RESPONSE_STR);

        conn->write(response_packet.share()).then([conn](ssize_t n) {
            if (n < 0) return;
            start_http_bench(conn);
        });
    });
}

int main() {
    Engine engine;

    engine.run([] {
        static thread_local std::unique_ptr<TcpServer> server;

        Reactor* r = Reactor::instance();
        server = std::make_unique<TcpServer>(r);

        server->set_connection_handler([r](Socket sock) {
            // ★ 新增：关闭 Nagle 算法，小包立即发送
            // 对 ~100 字节的 HTTP 响应至关重要
            sock.set_tcp_no_delay(true);

            auto conn = TcpConnection::create(std::move(sock), r);
            start_http_bench(conn);
        });

        try {
            server->listen(8080);
            std::cout << "Core " << cpu_id()
                      << " is ready (HTTP Bench Mode)." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Core " << cpu_id()
                      << " listen failed: " << e.what() << std::endl;
        }
    });

    return 0;
}