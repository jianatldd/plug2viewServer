#!/bin/sh
# On-device DuckDNS / ACME maintenance. Credentials are read only by cert-util.
# issue: obtain/check production cert, no installation until enabled.
# staging: prove DNS-01 with Let's Encrypt staging, never install that certificate.
# activate: install the validated production cert; call after firmware migration.
set -eu
umask 077
B=/mnt/sif/owrt
D=$B/certificates
U=$D/cert-util
MODE=${1:-check}
LOCK=/tmp/plug2view-cert-lock
[ -x "$U" ] && [ -f "$B/duckdns.conf" ] || exit 0
[ "$(date +%Y)" -ge 2026 ] || exit 0
case "$MODE" in check|issue|staging|activate) ;; *) exit 1;; esac
if [ -d "$LOCK" ] && [ ! -s "$LOCK/pid" ] && "$U" due "$LOCK" 120; then "$U" rmdir "$LOCK" 2>/dev/null || true; fi
if ! mkdir "$LOCK" 2>/dev/null; then
    [ -s "$LOCK/pid" ] || exit 0
    PID=$(cat "$LOCK/pid")
    case "$PID" in ''|*[!0-9]*) exit 1;; esac
    kill -0 "$PID" 2>/dev/null && exit 0
    rm -f "$LOCK/pid"; "$U" rmdir "$LOCK" 2>/dev/null || exit 0
    mkdir "$LOCK" 2>/dev/null || exit 0
fi
echo $$ > "$LOCK/pid"
trap 'rm -f "$LOCK/pid"; "$U" rmdir "$LOCK" 2>/dev/null || true' EXIT
trap 'exit 1' HUP INT TERM
if [ "$MODE" = check ]; then
    LOG=/tmp/certificates.log
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -ge 65536 ]; then mv -f "$LOG" "$LOG.previous"; fi
    exec >> "$LOG" 2>&1
fi
NOW=$(date +%s)
if [ "$MODE" != activate ] && "$U" due /tmp/plug2view-ddns.last 300; then
    if "$U" run 60 "$U" ddns; then echo "$NOW" > /tmp/plug2view-ddns.last; else echo 'DuckDNS update failed; keeping previous IP'; exit 1; fi
fi
[ "$MODE" != check ] || [ -f "$D/enabled" ] || exit 0
if [ "$MODE" = check ] && ! "$U" due /tmp/plug2view-cert.last 86400; then exit 0; fi
if [ "$MODE" = check ] && ! "$U" due /tmp/plug2view-cert.attempt 3600; then exit 0; fi
HOST=$("$U" hostname)
export UACME_CAINFO=$D/ca.pem
CONF=$D/acme
STAGING=
if [ "$MODE" = staging ]; then CONF=$D/acme-staging; STAGING=-s; fi
mkdir -p "$CONF"
if [ "$MODE" != activate ]; then
    echo "$NOW" > /tmp/plug2view-cert.attempt
    # -y accepts the subscriber agreement already authorized by the owner.
    if [ ! -f "$CONF/account-ready" ]; then
        "$U" run 180 "$D/uacme" $STAGING -y -t EC -b 256 -c "$CONF" new
        touch "$CONF/account-ready"
    fi
    RC=0
    "$U" run 600 "$D/uacme" $STAGING -t EC -b 256 -d 30 -c "$CONF" -h "$U" issue "$HOST" || RC=$?
    [ "$RC" -le 1 ] || { echo 'Certificate renewal failed; keeping current certificate'; exit 1; }
    if [ "$MODE" = staging ]; then echo 'Staging DNS-01 check passed; certificate not installed'; exit 0; fi
    [ -f "$D/enabled" ] || { echo "$NOW" > /tmp/plug2view-cert.last; echo 'Production certificate ready; awaiting hostname migration'; exit 0; }
fi
# One symlink switches the entire pair atomically; retain the previous pair.
NEXT=$B/tls/renewal-$NOW-$$
mkdir "$NEXT"
if ! "$U" convert "$HOST" "$CONF/$HOST/cert.pem" "$CONF/private/$HOST/key.pem" "$NEXT"; then
    rm -f "$NEXT/cert.der" "$NEXT/key.der"; "$U" rmdir "$NEXT"; exit 1
fi
if "$U" same "$NEXT/cert.der" "$B/tls/managed/cert.der" && "$U" same "$NEXT/key.der" "$B/tls/managed/key.der"; then
    rm -f "$NEXT/cert.der" "$NEXT/key.der"; "$U" rmdir "$NEXT"; echo "$NOW" > /tmp/plug2view-cert.last; echo 'Certificate remains current'; exit 0
fi
OLD=$("$U" readlink "$B/tls/managed" || true)
"$U" select "$(basename "$NEXT")"
sync
if [ -f "$D/reload-ready" ]; then
    killall -HUP camd 2>/dev/null || true
    echo 'Validated certificate installed; HTTPS reload requested'
else
    echo 'Validated certificate selected; restart with managed TLS paths to activate'
fi
echo "$NOW" > /tmp/plug2view-cert.last
# Preserve one rollback generation and remove only a previously recorded pair.
PREVIOUS=$(cat "$D/previous-pair" 2>/dev/null || true)
case "$PREVIOUS" in renewal-[0-9]*)
    if [ "$PREVIOUS" != "$OLD" ] && [ "$PREVIOUS" != "$(basename "$NEXT")" ]; then
        rm -f "$B/tls/$PREVIOUS/cert.der" "$B/tls/$PREVIOUS/key.der"
        "$U" rmdir "$B/tls/$PREVIOUS" 2>/dev/null || true
    fi;; esac
printf '%s\n' "$OLD" > "$D/previous-pair"
