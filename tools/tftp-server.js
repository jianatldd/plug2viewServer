// Tiny read-only TFTP server (RFC 1350, octet mode) used to push files to the camera.
// The camera only has a busybox `tftp -g` client, so this is the simplest transport.
//
//   node tools/tftp-server.js <directory> [port]
//   camera:  tftp -g -r <filename> <pc-ip>
'use strict';
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');

const root = path.resolve(process.argv[2] || '.');
const port = Number(process.argv[3] || 69);
const allowedClient = process.argv[4];
const BLOCK = 512;
const OP = { RRQ: 1, DATA: 3, ACK: 4, ERROR: 5 };

function startTransfer(req, rinfo) {
  if (allowedClient && rinfo.address !== allowedClient) return;
  const name = req.toString('latin1', 2, req.indexOf(0, 2));
  const file = path.join(root, path.basename(name)); // no directory traversal
  let data;
  try { data = fs.readFileSync(file); } catch (e) {
    const err = Buffer.concat([Buffer.from([0, OP.ERROR, 0, 1]), Buffer.from('not found\0')]);
    return dgram.createSocket('udp4').send(err, rinfo.port, rinfo.address, (_, s) => void 0);
  }
  console.log(`RRQ ${name} (${data.length} bytes) from ${rinfo.address}:${rinfo.port}`);

  const sock = dgram.createSocket('udp4');
  let block = 1, timer = null, done = false;
  const sendBlock = () => {
    const start = (block - 1) * BLOCK;
    const chunk = data.subarray(start, start + BLOCK);
    const pkt = Buffer.concat([Buffer.from([0, OP.DATA, block >> 8, block & 255]), chunk]);
    sock.send(pkt, rinfo.port, rinfo.address);
    clearTimeout(timer);
    timer = setTimeout(() => { if (!done) sendBlock(); }, 1500); // retransmit
  };
  sock.on('message', (msg) => {
    if (msg.readUInt16BE(0) !== OP.ACK) return;
    const acked = msg.readUInt16BE(2);
    if (acked !== (block & 0xffff)) return;
    // A full block is never terminal: exact multiples require an empty block.
    const lastWasFinal = (block - 1) * BLOCK + BLOCK > data.length;
    if (lastWasFinal) {
      done = true; clearTimeout(timer); sock.close();
      console.log(`done ${name}`);
      return;
    }
    block++; sendBlock();
  });
  sock.bind(0, () => sendBlock());
}

const server = dgram.createSocket('udp4');
server.on('message', (msg, rinfo) => {
  if (msg.readUInt16BE(0) === OP.RRQ) startTransfer(msg, rinfo);
});
server.bind(port, () => console.log(`tftp serving ${root} on udp/${port}`));
