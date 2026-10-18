#!/bin/sh
# Start / stop / restart the camera services. Installed at /mnt/sif/owrt/start.sh.
#   camd         - our static H.264 live/MP4 recording server (camera/src/camd.c), port 8080
#   hkctl serve  - pan/tilt, IR-cut and Wi-Fi control API, port 8081
# Both are started fully detached (hkctl daemon = setsid + double fork) so they survive
# the telnet session that launched them; cron (hk-watch.sh) restarts them if they die.
BASE=/mnt/sif/owrt
LOG=/tmp/camd.log

[ -f $BASE/mjpg.conf ] && . $BASE/mjpg.conf
RES=${RES:-1280x720}; FPS=${FPS:-15}; PORT=${PORT:-8080}; DEVICE=${DEVICE:-/dev/video0}; PLFREQ=${PLFREQ:-60}
export TZ=${TZ:-EST5EDT,M3.2.0,M11.1.0}      # local time for file names (POSIX TZ string)

stop() {
    rm -f /tmp/hk-watch/mjpg            # otherwise cron restarts us within a minute
    killall camd mjpgd 2>/dev/null
    killall mjpg_streamer 2>/dev/null   # pre-mjpgd installs
    killall hkctl 2>/dev/null
    sleep 1
}

start() {
    AUTH=""
    [ -n "$CREDENTIALS" ] && AUTH="-c $CREDENTIALS"
    pgrep camd >/dev/null || \
        $BASE/bin/hkctl daemon $LOG $BASE/bin/camd -d ${H264_DEVICE:-/dev/video1} -j $DEVICE -p $PORT -l $PLFREQ -a ${CTL_PORT:-8081} -w $BASE/www -s ${SAVE_DIR:-/media/sda1/plug2view} $AUTH
    pgrep -f 'hkctl serve' >/dev/null || \
        $BASE/bin/hkctl daemon /tmp/hkctl.log $BASE/bin/hkctl serve ${CTL_PORT:-8081}
    # Motion detector (polls the Sonix hardware detector; saves frames to the SD card).
    [ -f $BASE/motion.conf ] || printf 'MOTION=0\nTHRESHOLD=273\nZONES=511\nCONFIRM=2\n' > $BASE/motion.conf
    pgrep -f 'hkctl motion' >/dev/null || \
        $BASE/bin/hkctl daemon /tmp/motion.log $BASE/bin/hkctl motion $PORT
    # SD card + clock: mount if present, set the clock from NTP; re-checked by cron.
    sh $BASE/sd.sh check >/tmp/sd.log 2>&1
    mkdir -p /tmp/hk-watch
    echo 'pgrep camd >/dev/null && pgrep -f "hkctl serve" >/dev/null && pgrep -f "hkctl motion" >/dev/null || /mnt/sif/owrt/start.sh' > /tmp/hk-watch/mjpg
    echo 'sh /mnt/sif/owrt/sd.sh check >>/tmp/sd.log 2>&1' > /tmp/hk-watch/sd
}

case "$1" in
    stop)    stop ;;
    restart) stop; start ;;
    *)       start ;;
esac
