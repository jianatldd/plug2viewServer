// Requires ten free camera slots. Verifies real server exhaustion, MP4 fallback,
// and WebRTC recovery on Resume. Reservations are released without interrupting
// sessions owned by anyone else. Uses the isolated Chrome debug port 9223.
'use strict';
const path=require('path'),assert=require('assert/strict'),config=require('./config');
const {chromium}=require(path.resolve(__dirname,'../.build/browser/node_modules/playwright-core'));
const credentials=config.readLocal().server.CREDENTIALS,colon=credentials.indexOf(':');
const origin=`http://${config.cameraAddress(process.argv.slice(2))}:${config.servicePort('PORT')}`;
(async()=>{
 let browser,context,page;
 try {
  browser=await chromium.connectOverCDP('http://127.0.0.1:9223');
  context=await browser.newContext({httpCredentials:{username:credentials.slice(0,colon),password:credentials.slice(colon+1)}});
  page=await context.newPage();await page.goto(origin+'/?action=status');await page.goto(origin+'/');
  await page.evaluate(async()=>{stop();await rtcEnding;window.reserved=[];});
  const occupied=await page.evaluate(async()=>{
   const status=await (await fetch('/?action=status')).json();
   if(status.webrtc.active!==0)throw Error('Test requires ten free slots');
   for(let i=0;i<status.webrtc.max_sessions;i++){
    const pc=new RTCPeerConnection({iceServers:[]});
    try{
     pc.addTransceiver('video',{direction:'recvonly'});await pc.setLocalDescription(await pc.createOffer());
     const answer=await viewerSignal({action:'offer',sdp:pc.localDescription.sdp});window.reserved.push(answer.session);
    }finally{pc.close();}
   }
   return (await (await fetch('/?action=status')).json()).webrtc.active;
  });
  assert.equal(occupied,10);
  await page.locator('#pause').click();
  await page.waitForFunction(()=>rtc===null && !!mse.abort && vid.currentTime>1 && !vid.paused,null,{timeout:15000});
  assert.equal(await page.evaluate(async()=>(await (await fetch('/?action=status')).json()).webrtc.active),10);
  console.log('PASS: real ten-slot exhaustion starts MP4 without evicting any reservation');
  await page.evaluate(async()=>{stop();await Promise.all(window.reserved.map(endSession));window.reserved=[];start();});
  await page.waitForFunction(()=>rtc?.pc.connectionState==='connected' && !mse.abort && vid.videoWidth>0,null,{timeout:20000});
  console.log('PASS: after slots are released, Resume returns to WebRTC');
 }finally{
  if(page)await page.evaluate(async()=>{stop();await rtcEnding;await Promise.all((window.reserved||[]).map(endSession));}).catch(()=>{});
  if(context)await context.close();if(browser)await browser.close();
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
