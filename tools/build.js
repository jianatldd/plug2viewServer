// Cross-compile the camera binaries into camera/payload/ with Zig (no toolchain install
// needed; Zig's bundled musl targets this mipsel soft-float CPU). Get Zig from
// https://ziglang.org/download/ (Windows zip), then:
//
//   node tools/build.js [--zig <path-to-zig(.exe)>] [camd|hkctl|vidcap|bearssl ...]
// Add --webrtc for camd after running tools/build-webrtc.js (pinned DTLS/SRTP).
//
// Zig is found from --zig, $ZIG, or `zig` on PATH. BearSSL (https://bearssl.org, MIT) is
// built from its source tree into camera/src/bearssl/libbearssl.a when asked for
// (`bearssl` target, needs --bearssl-src <dir of bearssl-0.6>); the archive is committed
// so normal builds do not need the BearSSL sources.
'use strict';
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const argv = process.argv.slice(2);
const opt = (n) => { const i = argv.indexOf(n); return i >= 0 ? argv[i + 1] : null; };
const zig = opt('--zig') || process.env.ZIG || 'zig';
const webrtc = argv.includes('--webrtc');
const targets = argv.filter(a => !a.startsWith('--') && a !== opt('--zig') && a !== opt('--bearssl-src'));
const want = targets.length ? targets : ['camd', 'hkctl', 'vidcap'];
const TARGET = ['-target', 'mipsel-linux-musleabi', '-mcpu=mips32', '-Os', '-static', '-s'];

function cc(args, label) {
  const r = spawnSync(zig, ['cc', ...TARGET, ...args], { stdio: 'inherit', cwd: root });
  if (r.error) { console.error(`cannot run ${zig}: ${r.error.message}`); process.exit(1); }
  if (r.status !== 0) { console.error(`${label}: build failed`); process.exit(1); }
  console.log(`${label}: ok`);
}

for (const t of want) {
  if (t === 'camd') cc(['-Icamera/src', ...(webrtc ? ['-DP2V_WEBRTC', '-DMBEDTLS_CONFIG_FILE="webrtc-mbedtls.h"', '-I.build/webrtc/mbedtls/include', '-I.build/webrtc/libsrtp/include', '-pthread'] : []), '-o', 'camera/payload/camd', 'camera/src/camd.c', 'camera/src/tls.c', 'camera/src/google.c', ...(webrtc ? ['camera/src/webrtc.c', '.build/webrtc/libwebrtc.a'] : []), 'camera/src/bearssl/libbearssl.a', '-Wl,--gc-sections'], 'camd');
  else if (t === 'hkctl') cc(['-o', 'camera/payload/hkctl', 'camera/src/hkctl.c'], 'hkctl');
  else if (t === 'vidcap') cc(['-o', 'camera/payload/vidcap', 'camera/src/vidcap.c'], 'vidcap');
  else if (t === 'bearssl') {
    const src = opt('--bearssl-src');
    if (!src) { console.error('bearssl: pass --bearssl-src <path to bearssl-0.6>'); process.exit(1); }
    const files = [];
    (function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (p.endsWith('.c')) files.push(p); } })(path.join(src, 'src'));
    const objDir = path.join(root, 'camera/src/bearssl/obj'); fs.mkdirSync(objDir, { recursive: true });
    const objs = [];
    for (const f of files) {
      const o = path.join(objDir, path.basename(f, '.c') + '.o'); objs.push(o);
      const r = spawnSync(zig, ['cc', ...TARGET.filter(x => x !== '-static' && x !== '-s'), '-c', `-I${path.join(src, 'inc')}`, `-I${path.join(src, 'src')}`, '-o', o, f], { stdio: 'pipe' });
      if (r.status !== 0) { console.error(`bearssl: ${f} failed\n${r.stderr}`); process.exit(1); }
    }
    const ar = spawnSync(zig, ['ar', 'rcs', path.join(root, 'camera/src/bearssl/libbearssl.a'), ...objs], { stdio: 'inherit' });
    if (ar.status !== 0) process.exit(1);
    fs.rmSync(objDir, { recursive: true, force: true });
    fs.cpSync(path.join(src, 'inc'), path.join(root, 'camera/src/bearssl/inc'), { recursive: true });
    console.log(`bearssl: ok (${files.length} files)`);
  } else { console.error(`unknown target ${t}`); process.exit(1); }
}
