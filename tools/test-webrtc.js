// Real Chrome -> camera ICE/DTLS/SRTP test. Does not link/unlink or revoke OAuth.
// Starts one stream, replacing any active WebRTC test; closes it on completion.
// Use portable Node 22; Chrome debugging port 9223 and Playwright core are required.
// --reload-tls sends SIGHUP during playback and verifies that the daemon survives.
'use strict';
const fs=require('fs'),path=require('path'),https=require('https'),crypto=require('crypto'),assert=require('assert/strict');
const {spawnSync}=require('child_process');
const root=path.resolve(__dirname,'..'),camera=require('./config').cameraAddress(process.argv.slice(2));
const {chromium}=require(path.join(root,'.build/browser/node_modules/playwright-core'));
const cfg=Object.fromEntries(fs.readFileSync(path.join(root,'camera/google.conf'),'utf8').split(/\r?\n/).filter(s=>s&&!s.startsWith('#')).map(s=>{let i=s.indexOf('=');return[s.slice(0,i),s.slice(i+1)]}));
const origin=new URL(cfg.PUBLIC_ORIGIN);
const badFingerprint=process.argv.includes('--bad-fingerprint');
const receiverOffer=process.argv.includes('--receiver-offer');
const videoOnly=process.argv.includes('--video-only');
const sh=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'-c','cat /mnt/sif/owrt/google.conf.state'],{encoding:'utf8',timeout:20000});
if(sh.status) throw new Error('Cannot read camera token generation');
const generation=(sh.stdout.match(/^([a-f0-9]{64})\r?$/m)||[])[1];
if(!generation) throw new Error('Cannot parse camera token generation');
const exp=Math.floor(Date.now()/1000)+600;
const access=`a.${exp}.`+crypto.createHmac('sha256',cfg.TOKEN_KEY).update(`a.${exp}.${generation}`).digest('hex');
function request(url,body,token,method='POST') {
  const data=body===undefined?undefined:JSON.stringify(body);
  return new Promise((resolve,reject)=>{
    let responseStatus=0,responseBytes=0;
    const headers={host:origin.host,'content-type':'application/json'};
    if(token) headers.authorization='Bearer '+token;
    if(data) headers['content-length']=Buffer.byteLength(data);
    const req=https.request({host:camera,port:8443,servername:origin.hostname,path:url,method,headers,agent:false},res=>{
      responseStatus=res.statusCode;
      let text='';res.on('data',b=>{responseBytes+=b.length;text+=b;});res.on('end',()=>{try{resolve({status:res.statusCode,headers:res.headers,json:JSON.parse(text)});}catch(e){reject(new Error('Invalid JSON response from '+url));}});
      res.on('aborted',()=>reject(new Error('Truncated HTTPS response from '+url)));
    });
    req.setTimeout(20000,()=>req.destroy(new Error(`Camera HTTPS timeout at ${url}: status=${responseStatus}, received=${responseBytes}, sent=${data?Buffer.byteLength(data):0}`)));req.on('error',reject);req.end(data);
  });
}
async function offer(requestId='webrtc-browser-test') {
 const res=await request('/google/fulfillment',{requestId,inputs:[{intent:'action.devices.EXECUTE',payload:{commands:[{devices:[{id:'plug2view'}],execution:[{command:'action.devices.commands.GetCameraStream',params:{StreamToChromecast:!receiverOffer,SupportedStreamProtocols:receiverOffer?['webrtc']:['webrtc','hls']}}]}]}}]},access);
 assert.equal(res.status,200);const cmd=res.json.payload.commands[0];assert.equal(cmd.status,'SUCCESS');
 assert.equal(cmd.states.cameraStreamProtocol,'webrtc');return cmd.states;
}
(async()=>{
 let browser,context,token;
 try {
  const sync=await request('/google/fulfillment',{requestId:'webrtc-sync-test',inputs:[{intent:'action.devices.SYNC'}]},access);
  assert.deepEqual(sync.json.payload.devices[0].attributes.cameraStreamSupportedProtocols,['webrtc','hls']);
  const o=await offer();token=o.cameraStreamAuthToken;
  assert.equal(typeof o.cameraStreamOffer,receiverOffer?'undefined':'string','Phone should initiate negotiation; Hub should receive camera offer');
  const pending=await offer('webrtc-overlapping-test');
  assert.ok(pending.cameraStreamAuthToken===token,'Overlapping fulfillment must preserve the pending token');
  assert.ok(pending.cameraStreamOffer===o.cameraStreamOffer,'Overlapping fulfillment must preserve the pending SDP');
  const unauthorized=await request('/google/webrtc',{action:'end',deviceId:'plug2view'},'wrong');
  assert.equal(unauthorized.status,401);
  assert.equal(unauthorized.headers['www-authenticate'],undefined,'Google signaling must never challenge for camera viewer Basic login');
  assert.equal((await request('/google/webrtc',{action:'end',deviceId:'wrong'},token)).status,400);
  assert.equal((await request('/google/webrtc',{action:'answer',deviceId:'plug2view',sdp:'invalid'},token)).status,400);
  const largeStarted=Date.now();
  assert.equal((await request('/google/webrtc',{action:receiverOffer?'offer':'answer',deviceId:'plug2view',sdp:'x'.repeat(12000)},token)).status,400);
  assert.ok(Date.now()-largeStarted<5000,'TLS must drain a POST body larger than the HTTP header buffer without waiting for another network event');
  const cors=await request('/google/webrtc',undefined,undefined,'OPTIONS');assert.equal(cors.status,200);assert.equal(cors.headers['access-control-allow-origin'],'https://www.gstatic.com');
  console.log('PASS: WebRTC preferred in SYNC; signaling rejects wrong token/device/SDP; CORS preflight works');
  browser=await chromium.connectOverCDP('http://127.0.0.1:9223');context=await browser.newContext();const page=await context.newPage();
  await page.setContent('<video id="v" autoplay muted playsinline style="width:640px"></video>');
  let description=await page.evaluate(async ({sdp,receiverOffer,videoOnly})=>{
   const pc=window.pc=new RTCPeerConnection({iceServers:[]});window.samples=[];
   const video=document.querySelector('video');
   const stream=new MediaStream();video.srcObject=stream;
   pc.ontrack=e=>{stream.addTrack(e.track);video.play().catch(()=>{});};
   const frame=(now,m)=>{if(window.samples.length<300) window.samples.push({captureToCallbackMs:m.captureTime===undefined?null:now-m.captureTime,decodeMs:m.processingDuration===undefined?null:m.processingDuration*1000});video.requestVideoFrameCallback(frame);};video.requestVideoFrameCallback(frame);
   if(receiverOffer) {
    const audio=pc.addTransceiver('audio',{direction:'recvonly'});
    if(videoOnly)audio.setCodecPreferences(RTCRtpReceiver.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()==='audio/opus'));
    pc.addTransceiver('video',{direction:'recvonly'});
    pc.createDataChannel('diagnostics');
    await pc.setLocalDescription(await pc.createOffer());
   } else { await pc.setRemoteDescription({type:'offer',sdp});await pc.setLocalDescription(await pc.createAnswer()); }
   if(pc.iceGatheringState!=='complete') await new Promise(resolve=>{const t=setTimeout(resolve,4000);pc.addEventListener('icegatheringstatechange',()=>{if(pc.iceGatheringState==='complete'){clearTimeout(t);resolve();}});});
   return pc.localDescription.sdp;
  },{sdp:o.cameraStreamOffer,receiverOffer,videoOnly});
  const action=receiverOffer?'offer':'answer';
  if(badFingerprint) description=description.replace(/(a=fingerprint:sha-256 )([0-9A-F]{2})/ig,(_,prefix,byte)=>prefix+(byte==='00'?'01':'00'));
  const a=await request('/google/webrtc',{action,deviceId:'plug2view',sdp:description},token);
  assert.equal(a.status,200,'Camera must accept browser '+action);
  assert.equal(a.headers['www-authenticate'],undefined,'Authorized Google signaling must not require viewer credentials');
  if(receiverOffer) {
   assert.equal(a.json.action,'answer');
   assert.ok((videoOnly?/^m=audio 0 /m:/^m=audio 9 UDP\/TLS\/RTP\/SAVPF 0\r?$/m).test(a.json.sdp) && /^m=application 0 /m.test(a.json.sdp),'Negotiate PCMU if offered, reject unsupported audio and data');
   assert.equal((a.json.sdp.match(/^m=/gm)||[]).length,3,'Answer must preserve offered media count');
   await page.evaluate(sdp=>pc.setRemoteDescription({type:'answer',sdp}),a.json.sdp);
   console.log('PASS: receiver offer negotiates '+(videoOnly?'video only':'H264 plus silent PCMU')+'; data channel rejected');
  } else assert.deepEqual(a.json,{},'A camera-originated offer must acknowledge Google answer with an empty object');
  if(!badFingerprint) {
   await new Promise(resolve=>setTimeout(resolve,1000));
   const repeated=await offer('webrtc-after-answer-test');
   assert.ok(repeated.cameraStreamAuthToken===token,'Fulfillment after answer must preserve the stream token');
   assert.ok(repeated.cameraStreamOffer===o.cameraStreamOffer,'Fulfillment after answer must preserve the SDP');
   const ack=await request('/google/webrtc',{action,deviceId:'plug2view',sdp:description},token);
   assert.equal(ack.status,200,'Identical signaling retry must succeed');
   assert.ok(JSON.stringify(ack.json)===JSON.stringify(a.json),'Repeated signaling must use the same response');
   const changed=description.replace(/(a=fingerprint:sha-256 )([0-9A-F]{2})/ig,(_,prefix,byte)=>prefix+(byte==='00'?'01':'00'));
   assert.equal((await request('/google/webrtc',{action,deviceId:'plug2view',sdp:changed},token)).status,400,'Conflicting description must not replace the active peer');
   console.log('PASS: overlapping fulfillment and repeated signaling preserve the session; conflicting description rejected');
  }
  let first,latest;
  for(let i=0;i<(badFingerprint?3:6);i++) {
   if(i===1 && process.argv.includes('--reload-tls')) {
    const reload=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'--timeout','10000','-c','pgrep camd','-c','killall -HUP camd','-c','pgrep camd'],{encoding:'utf8',timeout:15000});
    const pids=(reload.stdout||'').replace(/\r/g,'').match(/^\d+$/gm)||[];
    assert.equal(reload.status,0);assert.equal(pids.length,2);assert.equal(pids[0],pids[1],'TLS reload must not restart camera daemon');
    console.log('PASS: TLS reload requested during WebRTC playback; camera PID unchanged');
   }
   await new Promise(resolve=>setTimeout(resolve,5000));
   latest=await page.evaluate(async()=>{
    const all=[...(await pc.getStats()).values()],inbound=all.find(x=>x.type==='inbound-rtp'&&x.kind==='video'),transport=all.find(x=>x.type==='transport'),audio=all.find(x=>x.type==='inbound-rtp'&&x.kind==='audio');
    const pair=all.find(x=>x.id===transport?.selectedCandidatePairId);
    return {connection:pc.connectionState,ice:pc.iceConnectionState,frames:inbound?.framesDecoded||0,fps:inbound?.framesPerSecond||0,packetsLost:inbound?.packetsLost||0,jitterMs:(inbound?.jitter||0)*1000,jitterBufferMs:inbound?.jitterBufferEmittedCount?1000*inbound.jitterBufferDelay/inbound.jitterBufferEmittedCount:null,roundTripMs:pair?.currentRoundTripTime===undefined?null:pair.currentRoundTripTime*1000,samples:window.samples.length,audioPackets:audio?.packetsReceived||0,audioSamples:audio?.totalSamplesReceived||0,audioLost:audio?.packetsLost||0,audioEnergy:audio?.totalAudioEnergy||0,audioCodec:all.find(x=>x.id===audio?.codecId)?.mimeType};
   });
   console.log(JSON.stringify(latest));if(!first) first=latest;
  }
  if(badFingerprint) {
   assert.notEqual(latest.connection,'connected');assert.equal(latest.frames,0);
   assert.equal(latest.audioPackets,0,'Unauthenticated peer must not receive audio');
   console.log('PASS: mismatched SDP certificate fingerprint cannot establish DTLS or receive video');return;
  }
  assert.equal(latest.connection,'connected');assert.ok(latest.frames-first.frames>100,'Must keep decoding moving video');
  if(receiverOffer&&!videoOnly) {
   assert.equal(latest.audioCodec,'audio/PCMU');assert.ok(latest.audioSamples-first.audioSamples>150000,'Must continuously decode silent audio');
   assert.equal(latest.audioEnergy,0,'Generated audio must remain silent');assert.equal(latest.audioLost,0);
  } else assert.equal(latest.audioPackets,0);
  const established=await offer('webrtc-after-answer-test');
  assert.ok(established.cameraStreamAuthToken===token && established.cameraStreamOffer===o.cameraStreamOffer,'Exact request retry after setup window must preserve the established session');
  const measurements=await page.evaluate(()=>window.samples.filter(x=>x.captureToCallbackMs!==null).map(x=>x.captureToCallbackMs));
  if(measurements.length) {measurements.sort((a,b)=>a-b);console.log('Browser estimated capture-to-render ms: median='+Math.round(measurements[Math.floor(measurements.length/2)])+', p95='+Math.round(measurements[Math.floor(measurements.length*.95)]));}
  console.log('PASS: Chrome received and continuously decoded camera H264 over authenticated DTLS/SRTP');
  assert.equal((await request('/google/webrtc',{action:'end',deviceId:'plug2view'},token)).status,200);
  const next=await offer('webrtc-new-session-test'),oldToken=token;token=next.cameraStreamAuthToken;
  assert.ok(token!==oldToken && (receiverOffer || next.cameraStreamOffer!==o.cameraStreamOffer),'Explicit end must allow fresh session credentials');
  assert.equal((await request('/google/webrtc',{action:'end',deviceId:'plug2view'},oldToken)).status,401,'Retired token must not end the new session');
  console.log('PASS: explicit end allows a new session; retired token cannot control it');
 } finally {
  if(token) await request('/google/webrtc',{action:'end',deviceId:'plug2view'},token).catch(()=>{});
  if(context) await context.close();if(browser) await browser.close();
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
