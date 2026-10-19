// Checks OAuth request sizes and state round-tripping without exchanging codes,
// linking/unlinking Google, or revoking existing access/refresh tokens.
// node tools/test-google-auth.js --config camera/google.conf
'use strict';
const assert = require('assert/strict'), fs = require('fs'), https = require('https');
const args = process.argv.slice(2), configIndex = args.indexOf('--config');
const cfg = require('./config').googleConfig(configIndex >= 0 ? args[configIndex + 1] : undefined);
const camera = require('./config').cameraAddress(args);
const origin = new URL(cfg.PUBLIC_ORIGIN);
const redirect = 'https://oauth-redirect.googleusercontent.com/r/' + cfg.PROJECT_ID;
const form = data => new URLSearchParams(data).toString();
const auth = state => ({client_id: cfg.CLIENT_ID, redirect_uri: redirect, response_type: 'code', state});
function request(query, data, extraHeaders = {}) {
  const body = data === undefined ? undefined : form(data);
  return new Promise((resolve, reject) => {
    const headers = {host: origin.host, ...extraHeaders};
    if (body !== undefined) Object.assign(headers, {'content-type': 'application/x-www-form-urlencoded', 'content-length': Buffer.byteLength(body)});
    const q = https.request({host: camera, port:require('./config').servicePort('TLS_PORT'), servername: origin.hostname,
      path: '/google/auth' + (query ? '?' + query : ''), method: body === undefined ? 'GET' : 'POST', headers, agent: false}, res => {
      const chunks = []; res.on('data', c => chunks.push(c));
      res.on('end', () => resolve({status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString()}));
    });
    q.setTimeout(15000, () => q.destroy(new Error('Camera request timed out')));
    q.on('error', reject); q.end(body);
  });
}
(async () => {
  for (const size of [511, 512, 1024, 2048]) {
    const r = await request(form(auth('s'.repeat(size))));
    assert.equal(r.status, 200, 'Authorization must accept state bytes=' + size);
  }
  assert.equal((await request(form(auth('s'.repeat(2048))), undefined, {'x-test-padding': 'h'.repeat(2500)})).status, 200);
  console.log('PASS: 511/512/1024/2048-byte state and large mobile-sized headers');
  const invalid = [
    form(auth('s'.repeat(2049))),
    form({...auth('state'), client_id: 'wrong-client'}),
    form({...auth('state'), redirect_uri: redirect + '.attacker.invalid'}),
    form({...auth('state'), response_type: 'token'}),
    form(auth('state')) + '&state=duplicate',
    form(auth('state')).replace('state=state', 'state=%00'),
    form(auth('state')).replace('state=state', 'state=%ZZ'),
    form(auth('state')).replace('&state=state', ''),
  ];
  for (const query of invalid) assert.equal((await request(query)).status, 400);
  console.log('PASS: oversized/malformed/duplicate/missing state, wrong client and redirect rejected');
  // Worst-case encoding: every byte expands to three bytes in the Location header.
  const state = '+&"/='.repeat(410).slice(0, 2048);
  let r = await request(form(auth(state)));
  assert.equal(r.status, 200);
  const csp = r.headers['content-security-policy'];
  assert.ok(csp.includes("form-action 'self' " + redirect + ' https://oauth-redirect-sandbox.googleusercontent.com/r/' + cfg.PROJECT_ID + ';'));
  const nonce = r.body.match(/name=nonce value='([^']+)'/)[1];
  r = await request('', {nonce, username: cfg.LOGIN_USER, password: cfg.LOGIN_PASSWORD, consent: 'yes'});
  assert.equal(r.status, 302);
  const location = new URL(r.headers.location);
  assert.equal(location.origin + location.pathname, redirect);
  assert.equal(location.searchParams.get('state'), state);
  assert.ok(location.searchParams.get('code'));
  assert.equal(Number(r.headers['content-length']), Buffer.byteLength(r.body));
  console.log('PASS: 2048-byte state round-trips intact through a 6 KB redirect; no tokens exchanged');
})().catch(e => { console.error(e.message); process.exitCode = 1; });
