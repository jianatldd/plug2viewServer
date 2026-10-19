// Optional PC-side certificate maintenance. No PC is needed after deployment.
// Install: npm install --prefix .build/acme --ignore-scripts acme-client@5.4.0
// Probe only: node tools/issue-google-cert.js --hostname HOST --probe
// Issue: node tools/issue-google-cert.js --hostname HOST --email EMAIL --accept-terms
// Public TCP 80 must forward here to TCP 8083 during validation/renewal.
'use strict';
const http = require('http'), fs = require('fs'), path = require('path'), crypto = require('crypto');
const {spawnSync} = require('child_process');
const args = process.argv.slice(2);
const opt = n => { const i = args.indexOf(n); return i < 0 ? null : args[i + 1]; };
const hostname = opt('--hostname');
if (!hostname || hostname.length > 253 || !/^[a-z0-9.-]+$/i.test(hostname)) throw new Error('Pass --hostname with the public DNS name');
const probe = args.includes('--probe'), staging = args.includes('--staging');
if (!probe && !args.includes('--accept-terms')) {
  console.error('Issuance requires your acceptance of the issuer terms: https://letsencrypt.org/repository/\nRun with --accept-terms only after accepting those terms. Use --probe to test forwarding without creating an account.');
  process.exit(2);
}
const tokens = new Map();
const probeToken = 'plug2view-' + crypto.randomBytes(16).toString('hex');
tokens.set(probeToken, 'plug2view certificate forwarding check');
const server = http.createServer((req, res) => {
  const token = req.url.startsWith('/.well-known/acme-challenge/') ? req.url.slice(28) : '';
  const text = req.method === 'GET' ? tokens.get(token) : undefined;
  res.writeHead(text ? 200 : 404, {'Content-Type': 'text/plain', 'Cache-Control': 'no-store'}); res.end(text || 'Not found');
});
server.requestTimeout = 15000; server.headersTimeout = 10000;
server.on('error', e => { console.error(e.message); process.exitCode = 1; });
server.listen(8083, '0.0.0.0', async () => {
  console.log(`Forwarding check: http://${hostname}/.well-known/acme-challenge/${probeToken}`);
  if (probe) { console.log('Probe server listening on TCP 8083; Ctrl+C stops it. No certificate account has been created.'); return; }
  try {
    const acme = require('../.build/acme/node_modules/acme-client');
    const dir = path.resolve(__dirname, '../camera/tls/acme' + (staging ? '-staging' : ''));
    fs.mkdirSync(dir, {recursive: true});
    const accountFile = path.join(dir, 'account.pem');
    if (!fs.existsSync(accountFile)) fs.writeFileSync(accountFile, await acme.crypto.createPrivateEcdsaKey('P-256'), {flag: 'wx', mode: 0o600});
    const key = await acme.crypto.createPrivateEcdsaKey('P-256');
    const [, csr] = await acme.crypto.createCsr({altNames: [hostname]}, key);
    const client = new acme.Client({directoryUrl: staging ? acme.directory.letsencrypt.staging : acme.directory.letsencrypt.production, accountKey: fs.readFileSync(accountFile)});
    const cert = await client.auto({
      csr, email: opt('--email') || undefined, termsOfServiceAgreed: true,
      challengePriority: ['http-01'],
      challengeCreateFn: async (authz, challenge, authorization) => {
        if (challenge.type !== 'http-01' || authz.identifier.value !== hostname) throw new Error('Unexpected certificate challenge');
        tokens.set(challenge.token, authorization);
      },
      challengeRemoveFn: async (_authz, challenge) => { tokens.delete(challenge.token); },
    });
    const leaf = new crypto.X509Certificate(cert);
    if (!leaf.checkHost(hostname) || !leaf.checkPrivateKey(crypto.createPrivateKey(key))) throw new Error('Issued certificate does not match hostname/key');
    // Preserve previous issuance files before a successful renewal replaces them.
    for (const [name, data] of [['key.pem', key], ['fullchain.pem', cert]]) {
      const file = path.join(dir, name); if (fs.existsSync(file)) fs.copyFileSync(file, file + '.previous');
      fs.writeFileSync(file, data, {mode: 0o600});
    }
    if (!staging) {
      const prepared = spawnSync(process.execPath, [path.join(__dirname, 'prepare-public-cert.js'), '--chain', path.join(dir, 'fullchain.pem'), '--key', path.join(dir, 'key.pem')], {stdio: 'inherit'});
      if (prepared.status) throw new Error('Certificate conversion failed');
      console.log('Certificate prepared. Deploy with node tools/deploy-google.js --public-cert');
    } else console.log('Staging certificate obtained for validation only; it is NOT trusted and was not prepared for deployment.');
  } catch (e) { console.error(e.message); process.exitCode = 1; }
  finally { server.close(); }
});
