// Reversible upgrade of camd + start.sh, with optional Google config/public cert.
// node tools/deploy-google.js [--config camera/google.conf] [--public-cert]
// node tools/deploy-google.js --rollback
'use strict';
const fs = require('fs'), path = require('path'), os = require('os'), crypto = require('crypto');
const {spawn, spawnSync} = require('child_process');
const args = process.argv.slice(2), root = path.resolve(__dirname, '..');
const opt = n => { const i = args.indexOf(n); return i < 0 ? null : args[i + 1]; };
const camera = require('./config').cameraAddress(args);
const pc = opt('--pc') || require('./config').readLocal().pc || Object.values(os.networkInterfaces()).flat().find(a => a.family === 'IPv4' && !a.internal && a.address.startsWith(camera.split('.').slice(0, 3).join('.') + '.'))?.address;
if (!pc) throw new Error('Pass --pc with this computer’s LAN IP');
const stage = fs.mkdtempSync(path.join(os.tmpdir(), 'plug2view-google-'));
const base = '/mnt/sif/owrt';
const latest = '/media/sda1/plug2view/.deploy-last-good';
const files = [ ['camera/payload/camd', 'camd', `${base}/bin/camd`], ['camera/payload/hkctl','hkctl',`${base}/bin/hkctl`], ['camera/start.sh', 'start.sh', `${base}/start.sh`] ];
if (opt('--config')) files.push([path.resolve(opt('--config')), 'google.conf', `${base}/google.conf`]);
// Recovery only: restore a previously saved generation without invalidating its tokens.
if (opt('--state')) files.push([path.resolve(opt('--state')), 'google.conf.state', `${base}/google.conf.state`]);
if (args.includes('--public-cert')) files.push(['camera/tls/public/cert.der', 'cert.der', `${base}/tls/cert.der`], ['camera/tls/public/key.der', 'key.der', `${base}/tls/key.der`]);
const optional = ['google.conf', 'google.conf.state', 'tls/cert.der', 'tls/key.der'];
const rollback = `#!/bin/sh\nset -e\nB=${base}\n[ -f $B/google-backup/camd ] || exit 1\n$B/start.sh stop\ncp $B/google-backup/camd $B/bin/camd\ncp $B/google-backup/start.sh $B/start.sh\n` + optional.map(name => `if [ -f $B/google-backup/${name} ]; then cp $B/google-backup/${name} $B/${name}; elif [ -f $B/google-backup/absent-${name.replaceAll('/', '_')} ]; then rm -f $B/${name}; fi\n`).join('') + '$B/start.sh\n';
const rollbackWithControls=rollback.replace(/\n\$B\/start\.sh\n$/,'\nif [ -f $B/google-backup/hkctl ]; then cp $B/google-backup/hkctl $B/bin/hkctl; fi\n$B/start.sh\n');
const recover = rollbackWithControls.replaceAll('$B/google-backup', latest);
let script;
if (args.includes('--rollback')) script = rollbackWithControls;
else {
  for (const [src, name] of files) {
    const data = fs.readFileSync(path.resolve(root, src));
    fs.writeFileSync(path.join(stage, name), name.endsWith('.sh') || name.endsWith('.conf') ? data.toString().replace(/\r\n/g, '\n') : data);
  }
  fs.writeFileSync(path.join(stage, 'rollback.sh'), rollbackWithControls);
  fs.writeFileSync(path.join(stage, 'recover.sh'), recover);
  script = `#!/bin/sh\nset -e\nB=${base}\nmkdir -p $B/google-backup/tls\nchmod 700 $B/google-backup\nif [ ! -f $B/google-backup/camd ]; then\n cp $B/bin/camd $B/google-backup/camd\n cp $B/start.sh $B/google-backup/start.sh\n` + optional.map(name => ` if [ -f $B/${name} ]; then cp $B/${name} $B/google-backup/${name}; else touch $B/google-backup/absent-${name.replaceAll('/', '_')}; fi\n`).join('') + `fi\ncd $B/google-backup\ntftp -g -r rollback.sh ${pc}\n`;
  // Keep each deployment's immediate predecessor on SD, separate from the
  // explicit original-install rollback. Stop before any mutation if SD is absent.
  script += `[ -f $B/google-backup/hkctl ] || cp $B/bin/hkctl $B/google-backup/hkctl\ngrep -q ' /media/sda1 ' /proc/mounts\nmkdir -p ${latest}/tls\nchmod 700 ${latest}\ncp $B/bin/camd ${latest}/camd\ncp $B/bin/hkctl ${latest}/hkctl\ncp $B/start.sh ${latest}/start.sh\n`;
  for (const name of optional) script += `if [ -f $B/${name} ]; then cp $B/${name} ${latest}/${name}; rm -f ${latest}/absent-${name.replaceAll('/', '_')}; else rm -f ${latest}/${name}; touch ${latest}/absent-${name.replaceAll('/', '_')}; fi\n`;
  script += `cd ${latest}\ntftp -g -r recover.sh ${pc}\n`;
  for (const [, name, dest] of files) {
    const data = fs.readFileSync(path.join(stage, name));
    const digest = crypto.createHash('md5').update(data).digest('hex');
    script += `cd ${path.posix.dirname(dest)}\ntftp -g -r ${name} -l ${path.posix.basename(dest)}.next ${pc}\n[ "$(wc -c < ${dest}.next)" -eq ${data.length} ]\nset -- $(md5sum ${dest}.next)\n[ "$1" = ${digest} ]\n`;
  }
  script += `$B/start.sh stop\n`;
  for (const [, name, dest] of files) script += `mv ${dest}.next ${dest}\nchmod ${name === 'camd' || name === 'hkctl' || name.endsWith('.sh') ? '755' : '600'} ${dest}\n`;
  script += `$B/start.sh\nsleep 4\nif ! pgrep camd >/dev/null; then cp /tmp/camd.log ${latest}/failed-camd.log; sh ${latest}/recover.sh; echo GOOGLE_DEPLOY_RECOVERED; exit 1; fi\necho GOOGLE_DEPLOY_OK\n`;
}
fs.writeFileSync(path.join(stage, 'deploy.sh'), script);
const tftp = spawn(process.execPath, [path.join(__dirname, 'tftp-server.js'), stage, '69', camera], {stdio: ['ignore', 'pipe', 'pipe']});
let failed = false;
tftp.on('error', e => { failed = true; console.error(e.message); });
tftp.stderr.on('data', d => { failed = true; process.stderr.write(d); });
setTimeout(() => {
  try {
    if (failed) throw new Error('TFTP server failed');
    const cmd = `cd /tmp && tftp -g -r deploy.sh -l plug2view-google-deploy.sh ${pc} && sh /tmp/plug2view-google-deploy.sh`;
    const r = spawnSync(process.execPath, [path.join(__dirname, 'camera-shell.js'), '--camera', camera, '--timeout', '120000', '-c', cmd], {encoding: 'utf8', timeout: 130000});
    // Deployment commands contain no secret values. Config files travel by filename.
    process.stdout.write(r.stdout || ''); process.stderr.write(r.stderr || '');
    if (r.status || (!args.includes('--rollback') && !r.stdout.includes('GOOGLE_DEPLOY_OK'))) process.exitCode = 1;
  } catch (e) { console.error(e.message); process.exitCode = 1; }
  finally {
    tftp.kill();
    // Only files this invocation created; no recursive deletion or computed shell paths.
    tftp.once('exit', () => {
      for (const name of fs.readdirSync(stage)) fs.unlinkSync(path.join(stage, name));
      fs.rmdirSync(stage);
    });
  }
}, 500);
