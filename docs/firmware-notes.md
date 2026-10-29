# Plug2View camera — firmware notes

Findings from inspecting the device over telnet, including hardware behavior,
firmware details, and pitfalls to avoid.

## Hardware

| Part | Detail |
| --- | --- |
| SoC | Ralink RT3xxx-class, MIPS 24K **little-endian** (`mipsel`), no FPU (soft-float), 240 BogoMIPS |
| RAM | 28 MB total; ~12 MB free with nothing but our streamer at 640x480 |
| Flash | jffs2 on `/dev/mtdblock5` → `/mnt/spinand` (3.7 MB, 2.8 MB free before install); rootfs is a RAM disk (`root=/dev/ram0`), so `/`, `/lib`, `/etc` changes vanish on reboot |
| Sensor | **Sonix USB UVC camera module** `0c45:6360` (chip id 0x90) behind an internal USB hub `058f:6254`. `/dev/video0` = MJPEG/YUV, `/dev/video1` = H.264 (rejects every format the vendor tool offers; unused) |
| Also on USB | `058f:6366` mass-storage reader (the SD slot) |
| Peripherals | `/dev/neck` pan/tilt motor, `/dev/ircut` IR-cut filter, `/dev/status_led`, `/dev/gpio`, `/dev/i2s0` audio, `dht11` temp sensor, `/dev/m433` (433 MHz RF); drivers are compiled into the kernel (no `.ko` files) |
| Network | Wired `eth2` (DHCP via `udhcpc`), Wi-Fi `ra0` (RT2860); the two interfaces have different MACs (printed in `/mnt/sif/net.cfg` as `Mac=` / `WifiMac=`) |

## Software

- Linux **2.6.21** (2013 build), uClibc 0.9.28, BusyBox 1.12.1 (no `nc`, `mkfifo`,
  `timeout`, `strings`, `head`).
- Only service exposed: **telnet (23)**, `admin`/`admin`, root shell.
- UDP listeners from the cloud client: 5000 (`LANPORT`), 543, 39, 2627, 357.
- Vendor stack in `/mnt/sif` (= `/mnt/spinand/sif`):
  - `bin/hkipc <device-id> <device-password> lixun.scc21.net 8080` — P2P cloud client;
    the per-unit id/password are in `/mnt/sif/hkclient.conf` (`USER=`/`PASSWD=`), servers
    `lixun.scc21.net`, `hk1..8.uipcam.net`.
    Owns the sensor, PTZ, IR-cut, SD recording, FTP/NAS upload.
  - `bin/globefish` — network/Wi-Fi setup daemon (runs udhcpc, writes `net.cfg`). Leave it alone.
  - `bin/SONiX_UVC_TestAP` — Sonix reference tool. Works for MJPEG stills
    (`-c5 -s 640x480 -f mjpg -S /dev/video0` → `frame-%06u.bin`), but cannot stream, and
    at 720p it mmaps 8 full-size buffers and gets OOM-killed. Option values must be
    attached (`-c60`, not `-c 60`).
  - `bin/ftpserver`, `bin/CKRate`, `bin/hk-update`, `bin/ls_user` — unused by us.
  - Config: `hkclient.conf`, `net.cfg`, `video.cfg`, `neck.cfg` (`bit1x=2021 bit1y=539`),
    `ircut.cfg`, `sdvideo.cfg`, `nas.conf`.
- `/bin/goahead` is the stock Ralink router web UI (GoAhead-Webs 2.x). Web root
  `/etc_ro/web` is empty, it supports `/cgi-bin` and `/goform`, and on start it runs
  `initInternet`/`internet.sh`/`wan.sh` and can `reboot`. Vendor disabled it in
  `03net.sh`. **Not usable as-is and risky to start.**
- Boot: `/etc_ro/rcS` → `/etc/rcS` mounts jffs2, symlinks `/mnt/spinand/etc/init.d/*`
  into `/etc/init.d`, then `rundir /etc/init.d` **sources every executable file** there.
  `55hk.sh` started `runhkipc`; `scc_start.sh` starts `crond` with `/mnt/sif/cron`,
  which runs `hk-watch.sh` every minute: it executes every file in `/tmp/hk-watch` with
  `sh file &` (we use this as our free restart-on-crash mechanism).
- Watchdog: `/bin/watchdog` is a trivial open/write/sleep loop on `/dev/watchdog`.
  `hkipc` strings include `killall watchdog; reboot`.

## Service recovery

`start.sh` installs a minute cron hook that runs `hkctl watch`. An advisory lock
prevents overlapping checks; it is released when the watchdog exits. Missing
`camd`, control-server or motion processes are started on the next check.
For running services, a failed check is remembered by PID. Two consecutive
failures for the same PID set trigger a targeted restart, typically within
about two minutes. A successful check or replacement PID resets the failure.

The video check requests loopback `/?action=status`; the controls check requests
loopback `/health`. Each probe has a five-second deadline covering connection,
write and response reading. The motion worker writes its PID and monotonic time
after each loop; a missing, mismatched or 30-second-old heartbeat fails its check.
Only failed services receive TERM, CONT (to release a stopped process), and KILL
after two seconds if they remain. Healthy video sessions continue through a
control/motion-worker restart. `start.sh stop` disables service recovery.

Recovery hooks are installed before NTP is attempted. Wi-Fi similarly registers
its minute retry before its first association/DHCP attempt; `wifi.sh off` removes
that hook. The video capture-stall deadline and reported frame age use monotonic
time, so wall-clock corrections cannot trigger or postpone the ten-second stall
exit. WebRTC independently retries failed initialization every five seconds.

Diagnostics: `/tmp/plug2view-watch.log` (rotated at 64 KB), failure markers under
`/tmp/plug2view-watch/`, and `/tmp/plug2view-motion-heartbeat`. These are RAM files.
This is process recovery: it does not recover a dead cron daemon or reboot a hung
kernel. A hardware operation stuck in uninterruptible kernel sleep may remain
despite KILL, and a frozen recorder may not finalize its current clip.

Build isolated tests with `node tools/build-service-tests.js --zig <zig-path>`.
Copy the five files in `.build/service-tests/` into a temporary camera directory
and run `./test-service-health`, `./test-recording-policy`, and
`sh test-service-watchdog.sh ./start-test-source.sh ./wifi-test-source.sh`.
The tests use private temporary files, ephemeral sockets and mocked process/network
actions. They cover hung-response deadlines, heartbeat freshness, watchdog locking,
selective restart/escalation, failure reset, deliberate stops, first-join failure,
and wall-clock-independent capture timeouts without interrupting live capture.

## The traps (each cost a reboot)

1. **Killing `hkipc` reboots the camera** — via `runhkipc stop`, `killall`, and even
   `killall -9`. Not the signal handler (SIGKILL has none); most likely the old uvcvideo
   driver crashing when a streaming process dies abruptly. Solution: stop it from
   starting (`55hk.sh`), reboot once.
2. **`rundir` sources any executable in `/etc/init.d`** — a `55hk.sh.orig` backup left
   there with its exec bit ran the original script anyway. Keep backups elsewhere
   (`/mnt/sif/55hk.sh.orig.bak`).
3. **After a reboot the camera may not come back on DHCP.** Happened once (needed a
   factory reset — which does *not* clear the jffs2 changes we rely on, it just reruns
   setup); the next three reboots were fine. Minimise reboots.
4. **PowerShell/Windows quoting**: inner quotes passed to `node script.js "..."` get
   stripped; `grep -E 'a|b'` became `grep -E a | b`, which ran `watchdog` and
   `globefish` as commands. Always send command *files* (`tools/camera-shell.js file`).
5. **`/bin/neck` recalibrates on start** and drove the head to its counter-clockwise
   stop; afterwards it reports garbage ranges and segfaults for every argument form tried.

## What we installed

Two static binaries of our own, cross-compiled on Windows with Zig
(`zig cc -target mipsel-linux-musleabi -mcpu=mips32 -Os -static`): `mjpgd` (streamer)
and `hkctl` (PTZ / IR-cut / Wi-Fi control API, plus a `daemon` helper = setsid + double
fork, because the stock BusyBox has neither `setsid` nor `nohup` and daemons started
from a telnet session otherwise die with it).

```
/mnt/sif/owrt/
  bin/mjpgd   bin/hkctl
  start.sh  mjpg.conf  wifi.sh  wifi-dhcp.sh  wifi.conf (after joining)
  www/index.html  neck.pos  park.pos
/mnt/spinand/etc/init.d/56mjpg.sh  (boot hook: start.sh after 8 s, wifi.sh after 20 s)
/mnt/spinand/etc/init.d/55hk.sh    (runhkipc line commented out)
/mnt/sif/55hk.sh.orig.bak          (original, used by uninstall.sh)
/tmp/hk-watch/mjpg                 (written by start.sh; cron runs it every minute)
```

History: the first working version used OpenWrt 18.06.9 `mipsel_24kc` packages
(musl 1.1.19 runtime + libjpeg + mjpg-streamer r182) — the musl loader runs fine on this
2.6.21 kernel given a `/lib/ld-musl-mipsel-sf.so.1` symlink. It was replaced because
mjpg-streamer's 4 capture buffers (1.84 MB each at 720p — the UVC max frame size, the
same for 800x600) plus per-client buffers exceed the RAM as soon as a viewer connects.

Measured with mjpgd: 1280x720 ≈ 12.5 fps, 5.9 Mbit/s, ~7 MB RAM free, two viewers OK;
640x480 ≈ 12.5 fps, 2 Mbit/s, ~9.8 MB free. The camera delivers ~12.5 fps regardless of
the requested 15.

V4L2 ABI gotcha: zig's musl has 64-bit `time_t`, so the header's `struct v4l2_buffer` is
76 bytes; this driver wants the classic 68-byte layout (32-bit timeval), and the ioctl
number encodes the size → QUERYBUF fails with EINVAL. `mjpgd.c` defines its own
`v4l2_buffer32` and ioctl numbers for QUERYBUF/QBUF/DQBUF. Frames from the Sonix module
are complete baseline JPEGs including Huffman tables (`FFC4`), so no DHT insertion.

Flicker: the module's `Power Line Frequency` UVC control (V4L2_CID_POWER_LINE_FREQUENCY)
defaulted to 50 Hz (vendor `video.cfg` had `Frequency=50`); on 60 Hz mains that produces
a dark horizontal band rolling slowly through live video. `mjpgd -l 60` sets it to 60 Hz
(2) at startup. Other UVC controls available: brightness, contrast, saturation, hue,
gamma, gain, sharpness (0–6), backlight compensation, white balance, exposure — no
focus or zoom.

Field of view: 640x480 is a centre crop of the 1280x720 image (verified with the vendor
tool's single-frame capture, `-c3 -n 2 -s 1280x720 -f mjpg -S`), so 720p is the only way
to get the lens's full angle. The lens itself is a fixed M12; no software focus/zoom.

Transport to the camera: it has only a `tftp -g` client and `wget`, so the PC runs
`tools/tftp-server.js`. From the camera to the PC: static BusyBox `nc` →
`tools/tcp-receive.js`.

## Pan/tilt and IR-cut

Both `/dev/neck` and `/dev/ircut` use the same five ioctl numbers, with different
meanings per driver (recovered by disassembling the vendor's `/bin/ircut` and
`/bin/neck`; see `camera/src/hkctl.c` for the implementation):

| cmd | `/dev/ircut` | `/dev/neck` |
| --- | --- | --- |
| 1018 | returns light-sensor state (polled by `ircut_change`) | one motor step, direction A; arg = axis (0/1) |
| 1019 | drive solenoid — `ir_cut_n` ("night") | one motor step, direction B; arg = axis |
| 1020 | drive solenoid — `ir_cut_p` ("day") | returns limit-switch bitmask (bits 0–3, set = pressed) |
| 1021 | stop (sent 500 ms after 1019/1020) | – |
| 1022 | register `{pid, signal 16}` so the driver signals on light changes | release motor coils |

The vendor tool steps once per ioctl with a `usleep` between steps, checks the limit
bitmask before every step, and tracks position itself (it kept x/y in a 28-byte file
it mmaps; it believes full travel is x 0–4040, y 0–1077, centre 2021/539). There is no
position readback from hardware — only the four end switches — so position must be
re-established by driving to a switch ("home").

Verified on the camera head:

| ioctl, axis arg | motion | blocked by limit bit |
| --- | --- | --- |
| 1018, 0 | tilt **down** | bit0 (bottom stop) |
| 1019, 0 | tilt **up** | bit1 (top stop) |
| 1018, 1 | pan toward x=0 (clockwise seen from above) | bit3 |
| 1019, 1 | pan toward x=max (counter-clockwise) | bit2 |

Measured travel: pan 4041 steps switch-to-switch (vendor constant 4040 confirmed);
tilt range taken from the vendor constant 1077. 3–10 ms per step all moved reliably;
`hkctl` uses 4 ms (full pan ≈ 16 s). Step counting is exact (530 out / 529 back to
re-trigger the switch).

Policy implemented in `hkctl`: software range 220–3820 on pan (~160° of the 180°
travel) and 20–1057 on tilt, limit bit checked before every step, coils released
(1022) after each move. `home` drives pan to x=0 and tilt to the bottom switch, then
backs off into the range; position is persisted in `/tmp/neck.pos` and
`/mnt/sif/owrt/neck.pos`. `hkctl serve 8081` exposes this as the JSON API the viewer
page uses; moves are refused until a `home` has established the position.

IR-cut filter: NOT software-controlled on this unit. `hkctl ircut day|night` (ioctl
1019/1020 + 1021, exactly what the vendor tool does) produces pixel-identical frames, and
no readable GPIO changes during any of ioctls 1018–1022 (bit 14, which once seemed to
react, is the light-sensor line). The vendor's `ircut.cfg` says `ircut=0` — the software
IR-cut path is for their "rt900" hardware variant. Day/Night buttons were removed from the
page.

Night light: the IR illuminator ring is driven directly by the photoresistor in
hardware — it came on by itself with no software involvement, `gpio r` showed no pin
following the Day/Night switch, and the ircut driver's "light" value (`hkctl light`,
`chklight`) read a constant 1 throughout, including while the sensor was covered, so
it is not a usable ambient-light reading. Consequently there is no software IR-LED
switch and no sensor to drive automatic filter switching; the filter is switched
manually (`hkctl ircut day|night`, page buttons). If automatic switching is ever wanted,
judge darkness from snapshot brightness (libjpeg is on the camera) or by a clock
schedule (`ntpclient` exists; no RTC). `/bin/status_led 0|1|2` and
`/bin/gpio l …` exist for the indicator LED but were not pursued.

## HTTP layout and auth

`mjpgd` (port 8080) serves video, static files and relays `/api/…` to `hkctl serve`,
which binds 127.0.0.1:8081 (`hkctl serve 8081 all` binds everything). The relay runs in
a forked child with blocking I/O (a pan move can take 16 s; the video loop must not
stall). `-c user:pass` enables basic auth for every URL; the expected header value is
precomputed as `Basic <base64>` and compared verbatim. Static files are read fully into
memory per request (fine for the 11 KB page; 4 MB cap).

## Wi-Fi

Ralink RT2860 STA driver on `ra0` (2.4 GHz b/g/n only), config template
`/etc/Wireless/RT2860/RT2860.dat`, controlled with the usual Ralink `iwpriv` set:
`NetworkType=Infra`, `AuthMode=WPA2PSK|WPAPSK|OPEN`, `EncrypType=AES|TKIP|NONE`,
`SSID=…`, `WPAPSK=…` (set SSID again afterwards to trigger association),
`iwpriv ra0 connStatus`, `iwpriv ra0 set SiteSurvey=1` + `get_site_survey` (fixed-width
table: Ch, SSID at col 4/33 wide, BSSID at 37, Security at 57, Signal% at 80).

Stock behaviour: `globefish` reads `WifiEnable/WifiSSID/WifiPASS/...` from
`/mnt/sif/net.cfg` and has a `StartWifiAdhoc` mode that advertises `AP(<id>)` at
192.168.10.222 — the vendor app's setup channel; ad-hoc is unusable from modern
phones/Windows. We bypass globefish for Wi-Fi: `wifi.sh` applies `/mnt/sif/owrt/wifi.conf`
via iwpriv and runs `udhcpc -i ra0` with our own handler (`wifi-dhcp.sh`), which only
installs a default route via Wi-Fi when `eth2` has no address. `hkctl serve` exposes
`/wifi/scan|status|join`; `56mjpg.sh` runs `wifi.sh` 20 s after boot.

Site survey detected a 2.4 GHz WPA2PSK/AES router at 100 % signal during testing.

globefish interference (found after the first reboot with Wi-Fi configured): the link
came up fine at boot (+20 s) and was torn down within minutes — `ra0` left up with no
address, `connStatus:Disconnected`, our udhcpc gone. globefish's strings show
`killall udhcpc` (kills every DHCP client, ours included) and an ad-hoc setup fallback
(`iwpriv ra0 set NetworkType=Adhoc`, `ifconfig ra0 192.168.10.222 up`) it applies when
its own `net.cfg` says `WifiEnable=0` — that 192.168.10.222 is the "self-claimed"
address the router showed. Countermeasures in `wifi.sh`: the DHCP client is started as
`/bin/busybox udhcpc …` (killall matches the command name, so it survives), and
`wifi.sh check` runs every minute from `/tmp/hk-watch/wifi` and re-joins when the link
or the address is lost. Observed stable for 5+ minutes after that; a reboot test is
still outstanding. Alternative not taken: filling `WifiEnable/WifiSSID/WifiPASS/
WifiAuthmode/WifiEnctype` in `/mnt/sif/net.cfg` so globefish joins by itself — the
numeric codes are unverified and its `wudhcpc.conf` assigns the *wired* static address
to ra0 when no lease arrives.

Flash leftovers: timestamp-suffixed `wifi.cfg_*` and `net.cfg_*` files in `/mnt/sif/`
can contain stale Wi-Fi credentials and network settings. Check that they are unused
before deleting them.

## SD card

USB card reader `058f:6366` on the same internal hub as the camera module; `usb-storage`
and FAT/VFAT are built into the kernel with NLS cp437/cp936/cp950/iso8859-1/utf8 (default
codepage cp950). Hotplug: mdev → `/etc/mdev/udisk_insert` → `automount.sh` (mounts to
`/media/<dev>`), but the kernel does not re-read the partition table when a card is
inserted after boot, so `/dev/sda1` never appears until `blockdev --rereadpt /dev/sda`
(new BusyBox) — `sd.sh mount` does this. Poking the reader produced a burst of
`uvcvideo: Failed to resubmit video URB (-38)` and one DQBUF error; mjpgd kept going, and
now exits (for cron restart) if no frame arrives for 10 s.

Mount gotchas (2.6.21 FAT): `mount -t vfat /dev/sda1 /media/sda1` with **no options**
works; adding `noatime`, `codepage=437` or `shortname=mixed` fails with EINVAL and no
kernel message. A tested 16 GB card had its primary FAT32 boot sector overwritten with
junk (backup at sector 6 was intact; copying sectors 6→0 and 7→1 made it mountable). It
was then reformatted on the camera: `mkdosfs -F 32 -s 32` — the default cluster size made
mkdosfs build a ~8 MB FAT in RAM and get OOM-killed.

Clock: no RTC, boots at 2000-01-01. `ntpclient -s -h pool.ntp.org -c 1` works (DNS via
8.8.8.8); `sd.sh check` runs it until the year looks sane. `TZ` is exported by start.sh
(POSIX string, default Eastern) so file names are local time.

Motion detection: `SONiX_UVC_TestAP /dev/video0 --motionget-result` prints
`golllll <mask>` (9-bit zone mask, 0 = none) and works while another process streams;
5 polls take well under a second. `--motionset-thre N` takes 0–65535 (vendor default
273; higher = less sensitive; 60000 accepted). `--motionset-areas` takes zone **digits**,
not a mask: `123456789` = all, `7` = zone 7 only (passing `511` enables zones 5 and 1!);
`--motionget-areas` reports the mask in data[0..1] and lists the zones. Vendor type 0,
one detection window (139,165,250,107). `hkctl motion` polls once a second, applies
`motion.conf` (THRESHOLD/ZONES/CONFIRM) whenever the file changes, debounces on
CONFIRM consecutive polls, and asks mjpgd (loopback requests skip basic auth) to save
frames. The vendor defaults fired on single zones with nothing moving in a lit room.

## The second video device: hardware H.264

`/dev/video1` is the Sonix module's H.264 stream (fourcc `H264`, 1280x720 only; the
vendor-patched uvcvideo describes it as "MPEG2 TS", which it is not). The vendor tool
cannot open it because it only knows `mjpg`/`yuyv`. Probed with
`camera/src/vidcap.c` while mjpgd was streaming MJPEG from video0 at the same time:

- Annex-B byte stream, Baseline profile (SPS 0x42), SPS+PPS repeated before every
  frame, IDR roughly every 25 frames, ~10.6 fps, **~1.07 Mbit/s** (vs ~6 Mbit/s for the
  720p MJPEG stream). Raw `.h264` plays directly in VLC.
- Both streams run concurrently (USB bandwidth is fine), but each open V4L2 stream
  costs 2 x 1.84 MB of kernel buffers: free RAM dropped to ~1.8 MB (+ reclaimable
  cache) with both running. An H.264 path should therefore open video1 on demand
  (recording or an H.264 viewer connected) and release the buffers otherwise.
- Browsers cannot play raw H.264 or AVI; options are an MP4/fMP4 muxer, or the
  WebCodecs API (Chrome/Edge/Safari 16.4+) decoding Annex-B frames in the page.
- `hkctl vidinfo <dev>` enumerates formats/frame sizes; `vidcap <dev> <fourcc> <WxH>
  <seconds> <file>` dumps raw frames (prints fps/bitrate).

## Recording format

`camd` records one MP4 per event. ISO BMFF with a single `avc1` track:
`ftyp` (isom/iso2/avc1/mp41), `mdat` written as frames arrive (each access unit as
one sample in AVCC form - 4-byte NAL length prefixes, SPS/PPS/AUD NALs stripped and kept
for `avcC`), then `moov` at stop with `stts` (one entry per sample, real capture
intervals, timescale 1e6), `stss` (IDR samples), `stsc` (1 sample/chunk), `stsz`,
`stco`; `mdat` size patched at close. Auto-closed after 30 s without frames or at
1 h. The pre-roll (ring of the last ~12 s / 1.6 MB of frames in RAM) is written first,
starting at the newest keyframe >= 5 s old. Validated: sum(stsz) == mdat payload, stco
offsets consistent, Baseline profile 0x42 level 31. Earlier recordings are MJPEG AVI
(mjpgd) and still play via the page's JS stepper.

Live view: `?action=fmp4` is fragmented MP4 for MSE - init segment (ftyp, moov with an
empty sample table and mvex/trex, avcC from the current SPS/PPS, codec string also in the
`X-Codec` header) then one moof+mdat per frame (tfhd default-base-is-moof, tfdt in
microseconds, trun with one sample carrying its real duration - the server stays one
frame behind the newest so the duration is known). Verified: fragments start on a
keyframe, trun sizes match mdat, timestamps monotonic, ~75 ms per frame. WebCodecs was
the first approach but is only available in secure contexts (HTTPS), which this camera
cannot serve yet; the framed stream below remains for custom clients.

`?action=raw` is the plain Annex-B stream (VLC: `--demux h264`). The 12-byte-framed
`?action=h264` variant used by the WebCodecs attempt was removed as unused. The page uses a <video> element fed by MSE (see above). JPEG snapshots open /dev/video0 for ~1 s on demand
(1 buffer, take the 4th frame, release) so the second stream costs RAM only briefly.
`/rec/` downloads are streamed from a forked child (files are MBs; RAM is scarce).

## Reference: useful one-liners on the camera

```sh
uptime; free; ps                              # health
cat /tmp/mjpg.log                             # streamer log
/mnt/sif/owrt/start.sh restart
cat /mnt/sif/net.cfg /mnt/sif/hkclient.conf   # network / cloud identity
cat /proc/bus/usb/devices                     # USB tree (sensor, SD reader)
df /mnt/spinand                               # flash use
```

## UVC driver lessons from the H.264 switch

- Starting an MJPEG capture on /dev/video0 right after the H.264 stream on /dev/video1
  was torn down (`killall camd` then a `vidcap` on video0) left the process stuck in
  D state inside the driver and the device unusable until a reboot. Give the driver a
  few seconds between stopping one interface and starting the other; never do it in a
  tight sequence.
- `VIDIOC_STREAMON` on video0 returns EBUSY after video1 has been streaming for a while
  (it worked within seconds of a cold start). Snapshots therefore no longer depend on
  video0: the page uploads the JPEG it renders from the decoded frame (POST
  `/?action=save`), and GET `/?action=snapshot` is best effort.
- The H.264 stream occasionally reports `VIDIOC_DQBUF: Protocol error` (USB EPROTO) and
  the kernel logs `uvcvideo: Failed to query UVC control ... -145 (ETIMEDOUT)` from the
  motion detector's XU polling; the stream usually continues. camd re-initialises the
  capture in place after 6 s without frames and exits for a cron restart after five
  failed attempts.
- Each open V4L2 stream costs 2 x 1.84 MB of kernel buffers; with H.264-only operation
  the camera has ~10 MB free at boot and ~3 MB plus reclaimable cache with all
  daemons and a viewer.

## HTTPS (BearSSL)

BearSSL 0.6 cross-compiles for mipsel with zig cc without changes (277 files,
`-DBR_LE_UNALIGNED=0`); the parts linked into camd add ~130 KB. Server profile
`br_ssl_server_init_full_ec` with an EC P-256 key (PKCS#8 DER) and a DER certificate,
bidirectional 32 KB I/O buffer per connection, LRU session cache, extra entropy from
/dev/urandom. Measured on the camera: a complete HTTPS request (TCP + handshake +
response) takes ~0.10 s; the 1.4 Mbit/s fMP4 stream over TLS costs no visible CPU.

Integration: camd's poll loop drives the BearSSL engine (encrypted bytes socket<->engine
in `tls_pump`, plaintext through `tls_recv`/`tls_send`, poll events from the engine
state). Forked children (file download, /api proxy) inherit the engine state and use
BearSSL's blocking `br_sslio` wrapper. Lesson: a response is not "sent" when the last
plaintext byte is handed to the engine - close immediately and the tail is lost (small
replies arrived empty, the page was cut at 16 KB). camd now enters a P_CLOSING phase:
`br_ssl_engine_close` (close_notify) and pumping until the engine has flushed.

Certificate: self-signed EC cert generated by tools/install.js with OpenSSL (Git for
Windows ships one), SAN = camera IP + DNS:plug2view, 10 years. Browsers warn once;
importing cert.der as a trusted root removes the warning.
