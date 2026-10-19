// Real website playback, pause/resume, authentication and MP4 fallback checks.
// Close other WebRTC viewers first. Does not move the camera or change recording.
'use strict';
const path = require('path'), http = require('http'), assert = require('assert/strict');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..'), camera = require('./config').cameraAddress(process.argv.slice(2));
const {chromium} = require(path.join(root, '.build/browser/node_modules/playwright-core'));
const origin = `http://${camera}:8080`;
const sh = spawnSync(process.execPath, [path.join(__dirname, 'camera-shell.js'), '--camera', camera, '-c', 'cat /mnt/sif/owrt/mjpg.conf'], {encoding:'utf8', timeout:20000});
if (sh.status) throw new Error('Cannot read viewer configuration');
const match = sh.stdout.replace(/\r/g, '').match(/^CREDENTIALS=(?:"([^"\n]*)"|'([^'\n]*)'|([^\s#\n]*))$/m);
const credentials = match && (match[1] || match[2] || match[3]);
if (!credentials || !credentials.includes(':')) throw new Error('Viewer credentials required');
function request(headers, data = '{"action":"end","session":"wrong"}', method = 'POST') {
  return new Promise((resolve, reject) => {
    const req = http.request(origin + '/viewer/webrtc', {method, headers:{...headers, 'Content-Length':Buffer.byteLength(data)}}, res => {
      res.resume(); res.on('end', () => resolve({status:res.statusCode, headers:res.headers}));
    });
    req.setTimeout(10000, () => req.destroy(new Error('HTTP timeout'))); req.on('error', reject); req.end(data);
  });
}
(async () => {
  const basic = {Authorization:'Basic ' + Buffer.from(credentials).toString('base64')};
  assert.equal((await request({'X-Plug2View-Viewer':'1'})).status, 401);
  assert.equal((await request(basic)).status, 403);
  assert.equal((await request(basic, '', 'OPTIONS')).status, 405);
  const stale = await request({...basic, 'X-Plug2View-Viewer':'1'});
  assert.equal(stale.status, 409); assert.equal(stale.headers['access-control-allow-origin'], undefined);
  console.log('PASS: viewer login, cross-origin preflight rejection, and stale-session protection');
  let browser, context;
  try {
    browser = await chromium.connectOverCDP('http://127.0.0.1:9223');
    const colon = credentials.indexOf(':');
    context = await browser.newContext({httpCredentials:{username:credentials.slice(0, colon), password:credentials.slice(colon + 1)}});
    const page = await context.newPage(), errors = [];
    page.on('pageerror', e => errors.push(e.message));
    page.on('console', m => { if (m.type() === 'error') console.log(m.text()); });
    page.on('requestfailed', r => console.log('Request failed: ' + new URL(r.url()).pathname + ' ' + r.failure()?.errorText));
    page.on('response', r => { if (r.status() >= 400) console.log('HTTP ' + r.status() + ' ' + new URL(r.url()).pathname); });
    // Establish the browser's Basic-auth cache before opening the player.
    await page.goto(origin + '/?action=status');
    await page.goto(origin + '/');
    await page.bringToFront();
    const waitVideo = async () => {
      try { await page.waitForFunction(() => rtc?.pc.connectionState === 'connected' && vid.videoWidth > 0 && vid.getVideoPlaybackQuality().totalVideoFrames > 30, null, {timeout:20000}); }
      catch (e) {
        console.log(await page.evaluate(() => ({rtc:rtc?.pc.connectionState, ice:rtc?.pc.iceConnectionState, mse:!!mse.abort, status:status.textContent, width:vid.videoWidth, frames:vid.getVideoPlaybackQuality().totalVideoFrames, hidden:document.hidden})));
        console.log('Page errors: ' + JSON.stringify(errors)); throw e;
      }
    };
    await waitVideo();
    const stats = () => page.evaluate(async () => {
      const s = [...(await rtc.pc.getStats()).values()].find(s => s.type === 'inbound-rtp' && s.kind === 'video');
      return {frames:s.framesDecoded, lost:s.packetsLost, jitterBufferMs:s.jitterBufferEmittedCount ? s.jitterBufferDelay * 1000 / s.jitterBufferEmittedCount : null};
    });
    const first = await stats(); await page.waitForTimeout(10000); const last = await stats();
    assert.ok(last.frames - first.frames > 80, 'Live video must keep decoding');
    console.log('PASS: website WebRTC playback ' + JSON.stringify(last));
    // Another viewer must not replace the established peer.
    assert.equal(await page.evaluate(async () => {
      const other = new RTCPeerConnection();
      try {
        other.addTransceiver('video', {direction:'recvonly'});
        await other.setLocalDescription(await other.createOffer());
        const r = await fetch('/viewer/webrtc', {method:'POST', headers:{'Content-Type':'application/json','X-Plug2View-Viewer':'1'}, body:JSON.stringify({action:'offer',sdp:other.localDescription.sdp})});
        return r.status;
      } finally { other.close(); }
    }), 409);
    await page.locator('#pause').click();
    await page.evaluate(() => rtcEnding);
    assert.equal(await page.evaluate(() => rtc === null && vid.srcObject === null), true);
    await page.locator('#pause').click(); await waitVideo();
    console.log('PASS: second viewer preserves active stream; Pause releases it and Resume reconnects');
    // Recording playback detaches the RTC source, then Back to live reconnects.
    await page.evaluate(() => playMp4('/test-missing-recording.mp4', 'test'));
    await page.evaluate(() => rtcEnding);
    assert.equal(await page.evaluate(() => !player.hidden && rtc === null && vid.srcObject === null), true);
    await page.locator('#plClose').click(); await waitVideo();
    await page.locator('#pause').click(); await page.evaluate(() => rtcEnding);
    await page.route('**/viewer/webrtc', route => route.fulfill({status:503, contentType:'application/json', body:'{"error":"test_unavailable"}'}));
    await page.locator('#pause').click();
    await page.waitForFunction(() => rtc === null && !!mse.abort && vid.currentTime > 1 && !vid.paused, null, {timeout:20000});
    console.log('PASS: recording/live transition and automatic MP4 fallback');
    await page.locator('#pause').click();
    assert.deepEqual(errors, [], 'No uncaught page errors');
  } finally {
    if (context) await context.close(); if (browser) await browser.close();
  }
})().catch(e => {console.error(e.message); process.exitCode = 1;});
