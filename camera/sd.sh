#!/bin/sh
# SD card handling. Installed at /mnt/sif/owrt/sd.sh.
#   sd.sh mount    make /dev/sda1 appear (the 2.6.21 kernel does not re-read the partition
#                  table when a card is inserted) and mount it FAT at /media/sda1
#   sd.sh check    cron: mount if a card is present but not mounted; sync NTP if clock is unset
#   sd.sh status
#   sd.sh format   erase the card and create one FAT32 filesystem (asks for CONFIRM=yes)
#
# The firmware's own hotplug script (/etc/mdev/udisk_insert -> automount.sh) fails on
# this card, so we do it ourselves. Recordings live in /media/sda1/plug2view.
DEV=/dev/sda
PART=/dev/sda1
MNT=/media/sda1
BB=/bin/busybox

mounted() { grep -q " $MNT " /proc/mounts; }

do_mount() {
    [ -b $DEV ] || { echo "no card reader"; return 1; }
    if [ ! -b $PART ] || ! grep -q sda1 /proc/partitions; then
        # Ask the kernel to re-read the partition table (BLKRRPART) via the new busybox
        # if present, else via fdisk's harmless re-read.
        if [ -x /tmp/busybox-mipsel ]; then /tmp/busybox-mipsel blockdev --rereadpt $DEV 2>/dev/null
        else echo w | fdisk $DEV >/dev/null 2>&1; fi
        sleep 2
    fi
    grep -q sda1 /proc/partitions || { echo "no card / no partition"; return 1; }
    [ -b $PART ] || mknod $PART b 8 1
    mkdir -p $MNT
    # No options at all: this kernel's built-in defaults (codepage cp950, iocharset utf8)
    # work; codepage=437, shortname= and even noatime made the mount fail with EINVAL.
    mount -t vfat $PART $MNT || { echo "mount failed (card not FAT32? try: CONFIRM=yes sd.sh format)"; return 1; }
    mkdir -p $MNT/plug2view/manual $MNT/plug2view/motion
    echo "mounted $PART at $MNT"
}

sync_clock() {
    # No RTC: the clock starts in 2000 at boot. One-shot NTP once the network is up.
    [ "`date +%Y`" -lt 2020 ] || return 0
    ntpclient -s -h pool.ntp.org -c 1 >/dev/null 2>&1 && echo "clock set: `date`"
}

case "$1" in
    mount)  do_mount ;;
    check)  sync_clock; mounted || { grep -q sda1 /proc/partitions || [ -b $DEV ] && do_mount >/dev/null 2>&1; } ;;
    status) date; mounted && df -h $MNT 2>/dev/null || df $MNT; mounted && du -s $MNT/plug2view 2>/dev/null ;;
    format) [ "$CONFIRM" = "yes" ] || { echo "refusing: run CONFIRM=yes sd.sh format"; exit 1; }
            mounted && umount $MNT
            grep -q sda1 /proc/partitions || { /tmp/busybox-mipsel blockdev --rereadpt $DEV 2>/dev/null; sleep 2; }
            # -s 32 (16 KB clusters) keeps the FAT small enough for mkdosfs to build it in
            # the ~6 MB of free RAM; the default cluster size got mkdosfs OOM-killed.
            /mnt/sif/owrt/start.sh stop
            /sbin/mkdosfs -F 32 -s 32 -n PLUG2VIEW $PART && sync && do_mount
            /mnt/sif/owrt/start.sh ;;
    *) echo "usage: sd.sh mount|check|status|format"; exit 1 ;;
esac
