// Offline validation/rendering tests with synthetic credentials only.
'use strict';
const assert=require('assert/strict'),{defaults,normalize,render,parseConf}=require('./configuration');
for(const ddns of [false,true])for(const le of [false,true]) {
  const c=structuredClone(defaults);
  c.ddns={...c.ddns,enabled:ddns,HOSTNAME:'example-camera.duckdns.org',TOKEN:'example-test-token'};
  c.letsEncrypt.enabled=le;
  const files=render(c),policy=parseConf(files['maintenance.conf']);
  assert.equal(policy.DDNS_ENABLED,ddns?'1':'0');assert.equal(policy.LE_ENABLED,le?'1':'0');
  assert.equal(policy.LE_RENEW_DAYS,'30');
}
const c=structuredClone(defaults);
c.server.CREDENTIALS="test:space ' quote $() & ; *";
c.wifi.SSID="Example ' Wi-Fi";c.wifi.PASS="test '$()";
assert.equal(parseConf(render(c)['mjpg.conf']).CREDENTIALS,c.server.CREDENTIALS);
assert.equal(parseConf(render(c)['wifi.conf']).PASS,c.wifi.PASS);
assert.throws(()=>normalize({...c,ddns:{enabled:'false'}}),/ddns.enabled/);
assert.throws(()=>normalize({...c,letsEncrypt:{enabled:true}}),/ddns.HOSTNAME/);
assert.throws(()=>normalize({...c,letsEncrypt:{renewBeforeDays:0}}),/renewBeforeDays/);
assert.throws(()=>normalize({...c,server:{CREDENTIALS:'user:bad\nvalue'}}),/server.CREDENTIALS/);
assert.throws(()=>normalize({...c,server:{PORT:0}}),/server.PORT/);
assert.throws(()=>normalize({...c,server:{TLS_CERT:'/tmp/test.der'}}),/TLS_CERT/);
assert.throws(()=>normalize({...c,unknown:true}),/Unknown config/);
assert.throws(()=>normalize(JSON.parse('{"__proto__":{}}')),/Unknown config/);
assert.throws(()=>normalize(null),/configuration object/);
assert.equal(parseConf("H264_DEVICE='/dev/video2'\n").H264_DEVICE,'/dev/video2');
const google={enabled:true,PROJECT_ID:'example-project',PUBLIC_ORIGIN:'https://camera.example.com',CLIENT_ID:'example-client',CLIENT_SECRET:'a'.repeat(64),LOGIN_USER:'test-owner',LOGIN_PASSWORD:'test-password',TOKEN_KEY:'b'.repeat(64)};
const ready=normalize({...c,google});assert.equal(ready.google.TOKEN_KEY,google.TOKEN_KEY);
assert.equal(parseConf(render(ready)['google.conf']).TOKEN_KEY,google.TOKEN_KEY);
assert.throws(()=>normalize({...ready,google:{...google,TOKEN_KEY:'invalid'}}),/TOKEN_KEY/);
console.log('PASS: independent schedule flags, defaults, secret-safe validation, shell quoting, preserved Google credentials');
