// Install an expiring browser playback diagnostic behind existing viewer Basic
// authentication. Does not change firmware, account linking or camera settings.
// Starts a new stream reservation; close other viewers before running.
'use strict';
const fs=require('fs'),path=require('path'),https=require('https'),crypto=require('crypto'),os=require('os');
const {spawn,spawnSync}=require('child_process');
const root=path.resolve(__dirname,'..'), camera=require('./config').cameraAddress(process.argv.slice(2));
const cfg=require('./config').googleConfig();
const origin=new URL(cfg.PUBLIC_ORIGIN);
function shell(command) {
 const r=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'-c',command],{encoding:'utf8',timeout:30000});
 if(r.status)throw new Error('Camera shell failed');return r.stdout.replace(/\r/g,'');
}
function request(url,authorization,body) {
 return new Promise((resolve,reject)=>{
  const data=body===undefined?undefined:JSON.stringify(body),headers={host:origin.host};
  if(authorization)headers.authorization=authorization;
  if(data){headers['content-type']='application/json';headers['content-length']=Buffer.byteLength(data);}
  const req=https.request({host:camera,port:require('./config').servicePort('TLS_PORT'),servername:origin.hostname,path:url,method:data?'POST':'GET',headers,agent:false},res=>{
   const chunks=[];res.on('data',d=>chunks.push(d));res.on('end',()=>resolve({status:res.statusCode,data:Buffer.concat(chunks)}));res.on('aborted',()=>reject(new Error('Truncated response')));
  });req.setTimeout(15000,()=>req.destroy(new Error('Camera HTTPS timeout')));req.on('error',reject);req.end(data);
 });
}
(async()=>{
 const config=shell('cat /mnt/sif/owrt/mjpg.conf');
 const credentials=require('./config').parseConf(config).CREDENTIALS;
 if(!credentials||!credentials.includes(':'))throw new Error('Existing viewer Basic authentication is required');
 if((await request('/webrtc-check.html')).status!==401)throw new Error('Viewer must reject unauthenticated access');
 const generation=(shell('cat /mnt/sif/owrt/google.conf.state').match(/^([a-f0-9]{64})$/m)||[])[1];
 if(!generation)throw new Error('Cannot read token generation');
 const expiry=Math.floor(Date.now()/1000)+600;
 const access=`a.${expiry}.`+crypto.createHmac('sha256',cfg.TOKEN_KEY).update(`a.${expiry}.${generation}`).digest('hex');
 const fulfillment=await request('/google/fulfillment','Bearer '+access,{requestId:'phone-browser-'+Date.now(),inputs:[{intent:'action.devices.EXECUTE',payload:{commands:[{devices:[{id:'plug2view'}],execution:[{command:'action.devices.commands.GetCameraStream',params:{StreamToChromecast:false,SupportedStreamProtocols:['webrtc']}}]}]}}]});
 if(fulfillment.status!==200)throw new Error('Stream reservation failed');
 const command=JSON.parse(fulfillment.data).payload.commands[0];
 if(command.status!=='SUCCESS'||command.states.cameraStreamOffer)throw new Error('Receiver-offer stream unavailable');
 const token=command.states.cameraStreamAuthToken;
 if(!/^[a-f0-9]{64}$/.test(token))throw new Error('Unexpected stream token');
 const html=fs.readFileSync(path.join(__dirname,'webrtc-phone.html'),'utf8').replace('__STREAM_TOKEN__',JSON.stringify(token)).replace('__STREAM_EXPIRES__',String(Date.now()+55*60000));
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'plug2view-phone-'));
 const staged=path.join(dir,'webrtc-check.html');fs.writeFileSync(staged,html);
 const pc=Object.values(os.networkInterfaces()).flat().find(a=>a.family==='IPv4'&&!a.internal&&a.address.startsWith(camera.split('.').slice(0,3).join('.')+'.'))?.address;
 if(!pc)throw new Error('No camera LAN interface');
 const server=spawn(process.execPath,[path.join(__dirname,'tftp-server.js'),dir,'69'],{stdio:['ignore','pipe','pipe']});
 let serverError;server.on('error',e=>{serverError=e;});server.stderr.on('data',()=>{serverError=new Error('TFTP server failed');});
 try {
  await new Promise(resolve=>setTimeout(resolve,500));if(serverError)throw serverError;
  const result=shell(`cd /mnt/sif/owrt/www && tftp -g -r webrtc-check.html -l webrtc-check.html.next ${pc} && mv webrtc-check.html.next webrtc-check.html && chmod 600 webrtc-check.html && echo PHONE_DIAGNOSTIC_OK`);
  if(!result.includes('PHONE_DIAGNOSTIC_OK'))throw new Error('Diagnostic transfer failed');
  const check=await request('/webrtc-check.html','Basic '+Buffer.from(credentials).toString('base64'));
  if(check.status!==200||check.data.toString()!==html)throw new Error('Diagnostic verification failed');
  console.log('Diagnostic ready: '+origin.origin+'/webrtc-check.html');
  console.log('Protected by existing camera viewer login; stream reservation expires within one hour.');
 } finally {server.kill();fs.unlinkSync(staged);fs.rmdirSync(dir);}
})().catch(e=>{console.error(e.message);process.exitCode=1;});
