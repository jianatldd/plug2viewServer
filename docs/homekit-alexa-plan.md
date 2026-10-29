# Native Apple HomeKit and Amazon Alexa implementation plan

Status: proposed, not implemented. Planning baseline: 2026-10-19.

Extend the camera firmware to support Apple HomeKit and Amazon Alexa while
retaining Google Home, the browser viewer, and SD recording. Capture, encoding,
media encryption, and camera controls should run on the camera. An always-on
PC, Raspberry Pi, NAS, Homebridge, or Scrypted installation is outside this plan.

HomeKit is a candidate for fully local accessory operation. The documented
Alexa WebRTC integration requires an AWS Lambda service endpoint, so it does
not meet a strict requirement that all custom software run on the camera.
The Alexa work below is conditional on accepting that small cloud component;
this plan does not authorize its deployment.

For an alternative shared protocol, see the [Matter camera plan](matter-camera-plan.md).
The [multi-camera deployment plan](multi-camera-plan.md) remains a separate,
unimplemented extension.

## Architecture and external dependencies

| Path | Camera responsibility | External dependency |
| --- | --- | --- |
| HomeKit local viewing | HAP pairing, camera services, snapshots, encrypted media | Compatible Apple device running Home |
| HomeKit remote viewing | Same native camera endpoint | Apple home hub, such as HomePod mini, and Apple's remote-access infrastructure |
| Alexa live viewing | Account linking, device state, stream negotiation, direct WebRTC media | Alexa integration and Lambda forwarding signaling to the camera |
| Existing Google Home | Existing OAuth, fulfillment, and WebRTC | Existing Google integration and public camera reachability |

HomeKit Accessory Protocol (HAP) is distinct from Matter. Apple documents local
camera streams going directly to the viewing device and remote streams relayed
through the home hub. HomePod mini supplies the hub role; it does not convert
the existing HTTP or WebRTC endpoints into HomeKit.
See [Apple camera security](https://support.apple.com/en-euro/guide/security/sec525461d19/web)
and [home hub setup](https://support.apple.com/en-us/102557).

Amazon's documented service endpoint is a Lambda ARN. Keep any future Lambda
limited to authenticated directive forwarding and response handling; it should
not receive or transcode video. If custom cloud software remains disallowed,
leave this Alexa path disabled and evaluate the Matter alternative.
See [Amazon endpoint configuration](https://developer.amazon.com/docs/alexaplus/smarthome/configure-endpoint-tutorial).

## Existing components and constraints

| Component | Reuse and required change |
| --- | --- |
| `camera/src/camd.c` | Keep one hardware H.264 capture pipeline; distribute bounded frame references to media consumers |
| `camera/src/webrtc.c` and `webrtc.h` | Reuse Alexa offer/answer and session machinery; extract reusable RTP/SRTP pieces for HomeKit |
| `camera/src/webrtc-sdp.h` | Extend only for negotiated Alexa requirements; HomeKit does not use this SDP signaling path |
| `camera/src/google.c` | Separate transport initialization and ownership from Google account state |
| `camera/src/google-controls.inc` and `hkctl.c` | Reuse recording, motion, and movement behavior through shared internal operations |
| `tools/build.js` and `tools/build-webrtc.js` | Add optional, reproducible integration builds with pinned dependencies |
| `tools/configuration.js`, `tools/configure.js`, and `camera/start.sh` | Add explicit enablement, validation, private state storage, and startup support |

The device has 28 MB RAM, MIPS32 little-endian soft-float execution, Linux
2.6.21, and limited writable flash. The current source stream is hardware
H.264 at 1280x720, nominally around 15 fps. Full software video transcoding is
outside the initial design. Hardware bitrate and keyframe controls need
measurement before depending on them.

The WebRTC limit is ten sessions, but measured smooth playback was six viewers
under the documented LAN conditions. New HomeKit streams also consume CPU,
buffers, and bandwidth; they cannot be added as an independent free allowance.
See [hardware notes](firmware-notes.md) and
[measured WebRTC capacity](google-home.md).

## Shared implementation work

1. Initialize media transport independently of `google_init()`. Support local
   operation without Google credentials or a public hostname.
2. Track the owner of each reservation: browser, Google, Alexa, or HomeKit,
   together with its node and platform session identifier. Replace Google's
   credential-rotation use of `rtc_end_all()` with scoped revocation.
3. Define one admission policy across media transports. Start experiments with
   one new-platform viewer and one existing viewer, then measure higher limits.
   Return a platform-appropriate busy response without evicting other viewers.
4. Share encoded access units and capture timestamps, while maintaining separate
   keys, sequence numbers, replay protection, and feedback state per recipient.
   Keep queues bounded and avoid blocking capture on pairing or network calls.
5. Centralize existing control semantics: manual recording timeout, movement
   limits, and motion suppression during pan/tilt must remain identical.
6. Keep credentials and revocation state separate per ecosystem. Unlinking one
   ecosystem must not reset another ecosystem or stop its streams.

Proposed modules include `camera/src/media-session.c/.h`,
`camera/src/homekit.c/.h`, and `camera/src/alexa.c/.h`. These names describe
implementation boundaries; these files and their configuration switches do
not exist yet.

## HomeKit implementation

### Pairing and accessory services

Evaluate a small C/C++ HAP implementation for the actual MIPS ABI and kernel.
Check camera-service coverage, maintenance, licensing, and cryptographic
dependencies before choosing it. Apple's public
[HomeKit ADK](https://github.com/apple/HomeKitADK) is archived and should not
be treated as a maintained, complete camera solution.

Implement local DNS-SD discovery, setup-code/QR provisioning, authenticated
pairing, encrypted HAP connections, persistent controller permissions, and
unpairing. Use established protocol and crypto implementations. Generate a
unique identity and setup secret per physical camera; never embed a shared
production pairing identity in the firmware.

Provide accessory information, camera stream-management services, and a
snapshot handler. Begin with one experimental stream slot, then validate the
stream count and required capabilities for the intended compatibility target.
Persist keys privately in camera flash using bounded, atomic updates. Provide
an explicit pairing reset that leaves Wi-Fi, recordings, and other integrations
intact.

Use the [HAP-NodeJS camera implementation](https://github.com/homebridge/HAP-NodeJS/wiki/IP-Camera)
as an interoperability reference, not as a Node.js runtime dependency on the
camera. Resolve the applicable protocol requirements before claiming
conformance or commercial compatibility.

### Live media and snapshots

HomeKit negotiates camera streams and SRTP keys through HAP. It is not a wrapper
around `/google/webrtc`. Implement preparation, start, reconfiguration, stop,
and disconnect cleanup using the negotiated addresses, ports, payload types,
SSRCs, and SRTP parameters. Reuse suitable packetization and crypto primitives,
but keep HomeKit key setup separate from WebRTC DTLS.

Advertise only media formats the camera can actually deliver. Validate the
encoder's H.264 profile, resolution, frame rate, keyframe interval, MTU, and
requested bitrate against a real iPhone and home hub. If a requested limit
cannot be met, reject that configuration; changing advertised metadata does
not resize or re-encode the stream.

Snapshots are an early feasibility gate. The existing JPEG endpoint can fail
when the second V4L2 interface is busy, and browser-uploaded snapshots are not
available when only Apple Home is open. Evaluate a safe hardware snapshot
path first, then a bounded, on-demand H.264 decode/JPEG encode path if needed.
Measure peak memory and latency. Do not reopen the capture devices in a tight
loop or interrupt active streams to manufacture a thumbnail.

The firmware has no captured audio path. Determine whether the target Home
clients accept the intended video-only behavior and what audio capabilities
the selected HAP implementation requires. Google's generated PCMU silence is
not proof of HomeKit compatibility. Advertise neither microphone nor talkback
functionality that is not implemented.

### Initial scope and later features

The first milestone is pairing, reliable thumbnails, and local live video.
Next validate remote live viewing through HomePod mini with no HomeKit ports
forwarded from the router. Existing Google forwarding may remain configured.

Add motion events and optional recording controls after playback is stable.
Do not assume Apple's camera UI provides pan/tilt buttons; choose supported
services or actions only after confirming their behavior. SD recordings do
not automatically appear in Apple's recording timeline.

HomeKit Secure Video is a later feature with its own recording transport,
buffering, motion, codec, and hub requirements. Review Apple's
[HKSV compatibility guide](https://developer.apple.com/download/files/HomeKit-Secure-Video-Open-Source-Compatibility-Guide.pdf)
against the target OS release before implementing it; preview requirements
must not be represented as deployed support.

## Alexa implementation if cloud signaling is accepted

### Account linking and directive routing

Use separate Alexa OAuth clients, redirect allowlists, authorization codes,
access/refresh tokens, and revocation state. Reuse audited mechanisms from
Google's implementation without sharing its credentials or broadening its
redirect rules.

Proposed routes are `/alexa/auth`, `/alexa/token`, and `/alexa/fulfillment`.
They are not currently implemented. Require trusted HTTPS for external
requests. The forwarding Lambda must use a configured camera origin, bounded
request sizes, short deadlines, and secret-free logs. Validate platform tokens
and authorized device IDs at the camera.

Implement discovery with stable endpoint IDs, endpoint health, and
`Alexa.RTCSessionController`. Map Alexa session IDs onto owned firmware
reservations. Handle `InitiateSessionWithOffer`, return
`AnswerGeneratedForSession`, and process connection/disconnection directives
idempotently. Use timeouts as well as notifications to reclaim sessions.
See the [RTCSessionController reference](https://developer.amazon.com/en-US/docs/alexa/device-apis/alexa-rtcsessioncontroller.html).

### Streaming compatibility

Feed the offered SDP to `rtc_accept_offer()` and return a complete answer within
Amazon's six-second limit, including the cloud forwarding time. Validate IPv4,
non-trickle ICE, BUNDLE, RTCP multiplexing, H.264, and any negotiated audio.
The current sender supports NACK and PLI; implement and validate the FIR and
REMB handling required by Amazon, including a workable response to bitrate
limits. Waiting for a periodic keyframe must be assessed against receiver
recovery requirements.
See [Alexa WebRTC requirements](https://developer.amazon.com/docs/alexaplus/device-apis/about-alexa-rtc).

Retain direct media using the configured public UDP mapping for the initial
experiment. A working HTTPS request does not prove UDP reachability. CGNAT
and blocked UDP remain limitations; adding TURN would be another explicit
infrastructure feature.

Start with honest video-only capability reporting. Amazon distinguishes
devices without audio from Works with Alexa certification, which requires
audio support. Generated silence is not a microphone implementation.

Add pan/tilt through the documented `RangeController` camera instances only
after measuring the required field-of-view mapping to motor steps. Preserve
mechanical limits and omit zoom. Add recording and motion controls separately
from motion event reporting; toggling detection is not reporting an event.

## Build and delivery phases

| Phase | Deliverable | Exit condition |
| --- | --- | --- |
| 1 | Shared initialization, ownership, admission, and control interfaces | Existing Google/browser tests pass; unlinking is isolated |
| 2 | Minimal HAP build, discovery, pairing, and persistence | Camera pairs and reconnects after reboot within measured resource limits |
| 3 | HomeKit media and snapshots | Real iPhone local viewing works with recording and an existing viewer |
| 4 | Home hub integration and motion events | Remote viewing through HomePod mini and event delivery are verified |
| 5 | Conditional Alexa account linking and signaling adapter | Authorized test integration discovers the camera and negotiates within deadline |
| 6 | Alexa media and controls | Real Echo Show/Alexa app playback and supported controls pass |
| 7 | Installer, diagnostics, rollback, and user instructions | Each integration can be enabled, disabled, and recovered independently |

Add optional build switches only after their dependencies and resource costs
are established. Preserve builds with no new integrations and with WebRTC
alone. Pin dependency commits and record licenses, binary sizes, and memory
measurements. Use the existing Zig MIPS toolchain where supported; do not
assume a successful desktop build proves Linux 2.6.21 compatibility.

Keep experimental payloads separate until validated. Back up the working
binary and configuration through the existing deployment workflow before a
later camera installation. No new feature should require resetting current
Google credentials.

For multiple cameras, initially pair each physical camera separately in
HomeKit. Alexa can later reuse head-node signaling and stable node IDs while
media stays on the selected child. Do not assume Google head/child trust
automatically grants HomeKit controller permissions.

## Acceptance checks and stop conditions

- Run existing SDP, session, signaling, viewer, and Google control regressions
  when their shared code changes.
- Verify wrong credentials, stale sessions, malformed negotiation, retries,
  unlinking, and capacity exhaustion without cross-platform disruption.
- Test pairing/re-pairing and power-cycle recovery using persistent identities.
- Measure idle, pairing, snapshot, streaming, recording, and combined peaks:
  memory, CPU, flash use, frame delivery, queue drops, startup time, and latency.
- Require at least one hour of sustained mixed playback and recording with
  bounded memory, no daemon restarts, and correct session reclamation.
- Confirm HomeKit local viewing after WAN disconnection and remote viewing
  through the home hub separately. Do not promise offline Alexa voice service.
- Verify disablement and rollback preserve recordings and unrelated credentials.

Stop a platform implementation if required codecs, snapshots, protocol
behavior, or memory headroom cannot be provided on this hardware. Record the
specific failing requirement instead of silently introducing an external media
bridge. Prototype success is separate from platform certification; consult
[Apple accessory requirements](https://developer.apple.com/apple-home/) before
planning distribution.
