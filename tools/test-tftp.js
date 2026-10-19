// Regression for exact-block files, including the terminating zero-byte block.
'use strict';
const fs=require('fs'),os=require('os'),path=require('path'),dgram=require('dgram'),assert=require('assert/strict');
const {spawn}=require('child_process');
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'plug2view-tftp-test-'));
const sizes=[0,511,512,1024,1025];let server;
(async()=>{
 const probe=dgram.createSocket('udp4');await new Promise(r=>probe.bind(0,'127.0.0.1',r));const port=probe.address().port;await new Promise(r=>probe.close(r));
 for(const n of sizes)fs.writeFileSync(path.join(dir,`${n}.bin`),Buffer.alloc(n,0x5a));
 server=spawn(process.execPath,[path.join(__dirname,'tftp-server.js'),dir,String(port),'127.0.0.1'],{stdio:['ignore','pipe','pipe'],windowsHide:true});
 await new Promise((resolve,reject)=>{server.stdout.once('data',resolve);server.once('error',reject);server.once('exit',c=>reject(Error('Server exited '+c)));});
 for(const n of sizes)await new Promise((resolve,reject)=>{
  const socket=dgram.createSocket('udp4');let received=0,block=1,finished=false;
  const done=error=>{if(finished)return;finished=true;clearTimeout(timer);socket.close();error?reject(error):resolve();};
  const timer=setTimeout(()=>done(Error(`Missing terminal block for ${n} bytes`)),3000);
  socket.on('error',done);socket.on('message',(packet,from)=>{try{
   assert.equal(packet.readUInt16BE(0),3);assert.equal(packet.readUInt16BE(2),block++);
   const bytes=packet.subarray(4);assert.ok(bytes.every(b=>b===0x5a));received+=bytes.length;
   const ack=Buffer.from([0,4,packet[2],packet[3]]);
   socket.send(ack,from.port,from.address,()=>{if(bytes.length<512){try{assert.equal(received,n);done();}catch(e){done(e);}}});
  }catch(e){done(e);}});
  socket.bind(0,'127.0.0.1',()=>socket.send(Buffer.concat([Buffer.from([0,1]),Buffer.from(`${n}.bin\0octet\0`)]),port,'127.0.0.1'));
 });
 console.log('PASS: TFTP empty, short, exact-block and multi-block files terminate correctly');
})().catch(e=>{console.error(e.message);process.exitCode=1;}).finally(()=>{
 if(server)server.kill();for(const n of sizes){const f=path.join(dir,`${n}.bin`);if(fs.existsSync(f))fs.unlinkSync(f);}fs.rmdirSync(dir);
});
