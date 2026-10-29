// Build isolated MIPS service recovery tests and stage the shell test sources.
// node tools/build-service-tests.js --zig <zig executable>
'use strict';
const fs=require('fs'),path=require('path'),{spawnSync}=require('child_process');
const root=path.resolve(__dirname,'..'),args=process.argv.slice(2);
const zig=args.includes('--zig')?args[args.indexOf('--zig')+1]:process.env.ZIG||'zig';
const out=path.join(root,'.build/service-tests');fs.mkdirSync(out,{recursive:true});
for(const [name,libs] of [
 ['service-health',[]],
 ['recording-policy',['camera/src/tls.c','camera/src/google.c','camera/src/bearssl/libbearssl.a']]
]) {
 const r=spawnSync(zig,['cc','-target','mipsel-linux-musleabi','-mcpu=mips32','-Os','-static','-s','-Icamera/src',`tools/test-${name}.c`,...libs,'-o',path.join(out,`test-${name}`)],{cwd:root,stdio:'inherit'});
 if(r.error)throw r.error;if(r.status)process.exit(r.status);
 console.log(`Built test-${name}`);
}
for(const [src,dst] of [['camera/start.sh','start-test-source.sh'],['camera/wifi.sh','wifi-test-source.sh'],['tools/test-service-watchdog.sh','test-service-watchdog.sh']]) {
 fs.writeFileSync(path.join(out,dst),fs.readFileSync(path.join(root,src),'utf8').replace(/\r\n/g,'\n'));
}
