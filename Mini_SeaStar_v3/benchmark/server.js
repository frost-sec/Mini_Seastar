const http = require('http');

const server = http.createServer((req, res) => {
  // 保持 header 和你的一致，保证公平
  res.writeHead(200, {
    'Content-Type': 'text/plain',
    'Content-Length': '12',
    'Connection': 'keep-alive' // Node.js 默认就是 keep-alive，显式写出来更好
  });
  res.end('Hello World!');
});

// 单进程模式，跟你做单挑
server.listen(8080, () => {
  console.log('Node.js server listening on 8080');
});