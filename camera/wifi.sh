#!/bin/sh
# Join the home Wi-Fi as a client (station) using the Ralink RT2860 driver's iwpriv
# interface. Installed at /mnt/sif/owrt/wifi.sh, run from 56mjpg.sh at boot and every
# minute by cron (via /tmp/hk-watch/wifi, "check" mode). Does nothing unless
# /mnt/sif/owrt/wifi.conf exists with an SSID.
#
#   wifi.sh            configure radio, associate, get a DHCP lease, install the check hook
#   wifi.sh check      re-join only if the link or the address has been lost (cron)
#   wifi.sh status     association status and ra0 address
#   wifi.sh off        drop the Wi-Fi link
#
# The vendor's globefish daemon runs `killall udhcpc` and may re-init ra0 into its
# ad-hoc setup mode; our DHCP client therefore runs under the `busybox` process name
# (killall matches the command name), and the cron check repairs the link.
BASE=/mnt/sif/owrt
CONF=$BASE/wifi.conf
IF=ra0
PIDF=/var/run/udhcpc_ra0.pid

connected() { iwpriv $IF connStatus 2>/dev/null | grep -q Connected; }
has_ip()    { ifconfig $IF 2>/dev/null | grep -q 'inet addr'; }
status() {
    iwpriv $IF connStatus
    ifconfig $IF | grep 'inet addr' || echo "$IF: no IP address"
}

case "$1" in
    status) status; exit 0 ;;
    off)    rm -f /tmp/hk-watch/wifi
            [ -f $PIDF ] && kill `cat $PIDF` 2>/dev/null; rm -f $PIDF
            ifconfig $IF 0.0.0.0 down; exit 0 ;;
    check)  [ -f $CONF ] || exit 0
            connected && has_ip && exit 0
            echo "wifi check: link lost, re-joining ($(date))" ;;
esac

[ -f $CONF ] || exit 0
. $CONF
[ -n "$SSID" ] || exit 0
AUTH=${AUTH:-WPA2PSK}; ENC=${ENC:-AES}

[ -f $PIDF ] && kill `cat $PIDF` 2>/dev/null; rm -f $PIDF
ifconfig $IF up
iwpriv $IF set NetworkType=Infra
iwpriv $IF set AuthMode=$AUTH
iwpriv $IF set EncrypType=$ENC
iwpriv $IF set SSID="$SSID"
[ "$AUTH" != "OPEN" ] && iwpriv $IF set WPAPSK="$PASS"
iwpriv $IF set SSID="$SSID"          # setting SSID again triggers the connection

# Wait up to 20 s for association. (No $((...)) here: the stock ash has math disabled.)
i=0
while [ $i -lt 20 ]; do
    connected && break
    sleep 1; i=`expr $i + 1`
done
connected || { echo "wifi: not associated with '$SSID'"; exit 1; }

# DHCP on the Wi-Fi interface with our own small handler so the vendor's Ethernet
# settings are not touched. Invoked as `busybox udhcpc` so globefish's killall misses it.
/bin/busybox udhcpc -b -i $IF -s $BASE/wifi-dhcp.sh -p $PIDF
sleep 4
status

# Self-heal: cron runs every file in /tmp/hk-watch once a minute.
mkdir -p /tmp/hk-watch
echo "sh $BASE/wifi.sh check >>/tmp/wifi.log 2>&1" > /tmp/hk-watch/wifi
