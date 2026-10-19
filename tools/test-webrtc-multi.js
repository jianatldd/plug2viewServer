// Live browser load test for an installed multi-session camd. Requires Chrome
// debugging on port 9223 and .build/browser/node_modules/playwright-core.
// node tools/test-webrtc-multi.js [--viewers 10] [--seconds 30] [--report results.json]
// Requires the requested number of free slots; cleans up only its own sessions.
'use strict';
const fs=require('fs'),path=require('path'),http=require('http'),assert=require('assert/strict');
const config=require('./config'),args=process.argv.slice(2);
const option=(name,fallback)=>args.includes(name)?Number(args[args.indexOf(name)+1]):fallback;
const count=option('--viewers',10),seconds=option('--seconds',30);
const reportPath=args.includes('--report')?args[args.indexOf('--report')+1]:null;
assert.ok(Number.isInteger(count)&&count>=2&&count<=10,'--viewers must be 2..10');
assert.ok(Number.isFinite(seconds)&&seconds>=10&&seconds<=3600,'--seconds must be 10..3600');
const camera=config.cameraAddress(args),port=config.servicePort('PORT');
const credentials=config.readLocal().server.CREDENTIALS;
assert.ok(credentials,'Viewer credentials required in config.local.json');
const {chromium}=require(path.resolve(__dirname,'../.build/browser/node_modules/playwright-core'));
function request(body) {
 const data=body===undefined?null:JSON.stringify(body);
 return new Promise((resolve,reject)=>{
  const req=http.request({host:camera,port,path:data?'/viewer/webrtc':'/?action=status',method:data?'POST':'GET',headers:{
   Authorization:'Basic '+Buffer.from(credentials).toString('base64'),'X-Plug2View-Viewer':'1',
   'Content-Type':'application/json',...(data?{'Content-Length':Buffer.byteLength(data)}:{})
  }},res=>{
   let text='';res.on('data',b=>text+=b);res.on('aborted',()=>reject(Error('Truncated response')));
   res.on('end',()=>{try{resolve({status:res.statusCode,body:JSON.parse(text)});}catch(e){reject(Error('Invalid signaling response'));}});
  });req.setTimeout(15000,()=>req.destroy(Error('Signaling timeout')));req.on('error',reject);req.end(data);
 });
}
(async()=>{
 let browser,context,page;const sessions=new Map(),report={viewers:count,seconds,started:new Date().toISOString(),samples:[]};
 const end=async i=>{
  if(!sessions.has(i))return;
  const session=sessions.get(i);
  const response=await request({action:'end',session});assert.equal(response.status,200);
  await page.evaluate(i=>window.peers[i]?.close(),i);
  sessions.delete(i);return session;
 };
 try {
  browser=await chromium.connectOverCDP('http://127.0.0.1:9223');context=await browser.newContext();page=await context.newPage();
  await page.setContent('<main></main>');await page.evaluate(()=>{window.peers=[];});
  const connect=async i=>{
   const sdp=await page.evaluate(async i=>{
    const pc=window.peers[i]=new RTCPeerConnection({iceServers:[]});
    const video=document.createElement('video');video.autoplay=true;video.muted=true;video.playsInline=true;
    video.style.width='160px';document.querySelector('main').appendChild(video);
    pc.ontrack=e=>{if(e.track.kind==='video'){video.srcObject=new MediaStream([e.track]);video.play().catch(()=>{});}};
    pc.addTransceiver('audio',{direction:'recvonly'});pc.addTransceiver('video',{direction:'recvonly'});
    await pc.setLocalDescription(await pc.createOffer());return pc.localDescription.sdp;
   },i);
   const response=await request({action:'offer',sdp});
   assert.equal(response.status,200,`Viewer ${i+1} needs a free slot`);sessions.set(i,response.body.session);
   await page.evaluate(({i,sdp})=>window.peers[i].setRemoteDescription({type:'answer',sdp}),{i,sdp:response.body.sdp});
  };
  const stats=()=>page.evaluate(async()=>Promise.all(window.peers.map(async(pc,i)=>{
   if(pc.connectionState==='closed')return {viewer:i+1,state:'closed',frames:0,lost:0,fps:0};
   const inbound=[...(await pc.getStats()).values()].find(s=>s.type==='inbound-rtp'&&s.kind==='video');
   return {viewer:i+1,state:pc.connectionState,frames:inbound?.framesDecoded||0,lost:inbound?.packetsLost||0,fps:inbound?.framesPerSecond||0};
  })));
  for(let i=0;i<count;i++)await connect(i);
  const deadline=Date.now()+30000;
  while(true){const s=await stats();if(s.every(x=>x.state==='connected'&&x.frames>10))break;if(Date.now()>=deadline){console.log(JSON.stringify(s));throw Error('All viewers must connect and decode');}await page.waitForTimeout(500);}
  const first=await stats();
  report.first=first; const began=Date.now();
  for(let elapsed=0;elapsed<seconds;elapsed+=5){await page.waitForTimeout(Math.min(5,seconds-elapsed)*1000);const sample={elapsed:(Date.now()-began)/1000,peers:await stats(),camera:(await request()).body};report.samples.push(sample);console.log(JSON.stringify(sample));}
  const before=await stats();
  report.last=before; report.elapsed=(Date.now()-began)/1000;
  report.averageFps=before.map((s,i)=>(s.frames-first[i].frames)/report.elapsed);
  if(count===10){const full=await request({action:'offer',sdp:'invalid'});assert.equal(full.status,409,'The eleventh viewer must not evict a session');}
  const retired=await end(0);await page.waitForTimeout(3000);
  const after=await stats();for(let i=1;i<count;i++)assert.ok(after[i].frames>before[i].frames,'Remaining viewers must continue after one leaves');
  await connect(0);
  assert.equal((await request({action:'end',session:retired})).status,409,'Retired token must not end a reused slot');
  await page.waitForFunction(async()=>{
   const stats=await window.peers[0].getStats();return [...stats.values()].some(s=>s.type==='inbound-rtp'&&s.kind==='video'&&s.framesDecoded>10);
  },null,{timeout:30000});
  report.lifecyclePassed=true;
  for(let i=0;i<count;i++)assert.ok(before[i].frames-first[i].frames>seconds*5,`Viewer ${i+1} must sustain at least 5 decoded fps`);
  console.log(`PASS: ${count} simultaneous viewers, sustained decoding, independent cleanup and slot reuse`);
  report.passed=true;
 } finally {
  for(const [i,session] of sessions){await page?.evaluate(i=>window.peers[i]?.close(),i).catch(()=>{});await request({action:'end',session}).catch(()=>{});}
  if(context)await context.close();if(browser)await browser.close();
  if(reportPath)fs.writeFileSync(reportPath,JSON.stringify(report,null,2)+'\n');
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
