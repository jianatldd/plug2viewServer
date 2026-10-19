// Run shell commands on the camera over telnet (the only service the stock firmware exposes).
//
//   node tools/camera-shell.js [--camera IP] [--user U] [--pass P] [--timeout ms] <script-file>
//   node tools/camera-shell.js ... -c "cmd1" -c "cmd2"
// The camera address comes from --camera, $PLUG2VIEW_CAMERA or config.local.json (see
// tools/config.js). Telnet login defaults to the firmware's stock admin/admin.
//
// Commands are read from a file (one per line) by default, because Windows argument
// quoting mangles shell syntax; -c is fine for simple commands without quotes.
// Each command is sent after the previous one returns to the prompt. Output is echoed.
'use strict';
const net = require('net');
const fs = require('fs');

const { cameraAddress } = require('./config');
const argv = process.argv.slice(2);
const local = require('./config').readLocal();
const opts = { host: cameraAddress(argv), user: local.telnet?.username || 'admin', pass: local.telnet?.password || 'admin', timeout: 60000 };
const cmds = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--host' || a === '--camera') opts.host = argv[++i];
  else if (a === '--user') opts.user = argv[++i];
  else if (a === '--pass') opts.pass = argv[++i];
  else if (a === '--timeout') opts.timeout = Number(argv[++i]);
  else if (a === '-c') cmds.push(argv[++i]);
  else cmds.push(...fs.readFileSync(a, 'utf8').split(/\r?\n/).filter(l => l.trim() && !l.startsWith('#')));
}
if (!cmds.length) { console.error('no commands given'); process.exit(2); }

const IAC = 255, DO = 253, DONT = 254, WILL = 251, WONT = 252;
const sock = net.connect(23, opts.host);
let buf = '', stage = 0, idx = 0;

sock.on('data', (d) => {
  // Strip telnet option negotiation, refusing everything we are asked for.
  let txt = '';
  for (let i = 0; i < d.length; i++) {
    if (d[i] === IAC && i + 2 < d.length) {
      const cmd = d[i + 1], opt = d[i + 2];
      if (cmd === DO) sock.write(Buffer.from([IAC, WONT, opt]));
      else if (cmd === WILL) sock.write(Buffer.from([IAC, DONT, opt]));
      i += 2;
    } else txt += String.fromCharCode(d[i]);
  }
  process.stdout.write(txt.replace(/\r/g, ''));
  buf += txt;
  if (stage === 0 && /login:\s*$/i.test(buf)) { sock.write(opts.user + '\n'); buf = ''; stage = 1; }
  else if (stage === 1 && /password:\s*$/i.test(buf)) { sock.write(opts.pass + '\n'); buf = ''; stage = 2; }
  else if (stage >= 2 && /[#$>] $/.test(buf)) {
    buf = '';
    if (idx < cmds.length) sock.write(cmds[idx++] + '\n');
    else { sock.write('exit\n'); setTimeout(() => sock.destroy(), 300); }
  }
});
sock.on('error', (e) => { console.error('telnet error:', e.message); process.exit(1); });
sock.on('close', () => process.exit(0));
setTimeout(() => { console.error('\ntimeout'); process.exit(1); }, opts.timeout);
