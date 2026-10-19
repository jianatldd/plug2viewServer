// Verify CA trust, hostname and the public camera endpoint (no credential bypass).
// --direct-camera tests the LAN IP with the public hostname for TLS verification.
'use strict';
const https = require('https');
const args = process.argv.slice(2);
const i = args.indexOf('--hostname'), hostname = args[i + 1];
if (i < 0 || !hostname || !/^[a-z0-9.-]+$/i.test(hostname)) throw new Error('Pass --hostname PUBLIC_DNS_NAME');
const direct = args.includes('--direct-camera');
const host = direct ? require('./config').cameraAddress(args) : hostname;
const q = https.get({host, port: direct ? 8443 : 443, servername: hostname, path: '/?action=status', timeout: 10000}, res => {
  const cert = res.socket.getPeerCertificate();
  console.log(JSON.stringify({reachable: true, trusted: res.socket.authorized, hostname, status: res.statusCode, expires: cert.valid_to, issuer: cert.issuer}, null, 2));
  if (!res.socket.authorized || res.statusCode !== 401) process.exitCode = 1;
  res.resume();
});
q.on('timeout', () => q.destroy(new Error('HTTPS timeout; check the 443-to-8443 port forward')));
q.on('error', e => { console.error(e.message); process.exitCode = 1; });
