# Native Matter camera implementation plan

Status: feasibility proposal, not implemented. Planning baseline: 2026-10-19.
Updated: 2026-10-20 to make Matter camera controls and Plug2View pan/tilt scope explicit.

Evaluate a native Matter camera endpoint on Plug2View so compatible controllers
can discover, authorize, view, and move the camera through one shared protocol.
Keep capture, encoding, media transport, and motor control on the camera, with
no always-on PC, NAS, Raspberry Pi, or custom cloud service required for local
viewing and control.

Matter 1.5 defines camera functionality including WebRTC video/audio, two-way
communication, multiple streams, pan/tilt/zoom (PTZ), detection and privacy
zones, and local/cloud storage options. Mechanical camera movement is part of
the standard. Individual devices advertise the features they implement;
support for every optional camera feature is not implied.
See the [CSA camera announcement](https://csa-iot.org/newsroom/matter-1-5-introduces-cameras-closures-and-enhanced-energy-management-capabilities/).

For Plug2View, the initial functional target includes live video and native
mechanical pan/tilt. Proving streaming first is an implementation sequence,
not a restriction of Matter to streaming. Hardware feasibility and each
ecosystem's camera UI/control support still need separate validation.

This work is separate from the
[native HomeKit and Alexa plan](homekit-alexa-plan.md). Preserve the working
Google Home integration while evaluating Matter.

## What a local controller provides

A compatible Matter controller provides commissioning, permissions, and
device interaction. Having a Matter controller on the LAN is necessary for
this integration but does not establish camera support. A HomePod mini being
a home hub or Thread border router does not make it a generic WebRTC bridge.

| Requirement | Meaning for this project |
| --- | --- |
| Native local camera | Firmware implements Matter security, camera services, and negotiated media |
| Native camera movement | Matter pan/tilt commands operate the existing motors through `hkctl` |
| Camera-capable controller | Controller and viewing app understand the relevant camera device type and features |
| Three ecosystems | Each ecosystem must support cameras and receive its own authorized Matter fabric membership |
| No custom local server | Achievable target for local viewing if the firmware and controllers interoperate |
| Remote viewing | Separate platform/network feature; may involve a home hub, cloud signaling, STUN, or TURN |
| Thread | Not the proposed video network; use existing Ethernet or Wi-Fi |

Matter multi-admin lets a device join multiple administrative domains, called
fabrics. It does not make an Apple controller act as the Google or Alexa
controller, nor does it remove per-viewer encryption and bandwidth costs.
See [Matter networking and administrators](https://developers.home.google.com/matter/primer)
and [Alexa Matter integration](https://developer.amazon.com/docs/alexaplus/smarthome/matter-support).

## Platform compatibility gate

The official lists consulted for this planning baseline do not list Matter
cameras. Treat support as unconfirmed and recheck the actual controller and
software release before selecting a platform target. This is a dated
documentation finding, not a permanent claim about platform capabilities.

| Platform | Published compatibility reference | Required evidence |
| --- | --- | --- |
| Apple Home and HomePod mini | [Apple Matter accessory types](https://support.apple.com/en-us/102135) | Exact iOS/HomePod versions pair and display a native Matter camera |
| Google Home | [Google supported Matter devices](https://developers.home.google.com/matter/supported-devices) | Exact app/controller versions expose Matter camera live view |
| Amazon Alexa | [Alexa supported Matter categories](https://developer.amazon.com/docs/alexaplus/smarthome/supported-matter-device-categories) | Exact Echo/app versions discover and stream the camera over Matter |

An SDK supporting Matter 1.5, a generic camera API, a camera with a Matter hub
built into it, and a native Matter camera are different capabilities. Record
the protocol used in each successful test. A HomeKit or cloud-to-cloud stream
does not count as a successful Matter camera test.

Create a results table during implementation containing controller model,
firmware, phone/app version, SDK commit, device-type revision, commissioning
result, live-view result, pan/tilt command and UI results, and remote-view result.
Test SDK command support separately from controls exposed by the consumer app.
If no intended ecosystem can consume the camera, limit work to a development
prototype.

## Hardware feasibility gates

### IPv6 and the old kernel

Matter control and operational communication use IPv6. The existing WebRTC
implementation uses IPv4 candidates; successful browser or Google playback
therefore does not establish Matter networking readiness.
See [Matter's IPv6 architecture](https://developers.home.google.com/matter/primer).

Before porting a camera application, verify:

- Kernel IPv6 support, AF_INET6 sockets, interface-scoped link-local addressing,
  multicast membership, neighbor discovery, and DNS-SD on Ethernet and Wi-Fi.
- Required UDP/TCP operations, timers, threading, entropy, and filesystem calls
  on Linux 2.6.21 using the intended libc and toolchain.
- LAN multicast and IPv6 connectivity between camera and controller; test
  guest-network isolation and VLAN boundaries separately.

Check the kernel configuration when available and run a small socket probe;
absence of a diagnostic file alone is not a complete test. IPv6 LAN operation
does not require an IPv6 internet subscription.

If the installed kernel cannot provide required networking, native Matter
cannot proceed on the current software baseline. A kernel replacement is a
separate recovery-sensitive project, not an implicit part of this plan.

### RAM, flash, and build dependencies

The camera has 28 MB total RAM and a small writable flash partition. Existing
streaming and recording already consume most of the useful budget. Start with
a minimal commissioning application and measure it alongside `camd` before
adding any camera cluster.

The upstream
[Linux camera example](https://github.com/project-chip/connectedhomeip/blob/master/examples/camera-app/linux/README.md)
uses GStreamer, FFmpeg, and curl and documents x86-64 and ARM64 builds. Use it
as a reference and development fixture; it is not a ready-made MIPS firmware
payload. Replace its media backend with the existing encoder interface rather
than assuming those desktop dependencies fit.

Pin a suitable `connectedhomeip` revision with camera support and its matching
specifications, generated data model, and toolchain. Establish a MIPS32
little-endian soft-float build. Audit C++ runtime, atomics, alignment, time ABI,
syscalls, crypto configuration, and entropy behavior. Existing Zig builds are
a starting point, not evidence that the Matter SDK already supports this target.

Record stripped binary and dependency sizes, flash space needed for rollback,
idle memory, commissioning peaks, and additional memory per session/fabric.
An optional SD-resident executable can address storage pressure only; it cannot
solve RAM pressure and adds an SD-card startup dependency.

## Proposed firmware architecture

Keep `camd` as the only capture owner. Start with a separate minimal
`matterd` prototype to measure the SDK's footprint and isolate failures. If it
passes, connect it to `camd` using bounded, local-only IPC for capabilities,
stream reservation, signaling, state, and controls. Video frames should stay
inside `camd` and its media worker.

The process split is provisional. Choose an in-process library only if measured
resource savings justify it and SDK callbacks cannot block the capture loop.
In either layout, only the Matter endpoint performs fabric authorization;
the media interface accepts requests from that trusted local component.

| Existing area | Proposed change |
| --- | --- |
| `camera/src/camd.c` | Shared capability/control interface and bounded stream allocation |
| `camera/src/webrtc.c`, `webrtc.h`, and `webrtc-sdp.h` | Protocol-neutral ownership and negotiation hooks; add required transport capabilities |
| `camera/src/google.c` | Remove dependency of transport initialization on Google configuration; scope revocation |
| `camera/src/hkctl.c` | Reuse movement limits and motion behavior through existing internal operations |
| Proposed `camera/src/matter/` | SDK platform adaptation, persistence, commissioning, camera delegates, and local IPC |
| Proposed `tools/build-matter.js` | Reproducible SDK preparation and target build, separate from normal builds |
| Configuration, installer, and startup scripts | Optional enablement, persistent state, diagnostics, and independent rollback |

These proposed paths and settings are not implemented. Do not add advertised
capabilities or installer switches before the corresponding firmware works.

Reuse the common session-ownership changes in the
[HomeKit and Alexa plan](homekit-alexa-plan.md). Matter sessions must be bound
to fabric, authorized peer, endpoint, and stream identity. Google unlinking,
Matter fabric removal, and browser cleanup must affect only their own sessions.

## Commissioning and persistent identity

Provision Ethernet or Wi-Fi through the existing setup first. The camera has
no established BLE onboarding implementation, so prototype on-network
commissioning and verify that the intended ecosystem accepts that flow.
Do not promise a scan-only phone setup until tested.

Implement unique setup credentials, a bounded commissioning window, PASE
commissioning, CASE operational sessions, access-control lists, and fabric
storage using the selected SDK. PASE and CASE are Matter's commissioning and
operational authenticated-session mechanisms.

Persist identities, fabric credentials, permissions, and required counters
privately and atomically. Keep pairing secrets out of discovery records,
public status responses, logs, and committed configuration. Preserve the
identity across firmware updates; provide an explicit factory reset and a
separate operation to remove one fabric.

Use development attestation credentials only for controlled tests supported
by the controller. Production attestation and certification are separate
deliverables; test credentials must not be presented as a certified device.
Commissioning policy can differ between consumer ecosystems and SDK tools.

## Camera services and media

Derive the exact mandatory clusters, revisions, attributes, commands, and
feature dependencies from the pinned Matter camera device type. Implement
camera AV stream management and WebRTC transport negotiation through SDK
delegates, plus required base services. Advertise only supported optional
features. Do not copy a cluster list from a different specification revision.

Connect stream allocation to the existing H.264 source. Preserve timestamps
and reuse the existing queue, packetization, DTLS/SRTP implementation, and
feedback processing where compatible. Matter supplies an additional secure
control plane; a new HTTP path to the Google signaling handler is insufficient.

Audit these gaps before declaring live-view support:

- Camera/controller offer and answer roles, full candidate gathering, ICE
  behavior, SDP requirements, and IPv4/IPv6 media expectations. Matter's IPv6
  control plane does not by itself prove the existing IPv4 media path is usable.
- Required codecs, audio direction, resolutions, frame rates, bitrate control,
  keyframe requests, congestion feedback, and stream reconfiguration.
- Snapshot requirements and a camera-side source that works without a browser.
  The existing JPEG endpoint can fail while H.264 is running.
- Start/stop, retries, timeout, resource exhaustion, fabric removal, and
  controller reboot without leaking sessions or exposing another fabric's keys.

Do not assume generated PCMU silence satisfies Matter camera audio
requirements. If mandatory functionality needs unavailable hardware controls,
unsupported codecs, or unaffordable processing, report that as a failed
feasibility gate instead of claiming partial protocol conformance.

The first media milestone is one local viewer with no custom remote service.
Then test two authorized fabrics with independent sessions and enforce a
shared resource limit with Google, browser, and any HomeKit viewers.

### Native pan and tilt

Implement the **Camera AV Settings User Level Management** cluster (`0x0552`)
with mechanical pan (`MPAN`) and tilt (`MTILT`). Its Matter 1.5 definition
includes absolute and relative movement, position/range attributes, movement
state, and optional saved presets.
See the [Matter 1.5 SDK cluster definition](https://github.com/project-chip/connectedhomeip/blob/v1.5.0.0/src/app/zap-templates/zcl/data-model/chip/camera-av-settings-user-level-management-cluster.xml).

| Matter interface | Proposed Plug2View mapping |
| --- | --- |
| `MPTZSetPosition` | Convert requested pan/tilt angles to absolute `hkctl` motor positions |
| `MPTZRelativeMove` | Convert angular deltas to relative motor movement |
| `MPTZPosition`, `PanMin/Max`, `TiltMin/Max` | Report calibrated position estimates and supported software-limited travel |
| `MovementState` | Report actual idle/moving state from the motor operation |
| Mechanical presets (`MPRESETS`) | Later implement save, recall, and removal with persistent preset storage |
| Mechanical zoom (`MZOOM`) | Leave disabled; this camera has no motorized zoom |
| Digital PTZ (`DPTZ`) | Leave disabled initially; stream cropping/zoom needs a separate implementation |

Calibrate angle-to-step conversion and direction for the installed orientation;
do not pass degrees directly into APIs that expect motor steps. Preserve the
existing end-switch checks and software limits. The camera estimates position
by step counting, so a lost reference must not be reported as a known angle.
Define protocol-correct failure behavior until the existing homing process has
restored that reference.

Serialize motor commands across Matter fabrics, Google, HomeKit, and the
browser through the same control path. Keep movement nonblocking for capture,
validate authorization before enqueueing, and apply command-specific range
handling from the pinned specification. Physical movement changes the view
for every viewer, even though stream permissions and keys remain independent.

Preserve motion-recording suppression during movement and the existing settling
interval. Camera movement and motion detection are separate features: a pan
command is not a motion event. Test movement while streaming and recording.

### Further camera features

Add motion events and presets after live view and basic pan/tilt. Expose the
existing park position as a Matter preset only when the advertised preset
lifecycle is implemented. Retain the manual recording timeout.

Detection/privacy zones need their own mapping and enforcement; the existing
motion-zone mask does not hide image regions. Camera SD recordings are not
automatically exposed as ecosystem recording history. Storage integration,
HomeKit Secure Video, and talkback remain separate implementation work even
though Matter defines broader camera capabilities.

## Remote access and multiple cameras

Local commissioning and viewing must not require public DNS, a public CA
certificate, router port forwarding, or Google credentials. Existing Google
remote access may retain those dependencies independently.

For remote Matter viewing, determine who handles signaling and supplies any
STUN/TURN service. Test the actual ecosystem flow through home NAT, mobile
data, CGNAT, and blocked UDP. The current sender has no TURN implementation.
Matter's remote-streaming design does not guarantee that a relay is supplied
or that every network can operate without one.

Initially commission each physical camera as its own Matter node. The planned
Google head node is not automatically a Matter bridge or fabric administrator.
Keep its LAN registry credentials separate from Matter fabric credentials.
Any future multi-camera aggregation needs its own supported device model,
authorization design, and per-camera session accounting.

## Implementation sequence

| Phase | Work | Required result |
| --- | --- | --- |
| 1 | Confirm target controller and camera support | Named platform/version can consume a reference Matter camera, or prototype is explicitly development-only |
| 2 | Probe IPv6, kernel APIs, toolchain, and storage | Native networking and dependency baseline work without kernel replacement |
| 3 | Build minimal commissioning endpoint | Pair, reboot, reconnect, remove fabric, and measure acceptable peaks alongside `camd` |
| 4 | Implement shared ownership and local media interface | Existing integrations remain functional; allocations and revocations are isolated |
| 5 | Add required camera services and one local stream | Real decoded video, correct capability reporting, snapshots/audio requirements resolved |
| 6 | Add native mechanical pan/tilt and multiple fabrics | Absolute/relative movement, position/state reporting, motor limits, independent permissions, and measured mixed-viewer capacity |
| 7 | Package optional installation and rollback | Upgrade preserves identities; disabling Matter restores the existing baseline |
| 8 | Evaluate remote viewing and further ecosystems | Separate recorded result for each platform and network path |

A desktop SDK build or controller tool can be used for development without
becoming an always-on runtime dependency. Clearly distinguish that fixture
from successful execution on the camera.

## Acceptance checks and decision

- Run existing Google/browser regressions when shared transport code changes.
- Confirm local camera operation with WAN disconnected after commissioning.
- Verify absolute and relative pan/tilt, calibrated directions, limits,
  movement-state updates, unknown-position errors, and command handling from
  multiple fabrics while video continues.
- Verify movement suppresses motion-triggered recording for the existing
  settling interval while manual recording continues. Unsupported zoom must
  not be advertised or silently treated as a different operation.
- Test pairing-window expiry, invalid credentials, unauthorized commands,
  malformed negotiation, and replay rejection through the selected SDK.
- Remove one fabric while another streams; verify only the removed fabric's
  access and sessions are revoked.
- Exercise power loss, address changes, Wi-Fi reconnects, controller restart,
  full session capacity, and repeated failed connection attempts.
- Measure CPU, RSS/heap peaks, available memory, flash growth, frame delivery,
  queue drops, and startup latency with recording enabled. Set admission
  limits from measurements rather than inheriting ten viewers automatically.
- Require a one-hour mixed-viewing run with no out-of-memory events, daemon
  restarts, unbounded growth, or interruption to unrelated sessions.
- Verify backup, update, disablement, and rollback preserve existing Google
  configuration and SD recordings.

Proceed to a supported release only when the chosen ecosystem, native runtime,
and mandatory camera features all pass. If IPv6, SDK footprint, or encoder
requirements fail, retain existing firmware and the separate HomeKit/Alexa
options. An external bridge would change the agreed deployment model.
