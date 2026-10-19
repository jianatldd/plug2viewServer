// Live integration checks. Uses a throwaway Google config deployed with deploy-google.
// node tools/test-google.js --config .build/google-test.conf [--ffmpeg path/to/ffmpeg]
// Exercises OAuth and HLS on the real MIPS camera; never logs credentials or tokens.
'use strict';
const assert = require('assert/strict'), fs = require('fs'), path = require('path');
const http = require('http'), https = require('https'), {spawnSync} = require('child_process');
const args = process.argv.slice(2), camera = require('./config').cameraAddress(args);
const opt = n => { const i = args.indexOf(n); return i < 0 ? null : args[i + 1]; };
if (!opt('--config')) throw new Error('Pass --config with the deployed throwaway config');
const cfg = Object.fromEntries(fs.readFileSync(opt('--config'), 'utf8').split(/\r?\n/).filter(l => l && !l.startsWith('#')).map(l => { const i = l.indexOf('='); return [l.slice(0, i), l.slice(i + 1)]; }));
const shell = spawnSync(process.execPath, [path.join(__dirname, 'camera-shell.js'), '--camera', camera, '--timeout', '10000', '-c', 'cat /mnt/sif/owrt/mjpg.conf'], {encoding: 'utf8', timeout: 15000});
if (shell.status) throw new Error('Cannot read camera test credentials');
const credentials = require('./config').parseConf(shell.stdout.replace(/\r/g, '')).CREDENTIALS;
if (!credentials) throw new Error('Tests require camera Basic authentication to be enabled');
const basic = 'Basic ' + Buffer.from(credentials).toString('base64');
function request(route, {method = 'GET', data, headers = {}, tls = true} = {}) {
  const body = data === undefined ? undefined : typeof data === 'string' ? data : JSON.stringify(data);
  if (body !== undefined) { headers['content-length'] = Buffer.byteLength(body); headers['content-type'] ||= 'application/json'; }
  return new Promise((resolve, reject) => {
    const q = (tls ? https : http).request({host: camera, port: require('./config').servicePort(tls ? 'TLS_PORT' : 'PORT'), path: route, method, headers, rejectUnauthorized: false, agent: false}, res => {
      const chunks = []; res.on('data', c => chunks.push(c)); res.on('end', () => resolve({status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks), text: Buffer.concat(chunks).toString()}));
    });
    q.setTimeout(25000, () => q.destroy(new Error('Camera request timeout'))); q.on('error', reject); q.end(body);
  });
}
const form = obj => new URLSearchParams(obj).toString();
const postForm = (url, obj) => request(url, {method: 'POST', data: form(obj), headers: {'content-type': 'application/x-www-form-urlencoded'}});
const redirect = `https://oauth-redirect.googleusercontent.com/r/${cfg.PROJECT_ID}`;
const oauth = {client_id: cfg.CLIENT_ID, client_secret: cfg.CLIENT_SECRET};
let access, refresh, stream;
async function intent(name, payload) {
  return request('/google/fulfillment', {method: 'POST', headers: {authorization: 'Bearer ' + access}, data: {requestId: 'test-"quoted', inputs: [{intent: 'action.devices.' + name, payload}]}});
}
async function main() {
  assert.equal((await request('/?action=status')).status, 401);
  assert.equal((await request('/?action=status', {headers: {authorization: basic + 'junk'}})).status, 401);
  assert.equal((await request('/?action=status', {headers: {authorization: basic}})).status, 200);
  assert.equal((await request('/google/auth', {tls: false})).status, 403);
  assert.equal((await request('/google/auth?' + form({client_id: cfg.CLIENT_ID, redirect_uri: 'https://attacker.invalid/', state: 'x', response_type: 'code'}))).status, 400);
  let r = await request('/google/auth?' + form({client_id: cfg.CLIENT_ID, redirect_uri: redirect, state: 'space + & quote"', response_type: 'code'}));
  assert.equal(r.status, 200); const nonce = r.text.match(/name=nonce value='([^']+)'/)[1];
  assert.equal((await postForm('/google/auth', {nonce, username: cfg.LOGIN_USER, password: 'wrong', consent: 'yes'})).status, 401);
  r = await postForm('/google/auth', {nonce, username: cfg.LOGIN_USER, password: cfg.LOGIN_PASSWORD, consent: 'yes'});
  assert.equal(r.status, 302); const location = new URL(r.headers.location);
  assert.equal(location.searchParams.get('state'), 'space + & quote"');
  const grant = {...oauth, grant_type: 'authorization_code', code: location.searchParams.get('code'), redirect_uri: redirect};
  r = await postForm('/google/token', {...grant, redirect_uri: redirect + '-wrong'}); assert.equal(r.status, 400);
  r = await postForm('/google/token', grant); assert.equal(r.status, 200);
  ({access_token: access, refresh_token: refresh} = JSON.parse(r.text));
  assert.equal((await postForm('/google/token', grant)).status, 400);
  r = await postForm('/google/token', {...oauth, grant_type: 'refresh_token', refresh_token: refresh}); assert.equal(r.status, 200); access = JSON.parse(r.text).access_token;
  console.log('PASS: HTTPS-only OAuth, redirect binding, login, state encoding, one-use codes and refresh');
  r = await intent('SYNC'); assert.equal(r.status, 200); assert.equal(JSON.parse(r.text).requestId, 'test-"quoted');
  assert.deepEqual(JSON.parse(r.text).payload.devices[0].attributes.cameraStreamSupportedProtocols, ['hls']);
  r = await intent('QUERY', {devices: [{id: 'plug2view'}]}); assert.equal(JSON.parse(r.text).payload.devices.plug2view.online, true);
  const command = protocols => ({commands: [{devices: [{id: 'plug2view'}], execution: [{command: 'action.devices.commands.GetCameraStream', params: {StreamToChromecast: false, SupportedStreamProtocols: protocols}}]}]});
  r = await intent('EXECUTE', command(['webrtc'])); assert.equal(JSON.parse(r.text).payload.commands[0].status, 'ERROR');
  r = await intent('EXECUTE', command(['hls', 'webrtc'])); assert.equal(JSON.parse(r.text).payload.commands[0].status, 'SUCCESS');
  stream = new URL(JSON.parse(r.text).payload.commands[0].states.cameraStreamAccessUrl);
  assert.equal((await request('/google/fulfillment', {method: 'POST', headers: {authorization: 'Bearer ' + access}, data: '{"inputs":['})).status, 400);
  assert.equal((await request('/google/fulfillment', {method: 'POST', headers: {authorization: 'Bearer ' + access}, data: '{}\0trailing'})).status, 400);
  assert.equal((await request('/google/fulfillment', {method: 'POST', headers: {authorization: 'Bearer ' + access}, data: '{"requestId":"a","inputs":[],"inputs":[]}'})).status, 400);
  console.log('PASS: SYNC, QUERY, protocol negotiation and malformed JSON rejection');
  assert.equal((await request('/hls/live.m3u8')).status, 401);
  assert.equal((await request('/hls/live.m3u8?token=' + encodeURIComponent(access))).status, 401);
  const expired = stream.searchParams.get('token').replace(/^s\.\d+\./, 's.1.');
  assert.equal((await request('/hls/live.m3u8?token=' + expired)).status, 401);
  assert.equal((await request('/?action=status?' + stream.search.slice(1))).status, 401);
  r = await request(stream.pathname + stream.search); assert.equal(r.status, 200);
  assert.match(r.headers['content-type'], /mpegurl/);
  const playlist = r.text, uris = playlist.split('\n').filter(l => l && !l.startsWith('#'));
  const durations = [...playlist.matchAll(/#EXTINF:([\d.]+)/g)].map(m => Number(m[1]));
  const target = Number(playlist.match(/#EXT-X-TARGETDURATION:(\d+)/)[1]);
  assert.ok(durations.every(d => Math.round(d) <= target)); assert.ok(durations.reduce((a, b) => a + b, 0) >= 3 * target);
  assert.equal([...playlist.matchAll(/#EXT-X-PROGRAM-DATE-TIME:/g)].length, uris.length);
  const init = playlist.match(/#EXT-X-MAP:URI="([^"]+)"/)[1];
  const chunks = [];
  for (const name of [init, ...uris]) {
    assert.ok(name.includes('?token=')); const part = await request('/hls/' + name); assert.equal(part.status, 200); chunks.push(part.body);
  }
  const video = Buffer.concat(chunks); fs.mkdirSync(path.resolve(__dirname, '../.build'), {recursive: true});
  const output = path.resolve(__dirname, '../.build/google-hls-test.mp4'); fs.writeFileSync(output, video);
  let pos = 0, fragments = 0;
  while (pos < video.length) { const size = video.readUInt32BE(pos); assert.ok(size >= 8 && pos + size <= video.length); if (video.toString('ascii', pos + 4, pos + 8) === 'moof') fragments++; pos += size; }
  assert.ok(fragments > 100);
  console.log(`PASS: signed HTTPS HLS, ${uris.length} retained segments, ${fragments} MP4 fragments, ${video.length} bytes`);
  if (opt('--ffmpeg')) {
    const decode = spawnSync(opt('--ffmpeg'), ['-v', 'error', '-xerror', '-i', output, '-f', 'null', '-'], {encoding: 'utf8', timeout: 60000});
    assert.equal(decode.status, 0, decode.stderr); console.log('PASS: FFmpeg decoded the camera HLS video without errors');
  }
  r = await intent('DISCONNECT'); assert.equal(r.status, 200);
  assert.equal((await intent('SYNC')).status, 401);
  assert.equal((await request(stream.pathname + stream.search)).status, 401);
  assert.equal((await postForm('/google/token', {...oauth, grant_type: 'refresh_token', refresh_token: refresh})).status, 400);
  console.log('PASS: unlink revokes access, refresh and stream capabilities');
}
main().catch(e => { console.error(e.stack); process.exitCode = 1; });
