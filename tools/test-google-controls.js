// Live control verification. Does not link/unlink or revoke OAuth tokens.
// --exercise briefly enables motion, records a clip, moves to Right 1, then
// Original. Refuses to interrupt an existing recording; restores switch state.
// --presets-only checks momentary preset switches without changing recording/motion.
// --recording-policy checks movement suppression and a real five-minute timeout.
'use strict';
const fs=require('fs'),path=require('path'),https=require('https'),crypto=require('crypto'),assert=require('assert/strict');
const {spawnSync}=require('child_process');
const camera=require('./config').cameraAddress(process.argv.slice(2));
const cfg=require('./config').googleConfig();
const origin=new URL(cfg.PUBLIC_ORIGIN);
function shell(command) {
 const r=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'-c',command],{encoding:'utf8',timeout:20000});
 if(r.status)throw new Error('Camera shell failed');return r.stdout.replace(/\r/g,'');
}
const generation=(shell('cat /mnt/sif/owrt/google.conf.state').match(/^([a-f0-9]{64})$/m)||[])[1];
assert.ok(generation);
const expiry=Math.floor(Date.now()/1000)+600;
const access=`a.${expiry}.`+crypto.createHmac('sha256',cfg.TOKEN_KEY).update(`a.${expiry}.${generation}`).digest('hex');
const credentials=require('./config').parseConf(shell('cat /mnt/sif/owrt/mjpg.conf')).CREDENTIALS;
assert.ok(credentials);const basic='Basic '+Buffer.from(credentials).toString('base64');
function request(url,body,authorization=basic) {
 const data=body===undefined?undefined:JSON.stringify(body);
 return new Promise((resolve,reject)=>{
  const headers={host:origin.host,authorization};if(data){headers['content-type']='application/json';headers['content-length']=Buffer.byteLength(data);}
  const req=https.request({host:camera,port:require('./config').servicePort('TLS_PORT'),servername:origin.hostname,path:url,method:data?'POST':'GET',headers,agent:false},res=>{
   const chunks=[];res.on('data',b=>chunks.push(b));res.on('end',()=>{
    const bytes=Buffer.concat(chunks);let json;
    try{if(res.headers['content-type']?.includes('json'))json=JSON.parse(bytes);}catch(e){reject(new Error('Invalid JSON at '+url));return;}
    resolve({status:res.statusCode,json,bytes});
   });res.on('aborted',()=>reject(new Error('Truncated response')));
  });req.setTimeout(20000,()=>req.destroy(new Error('HTTPS timeout at '+url)));req.on('error',reject);req.end(data);
 });
}
async function intent(name,payload) {
 const r=await request('/google/fulfillment',{requestId:'controls-'+Date.now(),inputs:[{intent:'action.devices.'+name,payload}]},'Bearer '+access);
 assert.equal(r.status,200);return r.json.payload;
}
const switchCommand=(id,on)=>({devices:[{id}],execution:[{command:'action.devices.commands.OnOff',params:{on}}]});
async function setSwitch(id,on){const r=await intent('EXECUTE',{commands:[switchCommand(id,on)]});assert.equal(r.commands[0].status,'SUCCESS');assert.equal(r.commands[0].states.on,on);}
async function query(ids){return(await intent('QUERY',{devices:ids.map(id=>({id}))})).devices;}
async function preset(id,on=true){const t=Date.now();const r=await intent('EXECUTE',{commands:[switchCommand(id,on)]});assert.equal(r.commands[0].status,'SUCCESS');assert.equal(r.commands[0].states.on,false);assert.ok(Date.now()-t<1500,'Motor activation must not block video capture');}
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function waitFor(check,label) {for(let i=0;i<45;i++){if(await check())return;await delay(400);}throw new Error('Timed out waiting for '+label);}
async function testPresets() {
 const park=(shell('cat /mnt/sif/owrt/park.pos').match(/^(\d+) (\d+)$/m)||[]).slice(1).map(Number);assert.equal(park.length,2);
 const initial=(await request('/api/status')).json;
 await preset('plug2view-right-1',false);await delay(500);
 const afterOff=(await request('/api/status')).json;assert.equal(afterOff.x,initial.x);assert.equal(afterOff.y,initial.y);
 const right=park[0]+Math.floor((3820-park[0])/3);let polls=0,moved=false;
 try {
  await preset('plug2view-right-1');moved=true;
  assert.equal((await query(['plug2view-right-1']))['plug2view-right-1'].on,false);
  await waitFor(async()=>{const status=(await request('/?action=status')).json;assert.ok(status.frame_age_ms<3000,'Capture must continue during motor movement');polls++;const pos=(await request('/api/status')).json;return pos.x===right&&pos.y===park[1];},'Right 1');
 } finally {
  if(moved) {
   await waitFor(async()=>{const r=await intent('EXECUTE',{commands:[switchCommand('plug2view-original',true)]});if(r.commands[0].errorCode==='deviceBusy')return false;assert.equal(r.commands[0].status,'SUCCESS');assert.equal(r.commands[0].states.on,false);return true;},'Original activation');
   await waitFor(async()=>{const pos=(await request('/api/status')).json;return pos.x===park[0]&&pos.y===park[1];},'Original');
  }
 }
 console.log('PASS: preset Off does not move; On resets to off; Right 1 and Original reach exact targets; capture stayed live during '+polls+' position polls');
}
(async()=>{
 const sync=await intent('SYNC');assert.equal(sync.devices.length,14);
 assert.equal(sync.devices[0].id,'plug2view');assert.deepEqual(sync.devices[0].traits,['action.devices.traits.CameraStream']);
 assert.equal(sync.devices.filter(d=>d.type.endsWith('.SWITCH')).length,13);assert.equal(sync.devices.filter(d=>d.type.endsWith('.SCENE')).length,0);
 for(const d of sync.devices.slice(1))assert.deepEqual(d.traits,['action.devices.traits.OnOff']);
 const states=await query(sync.devices.map(d=>d.id));for(const s of Object.values(states))assert.equal(s.status,'SUCCESS');
 for(const d of sync.devices.slice(3))assert.equal(states[d.id].on,false);
 const unauthorized=await request('/google/fulfillment',{requestId:'unauthorized',inputs:[{intent:'action.devices.EXECUTE',payload:{commands:[switchCommand('plug2view-motion',true)]}}]},'Bearer wrong');assert.equal(unauthorized.status,401);
 const invalid=await intent('EXECUTE',{commands:[switchCommand('plug2view',false),switchCommand('missing',true),switchCommand('plug2view-motion','false')]});
 assert.deepEqual(invalid.commands.map(c=>c.errorCode),['functionNotSupported','deviceNotFound','valueOutOfRange']);
 console.log('PASS: 14 devices, batched QUERY/EXECUTE, authentication, no camera power control, invalid commands rejected');
 if(process.argv.includes('--presets-only')){await testPresets();return;}
 if(process.argv.includes('--recording-policy')) {
  assert.equal((await request('/?action=status')).json.recording,false,'Refusing to interrupt an existing recording');
  const position=(await request('/api/status')).json;
  assert.equal(position.known,true);
  const initialMotion=states['plug2view-motion'].on;
  let moved=false;
  const restore=async()=>{if(moved){await request(`/api/ptz?x=${position.x}&y=${position.y}`);moved=false;}};
  const move=async()=>{moved=true;const dx=position.x<position.range.x[1]-120?120:-120;assert.equal((await request('/api/ptz?dx='+dx)).status,200);};
  try {
   await setSwitch('plug2view-motion',true);
   await move();
   await waitFor(async()=> (await request('/api/motion')).json.state.suppressed,'movement suppression');
   const events=(await request('/api/motion')).json.state.events;
   for(let i=0;i<5;i++) {
    const m=(await request('/api/motion')).json.state;
    assert.equal(m.suppressed,true);assert.equal(m.active,false);assert.equal(m.events,events);
    assert.equal((await request('/?action=status')).json.recording,false);
    await delay(500);
   }
   assert.equal((await request('/?action=rec&start=1&tag=motion&pre=1')).status,409);
   await restore();
   await setSwitch('plug2view-motion',false);
   await delay(8000);
   assert.equal((await request('/api/motion')).json.state.suppressed,false);
   console.log('PASS: real pan/return suppresses motion events and recording; cooldown expires');
   const started=Date.now();
   await setSwitch('plug2view-recording',true);
   let s=(await request('/?action=status')).json;
   assert.equal(s.manual_recording,true);assert.equal(s.recording_limit_seconds,300);
   assert.ok(s.recording_remaining_seconds>290 && s.recording_remaining_seconds<=300);
   await move();await restore();
   assert.equal((await request('/?action=rec&stop=1&tag=motion')).json.manual_recording,true);
   assert.equal((await request('/?action=status')).json.manual_recording,true);
   await delay(3000);await setSwitch('plug2view-recording',true);
   s=(await request('/?action=status')).json;
   assert.ok(s.recording_remaining_seconds<298,'Repeated On must not reset the deadline');
   console.log('PASS: manual recording survives movement and motion stop; repeated On preserves deadline');
   let previousLog=0;
   while(Date.now()-started<315000) {
    s=(await request('/?action=status')).json;
    if(!s.manual_recording)break;
    const elapsed=Date.now()-started;
    if(elapsed-previousLog>=30000){console.log('Auto-stop test: '+s.recording_remaining_seconds+' seconds remaining');previousLog=elapsed;}
    await delay(5000);
   }
   assert.equal(s.manual_recording,false);assert.equal(s.recording,false);
   assert.equal(s.recording_remaining_seconds,0);assert.ok(s.rec_samples>3000);
   assert.ok(Date.now()-started>=299000,'Must run for the configured five minutes');
   assert.equal((await query(['plug2view-recording']))['plug2view-recording'].on,false);
   console.log('PASS: real five-minute recording automatically finalized; Google QUERY reports Off');
  } finally {
   await setSwitch('plug2view-recording',false);await restore();await setSwitch('plug2view-motion',initialMotion);
  }
  return;
 }
 if(!process.argv.includes('--exercise'))return;
 const initial=(await request('/?action=status')).json;assert.equal(initial.recording,false,'Refusing to interrupt an existing recording');
 const motionBefore=(await request('/api/motion')).json;const initialMotion=states['plug2view-motion'].on;
 let mutated=false;
 try {
  mutated=true;await setSwitch('plug2view-recording',true);await setSwitch('plug2view-recording',true);
  const started=(await request('/?action=rec&start=1&tag=motion&pre=1')).json;
  assert.ok(started.recording?.startsWith('manual/'));
  assert.equal((await request('/?action=rec&stop=1&tag=motion')).json.manual_recording,true);
  await setSwitch('plug2view-motion',true);
  await waitFor(async()=> (await request('/api/motion')).json.state.enabled,'motion enable');
  assert.equal((await query(['plug2view-recording']))['plug2view-recording'].on,true);
  await setSwitch('plug2view-motion',false);
  await waitFor(async()=>!(await request('/api/motion')).json.state.enabled,'motion disable');
  const motionAfter=(await request('/api/motion')).json;
  for(const key of ['threshold','zones','confirm'])assert.equal(motionAfter[key],motionBefore[key]);
  const running=(await request('/?action=status')).json;assert.ok(running.rec_samples>0);assert.equal(running.recording,true);
  await setSwitch('plug2view-recording',false);
  const clip=await request('/rec/'+started.recording);assert.equal(clip.status,200);assert.ok(clip.bytes.includes(Buffer.from('moov'))&&clip.bytes.includes(Buffer.from('mdat')));
  fs.writeFileSync('.build/google-controls-test.mp4',clip.bytes);
  console.log('PASS: motion changes apply, settings preserved, manual recording survives motion stop, MP4 finalized');
  await testPresets();
 } finally {
  if(mutated){await setSwitch('plug2view-recording',false);await setSwitch('plug2view-motion',initialMotion);}
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
