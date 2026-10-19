// Convert a public EC P-256 certificate chain + matching private key for BearSSL.
// This prepares local files only; deployment is separate.
// node tools/prepare-public-cert.js --chain fullchain.pem --key privkey.pem
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const args = process.argv.slice(2);
const opt = n => { const i = args.indexOf(n); return i < 0 ? null : args[i + 1]; };
if (!opt('--chain') || !opt('--key')) { console.error('Pass --chain fullchain.pem --key privkey.pem (EC P-256)'); process.exit(2); }
const pem = fs.readFileSync(opt('--chain'), 'utf8');
const certs = [...pem.matchAll(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g)].map(m => new crypto.X509Certificate(m[0]));
const key = crypto.createPrivateKey(fs.readFileSync(opt('--key')));
if (!certs.length || certs.length > 5 || key.asymmetricKeyType !== 'ec' || key.asymmetricKeyDetails.namedCurve !== 'prime256v1' || !certs[0].checkPrivateKey(key)) throw new Error('Need a matching P-256 private key and a leaf-first chain of 1–5 certificates');
if (certs.some(c => Date.parse(c.validTo) <= Date.now() || Date.parse(c.validFrom) > Date.now())) throw new Error('Certificate outside its validity period');
for (let i = 0; i + 1 < certs.length; i++) if (!certs[i].verify(certs[i + 1].publicKey)) throw new Error('Certificate chain is not leaf-first or has a bad signature');
const dir = path.resolve(__dirname, '../camera/tls/public'); fs.mkdirSync(dir, {recursive: true});
fs.writeFileSync(path.join(dir, 'cert.der'), Buffer.concat(certs.map(c => c.raw)));
fs.writeFileSync(path.join(dir, 'key.der'), key.export({format: 'der', type: 'pkcs8'}), {mode: 0o600});
console.log(`Prepared ${certs.length} certificate(s) in ${dir}; expires ${certs[0].validTo}.\nExisting camera certificate is unchanged. Renew and redeploy before expiry.`);
