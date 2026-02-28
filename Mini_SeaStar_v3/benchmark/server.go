package main

import (
	"net"
)

// 预定义响应，和 C++ 保持一致
var response = []byte("HTTP/1.1 200 OK\r\n" +
	"Content-Type: text/plain\r\n" +
	"Content-Length: 12\r\n" +
	"Connection: keep-alive\r\n" +
	"\r\n" +
	"Hello World!")

func handleConnection(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 1024)
	for {
		// 1. 盲读 (对应 async_read_some)
		n, err := conn.Read(buf)
		if err != nil || n == 0 {
			return
		}

		// 2. 盲写 (对应 async_write)
		_, err = conn.Write(response)
		if err != nil {
			return
		}
	}
}

func main() {
	// 监听 TCP
	l, _ := net.Listen("tcp", ":8080")
	defer l.Close()

	for {
		conn, _ := l.Accept()
		// Go 是由 Runtime 调度的，这里开启一个 Goroutine
		go handleConnection(conn)
	}
}