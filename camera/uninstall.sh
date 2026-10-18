#!/bin/sh
# Runs ON THE CAMERA. Removes mjpg-streamer and restores the stock P2P client at boot.
rm -f /tmp/hk-watch/mjpg /etc/init.d/56mjpg.sh /mnt/spinand/etc/init.d/56mjpg.sh
killall mjpgd mjpg_streamer hkctl 2>/dev/null
rm -rf /mnt/sif/owrt
if [ -f /mnt/sif/55hk.sh.orig.bak ]; then
    cp /mnt/sif/55hk.sh.orig.bak /mnt/spinand/etc/init.d/55hk.sh
    chmod 755 /mnt/spinand/etc/init.d/55hk.sh
    rm -f /mnt/sif/55hk.sh.orig.bak
fi
sync
echo "removed; the stock hkipc client will start again on next boot (reboot to apply)"
