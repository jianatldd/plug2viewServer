// Install on-device maintenance tools without changing the live Google hostname.
'use strict';
const fs=require('fs'),path=require('path'),os=require('os'),crypto=require('crypto'),tls=require('tls');
const {spawn,spawnSync}=require('child_process');
const camera=require('./config').cameraAddress(process.argv.slice(2));
const pc=Object.values(os.networkInterfaces()).flat().find(a=>a.family==='IPv4'&&!a.internal&&a.address.startsWith(camera.split('.').slice(0,3).join('.')+'.'))?.address;
if(!pc)throw Error('No camera LAN interface');
const stage=fs.mkdtempSync(path.join(os.tmpdir(),'plug2view-cert-'));
const scriptsOnly=process.argv.includes('--scripts-only');
const files=scriptsOnly?[['camera/certificates-maintain.sh','maintain.sh',700],['camera/start.sh','start.sh',755]]:[['.build/certificates/uacme-camera','uacme',755],['.build/certificates/cert-util','cert-util',755],['camera/certificates-maintain.sh','maintain.sh',700],['camera/duckdns.conf','duckdns.conf',600]];
let required=0;
for(const [src,name]of files){let data=fs.readFileSync(src);if(name.endsWith('.sh')||name.endsWith('.conf'))data=Buffer.from(data.toString().replace(/\r\n/g,'\n'));fs.writeFileSync(path.join(stage,name),data,{mode:0o600});required+=data.length;}
if(!scriptsOnly){const roots=tls.rootCertificates.join('\n')+'\n';fs.writeFileSync(path.join(stage,'ca.pem'),roots);required+=Buffer.byteLength(roots);files.push([null,'ca.pem',644]);}
let script=`#!/bin/sh\nset -e\numask 077\nB=/mnt/sif/owrt\nD=$B/certificates\nS=/media/sda1/plug2view/.certificates\ngrep -q ' /media/sda1 ' /proc/mounts\nmkdir -p "$D" "$S"\ncd "$D"\n`;
for(const [,name,mode]of files){const file=fs.readFileSync(path.join(stage,name));const binary=name==='uacme'||name==='cert-util';const dest=(name==='duckdns.conf'||name==='start.sh'?'$B/':binary?'$S/':'$D/')+name;script+=`tftp -g -r ${name} -l ${dest}.next ${pc}\n[ "$(wc -c < ${dest}.next)" -eq ${file.length} ]\nset -- $(md5sum ${dest}.next)\n[ "$1" = ${crypto.createHash('md5').update(file).digest('hex')} ]\nchmod ${mode} ${dest}.next\nmv ${dest}.next ${dest}\n`;if(binary)script+=`ln -sf ${dest} $D/${name}\n`;}
script+='echo CERT_TOOLS_INSTALLED\n';fs.writeFileSync(path.join(stage,'install.sh'),script);
const server=spawn(process.execPath,['tools/tftp-server.js',stage,'69',camera],{stdio:['ignore','ignore','pipe']});
server.on('error',()=>{process.exitCode=1;});server.stderr.on('data',d=>process.stderr.write(d));
setTimeout(()=>{try{const r=spawnSync(process.execPath,['tools/camera-shell.js','--camera',camera,'--timeout','180000','-c',`cd /tmp && tftp -g -r install.sh -l plug2view-cert-install.sh ${pc} && sh /tmp/plug2view-cert-install.sh`],{encoding:'utf8',timeout:185000});process.stdout.write(r.stdout||'');if(r.status||!r.stdout.includes('CERT_TOOLS_INSTALLED'))process.exitCode=1;else console.log('Installed '+Math.ceil(required/1024)+' KiB; hostname and certificate unchanged.');}finally{server.kill();server.once('exit',()=>{for(const n of fs.readdirSync(stage))fs.unlinkSync(path.join(stage,n));fs.rmdirSync(stage);});}},500);
