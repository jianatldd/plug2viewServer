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
const pc = opt('--pc', null) || detectPcIp();

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
if (fs.existsSync(path.join(root, 'camera/wifi.conf'))) files['camera/wifi.conf'] = 'wifi.conf'; // optional, holds a password
for (const [src, dst] of Object.entries(files)) {
  // Shell scripts must have LF line endings on the camera.
  const buf = fs.readFileSync(path.join(root, src));
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
