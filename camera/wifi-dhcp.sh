#!/bin/sh
# udhcpc event handler for ra0 (called by wifi.sh). Keeps the wired interface's
# routing alone: a default route via Wi-Fi is only added when eth2 has no address,
# so the camera behaves the same whether or not the cable is plugged in.
[ -n "$1" ] || exit 1
case "$1" in
    deconfig)
        ifconfig $interface 0.0.0.0 up
        ;;
    bound|renew)
        ifconfig $interface $ip netmask ${subnet:-255.255.255.0} up
        if ! ifconfig eth2 2>/dev/null | grep -q 'inet addr'; then
            route del default 2>/dev/null
            [ -n "$router" ] && route add default gw ${router%% *} dev $interface
        fi
        ;;
esac
exit 0
