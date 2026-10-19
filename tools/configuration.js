// Central configuration validation/rendering. Never include values in errors.
'use strict';
const fs=require('fs'),path=require('path');
const root=path.resolve(__dirname,'..');
const defaults={
  schemaVersion:1,camera:'',pc:'',telnet:{username:'admin',password:'admin'},
  server:{PORT:8080,TLS_PORT:8443,CTL_PORT:8081,H264_DEVICE:'/dev/video1',DEVICE:'/dev/video0',PLFREQ:60,TZ:'EST5EDT,M3.2.0,M11.1.0',SAVE_DIR:'/media/sda1/plug2view',CREDENTIALS:'',TLS_CERT:'',TLS_KEY:''},
  google:{enabled:false,PROJECT_ID:'',PUBLIC_ORIGIN:'',CLIENT_ID:'plug2view-google',CLIENT_SECRET:'',LOGIN_USER:'owner',LOGIN_PASSWORD:'',TOKEN_KEY:''},
  ddns:{enabled:false,HOSTNAME:'',TOKEN:'',updateIntervalSeconds:300},
  letsEncrypt:{enabled:false,checkIntervalSeconds:86400,retryIntervalSeconds:3600,renewBeforeDays:30},
  motion:{MOTION:0,THRESHOLD:273,ZONES:511,CONFIRM:2},
  wifi:{SSID:'',PASS:'',AUTH:'WPA2PSK',ENC:'AES'}
};
function readLocal(file=path.join(root,'config.local.json')) {
  if(!fs.existsSync(file))return {};
  try{return JSON.parse(fs.readFileSync(file,'utf8'));}catch{throw Error('Invalid JSON in config.local.json');}
}
function normalize(input) {
  if(!input||typeof input!=='object'||Array.isArray(input))throw Error('Expected configuration object');
  const out=structuredClone(defaults);
  for(const [key,value]of Object.entries(input)) {
    if(key==='__proto__')throw Error('Unknown config option: '+key);
    if(key.startsWith('_'))continue;
    if(!Object.hasOwn(out,key))throw Error('Unknown config option: '+key);
    if(out[key]&&typeof out[key]==='object') {
      if(!value||typeof value!=='object'||Array.isArray(value))throw Error('Expected object: '+key);
      for(const k of Object.keys(value))if(!Object.hasOwn(out[key],k))throw Error('Unknown config option: '+key+'.'+k);
      Object.assign(out[key],value);
    }else out[key]=value;
  }
  const bad=k=>{throw Error('Invalid config option: '+k);};
  function text(k,v,max=512){if(typeof v!=='string'||/[\x00-\x1f\x7f]/.test(v)||Buffer.byteLength(v)>max)bad(k);}
  function integer(k,v,min,max){if(!Number.isInteger(v)||v<min||v>max)bad(k);}
  if(out.schemaVersion!==1)bad('schemaVersion');
  for(const k of ['camera','pc']){text(k,out[k]);if(out[k]&&!/^[a-zA-Z0-9.-]+$/.test(out[k]))bad(k);}
  for(const [k,v]of Object.entries(out.telnet))text('telnet.'+k,v,128);
  for(const k of ['PORT','TLS_PORT','CTL_PORT'])integer('server.'+k,out.server[k],k==='TLS_PORT'?0:1,65535);
  if(![0,50,60].includes(out.server.PLFREQ))bad('server.PLFREQ');
  for(const [k,v]of Object.entries(out.server))if(!['PORT','TLS_PORT','CTL_PORT','PLFREQ'].includes(k))text('server.'+k,v);
  if(out.server.CREDENTIALS && (!/^[^:]+:.+$/.test(out.server.CREDENTIALS)||Buffer.byteLength(out.server.CREDENTIALS)>90))bad('server.CREDENTIALS');
  if(!!out.server.TLS_CERT!==!!out.server.TLS_KEY)bad('server.TLS_CERT/TLS_KEY');
  for(const k of ['TLS_CERT','TLS_KEY','SAVE_DIR','H264_DEVICE','DEVICE'])if(out.server[k]&&!/^\/[a-zA-Z0-9_./-]+$/.test(out.server[k]))bad('server.'+k);
  for(const group of ['google','ddns','letsEncrypt'])if(typeof out[group].enabled!=='boolean')bad(group+'.enabled');
  for(const [k,v]of Object.entries(out.google))if(k!=='enabled')text('google.'+k,v,k==='PUBLIC_ORIGIN'?255:127);
  if(out.google.enabled) {
    if(!/^[a-z][a-z0-9-]{4,62}$/.test(out.google.PROJECT_ID))bad('google.PROJECT_ID');
    if(!/^https:\/\/[a-z0-9.-]+(?::\d+)?$/i.test(out.google.PUBLIC_ORIGIN))bad('google.PUBLIC_ORIGIN');
    if(!out.server.TLS_PORT)bad('server.TLS_PORT');
    for(const k of ['CLIENT_ID','LOGIN_USER'])if(!out.google[k])bad('google.'+k);
    if(out.google.CLIENT_SECRET.length<32)bad('google.CLIENT_SECRET');
    if(out.google.LOGIN_PASSWORD.length<6)bad('google.LOGIN_PASSWORD');
    if(!/^[a-f0-9]{64}$/i.test(out.google.TOKEN_KEY))bad('google.TOKEN_KEY');
  }
  text('ddns.HOSTNAME',out.ddns.HOSTNAME,253);text('ddns.TOKEN',out.ddns.TOKEN,128);
  if(out.ddns.enabled||out.letsEncrypt.enabled) {
    if(!/^[a-z0-9-]+\.duckdns\.org$/.test(out.ddns.HOSTNAME))bad('ddns.HOSTNAME');
    if(!/^[a-zA-Z0-9-]+$/.test(out.ddns.TOKEN))bad('ddns.TOKEN');
  }
  integer('ddns.updateIntervalSeconds',out.ddns.updateIntervalSeconds,60,86400);
  integer('letsEncrypt.checkIntervalSeconds',out.letsEncrypt.checkIntervalSeconds,3600,604800);
  integer('letsEncrypt.retryIntervalSeconds',out.letsEncrypt.retryIntervalSeconds,300,86400);
  integer('letsEncrypt.renewBeforeDays',out.letsEncrypt.renewBeforeDays,1,60);
  if(out.letsEncrypt.enabled&&out.server.TLS_CERT)bad('server.TLS_CERT (custom certificate conflicts with scheduled renewal)');
  integer('motion.MOTION',out.motion.MOTION,0,1);integer('motion.THRESHOLD',out.motion.THRESHOLD,0,65535);
  integer('motion.ZONES',out.motion.ZONES,0,511);integer('motion.CONFIRM',out.motion.CONFIRM,1,10);
  for(const [k,v]of Object.entries(out.wifi))text('wifi.'+k,v,128);
  if(!['WPA2PSK','WPAPSK','OPEN'].includes(out.wifi.AUTH))bad('wifi.AUTH');
  if(!['AES','TKIP','NONE'].includes(out.wifi.ENC))bad('wifi.ENC');
  return out;
}
function parseConf(data) {
  const out={};
  for(const line of data.split(/\r?\n/)) {
    const m=line.match(/^([A-Z][A-Z0-9_]*)=(.*)$/);if(!m)continue;
    let v=m[2];
    if(v.startsWith("'")&&v.endsWith("'"))v=v.slice(1,-1).replaceAll("'\\''","'");
    else if(v.startsWith('"')&&v.endsWith('"'))v=v.slice(1,-1);
    out[m[1]]=v;
  }
  return out;
}
const shellQuote=s=>"'"+String(s).replaceAll("'","'\\''")+"'";
function render(input) {
  const c=normalize(input),plain=o=>Object.entries(o).map(([k,v])=>k+'='+v+'\n').join('');
  const shell=o=>Object.entries(o).map(([k,v])=>k+'='+shellQuote(v)+'\n').join('');
  const {enabled,...google}=c.google;
  return {
    'mjpg.conf':shell({...c.server,GOOGLE_ENABLED:enabled?1:0}),
    'google.conf':plain(google),
    'duckdns.conf':plain({HOSTNAME:c.ddns.HOSTNAME,TOKEN:c.ddns.TOKEN}),
    'motion.conf':plain(c.motion),'wifi.conf':shell(c.wifi),
    'maintenance.conf':plain({DDNS_ENABLED:c.ddns.enabled?1:0,LE_ENABLED:c.letsEncrypt.enabled?1:0,DDNS_INTERVAL:c.ddns.updateIntervalSeconds,LE_INTERVAL:c.letsEncrypt.checkIntervalSeconds,LE_RETRY:c.letsEncrypt.retryIntervalSeconds,LE_RENEW_DAYS:c.letsEncrypt.renewBeforeDays})
  };
}
function writeRendered(input=readLocal()) {
  const files=render(input),dir=path.join(root,'.build/config');fs.mkdirSync(dir,{recursive:true});
  for(const [name,data]of Object.entries(files))fs.writeFileSync(path.join(dir,name),data,{mode:0o600});
  return dir;
}
function googleConfig(file) {
  if(file)return parseConf(fs.readFileSync(file,'utf8'));
  const c=readLocal();
  if(c.google){const {enabled,...g}=normalize(c).google;return g;}
  return parseConf(fs.readFileSync(path.join(root,'camera/google.conf'),'utf8'));
}
module.exports={root,defaults,readLocal,normalize,parseConf,render,writeRendered,googleConfig};
