// Build isolated MIPS regression tests; run the resulting binaries on the camera.
// node tools/build-webrtc-tests.js --zig <zig executable>
'use strict';
const fs=require('fs'),path=require('path'),{spawnSync}=require('child_process');
const root=path.resolve(__dirname,'..'),args=process.argv.slice(2);
const zig=args.includes('--zig')?args[args.indexOf('--zig')+1]:process.env.ZIG||'zig';
const out=path.join(root,'.build/webrtc-tests');fs.mkdirSync(out,{recursive:true});
const base=['cc','-target','mipsel-linux-musleabi','-mcpu=mips32','-Os','-static','-s','-Icamera/src'];
const media=['-DP2V_WEBRTC','-DMBEDTLS_CONFIG_FILE="webrtc-mbedtls.h"','-I.build/webrtc/mbedtls/include','-I.build/webrtc/libsrtp/include','-pthread'];
for(const [name,flags,libs] of [
 ['startup',media,['.build/webrtc/libwebrtc.a']],
 ['sessions',media,['.build/webrtc/libwebrtc.a']],
 ['signaling',['-DP2V_WEBRTC'],['camera/src/bearssl/libbearssl.a']],
 ['sdp',['-UNDEBUG'],[]]
]) {
 const r=spawnSync(zig,[...base,...flags,`tools/test-webrtc-${name}.c`,...libs,'-o',path.join(out,`test-webrtc-${name}`)],{cwd:root,stdio:'inherit'});
 if(r.error)throw r.error;if(r.status)process.exit(r.status);
 console.log(`Built test-webrtc-${name}`);
}
