#! /bin/sh
# Boot hook, installed as /mnt/spinand/etc/init.d/56mjpg.sh.
# The firmware's `rundir` *sources* every executable file in /etc/init.d, so this must
# never call exit and must background anything long-running.
#
# The stock cron job (/mnt/sif/bin/hk-watch.sh, every minute) runs every file found in
# /tmp/hk-watch with `sh file &`; we drop a one-liner there so the streamer is restarted
# if it ever dies.
( sleep 8; /mnt/sif/owrt/start.sh ) &     # start.sh also installs the cron restart hook
# Join the home Wi-Fi if /mnt/sif/owrt/wifi.conf exists (configured from the web page).
( sleep 20; sh /mnt/sif/owrt/wifi.sh >/tmp/wifi.log 2>&1 ) &
