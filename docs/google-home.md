# Google Home camera experiment

For the proposed head/child architecture, registration, and direct UDP streaming,
see the [multi-camera deployment plan](multi-camera-plan.md). The setup below
describes the current single-camera implementation.

Hostnames and network addresses in this public guide are placeholders. Use the
values in your ignored local configs when running setup commands; keep those
values and credentials out of commits. See the README's publishing check.

The camera can now serve HLS/WebRTC and implement Google's OAuth and CameraStream HTTP
endpoints inside `camd`. Video, account linking, token validation and fulfillment
run on the camera. There is no PC or media relay in the playback path.

**Experimental: Google Home Android, Google Hub and Google TV WebRTC playback have been confirmed.**
Google Home app playback was confirmed over mobile data with an estimated
delay under one second. Adding a negotiated silent PCMU audio track resolved
the app's loading screen in this test.
Google's real request selected H264 payload 100 and PCMU payload 0, completed
authenticated ICE/DTLS/SRTP, and received 245 video frames plus 965 silent audio
packets by the 20-second diagnostic, with zero reported loss or send errors.
Latency is a visual estimate, not an instrumented measurement; extended
session stability and broader device compatibility remain unverified. Earlier experiments
and their results follow.

During testing, Google TV repeatedly cancelled camera-originated offers before
sending an SDP answer or any ICE packets, while phone and browser streams worked
with free session slots. Cast fulfillment now omits `cameraStreamOffer`, so Google
generates the offer for TVs and displays as it already does for phones. This is a
[documented negotiation mode](https://developers.home.google.com/cloud-to-cloud/traits/camerastream#webrtc-results)
and lets the receiver propose its media layout and codecs, including PCMU audio.
The cancellation logs do not establish which part of the previous offer the TV
rejected. Successful Google TV playback was confirmed after this change;
the earlier Hub result below used the previous camera-offer mode. Signaling logs
now include a session handle and timestamp, without tokens or SDP.
After installation, concurrent Chrome tests using Cast and phone fulfillment
decoded 405 and 396 frames over about 30 seconds, respectively, with zero reported
video or audio packet loss. Both negotiated H264 and silent PCMU and passed
retry, conflicting-description and stale-token checks. The isolated signaling
regression also passed on the camera, including ten-slot capacity and cleanup.
A subsequent real Cast request completed receiver-offer negotiation, authenticated
ICE/DTLS/SRTP, and sent 451 video frames by the 40-second diagnostic with zero
reported loss or send errors. Visible TV playback was then confirmed.
The Cast fingerprint
mismatch test also passed with zero received audio or video.

Android HLS playback was established first. The camera was successfully linked
and added in Google Home on Android. The camera received real `SYNC` requests
followed by `GetCameraStream` offering HLS (`hls=1 webrtc=0 dash=0`), and started
its HLS stream. Visible video was confirmed, initially a minute or more behind
with intermittent freezes. After playlist tuning, the observed delay was about
18–20 seconds. Diagnostics
caught a 1.4-second capture gap resetting the HLS stream; recovery now preserves
completed segments and signals a discontinuity if an unfinished segment must be
dropped. After the latest changes, the estimated delay was 4–5 seconds, which
still does not meet the intended live-monitoring use case.
This is a visual estimate, not an instrumented end-to-end latency measurement.
Motion detection was confirmed disabled during investigation. Local tests also
exercise Google's documented requests. Other clients may request different
protocols; their offered protocols are logged without credentials. This is not
Matter, and Google's cloud still coordinates account linking and camera requests.

The initial experimental WebRTC sender was video-only. Google's
[CameraStream documentation](https://developers.home.google.com/cloud-to-cloud/traits/camerastream#webrtc-protocol-specifications)
currently names Nest Smart Displays and Chromecast with Google TV as supported
WebRTC destinations; it does not confirm Google Home phone playback. A Google Hub
display was tested first. Earlier `webrtc=0` requests were received
while the firmware advertised only HLS, so they do not establish phone support.
Chrome on the LAN successfully received 440 decoded frames in a 30-second test,
at about 15 fps with no reported packet loss. Its average jitter buffer was about
28 ms; this is **not** an end-to-end latency measurement.

The Hub initially requested HLS/DASH and showed the same
4–5 second delay. After a device sync, real Google requests switched to
`hls=0 webrtc=1 dash=0`. Two sessions accepted Google's answer, nominated an ICE
pair, and completed authenticated DTLS/SRTP; the first ended through signaling
after sending 19 frames, and the next connected successfully. Playback appeared
faster, with an estimated 1–2 seconds of delay when waving in front of the camera,
compared with 4–5 seconds using HLS. This is a visual estimate, not an
instrumented latency measurement. Longer-session
stability remains to be tested. This verifies Google Hub WebRTC interoperability;
the log does
not identify whether the selected ICE path used the LAN or the public forward.

The first Android attempt after enabling WebRTC left Google Home open with a
blank/loading view, both on home Wi-Fi and mobile data. The camera process was
alive and capturing about 13 fps. Public-hostname HTTPS from inside the LAN timed
out while direct LAN HTTPS passed certificate validation, but the cellular
failure means that routing issue alone does not explain the Android failure.
Phone WebRTC playback is not yet verified. Diagnostics now expose WebRTC frame,
packet, RTCP, loss, PLI/NACK, retransmission, queue and consent counters in the
authenticated `/?action=status` response and log them every ten seconds while
connected. Fulfillment logs also include the Cast flag and request time, without
tokens or SDP. The follow-up mobile-data test showed "Live view not available."
Its real Google requests offered WebRTC with `StreamToChromecast=false`. An
answer was accepted, but another fulfillment request one second later replaced
the pending session; subsequent signaling failed with `invalid_stream_token`.
No video packets were sent in that attempt.

The retry fix preserves an active offer and its bearer token during the first
30 seconds, without extending the setup deadline. An exact retry of the most
recent request also preserves an established session. Identical SDP answers are
idempotent; conflicting answers cannot replace the peer. This remains a single
viewer implementation: close the current viewer before switching devices.
Explicit end, timeout or a new request after the setup window permits fresh
session credentials. Signaling diagnostics now identify action and authorization
result; session-end logs count incoming UDP and authenticated ICE checks without
logging tokens or SDP. On the next mobile-data attempt the camera accepted
Google's answer, completed ICE and authenticated DTLS/SRTP, and sent 353 frames
in 30 seconds. Authenticated receiver reports showed zero packet loss, no NACKs
or PLIs, and about 12 ms jitter. The app still displayed a loading icon. These
counters establish a working transport to the Google peer, not successful
decoding or display on the phone.

The next compatibility correction makes the answer acknowledgment exactly `{}`,
as documented in Google's signaling examples. Previously it returned an
`action: answer` object without an SDP, although that response form belongs to
the opposite negotiation direction. On retest, the app still
showed a loading icon; the acknowledgment mismatch did not resolve playback.
The correction was deployed and its regression test verified empty acknowledgments
for both initial and repeated answers. Chrome decoded 388 frames over 30 seconds
with zero reported loss; overlapping fulfillment and retired-token checks passed.

The Google Home web Cameras page displayed "Only Google Nest cameras are
supported on home.google.com." This explains its empty camera view; it is not
evidence of Plug2View being classified as a legacy Nest device. The Developer
Console was verified to use the generic Camera type, the camera's own OAuth
URLs, and no App Flip. After removing the remaining Google Account connection
and relinking, the Nest prompt no longer appeared, but phone playback still loaded
indefinitely. The web page restriction and phone playback failure are separate.

The next experiment uses Google-originated offers for `StreamToChromecast=false`,
following the negotiation direction used in
[Scrypted's Google Home integration](https://github.com/koush/scrypted/blob/main/plugins/google-home/src/commands/camerastream.ts).
The camera answers with one H264 send-only track, preserves the offered MID and
payload number, and rejects audio and data channels in their original positions.
The Hub retains camera-originated offers. This is a compatibility experiment,
not confirmation that the phone supports our stream.
The first local receiver-offer test exposed a separate HTTPS bug: an 8,759-byte
signaling POST could leave decrypted bytes buffered in BearSSL after the HTTP
header buffer was consumed. The event loop waited for another socket event and
the request timed out. The loop now drains buffered TLS input before polling;
the playback test includes a 12 KB invalid SDP request that must promptly return
HTTP 400, as well as the real multi-media offer.
After this fix, Chrome's receiver-offer test negotiated H264 payload 108 and
decoded 372 frames over 30 seconds with zero reported loss. Audio and data media
were rejected while preserving their positions. A mismatched-certificate test
failed DTLS with zero decoded frames, as required. The real Android retest still
showed a loading icon. Google sent a two-media offer, the camera selected video
MID 1 / H264 payload 100, and ICE plus authenticated DTLS/SRTP completed. At
20 seconds the camera had sent 257 frames; receiver reports indicated zero loss
and about 11 ms jitter. This verifies transport, not decoding or phone display.
The existing camera-offer flow also passed after the change: Chrome decoded
384 frames over 30 seconds with zero reported loss. Both flows passed duplicate
signaling, overlapping fulfillment, and retired-token checks. Motion stayed off.

For a direct Chrome check on a phone, run
`node tools/prepare-webrtc-phone.js`. It installs `webrtc-check.html` behind the
existing camera viewer Basic authentication and verifies both the unauthenticated
401 response and the authenticated page contents. It embeds only a device-scoped
stream token, valid for at most an hour; OAuth credentials are not installed in
the page. Close other viewers before preparing or running it. The page runs for
30 seconds, displays decoded frames and packet counts, and ends the stream.
Prepare it again before another run. Open the printed public HTTPS URL on mobile
data if the router's public HTTPS address cannot be reached from the LAN.
This diagnostic uses the same receiver-offer stream as the phone
integration. It can distinguish browser playback from the Google Home failure;
it does not reproduce Google's receiver implementation.
The installed diagnostic page passed desktop Chrome validation: 383 decoded
1280x720 frames in 30 seconds, 5,858 received packets, zero reported loss, and
automatic stream shutdown. Its token was refreshed for a mobile browser test.
Chrome over mobile data then played moving video in real time with no noticeable
delay. The camera logged 383 sent frames and zero reported loss. No browser
decoded-frame count was recorded for this test. This verifies direct phone
browser playback, not Google Home playback.

The next compatibility experiment accepts a bundled PCMU/8000 audio track when
offered by the receiver and sends generated silence at 20 ms intervals. This
tests one difference from
[Scrypted's audio/video negotiation](https://github.com/koush/scrypted/blob/main/plugins/webrtc/src/ffmpeg-to-wrtc.ts),
which creates both audio and video transceivers. Missing audio was the hypothesis
behind this experiment. Unsupported audio
is still rejected, and the camera-offer Hub flow remains video-only. Audio uses
a separate SSRC/sequence space and authenticated SRTP on the same ICE/DTLS
transport, with its own RTCP sender reports. There is no microphone capture.
The camera viewer login protects the diagnostic HTML page; `/google/`
requests are routed to their own token validation before Basic authentication.
Tests assert that signaling responses never issue a viewer-login challenge.

The normal camera website now also prefers WebRTC. It uses
`POST /viewer/webrtc` behind the existing viewer Basic authentication, with a
required custom header and no cross-origin preflight permission. It does not
embed Google credentials or require a prepared stream token. The browser sends
an audio/video offer and receives an answer and a session-specific cleanup token.
Pause, page exit, and opening a recording release that session. Cleanup from an
older browser session cannot stop a newer Google or browser session.

Up to 10 WebRTC peers can connect simultaneously, shared between the website,
Google Home phones, and Cast receivers. Each has an independent signaling token,
ICE/DTLS/SRTP state, and retransmission cache. Joining, ending, timing out, or
requesting a keyframe affects only that peer. The website falls back to MP4/MSE
only when the server returns HTTP 409 with `stream_capacity_reached`. Signaling
errors, network failures, and playback stalls reconnect through WebRTC, with a
2–10 second retry delay. Pause, page hiding, and recording playback cancel pending
retries. Browsers without WebRTC display an unsupported-browser message.
Allocation failures with free slots return HTTP 503 rather than pretending the
camera is full, so they do not trigger MP4. Resume retries WebRTC after a capacity
fallback. Google still advertises WebRTC and HLS and prefers WebRTC; it does not
switch an established WebRTC session to HLS when playback fails.
The installed viewer policy passed `tools/test-viewer.js` with injected server,
network, and offer errors plus a playback stall, without requesting MP4 or
snapshots. `tools/test-viewer-capacity.js` reserved all ten real slots, verified
MP4 playback without evicting them, then released them and verified WebRTC on
Resume. The latter test requires ten free slots and releases only its own
reservations.

This is one-to-many delivery through separate WebRTC connections, rather than
IP multicast: the hardware encoder and eight-frame capture queue are shared,
but encryption and outgoing bandwidth scale with the number of viewers.
Retransmission caches are allocated on demand (about 159 KiB per occupied slot).
Ten viewers are permitted. A two-minute LAN test kept all ten
Chrome connections alive and decoded 6.35–6.36 fps per viewer, with zero reported
packet loss or sender errors. Capture ran at about 13.3 fps / 1.64 Mbit/s, but the
shared queue overflowed 48 times during the measured interval. CPU averaged
78% (sampled peak 90%); free RAM remained above 2.2 MB. These results show ten
working connections with reduced, uneven frame delivery, not full-rate playback.
Six viewers sustained 13.29–13.32 fps each in a subsequent one-minute test, with
zero reported packet loss, sender errors, or queue overflows during the measured
interval. Six is the verified smooth-playback count for these conditions; the
configured limit remains ten. Both tests exercised independent session cleanup
and slot reuse in follow-up checks.
The final ten-slot regression also rejected an eleventh offer without evicting
any viewer. Website playback/Pause/Resume/MP4 fallback and both Google signaling
modes passed concurrently afterward, using Chrome to emulate the Google peers.
The test used ten peer connections in one Chrome instance on one wired LAN PC,
including generated PCMU audio; it does not establish WAN or ten-device results.
The deployed build optimizes SRTP for speed (`-O2`) and uses an eight-frame shared
queue. The initial size-optimized/four-frame build delivered only 3.36 fps per
viewer in the same two-minute test. No additional router ports are needed: all peers
share UDP 50000, with STUN routed by authenticated ICE credentials and DTLS/RTCP
by the nominated peer address and port.

`node tools/test-viewer.js` passed against the deployed page: 164 decoded frames,
zero reported packet loss, approximately 21 ms average receiver jitter buffering,
Pause/Resume, recording/live transitions, and forced MP4 fallback. Buffering is
not an end-to-end latency measurement. A separate visual check confirmed that
the website plays in real time on a phone. `node tools/deploy-viewer.js` updates
the page without restarting the camera and retains `index.html.previous`.

Validation of this build passed: Chrome decoded 410 video frames and 247,360
audio samples with zero audio energy and zero reported loss over the roughly
30-second test. A receiver offering only Opus audio retained video-only playback
(406 decoded frames), and the existing Hub-style camera-offer path decoded 364
frames. Both video-only runs reported zero loss. A mismatched peer certificate
failed DTLS and received neither audio nor video. SDP bounds/negotiation tests
and the MIPS warning check passed; motion remained disabled. The subsequent
real Google Home test negotiated PCMU payload 0 and displayed moving video.
The visually estimated delay was under one second. This supports the audio-track
compatibility explanation for this integration; it does not establish that all
Google Home clients universally require audio.

## What is implemented

- Optional `--webrtc` build: up to 10 concurrent H.264 WebRTC sessions, with
  WebRTC preferred over HLS when Google requests both. Uses ICE-lite with fixed
  IPv4 candidates, DTLS 1.2 / ECDSA P-256, and AES-128-CM/HMAC-SHA1-80 SRTP.
  Cast receivers receive a complete camera offer; phone receivers instead post
  their offer and receive the camera's answer. Both use `/google/webrtc` with a
  random, session-specific bearer token. The DTLS peer certificate must match
  the remote SDP's SHA-256 fingerprint. Incoming SDP is bounded to 14 KB and
  Google POST bodies to 16 KB.
- WebRTC uses the existing hardware encoder and capture timestamps. A worker
  encrypts and sends RTP; a bounded eight-frame queue drops back to the next IDR
  if it falls behind. RTCP sender reports, PLI handling at the next periodic
  IDR, and a bounded packet cache for NACK retransmission are included.
  Sessions stop after 30 seconds without ICE consent or one hour total.
  Receiver offers can additionally negotiate generated PCMU silence. No TURN,
  TCP candidates, IPv6, captured audio or data channels are implemented.
- `GET /hls/live.m3u8`: on-demand H.264/fMP4 HLS. Uses the existing encoder, frame
  ring and MP4 muxer. No transcoding or second V4L2 stream.
- HLS uses a mounted FAT32 SD card with at least 32 MB available. It retains 16
  completed segments in `<SAVE_DIR>/.hls`, exposes at least 9 seconds in the
  playlist (3-second target duration) and stops writing after 90 seconds without requests. Startup reuses
  pre-roll; the initial playlist request can wait several seconds while warming.
  Segments close at each keyframe. Capture timestamps and a start hint nine seconds
  behind the playlist end help players locate recent video; the player controls its
  additional buffering. Expect more latency than the existing MSE viewer. No audio is added.
- `/google/auth`, `/google/token`, `/google/fulfillment`: single-account OAuth
  authorization-code flow, refresh, SYNC, QUERY, EXECUTE and DISCONNECT.
- Explicit login/consent, exact Google redirect allowlist, one-use codes, random
  secrets, HMAC stream capabilities, separate access/refresh/stream token scopes,
  one-hour access and stream expiry, persistent revocation state. Unlinking or
  linking again revokes previous tokens. One linking flow at a time is supported.
- Google routes require TLS (loopback is allowed for diagnostics). Stream URLs
  bypass Basic auth only for `/hls/` with a valid expiring stream capability.
  Normal viewer/control authentication remains in place.
- BearSSL accepts a leaf-first chain of up to five concatenated DER certificates,
  enabling public CA certificates. The private key must be EC P-256.

## Public endpoint and certificate

### DuckDNS and on-camera renewal

Use your own DuckDNS name in place of `YOUR_SUBDOMAIN.duckdns.org`. The maintenance tools use
DuckDNS HTTPS updates and Let's Encrypt DNS-01 validation. Port 80 is not needed;
keep TCP 443 forwarded to camera TCP 8443 and UDP 50000 to camera UDP 50000.
See the [DuckDNS API](https://www.duckdns.org/spec.jsp) and
[Let's Encrypt DNS-01 documentation](https://letsencrypt.org/docs/challenge-types/#dns-01-challenge).

Set `ddns.HOSTNAME`, `ddns.TOKEN`, `ddns.enabled`, and `letsEncrypt.enabled`
in private `config.local.json`. See the [central configuration reference](../README.md#updating-google-home-and-duckdns-settings)
for importing existing settings and independently disabling either schedule.
`tools/configure.js --apply` generates the camera's native `duckdns.conf`;
it contains `HOSTNAME=` and `TOKEN=`.
The token is loaded inside `cert-util`, never placed on a command line or logged.
On the camera it is stored with mode 0600 in `/mnt/sif/owrt/duckdns.conf`.
ACME account and certificate keys stay in private flash under
`/mnt/sif/owrt/certificates/acme`. Executables live on the mounted SD card at
`/media/sda1/plug2view/.certificates`, with flash symlinks for stable paths.
The SD card must remain inserted for maintenance; no PC is needed at runtime.

Build/install tools from the repository root (Mbed TLS sources must already be
present from the WebRTC dependency setup):

```powershell
node tools/prepare-certificate-build.js
node tools/build-certificates.js --fetch
node tools/install-certificates.js
node tools/configure.js --apply
```

The build pins uacme 1.8.1, curl 8.23.0, and Mbed TLS 3.6.7. It disables newer
kernel calls and curl's MIPS signal-based resolver timeout; a supervisor bounds
each operation and kills the entire child process group on timeout. Outbound
HTTPS verifies the CA chain, hostname, and certificate dates. The trust bundle
is copied from the building Node runtime's Mozilla CA roots.

`start.sh` registers `certificates/maintain.sh` with the existing minute watchdog
when either schedule is enabled; disabling both removes that job and retains the
installed certificate. The flags and intervals come from generated
`maintenance.conf`. With default intervals, enabled DDNS updates run every five
minutes, including when the public IP is unchanged. The configurable interval
is capped at one day, satisfying the two-week refresh requirement while the
camera is online and updates succeed. Disabling DDNS stops these refreshes.
Enabled renewal checks run daily after activation. uacme follows ACME Renewal Information, falling back to
30 days before expiry. A failed attempt is retried no sooner than an hour later.
Reboot clears temporary timing state, so maintenance checks again after clock
synchronization. A lock prevents overlapping renewals. DNS-01 waits 90 seconds
for propagation and removes its TXT record on challenge completion.

Maintenance modes, run through `tools/camera-shell.js`:

- `sh /mnt/sif/owrt/certificates/maintain.sh staging`: issue a test certificate
  in a separate account/directory; never install its untrusted certificate.
- `.../maintain.sh issue`: obtain/check a production certificate. Until the
  `certificates/enabled` marker exists, this does not install it.
- `.../maintain.sh activate`: validate and select the production certificate.
  Initial hostname migration also requires updating `google.PUBLIC_ORIGIN` in
  `config.local.json`, the three
  Google Developer Console URLs, and restarting with the new `start.sh`.
- With `certificates/enabled` and `certificates/reload-ready` installed after
  migration, ordinary checks install successful renewals and signal `camd`.

The installer verifies transfer sizes and digests. Before certificate activation,
the helper checks its public trust chain, dates, hostname and matching P-256 key.
An atomic `tls/managed` symlink switches the complete pair; one prior generation
is retained. SIGHUP loads a new TLS configuration for new connections while
existing connections retain their original keys. Invalid reloads preserve the
active configuration. WebRTC reads the latest verified public IP when starting
each new session, without changing an active session's SDP.

Diagnostics are `/tmp/certificates.log`, `/tmp/cert-staging.log`, and TLS reload
messages in `/tmp/camd.log`. These logs omit the DuckDNS token and private keys.

Validated: camera-side staging and production DNS-01 issuance
both passed. Normal Node CA/hostname verification passed for the camera's HTTPS
listener; its unauthenticated viewer still returned 401. Google Console's auth,
token and fulfillment URLs were saved under `https://YOUR_SUBDOMAIN.duckdns.org` and
verified after reopening the integration. The previous Google account token
state was byte-for-byte unchanged.

The client rejected expired and wrong-host HTTPS certificates. Isolated tests
verified safe TLS configuration replacement and rejection of invalid reloads.
Chrome's phone-style WebRTC test decoded 411 frames and silent PCMU with no
reported packet loss across a live SIGHUP reload; the camera PID stayed unchanged.
A subsequent renewal check retained the same certificate without reissuance,
and the cron-triggered DuckDNS update was observed. This does not simulate a
future ISP address change or prove Google Home phone playback after migration;
the latter still needs a playback check in the Google Home app.

### Optional manual HTTP-01 workflow

Use a hostname that resolves to the router's public address. Reserve the camera's
LAN IP and forward public TCP 443 to camera TCP 8443. A self-signed certificate
is insufficient for Google's connection, even if it is trusted on your own phone.

An ISP-supplied hostname can be tried if it has a forward DNS record pointing to
your connection. Certificate issuance still depends on CA policy and successful
domain validation; it is not guaranteed, and the name can change with your IP.
For HTTP-01, public TCP 80 must reach the certificate challenge responder. This
may run on a PC during issuance and renewal; it is not needed for normal playback.

Optional issuance helper (public TCP 80 forwarded to the PC's TCP 8083):

```powershell
npm install --prefix .build/acme --ignore-scripts --no-audit --no-fund acme-client@5.4.0
node tools/issue-google-cert.js --hostname camera.example.com --probe
# Stop the probe before issuance. --accept-terms accepts the issuer's subscriber agreement.
node tools/issue-google-cert.js --hostname camera.example.com --accept-terms
```

The probe serves only a validation string. Issuance uses an EC P-256 key, saves
the account key and PEM chain under git-ignored `camera/tls/acme`, and prepares
DER files under `camera/tls/public`. `--email` is optional. `--staging` uses a
separate test account and never prepares its untrusted certificate for deployment.
The temporary port 80 forward can be removed after issuance; restore it for renewal.

Prepare an existing public certificate without changing the installed certificate:

```powershell
node tools/prepare-public-cert.js --chain fullchain.pem --key privkey.pem
```

This validates the key/leaf match, dates and chain signatures, and writes
`camera/tls/public/{cert,key}.der` (git-ignored). It does not establish trust by
itself: the certificate must be issued by a publicly trusted CA for your hostname.
For this manual workflow, renew and redeploy before expiry. The DuckDNS workflow
above provides automatic renewal; avoid replacing its managed certificate with
a generic reinstall.

Verify the installed chain with normal CA and hostname validation:

```powershell
node tools/check-google-public.js --hostname camera.example.com --direct-camera
node tools/check-google-public.js --hostname camera.example.com
```

An HTTP 401 is the expected successful result: HTTPS is trusted and the private
camera viewer still requires its password. The first command checks TLS directly
on the LAN; the second also checks the public TCP 443 route. Testing from inside
the LAN depends on router NAT loopback; confirm from mobile data if it times out.

## Google project and configuration

Create a project in the [Google Home Developer Console](https://console.home.google.com/),
then create a Cloud-to-cloud integration. Copy the project **ID**, not its display name.

```powershell
node tools/google-config.js --project YOUR_PROJECT_ID --origin https://camera.example.com
node tools/configure.js --check
node tools/configure.js --apply
```

Use the current WebRTC firmware and a trusted certificate before applying.
`google-config.js` fills the `google` section in git-ignored `config.local.json`
and refuses to replace existing Google credentials. For an existing camera, run
`node tools/configure.js --import` once instead of generating new credentials.
Read secrets locally; do not commit them or paste them into chat.
`tools/configure.js --apply` generates the camera's native `google.conf` and
restarts services. `google.enabled` controls whether the integration is loaded.
See the [README setup walkthrough](../README.md#google-home-project-setup).

| Developer Console field | Value |
| --- | --- |
| OAuth client ID | `CLIENT_ID` from the private config |
| OAuth client secret | `CLIENT_SECRET` from the private config |
| Authorization URL | `https://camera.example.com/google/auth` |
| Token URL | `https://camera.example.com/google/token` |
| Cloud fulfillment URL | `https://camera.example.com/google/fulfillment` |
| Send credentials using HTTP Basic auth | Leave unchecked; this prototype expects form fields |

Use the console's testing flow and the same Google account in the Home app.
Link the test integration and sign in using `LOGIN_USER` and `LOGIN_PASSWORD`
from the config (these are separate from the camera viewer password). Open the
camera tile and inspect `/tmp/camd.log` for `SYNC` and `GetCameraStream`.
The latter logs whether Google offered `hls`, `webrtc` and/or `dash`, never tokens.
The WebRTC build advertises `["webrtc", "hls"]` once its worker is ready. It
prefers WebRTC when offered; HLS-only clients still receive the existing stream.
After upgrading, say “Hey Google, sync my devices” before requesting the camera.

The generated login password is random by default. For personal testing, a
manually chosen `LOGIN_PASSWORD` may be as short as six characters; deploy the
changed `config.local.json` with `node tools/configure.js --apply`.
The login form's Content Security Policy permits its same-origin POST and the
two exact Google callback URLs for the configured project. Chromium also checks
the redirect after form submission, so restricting `form-action` to `'self'`
alone prevents successful linking.

Public API advertises one camera with recording/motion toggle actions and five
direction settings. Legacy switch/scene IDs remain accepted for cached commands.
QUERY/EXECUTE accept bounded batches of up to 16 device targets; each execution
group can contain up to four commands, with at most one stream request per batch.
It does not implement Google video history, notifications, directional controls
inside Google's camera viewer, two-way audio, Report State,
Matter pairing or certification for public distribution.

## Motion, recording, and position controls

After installing, say **“Hey Google, sync my devices.”** No relinking is needed.
The integration advertises a single **Plug2View Camera** (nickname **Camera**).
Recording and motion detection use Google's
[Toggles trait](https://developers.home.google.com/cloud-to-cloud/traits/toggles);
position uses the
[Modes trait](https://developers.home.google.com/cloud-to-cloud/traits/modes).
All controls belong to this camera, rather than separate switch tiles.

| Camera action | Behavior | Example command to try |
| --- | --- | --- |
| Motion detection | Arms/disarms motion-triggered SD recordings; preserves sensitivity/zones | “Turn on motion detection for Camera” |
| Recording | Starts manual SD recording for up to five minutes, or stops it early | “Turn on recording for Camera” |
| Direction: Left | Pan left 120 steps from the current position | “Set Camera position to left” |
| Direction: Right | Pan right 120 steps from the current position | “Set Camera position to right” |
| Direction: Up | Tilt up 120 steps from the current position | “Set Camera position to up” |
| Direction: Down | Tilt down 120 steps from the current position | “Set Camera position to down” |
| Position: Center | Return to the saved default view; also named Centre, Original, Home | “Set Camera position to center” |

Google controls voice grammar and which controls each app surface displays.
These traits do not add directional buttons to Google's camera viewer. For exact
phrases, create voice routines with these starters and corresponding camera actions:

| Voice starter | Camera action |
| --- | --- |
| “Turn on camera recording” | Recording on |
| “Turn off camera recording” | Recording off |
| “Camera look left” | Position Left (`left`) |
| “Camera look right” | Position Right (`right`) |
| “Camera center” | Position Center (`center`) |

If the app does not expose a trait in its action picker, use a custom Assistant
action containing the corresponding “Set Camera position…” or recording command.
Voice recognition, routine availability, and app presentation require Google Home
testing after deployment; protocol tests alone do not establish them.

The position mode is command-only: success acknowledges starting a move, without
claiming the head has arrived or reporting a stale position after a browser move.
QUERY and successful control responses return `currentToggleSettings` with
`recording` and `motion_detection`. EXECUTE accepts `SetToggles` with
`updateToggleSettings`, and `SetModes` with `updateModeSettings.position`.
The five direction values are `center`, `left`, `right`, `up`, and `down`.
Every direction command is a new relative move, matching one website arrow press.
Repeated commands continue moving; they do not select a fixed view. Center alone
returns to the saved default view.

Earlier firmware advertised thirteen switches (and, previously, preset scenes).
They are no longer advertised. Cached OnOff and ActivateScene commands remain
accepted during migration; preset Off still does nothing. Existing routines using
separate switches should be recreated against the camera after syncing devices.
No account unlink is required by the firmware.
No camera power or day/night control is advertised.

Directions use the same 120-step increments as the website: Left/Right change
pan by -120/+120; Up/Down change tilt by +120/-120. The other axis stays in place.
Moves clamp to the existing software limits, x=220..3820 and y=20..1057. Center
uses the existing saved `park.pos`; the viewer's saved default remains authoritative.
The helper resolves each relative target from the latest position while holding
the shared motor lock, so a previous website move is taken into account.
The motor runs in a separate process under the same lock as the viewer. An
activation acknowledges starting the move; it does not claim that movement has
finished. Any new direction command during an active move returns `deviceBusy`.
Avoid requesting multiple positions in one automation; conflicting moves can
return `deviceBusy`.

The Recording toggle controls manual recording independently of motion arming.
Repeated starts preserve the current file; a manual start can take over an active
motion clip. Motion-event stops cannot terminate a manual recording. Switching
manual recording off does not disarm future motion clips. Manual recording now
automatically stops and finalizes its MP4 five minutes after the first manual On
command. Repeated On commands do not extend that deadline. The timer uses the
monotonic clock, so NTP corrections cannot extend the recording. QUERY then
reports the recording toggle Off. The status endpoint includes
`manual_recording`, `recording_limit_seconds`, and `recording_remaining_seconds`.
Recording stops on
restart and retains the existing recorder sample-limit/capture-idle safeguards.
Motion arming persists across restart. State is queried and returned in command
responses; push Report State is not implemented.

All software pan/tilt movement (website buttons, presets, homing, and `hkctl neck`
commands) suppresses motion-triggered recording during motor steps and for seven
seconds afterward. A shared RAM-file lock covers long moves; its monotonic
deadline also catches moves completed between detector polls. Hardware motion
results are drained during suppression, pending confirmation/post-roll is reset,
and camd independently rejects motion-start requests during that window. The
settling interval also clears the five-second recording pre-roll. Explicit manual
recordings continue during movement. `/api/motion` exposes `state.suppressed`;
the saved motion arming/sensitivity/zone settings remain unchanged.

Historical switch-firmware validation: isolated on-device tests covered all 14 advertised devices, batched
queries/commands, invalid/offline/unknown requests, all preset mappings, safe
travel bounds, and the original streaming response. Live tests exercised both
switches, verified preserved motion settings and recording ownership, then moved
to Right 1 and Original while capture stayed active. The finalized 6.83-second
test MP4 decoded 91 frames without errors using its original timestamp precision.
Phone-style Chrome WebRTC still decoded 407 video frames plus silent PCMU with
zero reported packet loss after the controls update.

The subsequent preset-switch update passed on-device tests with assertions
explicitly enabled: thirteen Switch/OnOff controls, all eleven momentary triggers,
off no-ops, reset queries, busy errors, and compatibility with cached scene
commands. After deployment, the live JSON response contained one camera plus
thirteen switches. Right 1 and Original reached the exact targets, off did not
move the motor, and capture stayed live during nine position polls. Google Home
tile discovery and operation were subsequently confirmed in testing.

The current isolated regression checks one advertised camera, recording/motion
toggles, Center and all four relative jogs, repeated moves, travel bounds,
invalid/offline/busy requests, cached legacy commands, and the stream response.
Its complete SYNC JSON is also parsed and checked separately on the host.
This does not establish Google Home voice or UI behavior.

`tools/test-google-controls.js` is read-only apart from temporary OAuth-token
generation in memory. Add `--exercise` to briefly enable motion, record a clip,
and test relative movement/Center; it refuses to interrupt an existing recording and
restores the initial switch states. It never links, unlinks, or rotates tokens.
Use `--movement-only` (`--presets-only` is a compatibility alias) to test the
camera's direction actions without changing motion or recording settings; it
finishes at the saved Center view.
Use `--recording-policy` for a short pan/return suppression test and a real
five-minute manual recording timeout; it restores the initial position and motion
arming setting. `tools/test-recording-policy.c` runs isolated lifecycle tests with
an injected monotonic clock, including the exact timeout boundary, repeated On,
manual takeover, motion suppression, and Google state queries.
Both passed. The live test observed no motion recording during pan
and return, verified that the settling flag cleared, kept a manual recording
running through movement, and watched it stop automatically after five minutes.
Google QUERY then reported Off; the initial position and motion setting were
restored.
Deployment and automatic recovery now include both `camd` and `hkctl` binaries.

## WebRTC build and display test

Keep external TCP 443 forwarded to camera TCP 8443. Also forward external
**UDP 50000 to camera UDP 50000**.
The sender advertises the camera's LAN IPv4 address and the public hostname's
resolved IPv4 address on this fixed port. It does not use a PC relay or discover
TURN servers. The public address is resolved at daemon startup; with DuckDNS
maintenance enabled, new sessions also read its latest verified IP update.
A LAN test cannot verify the public port forward.

At boot, capture can start before Wi-Fi has an address and route. A failed
WebRTC initialization now releases its sockets and crypto state, then retries
after five seconds until ready. The timer uses monotonic time, so NTP clock
corrections do not delay recovery. It does not restart healthy sessions or the
capture/recording service. Local WebRTC authenticates the peer's SDP certificate
fingerprint and does not wait for wall-clock synchronization; Google authorization
and certificate maintenance still need the correct time. This retry handles
startup failure, not a later change of the camera's LAN address.

The `webrtc` object in `/?action=status` reports `ready` and `startup_attempts`.
`/tmp/camd.log` identifies the failed startup stage and the next retry.

```powershell
node tools/build-webrtc.js --zig .build/zig-x86_64-windows-0.14.1/zig.exe --fetch
node tools/build.js --zig .build/zig-x86_64-windows-0.14.1/zig.exe --webrtc camd
node tools/deploy-google.js
```

The build script pins Mbed TLS 3.6.7 and libsrtp 2.8.1 to exact commits, rejects
modified dependency trees, and builds them into `.build/webrtc/libwebrtc.a`.
Subsequent builds need no network access. Mbed TLS is used under Apache-2.0 and
libsrtp under its BSD license; copies are in `camera/licenses/`.
This old kernel lacks `getrandom()`, and Mbed TLS's `/dev/random` fallback blocks;
the custom entropy source reads the kernel's `/dev/urandom`, as HTTPS does.
The WebRTC binary is approximately 744 KB. Omitting `--webrtc` builds HLS-only.

With the separate Chrome debug session on port 9223 and Playwright core under
`.build/browser/node_modules`, run:

```powershell
& .build/node-v22.23.3-win-x64/node.exe tools/test-webrtc.js
& .build/node-v22.23.3-win-x64/node.exe tools/test-webrtc.js --bad-fingerprint
& .build/node-v22.23.3-win-x64/node.exe tools/test-webrtc.js --phone
& .build/node-v22.23.3-win-x64/node.exe tools/test-webrtc.js --phone --bad-fingerprint
& .build/node-v22.23.3-win-x64/node.exe tools/test-webrtc-multi.js --viewers 10 --seconds 30
```

These tests start and end their own WebRTC session without replacing other
viewers (a free slot is required). They read the existing token generation internally and do not perform
OAuth linking, unlinking or revocation. They verify protocol preference, CORS,
rejection of invalid tokens/devices/SDP, continuous real Chrome video decoding,
and refusal to establish DTLS when the certificate fingerprint does not match.
Both tests passed on the camera. No credentials, SDP or tokens are printed.
The default test sends `StreamToChromecast=true`; `--phone` sends `false`.
Both use receiver-originated offers. `--receiver-offer` remains an alias for
`--phone`. Chrome emulates the signaling and media receiver, not Google TV hardware.
The retry regression checks exact-request fulfillment retries before and after the
answer, identical-answer acceptance, conflicting-answer rejection, established
session retries after 30 seconds, fresh credentials after explicit end, and
rejection of a retired token. On-device Chrome playback decoded 380 frames over
30 seconds with zero reported loss while exercising these cases.

Different Google request IDs now allocate separate sessions, including requests
arriving during the same 30-second setup window. Only an identical request ID
reuses the token/SDP; setup deadlines are never extended.

Build isolated session regressions with
`node tools/build-webrtc-tests.js --zig <zig-path>`, then run the four binaries
from `.build/webrtc-tests/` in the camera's `/tmp` directory. They do not stop or
replace the live daemon. The session test uses an ephemeral UDP port and checks
10 independent SRTP recipients, authenticated ICE routing, PLI isolation, full
capacity, timeout reclamation, and stale-handle cleanup. The signaling test
checks separate browser/phone/Hub tokens, exact request retries, capacity,
cleanup, and stale-token rejection. The startup test injects repeated route,
bind, nonblocking-mode, and frame-allocation failures, checks resource cleanup
and timed recovery, and verifies that a healthy session is not restarted.
These isolated tests passed on the camera;
they do not establish sustained video throughput or browser DTLS interoperability.

On the display: “Hey Google, sync my devices”, then “Hey Google, show Plug2View
Camera”. A successful camera log sequence is `google: awaiting receiver offer`,
`remote offer accepted`, `ICE nominated`, then `DTLS authenticated; SRTP connected`.
Motion detection remains disabled during this experiment.

## Deployment, tests and rollback

`deploy-google.js` stages a replacement before stopping the existing server. It
backs up the original binary, startup script, certificate and Google settings
under `/mnt/sif/owrt/google-backup`, and restarts video without rebooting the camera.
The original backup is preserved across subsequent prototype deployments.
Normal deployments additionally save the immediate predecessor under
`/media/sda1/plug2view/.deploy-last-good`, require the mounted SD card, and verify
transfer lengths and MD5 digests before stopping services. A failed startup now
preserves `failed-camd.log` there and automatically restores this immediate
predecessor, including its Google account state. Explicit `--rollback` below
still restores the original pre-experiment installation.

During Android diagnostics, the earlier automatic rollback incorrectly selected
the original backup and removed the Google configuration/state. The configuration
and trusted certificate were restored from local copies; the state was recovered
from obsolete JFFS2 nodes without printing it. The original startup failure's log
was overwritten by that rollback, so its cause is not established. The retry with
verified transfers started successfully, and Chrome decoded 372 frames over
30 seconds with no reported loss. The temporary camera-side flash snapshot was
removed. Subsequent real Google phone `GetCameraStream` requests passed OAuth
validation, confirming that the recovered account link remained valid.

```powershell
node tools/deploy-google.js --rollback
```

The generic `tools/install.js` continues to install the normal payload and the
existing local certificate files. `deploy-google.js --public-cert` is for the
legacy manual certificate paths. With DuckDNS maintenance, use the managed
renewal workflow above; ordinary firmware deployments preserve that certificate.
Do not replace this setup with a generic reinstall.

For an `invalid_request` error before the login page, inspect `/tmp/camd.log`.
Authorization diagnostics record the failed validation check and request size,
never the state, passwords or tokens. Google supplies opaque OAuth state; the
camera accepts up to 2048 decoded bytes and reserves space for its fully encoded
redirect. HTTP request headers are bounded to 8191 bytes, including the URL.
The initial 511-byte state limit rejected a real Android Google Home linking
request and has been replaced.

The focused regression check below verifies state limits, invalid requests,
large browser headers and a complete 6 KB redirect. It uses the installed public
certificate with normal trust validation. It starts test login sessions but does
not exchange authorization codes, link/unlink Google or revoke existing tokens;
do not run it while someone is midway through a login.

```powershell
node tools/test-google-auth.js
```

`tools/test-hls-stall.c` includes the actual muxer and simulates 1.4-second and
5-second capture pauses in a private temporary directory on the camera. It checks
stream continuity, retained segment files, consecutive media IDs and discontinuity
tags without opening the capture device or altering the running stream. Compile
it for the same MIPS target as `camd`, linking `tls.c`, `google.c` and BearSSL.

For development, use a **throwaway** Google config: the tests link and unlink it,
invalidating its tokens. Do not run against an account already linked to Google.

```powershell
node tools/google-config.js --project plug2view-test --origin https://plug2view.invalid --output .build/google-test.conf
node tools/deploy-google.js --config .build/google-test.conf
node tools/test-google.js --config .build/google-test.conf --ffmpeg PATH_TO_FFMPEG
```

The live test checks HTTPS enforcement, redirect and authorization-code binding,
incorrect credentials, state encoding, token refresh, malformed JSON, Google
intent responses, protocol negotiation, signed HLS segment retrieval, MP4 structure,
optional FFmpeg decoding, and revocation on unlink. A short private video sample
is saved as `.build/google-hls-test.mp4` (git-ignored). It accepts the known camera's
self-signed TLS certificate for this local test only; this is not Google's trust test.

On-device validation passed OAuth, HLS and unlink tests; FFmpeg decoded
306 frames from a 13.35-second HLS sample without errors: H.264 Constrained Baseline,
1280x720, level 3.1, approximately 24 fps. Rollback restored the original 259,076-byte
binary and healthy video capture, then the prototype was reinstalled. The prototype
adds about 54 KB to the static binary. Google Home phone playback remains unverified.
Public certificate issuance and deployment also succeeded, and a direct LAN
connection verified the public hostname and complete certificate chain using the
normal Node CA trust store, with certificate verification enabled.

## References

- [Google CameraStream protocol and surface support](https://developers.home.google.com/cloud-to-cloud/traits/camerastream)
- [Google OAuth requirements](https://developers.home.google.com/cloud-to-cloud/project/authorization)
- [Create an integration](https://developers.home.google.com/cloud-to-cloud/integration/create)
- [Let's Encrypt challenge types](https://letsencrypt.org/docs/challenge-types/)
