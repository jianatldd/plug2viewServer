# plug2view — reviving a Plug2View IP camera without its dead cloud

The Plug2View (HK / uipcam) camera only ever talked to the vendor's P2P cloud
(`lixun.scc21.net`), and the Android app is gone. This project replaces that with a
plain HTTP MJPEG server **running on the camera itself**, so any browser or VLC on the
LAN can view it — no PC in the loop.

Live view (PC, phone, tablet on the same network):

| What | URL |
| --- | --- |
| Viewer page (video, pan/tilt, recordings, Wi-Fi setup) | `http://<camera-ip>:8080/` |
| Raw MJPEG stream (VLC, Home Assistant, Frigate, …) | `http://<camera-ip>:8080/?action=stream` |
| Single JPEG | `http://<camera-ip>:8080/?action=snapshot` |

`<camera-ip>` is whatever your router's DHCP gave the camera (wired and Wi-Fi get
different addresses — see the camera's MACs in your router's client list) — reserve them
in the router so they never change. On a phone, open the viewer page and "Add to Home
Screen" for an app-like icon.

The PC-side tools read the address from `config.local.json` (git-ignored; copy
`config.local.example.json`), or `--camera <ip>`, or `PLUG2VIEW_CAMERA`.

## Current state

- Streaming: **working and persistent** — 1280x720 @ ~12.5 fps (~6 Mbit/s) from our
  own tiny streamer `mjpgd` (`camera/src/mjpgd.c`), with ~7 MB RAM to spare; survives
  reboot and telnet hang-ups, auto-restarts via cron if it dies. 720p matters: the
  module's 640x480 mode is a centre crop with a much narrower field of view.
  (mjpg-streamer was used first; it needs 4 full-size buffers and OOM-dies at 720p.)
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

## Layout

```
camera/
  payload/        binaries pushed to the camera: mjpgd + hkctl (ours, static), and a
                  static BusyBox 1.31 (busybox-mipsel) that is handy for debugging only
  src/mjpgd.c     MJPEG/HTTP streamer: V4L2 with 2 buffers, poll loop, static files
  install.sh      runs ON the camera: copies files to /mnt/sif/owrt, installs boot hook
  uninstall.sh    runs ON the camera: removes everything, re-enables hkipc at boot
  start.sh        start|stop|restart mjpgd + hkctl serve (installed as /mnt/sif/owrt/start.sh)
  mjpg.conf       resolution / fps / port / optional password
  56mjpg.sh       boot hook (installed as /mnt/spinand/etc/init.d/56mjpg.sh)
  www/index.html  viewer page served by the camera (video, pan/tilt, Day/Night, Wi-Fi setup)
  wifi.sh         join the home Wi-Fi as a client (iwpriv on ra0) + wifi-dhcp.sh handler
  sd.sh           SD card mount/check/format + NTP clock sync
  wifi.conf.example  template; a real camera/wifi.conf (with password) is optional
  src/hkctl.c     pan/tilt + IR-cut helper and its HTTP control server (port 8081)
  payload/hkctl   the compiled helper (zig cc -target mipsel-linux-musleabi -static)
tools/
  install.js      one-shot installer from the PC (TFTP server + telnet driver)
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

- **Save** button: stores the current frame (`/?action=save&tag=manual`).
- **Motion: On/Off**: the Sonix module's hardware motion detector (9 zones) is polled
  twice a second by `hkctl motion`; once motion is confirmed (default: 2 consecutive
  polls, ≈1 s) `mjpgd` opens **one AVI file per event** (`motion/<date>/<start-time>.avi`,
  MJPEG at 2 fps), starting with the 5 s pre-roll and running until 5 s after motion
  stops (max 1 h). Plays in VLC / Windows Photos; the page has its own player
  (click a 🎬 file: play/pause, scrub, "Back to live"). ~120 KB/s while recording. Button colours: grey = off, green = armed
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
  `/?action=rec&start=1&tag=motion&pre=1` / `&frame=1` / `&stop=1` (AVI recording),
  `/api/motion[?enable=0|1][&threshold=0..65535][&zones=<9-bit mask>][&confirm=1..10]`.

Card notes: the kernel does not notice a card inserted after boot by itself — `sd.sh`
re-reads the partition table (or reboot). Mount options must be *none* (`noatime`,
`codepage=`, `shortname=` all fail on this kernel). To erase and format a card on the
camera: `CONFIRM=yes sh /mnt/sif/owrt/sd.sh format` (16 KB clusters; the default
cluster size makes `mkdosfs` run out of RAM). Max 32 GB / FAT32 (no exFAT driver).

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

Edit `/mnt/sif/owrt/mjpg.conf` on the camera, then restart:

```powershell
node tools/camera-shell.js -c "sed -i 's/^RES=.*/RES=640x480/' /mnt/sif/owrt/mjpg.conf" -c "/mnt/sif/owrt/start.sh restart"
node tools/camera-shell.js -c "cat /tmp/mjpgd.log"
```

Keys: `RES` (1280x720 default = full field of view; 640x480 = cropped, ~2 Mbit/s),
`FPS`, `PORT` (8080), `CTL_PORT` (8081), `PLFREQ` (mains frequency for anti-flicker:
60 by default; the vendor shipped 50, which shows as a slow dark band rolling through the
picture), `CREDENTIALS=user:pass` to require HTTP basic auth on the video server. Changing `camera/mjpg.conf` in this repo does *not* change an
installed camera (install keeps the existing file).

## Day-to-day commands

```powershell
node tools/camera-shell.js -c "uptime; free; ps"          # health
node tools/camera-shell.js -c "cat /tmp/mjpgd.log /tmp/hkctl.log /tmp/wifi.log"   # logs
node tools/camera-shell.js -c "/mnt/sif/owrt/start.sh restart"
node tools/camera-shell.js some-script.txt                 # one command per line
```

For anything with quotes or `$`, put the commands in a file — Windows argument quoting
otherwise mangles them (a stripped quote once ran `watchdog` and
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

## Remote access (password + port-forward)

Everything — page, video, snapshot and the `/api/…` control calls — goes through the
single video port (8080), and all of it can be put behind HTTP basic auth. Set the
password on the camera (letters/digits/symbols, no spaces or colons):

```powershell
node tools/camera-shell.js -c "sed -i 's/^CREDENTIALS=.*/CREDENTIALS=someuser:YourLongPassphrase/' /mnt/sif/owrt/mjpg.conf" -c "/mnt/sif/owrt/start.sh restart"
```

Then in the router forward **one** external port (pick something unusual, e.g. 48080)
to the camera's reserved address, port 8080, and open `http://<your-public-ip>:48080/`.
The browser asks for the login once per session; VLC accepts
`http://user:pass@host:48080/?action=stream`.

Caveats, honestly: basic auth sends the password readable (base64) with every request,
and the camera has no TLS and a 2013 kernel. Use a long unique passphrase, an unusual
external port, and never forward telnet (23) or the control port (8081 — it now listens
on the camera's localhost only). A VPN into your home network (Tailscale / WireGuard on
the router or a PC) is the safer alternative if you want to avoid exposing the camera
at all.
