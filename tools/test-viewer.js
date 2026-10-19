// Real website playback, pause/resume, authentication and MP4 fallback checks.
// Needs two free WebRTC slots. Does not move the camera or change recording.
'use strict';
const path = require('path'), http = require('http'), assert = require('assert/strict');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..'), camera = require('./config').cameraAddress(process.argv.slice(2));
const {chromium} = require(path.join(root, '.build/browser/node_modules/playwright-core'));
const origin = `http://${camera}:${require('./config').servicePort('PORT')}`;
const sh = spawnSync(process.execPath, [path.join(__dirname, 'camera-shell.js'), '--camera', camera, '-c', 'cat /mnt/sif/owrt/mjpg.conf'], {encoding:'utf8', timeout:20000});
if (sh.status) throw new Error('Cannot read viewer configuration');
const credentials = require('./config').parseConf(sh.stdout).CREDENTIALS;
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
    // Another viewer gets an independent session and receives video too.
    assert.equal(await page.evaluate(async () => {
      const other = new RTCPeerConnection();
      const video = document.createElement('video'); video.muted = true; video.autoplay = true; video.playsInline = true;
      video.style.width = '160px'; document.body.appendChild(video);
      other.ontrack = e => { video.srcObject = new MediaStream([e.track]); video.play().catch(() => {}); };
      let session;
      try {
        other.addTransceiver('video', {direction:'recvonly'});
        await other.setLocalDescription(await other.createOffer());
        const r = await fetch('/viewer/webrtc', {method:'POST', headers:{'Content-Type':'application/json','X-Plug2View-Viewer':'1'}, body:JSON.stringify({action:'offer',sdp:other.localDescription.sdp})});
        if (!r.ok) return r.status;
        const answer = await r.json(); session = answer.session;
        await other.setRemoteDescription({type:'answer',sdp:answer.sdp});
        const deadline = Date.now() + 20000;
        while (Date.now() < deadline) {
          const stats = [...(await other.getStats()).values()].find(s => s.type === 'inbound-rtp' && s.kind === 'video');
          if (stats?.framesDecoded > 20) return 200;
          await new Promise(resolve => setTimeout(resolve, 250));
        }
        throw new Error('Second viewer did not decode video');
      } finally { other.close(); video.remove(); await endSession(session); }
    }), 200);
    const afterSecond = await stats();
    assert.ok(afterSecond.frames > last.frames, 'First viewer must continue while the second joins and leaves');
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
    let failure = {status:503, error:'stream_unavailable'}, offers = 0, fallbackRequests = 0;
    page.on('request', r => { if (/[?&]action=(fmp4|snapshot)\b/.test(r.url())) fallbackRequests++; });
    await page.route('**/viewer/webrtc', route => {
      if (route.request().postDataJSON()?.action !== 'offer') return route.continue();
      offers++;
      if (failure === 'network') return route.abort();
      return route.fulfill({status:failure.status, contentType:'application/json', body:JSON.stringify({error:failure.error})});
    });
    for (const injected of [{status:503,error:'stream_unavailable'}, {status:409,error:'session_replaced'}, {status:400,error:'invalid_offer'}, 'network']) {
      failure = injected; const before = offers;
      await page.locator('#pause').click();
      await page.waitForFunction(() => rtcRetry !== null && !mse.abort && snapTimer === null, null, {timeout:10000});
      const deadline = Date.now() + 12000;
      while (offers < before + 2 && Date.now() < deadline) await page.waitForTimeout(200);
      assert.ok(offers >= before + 2, 'WebRTC must retry failed signaling');
      assert.equal(fallbackRequests, 0, 'Signaling failure must not request MP4 or snapshots');
      await page.locator('#pause').click(); await page.evaluate(() => rtcEnding);
    }
    const pausedOffers = offers; await page.waitForTimeout(4500);
    assert.equal(offers, pausedOffers, 'Pause must cancel scheduled WebRTC retries');
    await page.unroute('**/viewer/webrtc');
    // A stalled connection must reconnect through WebRTC, preserving the policy.
    await page.locator('#pause').click(); await waitVideo();
    await page.evaluate(() => { window.beforeStall = rtc.pc; rtc.pc.close(); rtc.lastFrame = performance.now() - 13000; });
    await page.waitForFunction(() => rtc?.pc && rtc.pc !== window.beforeStall, null, {timeout:20000});
    await waitVideo(); assert.equal(fallbackRequests, 0);
    await page.locator('#pause').click(); await page.evaluate(() => rtcEnding);
    // Unsupported browsers show an explicit message rather than silently changing transport.
    await page.evaluate(() => { window.savedRtc = window.RTCPeerConnection; window.RTCPeerConnection = undefined; start(); });
    assert.equal(await page.evaluate(() => !mse.abort && snapTimer === null && status.textContent.includes('does not support WebRTC')), true);
    await page.evaluate(() => { stop(); window.RTCPeerConnection = window.savedRtc; });
    assert.equal(fallbackRequests, 0);
    console.log('PASS: server errors, invalid offers, network failures and stalled playback retry WebRTC; Pause cancels retries; unsupported browsers do not switch transport');
    await page.route('**/viewer/webrtc', route => route.fulfill({status:409, contentType:'application/json', body:'{"error":"stream_capacity_reached"}'}));
    await page.locator('#pause').click();
    await page.waitForFunction(() => rtc === null && !!mse.abort && vid.currentTime > 1 && !vid.paused, null, {timeout:20000});
    console.log('PASS: recording/live transition and MP4 fallback only for explicit slot exhaustion');
    await page.locator('#pause').click();
    assert.deepEqual(errors, [], 'No uncaught page errors');
  } finally {
    if (context) await context.close(); if (browser) await browser.close();
  }
})().catch(e => {console.error(e.message); process.exitCode = 1;});
