// Reproducible MIPS DTLS/SRTP dependencies. Run --fetch once, then build offline.
// node tools/build-webrtc.js --zig .build/zig-x86_64-windows-0.14.1/zig.exe [--fetch]
'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const {spawn, spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..'), dir = path.join(root, '.build/webrtc');
const args = process.argv.slice(2), zig = args.includes('--zig') ? args[args.indexOf('--zig') + 1] : 'zig';
const deps = [
  ['mbedtls', 'https://github.com/Mbed-TLS/mbedtls.git', 'mbedtls-3.6.7', '068ff080b369adfac81509f9b57b2afabaf82dc5'],
  ['libsrtp', 'https://github.com/cisco/libsrtp.git', 'v2.8.1', '6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa']
];
function run(exe, argv, cwd = root) {
  const r = spawnSync(exe, argv, {cwd, encoding: 'utf8'});
  if (r.error || r.status) throw new Error(r.error || r.stderr || `${exe} failed`);
  return r.stdout.trim();
}
fs.mkdirSync(dir, {recursive:true});
for (const [name, url, tag, commit] of deps) {
  const dest = path.join(dir, name);
  if (!fs.existsSync(dest) && args.includes('--fetch')) run('git', ['clone', '--depth', '1', '--branch', tag, url, dest]);
  if (!fs.existsSync(dest)) throw new Error('Run with --fetch to download pinned sources');
  if (run('git', ['rev-parse', 'HEAD'], dest) !== commit) throw new Error(`${name}: unexpected revision`);
  if (run('git', ['status', '--porcelain', '--untracked-files=no'], dest)) throw new Error(`${name}: modified source tree`);
}
const cfg = path.join(dir, 'config'); fs.mkdirSync(cfg, {recursive:true});
const features = 'ARPA_INET_H BYTESWAP_H INTTYPES_H NETINET_IN_H STDINT_H STDLIB_H SYS_SOCKET_H SYS_TYPES_H UNISTD_H INET_ATON INET_PTON SIGACTION USLEEP UINT8_T UINT16_T UINT32_T UINT64_T INT32_T INLINE'.split(' ');
fs.writeFileSync(path.join(cfg, 'config.h'), '#define CPU_RISC 1\n#define SIZEOF_UNSIGNED_LONG 4\n#define SIZEOF_UNSIGNED_LONG_LONG 8\n#define PACKAGE_VERSION "2.8.1"\n#define PACKAGE_STRING "libsrtp2 2.8.1"\n' + features.map(x => `#define HAVE_${x} 1\n`).join(''));
const common = ['-target','mipsel-linux-musleabi','-mcpu=mips32','-Os','-ffunction-sections','-fdata-sections'];
const mbFlags = ['-Icamera/src', '-I.build/webrtc/mbedtls/include', '-DMBEDTLS_CONFIG_FILE="webrtc-mbedtls.h"'];
const srFlags = ['-I.build/webrtc/config','-I.build/webrtc/libsrtp/include','-I.build/webrtc/libsrtp/crypto/include','-DHAVE_CONFIG_H'];
const srFiles = ['srtp/srtp.c','crypto/cipher/cipher.c','crypto/cipher/cipher_test_cases.c','crypto/cipher/null_cipher.c','crypto/cipher/aes.c','crypto/cipher/aes_icm.c','crypto/hash/auth.c','crypto/hash/auth_test_cases.c','crypto/hash/null_auth.c','crypto/hash/hmac.c','crypto/hash/sha1.c','crypto/kernel/alloc.c','crypto/kernel/crypto_kernel.c','crypto/kernel/err.c','crypto/kernel/key.c','crypto/math/datatypes.c','crypto/replay/rdb.c','crypto/replay/rdbx.c'];
const jobs = fs.readdirSync(path.join(dir,'mbedtls/library')).filter(f=>f.endsWith('.c')).map(f=>['.build/webrtc/mbedtls/library/'+f,mbFlags]);
jobs.push(...srFiles.map(f=>['.build/webrtc/libsrtp/'+f,srFlags]));
const objects = path.join(dir,'obj'); fs.mkdirSync(objects,{recursive:true});
const configHash = crypto.createHash('sha256').update(fs.readFileSync(path.join(root,'camera/src/webrtc-mbedtls.h'))).update(fs.readFileSync(path.join(cfg,'config.h'))).update(fs.readFileSync(__filename)).digest('hex');
const out = [], pending = [];
for (const [src, flags] of jobs) {
  const obj = path.join(objects,src.replace(/[^a-zA-Z0-9]/g,'_')+'.o'); out.push(obj);
  const hash = crypto.createHash('sha256').update(configHash).update(fs.readFileSync(path.join(root,src))).digest('hex');
  if (fs.existsSync(obj) && fs.existsSync(obj+'.hash') && fs.readFileSync(obj+'.hash','utf8')===hash) continue;
  pending.push({src,flags,obj,hash});
}
async function worker() {
  for (;;) {
    const j = pending.pop(); if (!j) return;
    await new Promise((resolve,reject)=> {
      const p = spawn(zig,['cc',...common,...j.flags,'-c',j.src,'-o',j.obj],{cwd:root,stdio:['ignore','pipe','pipe']});
      let err=''; p.stderr.on('data',d=>err+=d); p.on('error',reject);
      p.on('exit',code=>{if(code) reject(new Error(`${j.src}: ${err}`)); else {fs.writeFileSync(j.obj+'.hash',j.hash);resolve();}});
    });
  }
}
(async()=>{
  console.log(`WebRTC dependencies: compiling ${pending.length} files`);
  await Promise.all([worker(),worker(),worker(),worker()]);
  run(zig,['ar','rcs',path.join(dir,'libwebrtc.a'),...out]);
  console.log('WebRTC dependencies ready: Mbed TLS 3.6.7 / libsrtp 2.8.1');
})().catch(e=>{console.error(e.message);process.exitCode=1;});
