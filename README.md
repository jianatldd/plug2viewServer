# plug2view — reviving a Plug2View IP camera without its dead cloud

The Plug2View (HK / uipcam) camera only ever talked to the vendor's P2P cloud
(`lixun.scc21.net`), and the Android app is gone. This project replaces that with a
plain HTTP MJPEG server **running on the camera itself**, so any browser or VLC on the
LAN can view it — no PC in the loop.

Live view (PC, phone, tablet on the same network):

| What | URL |
| --- | --- |
| Viewer page (video, pan/tilt, recordings, Wi-Fi setup) | `http://<camera-ip>:8080/` |
| Raw H.264 stream for VLC (`vlc --demux h264 <url>`) | `http://<camera-ip>:8080/?action=raw` |
| Fragmented MP4 fallback stream (MSE) | `http://<camera-ip>:8080/?action=fmp4` |
| Single JPEG (grabbed from the MJPEG stream on demand) | `http://<camera-ip>:8080/?action=snapshot` |
| Server stats (fps, bitrate, viewers, recording) | `http://<camera-ip>:8080/?action=status` |
| Everything above over HTTPS | `https://<camera-ip>:8443/…` |

`<camera-ip>` is whatever your router's DHCP gave the camera (wired and Wi-Fi get
different addresses — see the camera's MACs in your router's client list) — reserve them
in the router so they never change. On a phone, open the viewer page and "Add to Home
Screen" for an app-like icon.

The viewer prefers WebRTC for low latency, using the existing website login.
Up to 10 WebRTC viewers can connect at once, including website and Google Home
viewers. They share the hardware-encoded H.264 stream; each connection has its
own encryption and network traffic. The page switches to MP4 only when the server
explicitly reports that all WebRTC slots are occupied. Connection errors and
playback stalls retry WebRTC; browsers without WebRTC show an unsupported-browser
message. In a two-minute LAN test,
10 viewers stayed connected but averaged about 6.35 fps each due to sender
overload; six viewers sustained about 13.3 fps each. See
[measured capacity](docs/google-home.md) for test conditions.

The PC-side tools read the address from `config.local.json` (git-ignored; copy
`config.local.example.json`), or `--camera <ip>`, or `PLUG2VIEW_CAMERA`.

## Current state

- Video: **H.264 from the module's hardware encoder** (`/dev/video1`, 1280x720,
  ~15 fps, ~1-2 Mbit/s) served by our tiny static daemon `camd` (`camera/src/camd.c`).
  The page plays it through Media Source Extensions as fragmented MP4 (works over plain
  HTTP in Chrome, Edge, Firefox, Safari, iOS 17.1+; older browsers get a 2 s snapshot
  refresh). WebCodecs was tried first but browsers only allow it on HTTPS pages. Survives
  reboot and telnet hang-ups, auto-restarts via cron if it dies. History: MJPEG via
  mjpg-streamer, then our own `mjpgd`, were used first; H.264 halves RAM use (one
  V4L2 stream instead of two) and cuts bandwidth ~4x.
- Pan/tilt and IR-cut control: **working**. `hkctl` (our static MIPS binary,
  `camera/src/hkctl.c`, built with `zig cc`) talks to the `/dev/neck` and `/dev/ircut`
  drivers and serves a tiny JSON API on port 8081 that the viewer page uses (pad,
  Day/Night buttons, arrow keys; ● tap = default view, double-tap = save current view
  as default, hold = re-home). Pan travel measured at 4041 steps; software-limited to
  220–3820 (~160° of 180°), tilt 20–1057. The camera is designed to hang from a
  ceiling, so standing on a table its tilt range is almost all "up": level is tilt ≈ 58,
  saved as the default view. The head remembers its position across reboots and
  re-syncs whenever it touches an end switch. Don't run the vendor's `/bin/neck` tool —
  it drives the head into its mechanical stop.
- Night vision is **entirely hardware-automatic**: a photoresistor switches the IR
  illuminator ring (and the IR-cut filter, if this unit has one). Software can neither
  control nor read it — the `/dev/ircut` driver's commands provably move nothing on this
  model (identical Day/Night frames, no GPIO change; the vendor's own config has
  `ircut=0`). The page therefore has no Day/Night buttons.
- The stock cloud client `hkipc` is disabled at boot (backup kept, see *Uninstall*).

## Versions

Google Home experiment: direct WebRTC playback has been tested in the Google Home app
over mobile data, with an estimated delay under one
second. Negotiating a silent PCMU audio track resolved the app's loading screen.
The Google Hub also plays WebRTC, with an estimated 1–2 seconds of delay. These
are visual estimates; extended-session stability remains to be tested. HLS
playback also works but had an estimated 4–5 second delay.
Google Home exposes one Camera device with Recording and Motion Detection
toggle actions and direction actions: Left, Right, Up, Down, and Center.
Say “sync my devices” after upgrading, then try “turn on recording for Camera” or
“set Camera position to center.” For exact phrases such as “camera look left,”
create a Google Home voice routine that sets Camera position to Left.
Manual recording automatically stops after five minutes; repeated On commands
do not extend it. Pan/tilt moves suppress motion-triggered recording while moving
and for seven seconds afterward. Manual recording continues during movement.
See [setup, validation and rollback](docs/google-home.md).
This needs a public hostname and trusted certificate; WebRTC also uses forwarded
UDP port 50000. It does not use Matter or a PC media relay.
DuckDNS updates and Let's Encrypt DNS-01 renewal now run on the camera for
`YOUR_SUBDOMAIN.duckdns.org`: DNS checks every five minutes, certificate checks daily,
and certificate reloads preserve running streams. Port 80 is unnecessary.
The maintenance executables require the SD card; credentials and certificates
are stored in private camera flash. See the Google Home setup guide above.

Planned multi-camera support uses one HTTPS head node and direct WebRTC from each
camera on its own configured UDP port. See the
[multi-camera deployment plan](docs/multi-camera-plan.md) for registration,
router forwarding, and implementation scope; this is not implemented yet.

Additional integration proposals are documented in the
[native HomeKit and Alexa plan](docs/homekit-alexa-plan.md) and the
[Matter camera plan](docs/matter-camera-plan.md). Both are unimplemented:
HomeKit and Matter need hardware feasibility checks, and the documented Alexa
WebRTC route requires a cloud signaling endpoint.

- **`main`** — current: H.264 live view (WebRTC with MP4/MSE fallback), MP4 motion recordings
  from the module's hardware encoder, and **HTTPS** (BearSSL built into `camd`).
- **`mjpg-video`** branch — the earlier MJPEG version (`mjpgd`: MJPEG live stream that
  every browser plays as a plain `<img>`, AVI motion recordings, no decoder needed). Kept
  for anyone who prefers MJPEG or has a browser without Media Source Extensions:
  `git checkout mjpg-video && node tools/install.js`.

## Layout

```
camera/
  payload/        binaries pushed to the camera: camd + hkctl (ours, static), vidcap (probe
                  tool) and a static BusyBox 1.31 (busybox-mipsel) handy for debugging only
  src/camd.c      H.264 live/HTTP(S) server + MP4 recorder (V4L2 2 buffers, poll loop)
  src/tls.c/.h    HTTPS layer on BearSSL (non-blocking engine + blocking helper for children)
  src/bearssl/    BearSSL 0.6 headers and the mipsel static library (rebuild: tools/build.js bearssl)
  src/vidcap.c    probe: dump N seconds of raw frames from a V4L2 device
  install.sh      runs ON the camera: copies files to /mnt/sif/owrt, installs boot hook
  uninstall.sh    runs ON the camera: removes everything, re-enables hkipc at boot
  start.sh        start|stop|restart camd + hkctl serve/motion (installed as /mnt/sif/owrt/start.sh)
  mjpg.conf       resolution / fps / port / optional password
  56mjpg.sh       boot hook (installed as /mnt/spinand/etc/init.d/56mjpg.sh)
  www/index.html  viewer page served by the camera (video, pan/tilt, Day/Night, Wi-Fi setup)
  wifi.sh         join the home Wi-Fi as a client (iwpriv on ra0) + wifi-dhcp.sh handler
  sd.sh           SD card mount/check/format + NTP clock sync
  wifi.conf.example  template; a real camera/wifi.conf (with password) is optional
  src/hkctl.c     pan/tilt + IR-cut helper and its HTTP control server (port 8081)
  payload/hkctl   the compiled helper (zig cc -target mipsel-linux-musleabi -static)
tools/
  build.js        cross-compile camd/hkctl/vidcap (and BearSSL) with Zig
  install.js      one-shot installer from the PC (TFTP server + telnet driver; makes the TLS key/cert)
  camera-shell.js run commands on the camera over telnet
  tftp-server.js  read-only TFTP server (the camera can only fetch files this way)
  tcp-receive.js  receive a file the camera pushes with `nc`
docs/
  firmware-notes.md  everything learned about the hardware/firmware, incl. the traps
```

## SD card: manual snapshots and motion recording

A FAT32 card in the slot is mounted at `/media/sda1` (by `sd.sh`, at boot and checked
every minute). Recordings go to `/media/sda1/plug2view/<manual|motion>/YYYY-MM-DD/HHMMSS-nnn.jpg`
in local time (`TZ` in `mjpg.conf`; the clock is set from NTP at boot since the camera
has no battery clock). When the card has < 200 MB free the oldest day is deleted.

- **Save** button: stores the current frame as a JPEG — the page renders the decoded
  frame and uploads it (`POST /?action=save`), so it works without touching the MJPEG
  stream. `GET /?action=snapshot` (for scripts) is best effort: the driver may refuse a
  second stream while H.264 runs.
- **Motion: On/Off**: the Sonix module's hardware motion detector (9 zones) is polled
  twice a second by `hkctl motion`; once motion is confirmed (default: 2 consecutive
  polls, ≈1 s) `camd` records **one MP4 file per event** (`motion/<date>/<start-time>.mp4`,
  H.264 at the encoder's native ~15 fps) beginning with the 5 s pre-roll kept in RAM and
  ending 5 s after motion stops (max 1 h). Plays in any browser, VLC, Windows and phones;
  in the page click a 🎬 file. ~150-250 KB/s while recording (a 20 s event ≈ 4 MB). Button colours: grey = off, green = armed
  (watching, not recording), red blinking "Recording ●" = frames being saved now.
- **⚙ next to Motion**: sensitivity slider (1 = least … 10 = most; maps onto the
  detector threshold, shown as a number you can also type), **confirm polls** (motion
  must be seen on N consecutive 1-second polls before recording starts — kills one-off
  flicker), and a **3×3 zone grid** laid out like the picture: green = zone armed, click
  to exclude a zone, **orange flash = zone reporting motion right now**, so a zone that
  keeps lighting up with nothing moving (a lamp, a TV, a curtain) can simply be switched
  off. Settings persist in `/mnt/sif/owrt/motion.conf` and are re-applied at boot.
- **Recordings** button: browse folders/days/files (`<tag>/<date>/YYYY-MM-DD_HH-MM-SS-nnn.jpg`);
  🗑 deletes a file or a whole day (asks first). Automatic cleanup: when the card has
  less than 200 MB free, the oldest day is deleted before a new frame is saved.
- API: `/files?dir=<rel>` (JSON listing), `/rec/<rel>` (file), `/?action=delete&path=<rel>`,
  `/?action=rec&start=1&tag=motion&pre=1` / `&stop=1` (MP4 recording), `/?action=status`,
  `/api/motion[?enable=0|1][&threshold=0..65535][&zones=<9-bit mask>][&confirm=1..10]`.

Card notes: the kernel does not notice a card inserted after boot by itself — `sd.sh`
re-reads the partition table (or reboot). Mount options must be *none* (`noatime`,
`codepage=`, `shortname=` all fail on this kernel). To erase and format a card on the
camera: `CONFIRM=yes sh /mnt/sif/owrt/sd.sh format` (16 KB clusters; the default
cluster size makes `mkdosfs` run out of RAM). Max 32 GB / FAT32 (no exFAT driver).

### Replacing the SD card

Stop recording and power off before swapping cards. Format the replacement as
FAT32 and copy the entire `plug2view` folder to its root, including dot-prefixed
folders. If copying selectively:

| Folder on card | Copy? |
| --- | --- |
| `plug2view/.certificates/` | Required for automatic DNS updates and certificate renewal; contains `cert-util` and `uacme`. |
| `plug2view/.deploy-last-good/` | Recommended: firmware recovery backup. |
| `plug2view/manual/` and `plug2view/motion/` | Optional: saved recordings and snapshots. |
| `plug2view/.hls/` | Temporary streaming cache; recreated when needed. |

Website credentials, Google configuration, DuckDNS token, ACME account keys, and
active SSL certificates stay in camera flash. Replacing the card does not require
Google relinking. Keep the maintenance executables at the same relative paths.

## Wi-Fi (cut the cable)

The camera has a 2.4 GHz b/g/n radio. In the page press **Wi-Fi → Scan networks**,
pick your network, enter the password, **Join**. The camera then has a *second*
address on Wi-Fi (shown in the panel) — use that one after unplugging the Ethernet
cable, and reserve it in your router. Settings are
stored in `/mnt/sif/owrt/wifi.conf` on the camera, applied at boot by `wifi.sh`, and
checked every minute by cron (`wifi.sh check`) because the vendor's network daemon
occasionally resets the radio and kills DHCP clients (see docs). Check the link with
`node tools/camera-shell.js -c "sh /mnt/sif/owrt/wifi.sh status"`.

The "open Wi-Fi" the camera itself advertised was the vendor app's ad-hoc setup
network; phones and Windows 11 cannot join ad-hoc networks, so ignore it.

Alternative without the page: copy `camera/wifi.conf.example` to `camera/wifi.conf`,
fill it in, run `node tools/install.js` (the file is pushed only if present).

## Control API (`/api/…` on port 8080)

The page calls these as `http://camera:8080/api/<path>`; `mjpgd` relays them to `hkctl
serve`, which listens on the camera's localhost:8081 only. Same password as the video.

| Request | Effect |
| --- | --- |
| `GET /status` | `{"x","y","known","limits","light","range"}` |
| `GET /ptz?dx=120&dy=0` | relative move in motor steps (≈22 steps per degree of pan) |
| `GET /ptz?x=2020&y=540` | absolute move; clamped to the software range |
| `GET /ptz?home=1` | drive to the limit switches, re-zero, return to the park view (slow) |
| `GET /ptz?park=1` / `setpark=1` | go to / save the default "park" view (`/mnt/sif/owrt/park.pos`) |
| `GET /ircut?mode=day` / `night` | drives the IR-cut ioctls — no effect on this hardware variant, kept for experiments |
| `GET /wifi/scan` | nearby networks `[{ssid,bssid,security,signal,ch}]` (~5 s) |
| `GET /wifi/status` | `{ssid,state,wifi_ip,eth_ip}` |
| `POST /wifi/join` (`ssid=&pass=&security=`) | write `wifi.conf`, connect, DHCP on `ra0` |

Moves are refused (`"known":false`) until the head has been homed once; the position
is kept in `/tmp/neck.pos` and mirrored to `/mnt/sif/owrt/neck.pos`. There is no
encoder — only the four end switches — so **don't move the head by hand** (hold the
body when adjusting the lens). If it does get bumped, the position re-syncs the next
time a move touches an end switch, or long-press ● in the page to re-home. Same commands
from the camera shell: `/mnt/sif/owrt/bin/hkctl neck status|step|goto|home`,
`hkctl ircut day|night`, `hkctl light`.

## Install / reinstall

Requirements: Node.js ≥ 18 on a PC on the same LAN; camera reachable on telnet
(`admin` / `admin`). Windows Firewall must allow Node inbound on the private profile.

```powershell
node tools/install.js                     # camera from config.local.json, PC IP auto-detected
node tools/install.js --camera <ip> --pc <pc-ip>
```

It stages the files in a temp TFTP directory, telnets into the camera, runs
`camera/install.sh` there, and prints the streamer log. Flash use after install: ~1.1 MB
of 3.7 MB. Reinstalling keeps the camera's `mjpg.conf`, `wifi.conf`, saved position and
park view.

## Configuration

All hostnames and LAN addresses in this README are examples. Substitute values
from your private local configuration; do not copy real credentials or network
details back into public documentation.

Keep all settings in git-ignored `config.local.json`. Copy
`config.local.example.json` for a new camera, or run
`node tools/configure.js --import` once to preserve an existing camera's settings.
Edit the JSON, then run `node tools/configure.js --check` and
`node tools/configure.js --apply`. See the
[complete configuration reference](#updating-google-home-and-duckdns-settings)
below, including independent DDNS and certificate-renewal switches.

### Google Home project setup

Start with the camera installed, a trusted HTTPS certificate for your public
hostname, and the [public port forwards](#remote-access-https--password--port-forward)
configured. The commands below run on the PC from the repository root.

1. Open the [Google Home Developer Console](https://console.home.google.com/) and
   create a project. Copy the project **ID**, not its display name.
2. Add a **Cloud-to-cloud integration**. Use **Camera** as the device type and
   **Plug2View** as the integration name.
3. For a new integration, generate the private config, substituting your project
   ID and public hostname:

   ```powershell
   node tools/google-config.js --project YOUR_PROJECT_ID --origin https://YOUR_SUBDOMAIN.duckdns.org
   ```

   This fills the `google` section of git-ignored `config.local.json` with
   random credentials and refuses to overwrite existing Google credentials.
   If you already have a working setup, import it once and follow
   [configuration updates](#updating-google-home-and-duckdns-settings).
   Keep the generated values private.
4. Deploy the config using the current WebRTC-enabled camera payload:

   ```powershell
   node tools/configure.js --apply
   ```

   Applying settings restarts camera services. If building firmware from
   source, follow the [WebRTC build instructions](docs/google-home.md) first and
   build/deploy `camd` with `--webrtc` before applying settings. This assumes trusted certificates are
   already installed; do not use the initial self-signed certificate for Google.
5. Fill in the integration's account-linking and fulfillment settings:

   | Developer Console field | Value |
   | --- | --- |
   | OAuth client ID | `google.CLIENT_ID` from private `config.local.json` |
   | OAuth client secret | `google.CLIENT_SECRET` from the same file |
   | Authorization URL | `https://YOUR_SUBDOMAIN.duckdns.org/google/auth` |
   | Token URL | `https://YOUR_SUBDOMAIN.duckdns.org/google/token` |
   | Cloud fulfillment URL | `https://YOUR_SUBDOMAIN.duckdns.org/google/fulfillment` |
   | Send credentials using HTTP Basic auth | Leave unchecked; this implementation expects form fields |

   Replace the example hostname in all three URLs with the hostname used in
   `PUBLIC_ORIGIN`. Save the settings and use the console's testing flow.
6. In the Google Home app, use the same Google account as the Developer Console.
   Under **Works with Google Home**, find **[test] Plug2View** and link it. Enter
   `LOGIN_USER` and `LOGIN_PASSWORD` from the private config on the Plug2View
   login form. These are separate from the camera website login; no Nest account
   credentials are needed.
7. Assign the linked devices to your home/room and open the camera tile. Say
   “Hey Google, sync my devices” after updates before requesting the camera again.

For troubleshooting, inspect `/tmp/camd.log` for `SYNC` and `GetCameraStream`:

```powershell
node tools/camera-shell.js -c "cat /tmp/camd.log"
```

The WebRTC build advertises WebRTC and HLS, preferring WebRTC when the receiver
offers it. The integration exposes one camera with motion/recording toggles and
four relative direction actions plus Center. It does not provide Google video history,
notifications, directional buttons inside Google's camera viewer, two-way audio,
Matter pairing, or public-distribution certification. See
[the full Google Home guide](docs/google-home.md) for validation and limitations.

### Updating Google Home and DuckDNS settings

`config.local.json` is the private source for all deployment settings. Run tools
from the repository root on the camera's LAN. If `node` is not on PATH, use
`& .\.build\node-v22.23.3-win-x64\node.exe` in PowerShell.

For an existing camera, import its current settings **once** before editing:

```powershell
node tools/configure.js --import
```

Import preserves the installed passwords, Google token key, Wi-Fi settings, and
enabled schedules without printing credentials. It refuses to replace an already
migrated config. For a new camera, copy `config.local.example.json` to
`config.local.json` and fill in your settings instead.

After editing the JSON, validate and apply:

```powershell
node tools/configure.js --check
node tools/configure.js --apply
```

Apply generates the camera's native config files, checks transfer hashes, backs
up the previous files on the camera, finalizes any current recording, and restarts
services. It preserves installed certificates and Google token state. If `camd`
fails to start, it restores the previous configuration. It uploads configuration
and startup scripts; firmware builds and certificate tools are installed
separately. `--render` only generates private files under `.build/config/`.

Do not maintain separate copies in `camera/google.conf` or `camera/duckdns.conf`
after migrating. Generated native files remain necessary on the camera, but the
JSON is the source for the next apply. Motion/Wi-Fi changes made through the
website are not automatically copied back to the PC; applying JSON restores its
values. Wi-Fi credentials are saved for the Wi-Fi connection workflow/boot;
apply does not deliberately reconnect the current Wi-Fi connection.

| JSON section / key | Purpose and defaults |
| --- | --- |
| `schemaVersion` | Configuration format, currently `1`. |
| `camera`, `pc` | Camera LAN address and optional PC LAN address for uploads. PC is detected when blank. `--camera` and `PLUG2VIEW_CAMERA` override the camera address. |
| `telnet.username`, `telnet.password` | Login used by PC tools; does not change the firmware's telnet account. |
| `server.PORT`, `TLS_PORT`, `CTL_PORT` | HTTP `8080`, HTTPS `8443`, internal control `8081`. HTTPS can be disabled with `0` when Google is disabled. Update router forwards if changing ports. |
| `server.H264_DEVICE`, `DEVICE` | H.264 `/dev/video1` and JPEG `/dev/video0` devices. |
| `server.PLFREQ` | Anti-flicker mains frequency: `50`, `60` (default), or `0`. |
| `server.TZ` | POSIX time-zone string for recording filenames. |
| `server.SAVE_DIR` | Recording directory, default `/media/sda1/plug2view`. |
| `server.CREDENTIALS` | Website `username:password`; blank disables Basic authentication. |
| `server.TLS_CERT`, `TLS_KEY` | Optional absolute camera paths to an installed DER certificate/key pair. Both blank selects the managed pair, falling back to legacy TLS files. Custom paths require scheduled Let's Encrypt renewal disabled. |
| `google.enabled` | Whether the server loads the Google integration. |
| `google.PROJECT_ID` | Developer Console project ID, not display name. Changing projects requires its own integration setup/linking. |
| `google.PUBLIC_ORIGIN` | Public HTTPS origin, for example `https://YOUR_SUBDOMAIN.duckdns.org`, without a trailing slash/path. |
| `google.CLIENT_ID`, `CLIENT_SECRET` | OAuth credentials matching the Google Developer Console; secret minimum 32 characters. |
| `google.LOGIN_USER`, `LOGIN_PASSWORD` | Plug2View account-linking login; separate from the website login. Password minimum six characters. Changing it does not revoke existing tokens. |
| `google.TOKEN_KEY` | Exactly 64 hexadecimal characters. Preserve for ordinary updates; replacing it invalidates existing tokens and requires relinking. |
| `ddns.enabled` | Enables scheduled DuckDNS public-address updates. |
| `ddns.HOSTNAME`, `TOKEN` | Full DuckDNS hostname and its account token. Also used for Let's Encrypt DNS-01 validation. |
| `ddns.updateIntervalSeconds` | Send a DuckDNS update even when the IP is unchanged: `300` seconds by default, allowed `60`–`86400` (one day). |
| `letsEncrypt.enabled` | Enables scheduled certificate checks/renewal independently of DDNS updates. |
| `letsEncrypt.checkIntervalSeconds` | Successful check interval: `86400` by default, allowed `3600`–`604800`. |
| `letsEncrypt.retryIntervalSeconds` | Retry interval after failure: `3600` by default, allowed `300`–`86400`. |
| `letsEncrypt.renewBeforeDays` | Renewal fallback window: `30` by default, allowed `1`–`60`; issuer renewal information is used when available. |
| `motion.MOTION` | Motion detection `0` (off) or `1` (on). |
| `motion.THRESHOLD`, `ZONES`, `CONFIRM` | Detector threshold `273`, nine-zone bitmask `511`, consecutive detections `2`. |
| `wifi.SSID`, `PASS` | Wi-Fi network name/password. |
| `wifi.AUTH`, `ENC` | Authentication `WPA2PSK`, `WPAPSK`, or `OPEN`; encryption `AES`, `TKIP`, or `NONE`. Default `WPA2PSK`/`AES`. |

### Disabling DDNS or certificate renewal

When DDNS is enabled, each scheduled run sends an update to DuckDNS even if the
public IP has not changed. This also refreshes an unchanged address at least
every two weeks: the default interval is five minutes, and the maximum allowed
interval is one day. This requires the camera to be running with working
internet access. A failed request does not advance the successful-update timer;
the minute watchdog retries it. Setting `ddns.enabled` to `false` stops these
refreshes too.

Edit these two booleans in the existing JSON, preserving other fields:

```json
"ddns": { "enabled": false },
"letsEncrypt": { "enabled": false }
```

This snippet shows only the switches, not a complete configuration file. Apply
with `node tools/configure.js --apply`. Each switch is independent:

| DDNS | Let's Encrypt | Scheduled behavior |
| --- | --- | --- |
| `false` | `false` | No DNS/certificate maintenance job. Existing certificate remains installed. |
| `true` | `false` | Update DuckDNS public address only; manage certificate renewal yourself. |
| `false` | `true` | Renew using DuckDNS TXT validation without updating its address record. Keep hostname/token configured and manage the address yourself. |
| `true` | `true` | Automatic address updates and certificate renewal. |

New example configs default to both disabled. Import preserves an existing
camera's enabled schedules. Turning renewal off does not remove the certificate;
it will eventually expire unless renewed manually. Explicit `maintain.sh issue`,
`staging`, and `activate` commands still work when the schedule is disabled.
Disabling DDNS also skips address updates during those manual certificate commands.

For a static/custom DNS name and externally managed certificate, disable both
schedules, install the certificate/key on the camera, and set `server.TLS_CERT`
and `server.TLS_KEY` to their DER paths. Google still needs a trusted certificate
matching `google.PUBLIC_ORIGIN`. These settings do not upload certificate files.

Before enabling a schedule on a new camera, build/install its tools with
`node tools/install-certificates.js`, then apply the config. See the
[certificate build instructions](docs/google-home.md#duckdns-and-on-camera-renewal).
Certificate executables use the SD card; the PC is not needed at runtime.
DuckDNS DNS-01 validation does not require a port 80 forward.

### Changing the public hostname

Keep LAN access available during migration; public access can be interrupted
until the matching certificate and Google's URLs are updated.

1. Register the new name. Update `ddns.HOSTNAME`, its `TOKEN` if needed, and
   `google.PUBLIC_ORIGIN` in `config.local.json`; preserve Google credentials.
2. Apply with `node tools/configure.js --apply`. When DDNS is disabled, arrange
   the new DNS address yourself.
3. For the installed DuckDNS/Let's Encrypt setup, obtain the matching certificate:

   ```powershell
   node tools/camera-shell.js --timeout 800000 -c "sh /mnt/sif/owrt/certificates/maintain.sh issue"
   ```

   In an activated setup, success installs the certificate and reloads HTTPS.
   Wait for success; DNS validation includes a 90-second wait. `staging` only
   tests issuance and does not install a trusted certificate. A fresh manual
   setup also needs `maintain.sh activate` to select its issued certificate,
   followed by `node tools/configure.js --apply` to restart using that pair.
4. In the existing Google Home Developer Console integration, change all three
   URLs to the new origin: `/google/auth`, `/google/token`, and
   `/google/fulfillment`.
5. Test HTTPS and Google Home playback. A hostname-only migration preserves
   linking credentials; relinking is not normally needed.

### Changing the website username and password

Edit `server.CREDENTIALS` in `config.local.json`, for example:

```json
"CREDENTIALS": "someuser:ReplaceThisPassword123"
```

Then run `node tools/configure.js --apply`. The username cannot contain a colon;
the combined value must fit within 90 UTF-8 bytes. JSON quoting applies normally;
the tool handles shell quoting without printing the password. Reopen the website
with the new login; a private window avoids cached credentials. This login
protects website playback, signaling, recordings, and controls. It is separate
from Google linking and telnet credentials; Google Home needs no relinking.


### Check before committing or publishing

Private configs, their backup files, TLS material, and `.build/` are excluded by
`.gitignore`. `camera/mjpg.conf` is a public template: keep its `CREDENTIALS=`
empty. Website credentials belong only in ignored `config.local.json` and the installed camera config.

Run this read-only check before staging or publishing changes:

```powershell
node tools/check-public.js --history
```

It checks publishable working files, the Git index, and locally available branch
and tag history for known local config values and private config paths. Matched
values are never printed. Run it again after staging: removing a value from the
working file alone does not remove an earlier staged copy. The check depends on
your local configs and available Git history; it is not a general secret scanner
or a remote-host audit. Never force-add the ignored private files.

## Day-to-day commands

```powershell
node tools/camera-shell.js -c "uptime; free; ps"          # health
node tools/camera-shell.js -c "cat /tmp/camd.log /tmp/hkctl.log /tmp/wifi.log"    # logs
node tools/camera-shell.js -c "/mnt/sif/owrt/start.sh restart"
node tools/camera-shell.js some-script.txt                 # one command per line
```

For anything with quotes or `$`, put the commands in a file — Windows argument quoting
otherwise mangles them (this bit us: a stripped quote once ran `watchdog` and
`globefish` as commands).

## Uninstall

```powershell
node tools/install.js --uninstall
```

Removes `/mnt/sif/owrt` and the boot hook and restores the original `55hk.sh`, so the
stock `hkipc` cloud client starts again after the next reboot.

## Things to know before poking at the camera

Full details in [docs/firmware-notes.md](docs/firmware-notes.md). The short list:

1. **Never kill `hkipc` while it runs** (not even `kill -9`): the camera reboots within
   seconds. Prevent it from *starting* instead (that is what `install.sh` does).
2. **Don't leave executable files in `/etc/init.d`** on the flash: the boot script
   `rundir` sources *every* executable there, including your `.orig` backups.
3. **Reboots are not free**: once, the camera came back without a DHCP lease and needed
   a physical reset; other times it was fine. Avoid reboots when you can.
4. **RAM is the limit** (28 MB total). Anything you copy into `/tmp` lives in RAM.
5. **Don't start `/bin/goahead`**: it is the Ralink router web UI; on start it re-runs
   the router LAN/WAN init and can change the IP or reboot.
6. **Don't run `/bin/neck`**: its "recalibrate" drives the pan motor into the end stop.

## Remote access (HTTPS + password + port-forward)

Example public website: **https://YOUR_SUBDOMAIN.duckdns.org/**. Replace
`YOUR_SUBDOMAIN` with your DuckDNS name. Reserve the
camera's LAN IP in the router, then add these two forwarding rules:

| Protocol | Public/external port | Destination camera IP | Internal port | Purpose |
| --- | --- | --- | --- | --- |
| TCP | 443 | `192.168.1.100` | 8443 | HTTPS website, Google account linking/fulfillment, WebRTC signaling |
| UDP | 50000 | `192.168.1.100` | 50000 | Encrypted WebRTC video/audio transport |

Replace the destination IP if your camera uses a different reserved address.
Both rules point directly to the camera, not the PC. The public URL uses port
443 implicitly; do not append `:8443`. Keep UDP 50000 mapped to 50000, as those
are the connection details advertised by this firmware.

**Port 80 is not required.** Certificate renewal uses DuckDNS DNS-01 validation.
The previous temporary port-80 forward to the PC can be removed. Keep HTTP 8080,
control port 8081, and telnet port 23 private; do not forward them publicly.

The website uses the [Basic-auth login configured above](#changing-the-website-username-and-password).
The active trusted Let's Encrypt certificate is selected under
`/mnt/sif/owrt/tls/managed/` and renews automatically. Use the public hostname for
trusted HTTPS: connecting to the LAN IP does not match that certificate. The
initial installer's self-signed certificate is a bootstrap option; it is not
the certificate used by this public setup.

To test, turn off the phone's Wi-Fi and open the public HTTPS URL, then test live
video. Some routers cannot reach their own public address from home Wi-Fi even
when external access works. Close Google Home live view before testing website
WebRTC: the camera supports up to 10 WebRTC viewers, with MP4 fallback on the website.

See [configuration updates](#updating-google-home-and-duckdns-settings) and the
[hostname migration procedure](#changing-the-public-hostname) when changing DNS
or Google account-linking settings.
