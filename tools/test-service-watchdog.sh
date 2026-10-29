#!/bin/sh
# Run on the camera: sh test-service-watchdog.sh /tmp/start-test-source.sh /tmp/wifi-test-source.sh
# All commands that could signal processes or touch networking are mocked.
set -eu
D=/tmp/p2v-watch-test-$$
mkdir -p "$D/base/bin" "$D/hooks" "$D/bin"
export WATCH_TEST_ROOT=$D
trap 'rm -f "$D/base/bin/hkctl" "$D/base/motion.conf" "$D/base/wifi.conf" "$D/bin/pgrep" "$D/bin/sleep" "$D/start.sh" "$D/wifi.sh" "$D/hooks/mjpg" "$D/hooks/wifi" "$D/state/video.failed" "$D/state/controls.failed" "$D/state/motion.failed" "$D/video.pid" "$D/controls.pid" "$D/motion.pid" "$D/video.bad" "$D/controls.bad" "$D/motion.bad" "$D/launches" "$D/kills"; rmdir "$D/base/bin" "$D/base" "$D/hooks" "$D/bin" "$D/state" "$D" 2>/dev/null || true' EXIT
fail() { echo "FAIL: $*"; exit 1; }
cat > "$D/bin/pgrep" <<'EOF'
#!/bin/sh
for pattern; do :; done
case "$pattern" in
 *camd*) name=video; command="$WATCH_TEST_ROOT/base/bin/camd -p 8080";;
 *serve*) name=controls; command="$WATCH_TEST_ROOT/base/bin/hkctl serve 8081";;
 *motion*) name=motion; command="$WATCH_TEST_ROOT/base/bin/hkctl motion 8080";;
 *) exit 1;;
esac
# Match using this camera's basic regex engine, as its pgrep does.
echo "$command" | grep -q "$pattern" || exit 1
cat "$WATCH_TEST_ROOT/$name.pid" 2>/dev/null || exit 1
EOF
cat > "$D/base/bin/hkctl" <<'EOF'
#!/bin/sh
case "$1" in
health) [ ! -f "$WATCH_TEST_ROOT/$2.bad" ];;
daemon)
 case "$3 $4" in *camd*) name=video;; *serve*) name=controls;; *motion*) name=motion;; *) exit 2;; esac
 echo "$name" >> "$WATCH_TEST_ROOT/launches"
 echo 12345 > "$WATCH_TEST_ROOT/$name.pid";;
*) exit 2;;
esac
EOF
printf '#!/bin/sh\nexit 0\n' > "$D/bin/sleep"
chmod 700 "$D/bin/pgrep" "$D/bin/sleep" "$D/base/bin/hkctl"
PATH=$D/bin:$PATH; export PATH
# Replace fixed production paths only; retain the actual decision/restart logic.
cat > "$D/start.sh" <<'EOF'
kill() {
 echo "$*" >> "$WATCH_TEST_ROOT/kills"
 case "$1" in
 -TERM) [ -f "$WATCH_TEST_ROOT/ignore-term" ] || rm -f "$WATCH_TEST_ROOT/video.pid";;
 -KILL) rm -f "$WATCH_TEST_ROOT/video.pid";;
 esac
}
EOF
sed "s#/mnt/sif/owrt#$D/base#g; s#/tmp/hk-watch#$D/hooks#g; s#/tmp/plug2view-watch#$D/state#g" "$1" >> "$D/start.sh"
echo 11111 > "$D/video.pid"; echo 22222 > "$D/controls.pid"; echo 33333 > "$D/motion.pid"
touch "$D/hooks/mjpg"
sh "$D/start.sh" check
[ ! -f "$D/launches" ] && [ ! -f "$D/kills" ] || fail 'healthy services changed'
touch "$D/video.bad"
sh "$D/start.sh" check
[ -f "$D/state/video.failed" ] && [ ! -f "$D/kills" ] || fail 'first failure restarted service'
rm "$D/video.bad"
sh "$D/start.sh" check
[ ! -f "$D/state/video.failed" ] || fail 'success did not reset failure'
touch "$D/video.bad"
sh "$D/start.sh" check
echo 44444 > "$D/video.pid"
sh "$D/start.sh" check
[ ! -f "$D/kills" ] || fail 'replacement PID inherited previous failure'
sh "$D/start.sh" check
grep -q 'TERM 44444' "$D/kills" || fail 'second failure did not stop target'
[ "$(cat "$D/launches")" = video ] || fail 'healthy services restarted'
touch "$D/ignore-term"
sh "$D/start.sh" check
sh "$D/start.sh" check
grep -q 'KILL 12345' "$D/kills" || fail 'unresponsive survivor not force-stopped'
rm "$D/ignore-term"
rm "$D/video.bad" "$D/launches" "$D/video.pid"
sh "$D/start.sh" check
[ "$(cat "$D/launches")" = video ] || fail 'missing service not restarted'
rm "$D/hooks/mjpg" "$D/launches" "$D/video.pid"
sh "$D/start.sh" check
[ ! -f "$D/launches" ] || fail 'intentional stop ignored'
echo 'PASS: healthy services, two-failure threshold, reset, replacement PID, selective restart and intentional stop'

# Exercise first-boot association failure without using the radio or DHCP.
cat > "$D/wifi.sh" <<'EOF'
iwpriv() { return 1; }
ifconfig() { return 0; }
sleep() { return 0; }
kill() { return 0; }
EOF
sed "s#/mnt/sif/owrt#$D/base#g; s#/tmp/hk-watch#$D/hooks#g; s#/var/run/udhcpc_ra0.pid#$D/no-dhcp-pid#g" "$2" >> "$D/wifi.sh"
printf 'SSID=test\nPASS=test\n' > "$D/base/wifi.conf"
if sh "$D/wifi.sh"; then fail 'mock association unexpectedly succeeded'; fi
[ -s "$D/hooks/wifi" ] || fail 'first failed join has no recovery hook'
sh "$D/wifi.sh" off
[ ! -f "$D/hooks/wifi" ] || fail 'Wi-Fi off kept recovery enabled'
echo 'PASS: Wi-Fi recovery registered before first failed join; explicit off disables it'
