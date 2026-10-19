// Exercise schedule switches in an isolated camera /tmp fixture, with fake
// DNS/ACME executables. Does not change installed services or contact an issuer.
'use strict';
const fs=require('fs'),path=require('path'),os=require('os');
const {spawn,spawnSync}=require('child_process');
const {cameraAddress,readLocal,render}=require('./config');
const root=path.resolve(__dirname,'..'),camera=cameraAddress(process.argv.slice(2));
const pc=readLocal().pc||Object.values(os.networkInterfaces()).flat().find(a=>a.family==='IPv4'&&!a.internal&&a.address.startsWith(camera.split('.').slice(0,3).join('.')+'.'))?.address;
if(!pc)throw Error('Set pc in config.local.json');
const remote='/tmp/p2v-schedule-test-'+Date.now();
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'plug2view-schedule-test-'));
const files={};
files['maintain.sh']=fs.readFileSync(path.join(root,'camera/certificates-maintain.sh'),'utf8').replace(/\r\n/g,'\n').replaceAll('/mnt/sif/owrt',remote).replaceAll('/tmp/plug2view-',remote+'/state-').replaceAll('/tmp/certificates.log',remote+'/certificates.log');
files['start.sh']=fs.readFileSync(path.join(root,'camera/start.sh'),'utf8').replace(/\r\n/g,'\n').replaceAll('/mnt/sif/owrt',remote).replaceAll('/tmp/hk-watch',remote+'/watch').replaceAll('/tmp/sd.log',remote+'/sd.log');
files['cert-util']=`#!/bin/sh
echo "$1" >> ${remote}/calls
case "$1" in
 run) shift; shift; exec "$@";;
 hostname) echo test-camera.duckdns.org;;
 rmdir) exec /mnt/sif/owrt/certificates/cert-util rmdir "$2";;
 due|ddns|convert|same) exit 0;;
 *) exit 99;;
esac
`;
files.uacme=`#!/bin/sh\necho issue >> ${remote}/calls\nexit 1\n`;
files.pgrep='#!/bin/sh\nexit 0\n';
files['sd.sh']='#!/bin/sh\nexit 0\n';
let script=`#!/bin/sh
set -eu
T=${remote}
export PATH="$T/bin:$PATH"
mkdir -p "$T/bin" "$T/certificates/acme" "$T/tls" "$T/watch"
mv "$T/cert-util" "$T/certificates/cert-util"
mv "$T/uacme" "$T/certificates/uacme"
mv "$T/maintain.sh" "$T/certificates/maintain.sh"
mv "$T/pgrep" "$T/bin/pgrep"
chmod 700 "$T/certificates/cert-util" "$T/certificates/uacme" "$T/bin/pgrep"
touch "$T/certificates/enabled" "$T/certificates/acme/account-ready" "$T/duckdns.conf"
echo "CREDENTIALS=''" > "$T/mjpg.conf"
sh -n "$T/start.sh"
sh -n "$T/certificates/maintain.sh"
`;
for(const ddns of [false,true])for(const le of [false,true]) {
  const config=render({ddns:{enabled:ddns,HOSTNAME:'test-camera.duckdns.org',TOKEN:'test-token'},letsEncrypt:{enabled:le}})['maintenance.conf'];
  script+=`cat > "$T/maintenance.conf" <<'CONFIG'
${config}CONFIG
: > "$T/calls"
sh "$T/certificates/maintain.sh" check
${ddns?"grep -q '^ddns$' \"$T/calls\"":"if grep -q '^ddns$' \"$T/calls\"; then exit 1; fi"}
${le?"grep -q '^issue$' \"$T/calls\"":"if grep -q '^issue$' \"$T/calls\"; then exit 1; fi"}
touch "$T/watch/certificates"
sh "$T/start.sh"
[ ${ddns||le?'-f':'! -e'} "$T/watch/certificates" ]
echo 'PASS: DDNS=${ddns}, renewal=${le}, watchdog=${ddns||le}'
`;
}
script+=`echo 'DDNS_ENABLED=0' > "$T/maintenance.conf"
echo 'LE_ENABLED=0' >> "$T/maintenance.conf"
rm -f "$T/certificates/cert-util"
sh "$T/certificates/maintain.sh" check
echo 'PASS: disabled schedules need no certificate tools'
echo SCHEDULE_TEST_PASSED
`;
files['test.sh']=script;
let server;
(async()=>{
 try {
  for(const [name,data]of Object.entries(files))fs.writeFileSync(path.join(dir,name),data);
  server=spawn(process.execPath,[path.join(__dirname,'tftp-server.js'),dir,'69',camera],{stdio:'ignore'});
  let failed=false;server.on('error',()=>{failed=true;});
  await new Promise(r=>setTimeout(r,500));if(failed||server.exitCode!==null)throw Error('TFTP server failed');
  const command=`mkdir -p ${remote} && cd ${remote} && `+Object.keys(files).map(n=>`tftp -g -r ${n} -l ${n} ${pc}`).join(' && ')+` && sh ${remote}/test.sh`;
  const r=spawnSync(process.execPath,[path.join(__dirname,'camera-shell.js'),'--camera',camera,'--timeout','45000','-c',command],{encoding:'utf8',timeout:50000});
  if(r.status||!r.stdout.includes('\nSCHEDULE_TEST_PASSED'))throw Error('Isolated schedule test failed; fixture retained in camera /tmp');
  console.log(r.stdout.split(/\r?\n/).filter(l=>l.startsWith('PASS:')).join('\n'));
 } finally {
  if(server){const exited=new Promise(r=>server.once('exit',r));server.kill();if(server.exitCode===null)await exited;}
  for(const name of fs.readdirSync(dir))fs.unlinkSync(path.join(dir,name));fs.rmdirSync(dir);
 }
})().catch(e=>{console.error(e.message);process.exitCode=1;});
