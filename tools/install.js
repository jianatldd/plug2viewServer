// One-shot installer: serves the payload over TFTP from this PC and drives the camera's
// telnet shell through camera/install.sh.
//
//   node tools/install.js [--camera <ip>] [--pc <this PC's LAN IP>] [--uninstall]
//
// The camera address comes from --camera, $PLUG2VIEW_CAMERA or config.local.json.
// The PC IP is auto-detected (first non-internal IPv4 on the camera's /24) when omitted.
'use strict';
const os = require('os');
const path = require('path');
const fs = require('fs');
const { spawn, spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const argv = process.argv.slice(2);
const opt = (name, dflt) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : dflt; };
const camera = require('./config').cameraAddress(argv);
const uninstall = argv.includes('--uninstall');

function detectPcIp() {
  const prefix = camera.split('.').slice(0, 3).join('.') + '.';
  for (const ifaces of Object.values(os.networkInterfaces()))
    for (const a of ifaces)
      if (a.family === 'IPv4' && !a.internal && a.address.startsWith(prefix)) return a.address;
  throw new Error(`no local IPv4 on ${prefix}0/24 — pass --pc <ip>`);
}
const settings=require('./configuration'),local=settings.readLocal();
const rendered=local.schemaVersion===1?settings.writeRendered(local):null;
const pc = opt('--pc', null) || local.pc || detectPcIp();

// Stage everything the camera will fetch into one TFTP directory.
const stage = path.join(os.tmpdir(), 'plug2view-tftp');
fs.rmSync(stage, { recursive: true, force: true });
fs.mkdirSync(stage, { recursive: true });
const files = {
  'camera/payload/camd': 'camd', 'camera/payload/hkctl': 'hkctl',
  'camera/start.sh': 'start.sh', 'camera/mjpg.conf': 'mjpg.conf', 'camera/56mjpg.sh': '56mjpg.sh',
  'camera/wifi.sh': 'wifi.sh', 'camera/wifi-dhcp.sh': 'wifi-dhcp.sh', 'camera/sd.sh': 'sd.sh',
  'camera/www/index.html': 'index.html', 'camera/install.sh': 'install.sh', 'camera/uninstall.sh': 'uninstall.sh',
};
if (rendered || fs.existsSync(path.join(root, 'camera/wifi.conf'))) files['camera/wifi.conf'] = 'wifi.conf'; // optional, holds a password
// HTTPS key + self-signed certificate (EC P-256), generated once per camera into the
// git-ignored camera/tls/ with openssl (Git for Windows ships one). Only pushed when present.
const tlsDir = path.join(root, 'camera/tls');
if (!fs.existsSync(path.join(tlsDir, 'key.der'))) {
  try {
    fs.mkdirSync(tlsDir, { recursive: true });
    const run = (args) => spawnSync('openssl', args, { stdio: 'pipe' });
    run(['ecparam', '-genkey', '-name', 'prime256v1', '-noout', '-out', path.join(tlsDir, 'key.pem')]);
    run(['pkcs8', '-topk8', '-nocrypt', '-in', path.join(tlsDir, 'key.pem'), '-outform', 'DER', '-out', path.join(tlsDir, 'key.der')]);
    run(['req', '-x509', '-new', '-key', path.join(tlsDir, 'key.pem'), '-days', '3650', '-subj', '/CN=plug2view camera',
         '-addext', `subjectAltName=IP:${camera},DNS:plug2view`, '-outform', 'DER', '-out', path.join(tlsDir, 'cert.der')]);
    if (fs.existsSync(path.join(tlsDir, 'cert.der'))) console.log('generated HTTPS key/certificate in camera/tls/');
    else console.log('openssl not available: HTTPS key not generated (plain HTTP only)');
  } catch (e) { console.log('HTTPS key generation skipped:', e.message); }
}
if (fs.existsSync(path.join(tlsDir, 'key.der'))) { files['camera/tls/key.der'] = 'key.der'; files['camera/tls/cert.der'] = 'cert.der'; }
for (const [src, dst] of Object.entries(files)) {
  // Shell scripts must have LF line endings on the camera.
  const generated=rendered && ['mjpg.conf','wifi.conf'].includes(dst) ? path.join(rendered,dst) : null;
  const buf = fs.readFileSync(generated || path.join(root, src));
  fs.writeFileSync(path.join(stage, dst), /\.(sh|conf|html)$/.test(dst) ? buf.toString().replace(/\r\n/g, '\n') : buf);
}

console.log(`camera ${camera}, this PC ${pc}, tftp dir ${stage}`);
const tftp = spawn(process.execPath, [path.join(root, 'tools/tftp-server.js'), stage, '69'], { stdio: 'inherit' });

const script = uninstall ? 'uninstall.sh' : 'install.sh';
const cmds = [
  `cd /tmp && tftp -g -r ${script} ${pc} && sh /tmp/${script} ${pc}`,
];
setTimeout(() => {
  const r = spawnSync(process.execPath,
    [path.join(root, 'tools/camera-shell.js'), '--camera', camera, '--timeout', '180000', ...cmds.flatMap(c => ['-c', c])],
    { stdio: 'inherit' });
  tftp.kill();
  if (!uninstall && r.status === 0) console.log(`\nOpen http://${camera}:8080/ on any device on this network.`);
  process.exit(r.status ?? 1);
}, 500);
