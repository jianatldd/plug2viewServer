#!/bin/sh
# Start / stop / restart the camera services. Installed at /mnt/sif/owrt/start.sh.
#   camd         - our static H.264 live/MP4 recording server (camera/src/camd.c), port 8080
#   hkctl serve  - pan/tilt, IR-cut and Wi-Fi control API, port 8081
# Both are started fully detached (hkctl daemon = setsid + double fork) so they survive
# the telnet session that launched them; cron checks liveness and responsiveness.
BASE=/mnt/sif/owrt
LOG=/tmp/camd.log

[ -f $BASE/mjpg.conf ] && . $BASE/mjpg.conf
[ ! -f $BASE/maintenance.conf ] || . $BASE/maintenance.conf
DDNS_ENABLED=${DDNS_ENABLED:-1}; LE_ENABLED=${LE_ENABLED:-1}
RES=${RES:-1280x720}; FPS=${FPS:-15}; PORT=${PORT:-8080}; DEVICE=${DEVICE:-/dev/video0}; PLFREQ=${PLFREQ:-60}
export TZ=${TZ:-EST5EDT,M3.2.0,M11.1.0}      # local time for file names (POSIX TZ string)

stop() {
    rm -f /tmp/hk-watch/mjpg            # otherwise cron restarts us within a minute
    rm -f /tmp/hk-watch/certificates
    rm -f /tmp/plug2view-watch/video.failed /tmp/plug2view-watch/controls.failed /tmp/plug2view-watch/motion.failed
    killall camd 2>/dev/null
    killall mjpgd mjpg_streamer 2>/dev/null   # daemons of earlier versions (upgrade path)
    killall hkctl 2>/dev/null
    sleep 1
}

start_services() {
    TLS_DIR=$BASE/tls
    [ -s $BASE/tls/managed/cert.der ] && [ -s $BASE/tls/managed/key.der ] && TLS_DIR=$BASE/tls/managed
    set -- -d "${H264_DEVICE:-/dev/video1}" -j "$DEVICE" -p "$PORT" -l "$PLFREQ" -a "${CTL_PORT:-8081}" -w "$BASE/www" -s "${SAVE_DIR:-/media/sda1/plug2view}" -t "${TLS_PORT:-8443}" -C "${TLS_CERT:-$TLS_DIR/cert.der}" -K "${TLS_KEY:-$TLS_DIR/key.der}"
    [ -z "$CREDENTIALS" ] || set -- "$@" -c "$CREDENTIALS"
    if [ "${GOOGLE_ENABLED:-1}" = 1 ] && [ -f $BASE/google.conf ]; then set -- "$@" -g "$BASE/google.conf"; fi
    pgrep camd >/dev/null || \
        $BASE/bin/hkctl daemon "$LOG" $BASE/bin/camd "$@"
    pgrep -f 'hkctl serve' >/dev/null || \
        $BASE/bin/hkctl daemon /tmp/hkctl.log $BASE/bin/hkctl serve ${CTL_PORT:-8081}
    # Motion detector (polls the Sonix hardware detector; saves frames to the SD card).
    [ -f $BASE/motion.conf ] || printf 'MOTION=0\nTHRESHOLD=273\nZONES=511\nCONFIRM=2\n' > $BASE/motion.conf
    pgrep -f 'hkctl motion' >/dev/null || \
        $BASE/bin/hkctl daemon /tmp/motion.log $BASE/bin/hkctl motion $PORT
}

# Missing processes restart on the next check. Running processes must fail twice
# with the same PID set, so a temporary busy server does not interrupt recording.
check_service() {
    name=$1; pattern=$2; target=$3
    pids=$(pgrep -f "$pattern")
    marker=/tmp/plug2view-watch/$name.failed
    if [ -z "$pids" ]; then rm -f "$marker"; return; fi
    if [ "$name" = motion ]; then set -- $pids; target=$1; fi
    if $BASE/bin/hkctl health "$name" "$target"; then rm -f "$marker"; return; fi
    [ -f /tmp/hk-watch/mjpg ] || return
    previous=$(cat "$marker" 2>/dev/null)
    if [ "$previous" != "$pids" ]; then
        echo "$pids" > "$marker"
        echo "watch: $name failed health check; will recheck next minute"
        return
    fi
    echo "watch: $name failed twice; restarting PIDs $pids"
    kill -TERM $pids 2>/dev/null
    kill -CONT $pids 2>/dev/null
    sleep 2
    for pid in $pids; do
        # Only signal survivors still matching the original service.
        pgrep -f "$pattern" | grep -q "^$pid\$" && kill -KILL "$pid" 2>/dev/null
    done
    rm -f "$marker"
}

check() {
    [ -f /tmp/hk-watch/mjpg ] || return 0
    mkdir -p /tmp/plug2view-watch
    if [ -f /tmp/plug2view-watch.log ] && [ "$(wc -c < /tmp/plug2view-watch.log)" -ge 65536 ]; then
        mv -f /tmp/plug2view-watch.log /tmp/plug2view-watch.log.previous
    fi
    # Stock pgrep uses basic regexes. All three daemons have explicit arguments.
    check_service video "^$BASE/bin/camd " "$PORT"
    check_service controls "^$BASE/bin/hkctl serve " "${CTL_PORT:-8081}"
    check_service motion "^$BASE/bin/hkctl motion " 0
    [ -f /tmp/hk-watch/mjpg ] && start_services
}

start() {
    start_services
    # Install recovery before NTP or mounting can block or fail.
    mkdir -p /tmp/hk-watch
    echo '/mnt/sif/owrt/bin/hkctl watch >>/tmp/plug2view-watch.log 2>&1' > /tmp/hk-watch/mjpg
    echo 'sh /mnt/sif/owrt/sd.sh check >>/tmp/sd.log 2>&1' > /tmp/hk-watch/sd
    if { [ "$DDNS_ENABLED" = 1 ] || [ "$LE_ENABLED" = 1 ]; } && [ -f $BASE/duckdns.conf ] && [ -f $BASE/certificates/maintain.sh ]; then
        echo '/mnt/sif/owrt/bin/hkctl daemon /dev/null /bin/sh /mnt/sif/owrt/certificates/maintain.sh' > /tmp/hk-watch/certificates
    else
        rm -f /tmp/hk-watch/certificates
    fi
    # SD card + clock: install all recovery hooks before trying NTP.
    sh $BASE/sd.sh check >/tmp/sd.log 2>&1
}

case "$1" in
    check)   check ;;
    stop)    stop ;;
    restart) stop; start ;;
    *)       start ;;
esac
