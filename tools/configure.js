// node tools/configure.js --import | --check | --render | --apply
// Import reads camera settings privately. Apply uploads config/scripts only.
'use strict';
const fs=require('fs'),path=require('path'),os=require('os'),crypto=require('crypto');
const {spawn,spawnSync}=require('child_process');
const settings=require('./configuration'),{cameraAddress}=require('./config');
const args=process.argv.slice(2),mode=args.find(x=>['--import','--check','--render','--apply'].includes(x));
function shell(camera,command,timeout=30000) {
  const r=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'--timeout',String(timeout),'-c',command],{encoding:'utf8',timeout:timeout+5000});
  if(r.status)throw Error('Camera configuration command failed; no credentials printed');
  return r.stdout.replace(/\r/g,'');
}
function importSettings() {
  const current=settings.readLocal();
  if(current.schemaVersion)throw Error('Central config already exists; edit config.local.json instead of importing again');
  const camera=cameraAddress(args);
  const output=shell(camera,'for name in mjpg.conf google.conf duckdns.conf motion.conf wifi.conf maintenance.conf; do echo "[P2V:$name]"; if [ -f /mnt/sif/owrt/$name ]; then cat /mnt/sif/owrt/$name; fi; done; echo "[P2V:renewal]"; if [ -f /mnt/sif/owrt/certificates/enabled ]; then echo ENABLED=1; fi; echo "[P2V:end]"');
  const parts={},re=/^\[P2V:([a-z.]+)\]\n([\s\S]*?)(?=^\[P2V:)/gm;let m;
  while((m=re.exec(output)))parts[m[1]]=settings.parseConf(m[2]);
  if(!parts['mjpg.conf']?.PORT)throw Error('Cannot import camera settings');
  const c=structuredClone(settings.defaults);c.camera=camera;c.pc=current.pc||'';if(current.telnet)c.telnet=current.telnet;
  for(const section of ['server','motion','wifi','google']) {
    const name=section==='server'?'mjpg.conf':section+'.conf';
    for(const [k,v]of Object.entries(parts[name]||{}))if(k in c[section])c[section][k]=typeof c[section][k]==='number'?Number(v):v;
  }
  c.google.enabled=!!parts['google.conf']?.PROJECT_ID&&parts['mjpg.conf'].GOOGLE_ENABLED!=='0';
  c.ddns.HOSTNAME=parts['duckdns.conf']?.HOSTNAME||'';c.ddns.TOKEN=parts['duckdns.conf']?.TOKEN||'';
  const maint=parts['maintenance.conf']||{};
  c.ddns.enabled=!!c.ddns.HOSTNAME&&maint.DDNS_ENABLED!=='0';
  c.letsEncrypt.enabled=parts.renewal?.ENABLED==='1'&&maint.LE_ENABLED!=='0';
  for(const [section,key,raw]of [['ddns','updateIntervalSeconds','DDNS_INTERVAL'],['letsEncrypt','checkIntervalSeconds','LE_INTERVAL'],['letsEncrypt','retryIntervalSeconds','LE_RETRY'],['letsEncrypt','renewBeforeDays','LE_RENEW_DAYS']])if(maint[raw])c[section][key]=Number(maint[raw]);
  settings.normalize(c);
  fs.mkdirSync(path.join(settings.root,'.build'),{recursive:true});
  if(fs.existsSync(path.join(settings.root,'config.local.json')))fs.copyFileSync(path.join(settings.root,'config.local.json'),path.join(settings.root,'.build/config-before-import.json'));
  fs.writeFileSync(path.join(settings.root,'config.local.json'),JSON.stringify(c,null,2)+'\n',{mode:0o600});
  console.log('Imported camera settings into ignored config.local.json; credentials preserved and not printed.');
}
async function apply(c) {
  const camera=cameraAddress(args),pc=c.pc||Object.values(os.networkInterfaces()).flat().find(a=>a.family==='IPv4'&&!a.internal&&a.address.startsWith(camera.split('.').slice(0,3).join('.')+'.'))?.address;
  if(!pc)throw Error('Set pc in config.local.json to this computer LAN address');
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'plug2view-config-'));
  const contents={...settings.render(c),'start.sh':fs.readFileSync(path.join(settings.root,'camera/start.sh'),'utf8').replace(/\r\n/g,'\n'),'maintain.sh':fs.readFileSync(path.join(settings.root,'camera/certificates-maintain.sh'),'utf8').replace(/\r\n/g,'\n')};
  let server;
  try {
    for(const [name,data]of Object.entries(contents))fs.writeFileSync(path.join(dir,name),data,{mode:0o600});
    const dest=name=>name==='maintain.sh'?'certificates/maintain.sh':name;
    let script=`#!/bin/sh\nset -e\numask 077\nB=/mnt/sif/owrt\nR=$B/config-backup\nmkdir /tmp/plug2view-cert-lock 2>/dev/null || { echo CONFIG_MAINTENANCE_BUSY; exit 1; }\necho $$ > /tmp/plug2view-cert-lock/pid\ntrap 'rm -rf /tmp/plug2view-cert-lock' EXIT\ntrap 'exit 1' HUP INT TERM\n`;
    if(c.ddns.enabled||c.letsEncrypt.enabled)script+='[ -x $B/certificates/cert-util ] || { echo CONFIG_INSTALL_CERT_TOOLS_FIRST; exit 1; }\n';
    if(c.letsEncrypt.enabled)script+='[ -x $B/certificates/uacme ] || { echo CONFIG_INSTALL_CERT_TOOLS_FIRST; exit 1; }\n';
    script+='mkdir -p "$R/certificates" "$B/certificates"\nchmod 700 "$R"\n';
    for(const marker of ['enabled','reload-ready'])script+=`if [ -f "$B/certificates/${marker}" ]; then touch "$R/${marker}.present"; else rm -f "$R/${marker}.present"; fi\n`;
    for(const [name,data]of Object.entries(contents)) {
      const target=dest(name),hash=crypto.createHash('md5').update(data).digest('hex');
      script+=`tftp -g -r ${name} -l "$B/${target}.next" ${pc}\n[ "$(wc -c < "$B/${target}.next")" -eq ${Buffer.byteLength(data)} ]\nset -- $(md5sum "$B/${target}.next")\n[ "$1" = ${hash} ]\nif [ -f "$B/${target}" ]; then cp "$B/${target}" "$R/${target}"; rm -f "$R/${name}.absent"; else touch "$R/${name}.absent"; fi\n`;
    }
    // Finalize any current recording before a service restart.
    script+='set +e\n( . "$B/mjpg.conf"; wget -q -O /dev/null "http://127.0.0.1:${PORT:-8080}/?action=rec&stop=1" )\nset -e\n"$B/start.sh" stop\n';
    for(const name of Object.keys(contents))script+=`mv "$B/${dest(name)}.next" "$B/${dest(name)}"\nchmod ${name.endsWith('.sh')?'700':'600'} "$B/${dest(name)}"\n`;
    if(c.letsEncrypt.enabled)script+='touch "$B/certificates/enabled"\nif [ -s "$B/tls/managed/cert.der" ] && [ -s "$B/tls/managed/key.der" ]; then touch "$B/certificates/reload-ready"; fi\n';
    script+='rm -f /tmp/plug2view-ddns.last\nsync\n"$B/start.sh"\nsleep 3\nif ! pgrep camd >/dev/null; then\n "$B/start.sh" stop\n';
    for(const name of Object.keys(contents))script+=` if [ -f "$R/${name}.absent" ]; then rm -f "$B/${dest(name)}"; else cp "$R/${dest(name)}" "$B/${dest(name)}"; fi\n`;
    for(const marker of ['enabled','reload-ready'])script+=` if [ -f "$R/${marker}.present" ]; then touch "$B/certificates/${marker}"; else rm -f "$B/certificates/${marker}"; fi\n`;
    script+=' "$B/start.sh"\n echo CONFIG_ROLLED_BACK\n exit 1\nfi\necho CONFIG_APPLIED\n';
    fs.writeFileSync(path.join(dir,'apply.sh'),script);
    let failure=false;
    server=spawn(process.execPath,[path.join(__dirname,'tftp-server.js'),dir,'69',camera],{stdio:['ignore','ignore','pipe']});
    server.on('error',()=>{failure=true;});server.stderr.on('data',()=>{failure=true;});
    await new Promise(r=>setTimeout(r,500));if(failure)throw Error('TFTP server failed');
    const output=shell(camera,`cd /tmp && tftp -g -r apply.sh -l plug2view-config-apply.sh ${pc} && sh /tmp/plug2view-config-apply.sh`,90000);
    if(!output.includes('CONFIG_APPLIED')) {
      for(const marker of ['CONFIG_MAINTENANCE_BUSY','CONFIG_INSTALL_CERT_TOOLS_FIRST','CONFIG_ROLLED_BACK'])if(output.includes(marker))throw Error(marker);
      throw Error('Configuration application failed; inspect the camera locally');
    }
    console.log('Applied config.local.json; services restarted. Google token state and installed certificates retained.');
    console.log('DDNS schedule: '+c.ddns.enabled+'; certificate-renewal schedule: '+c.letsEncrypt.enabled+'.');
  } finally {
    if(server){const exited=new Promise(r=>server.once('exit',r));server.kill();if(server.exitCode===null)await exited;}
    for(const name of fs.readdirSync(dir))fs.unlinkSync(path.join(dir,name));fs.rmdirSync(dir);
  }
}
(async()=>{
  if(mode==='--import'){importSettings();return;}
  if(!mode)throw Error('Usage: node tools/configure.js --import | --check | --render | --apply');
  const raw=settings.readLocal();if(raw.schemaVersion!==1)throw Error('First import the current camera with --import, or copy config.local.example.json');
  const c=settings.normalize(raw);
  if(mode==='--apply')await apply(c);
  else if(mode==='--render'){settings.writeRendered(c);console.log('Rendered private camera configs under .build/config/');}
  else console.log('Central configuration is valid; no credentials printed.');
})().catch(e=>{console.error(e.message);process.exitCode=1;});
