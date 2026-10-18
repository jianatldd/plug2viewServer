#!/bin/sh
# Runs ON THE CAMERA. Fetches our files from the PC's TFTP server and installs them
# persistently under /mnt/sif/owrt (jffs2 flash). Everything is static, no runtime libs.
#
#   sh install.sh <pc-ip>
#
# Expects these files to be served by tools/tftp-server.js:
#   mjpgd hkctl start.sh mjpg.conf 56mjpg.sh index.html wifi.sh wifi-dhcp.sh [wifi.conf]
PC=$1
[ -n "$PC" ] || { echo "usage: install.sh <pc-ip>"; exit 1; }
BASE=/mnt/sif/owrt

get() { tftp -g -r "$1" "$PC" || { echo "tftp failed: $1"; exit 1; }; }

# Make sure nothing is holding the sensor or the ports.
rm -f /tmp/hk-watch/mjpg
killall mjpgd mjpg_streamer hkctl 2>/dev/null

mkdir -p $BASE/bin $BASE/www && cd /tmp || exit 1
# Leftovers from the mjpg-streamer based install (musl runtime, libjpeg) are no longer needed.
rm -rf $BASE/lib $BASE/usr $BASE/etc /lib/ld-musl-mipsel-sf.so.1

get mjpgd;        mv mjpgd $BASE/bin/mjpgd;              chmod 755 $BASE/bin/mjpgd
get hkctl;        mv hkctl $BASE/bin/hkctl;              chmod 755 $BASE/bin/hkctl
get start.sh;     mv start.sh $BASE/start.sh;            chmod 755 $BASE/start.sh
get wifi.sh;      mv wifi.sh $BASE/wifi.sh;              chmod 755 $BASE/wifi.sh
get wifi-dhcp.sh; mv wifi-dhcp.sh $BASE/wifi-dhcp.sh;    chmod 755 $BASE/wifi-dhcp.sh
get sd.sh;        mv sd.sh $BASE/sd.sh;                  chmod 755 $BASE/sd.sh
get index.html;   mv index.html $BASE/www/index.html
[ -f $BASE/mjpg.conf ] || { get mjpg.conf; mv mjpg.conf $BASE/mjpg.conf; }
# wifi.conf is optional (normally written from the web page); only replace the camera's
# copy if the PC supplies one. Done in /tmp so a failed fetch can't touch $BASE/wifi.conf.
rm -f /tmp/wifi.conf; tftp -g -r wifi.conf "$PC" 2>/dev/null
[ -s /tmp/wifi.conf ] && mv /tmp/wifi.conf $BASE/wifi.conf; rm -f /tmp/wifi.conf
get 56mjpg.sh;    mv 56mjpg.sh /mnt/spinand/etc/init.d/56mjpg.sh; chmod 755 /mnt/spinand/etc/init.d/56mjpg.sh
ln -sf /mnt/spinand/etc/init.d/56mjpg.sh /etc/init.d/56mjpg.sh

# Disable the dead P2P cloud client so the sensor is free (keeps a backup for uninstall.sh).
HK=/mnt/spinand/etc/init.d/55hk.sh
if grep -q '^/mnt/sif/bin/runhkipc' $HK; then
    cp $HK /mnt/sif/55hk.sh.orig.bak
    sed -i 's#^/mnt/sif/bin/runhkipc &#\#/mnt/sif/bin/runhkipc \&   \# disabled by plug2view install#' $HK
fi
sync

df /mnt/spinand
$BASE/start.sh
sleep 5
cat /tmp/mjpgd.log
PORT=$(sed -n 's/^PORT=//p' $BASE/mjpg.conf)
netstat -tln | grep -q ":${PORT:-8080} " && echo "OK: mjpgd is listening" || echo "WARNING: mjpgd not listening, check /tmp/mjpgd.log"
