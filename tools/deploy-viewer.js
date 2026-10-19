// Update only the website; verify the transfer and retain its previous version.
'use strict';
const fs = require('fs'), path = require('path'), os = require('os'), crypto = require('crypto');
const {spawn, spawnSync} = require('child_process');
const camera = require('./config').cameraAddress(process.argv.slice(2));
const pc = Object.values(os.networkInterfaces()).flat().find(a => a.family === 'IPv4' && !a.internal && a.address.startsWith(camera.split('.').slice(0, 3).join('.') + '.'))?.address;
if (!pc) throw new Error('Cannot find camera LAN interface');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'plug2view-viewer-'));
const html = fs.readFileSync(path.join(__dirname, '../camera/www/index.html'));
fs.writeFileSync(path.join(dir, 'index.html'), html);
const hash = crypto.createHash('md5').update(html).digest('hex');
const script = `#!/bin/sh
set -e
cd /mnt/sif/owrt/www
tftp -g -r index.html -l index.html.next ${pc}
[ "$(wc -c < index.html.next)" -eq ${html.length} ]
set -- $(md5sum index.html.next)
[ "$1" = ${hash} ]
cp index.html index.html.previous
chmod 600 index.html.next index.html.previous
mv index.html.next index.html
sync
echo VIEWER_DEPLOY_OK
`;
fs.writeFileSync(path.join(dir, 'viewer-deploy.sh'), script);
const server = spawn(process.execPath, [path.join(__dirname, 'tftp-server.js'), dir, '69', camera], {stdio:['ignore','pipe','pipe']});
let failed = false;
server.on('error', () => {failed = true;}); server.stderr.on('data', () => {failed = true;});
setTimeout(() => {
  try {
    if (failed) throw new Error('TFTP server failed');
    const cmd = `cd /tmp && tftp -g -r viewer-deploy.sh -l plug2view-viewer-deploy.sh ${pc} && sh /tmp/plug2view-viewer-deploy.sh`;
    const r = spawnSync(process.execPath, [path.join(__dirname, 'camera-shell.js'), '--camera', camera, '--timeout', '30000', '-c', cmd], {encoding:'utf8', timeout:40000});
    if (r.status || !r.stdout.includes('VIEWER_DEPLOY_OK')) {
      process.stderr.write(r.stdout || ''); process.stderr.write(r.stderr || '');
      throw new Error('Viewer deployment failed; previous page retained until verified transfer');
    }
    console.log('Viewer installed and transfer checksum verified; previous page retained as index.html.previous.');
  } catch (e) {console.error(e.message); process.exitCode = 1;}
  finally {
    server.kill();
    for (const name of ['index.html', 'viewer-deploy.sh']) fs.unlinkSync(path.join(dir, name));
    fs.rmdirSync(dir);
  }
}, 500);
