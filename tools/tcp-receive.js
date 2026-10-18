// Receive one file over a raw TCP connection (camera side: `nc <pc-ip> <port> < file`).
//   node tools/tcp-receive.js <output-file> [port=9999]
'use strict';
const net = require('net');
const fs = require('fs');

const out = process.argv[2];
const port = Number(process.argv[3] || 9999);
if (!out) { console.error('usage: tcp-receive.js <output-file> [port]'); process.exit(2); }

const server = net.createServer((sock) => {
  console.log(`connection from ${sock.remoteAddress}`);
  const ws = fs.createWriteStream(out);
  let n = 0;
  sock.on('data', (d) => { n += d.length; });
  sock.pipe(ws);
  sock.on('close', () => { ws.end(); console.log(`received ${n} bytes -> ${out}`); server.close(); });
});
server.listen(port, () => console.log(`listening on tcp/${port}`));
