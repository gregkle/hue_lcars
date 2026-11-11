#!/bin/sh
# Flash Hue lights red, then leave them red. Use --reset to go back to neutral white.
# Needs curl (opkg install curl) and jsonfilter (part of base OpenWrt).
set -e

LIGHTS="43"  # Treehouse color; space-separated
FLASHES=5
INTERVAL=0.5  # seconds per on/off phase

RED='{"on": true, "bri": 254, "xy": [0.675, 0.322], "transitiontime": 0}'
DARK='{"bri": 1, "transitiontime": 0}'
WHITE='{"on": true, "bri": 254, "ct": 366}'

reset=0
case "$1" in
    "") ;;
    --reset) reset=1 ;;
    -h|--help) echo "usage: $0 [--reset]"; exit 0 ;;
    *) echo "usage: $0 [--reset]" >&2; exit 2 ;;
esac

config="$(dirname "$0")/config.json"
bridge=$(jsonfilter -i "$config" -e '@.bridge')
username=$(jsonfilter -i "$config" -e '@.username')
base="http://$bridge/api/$username/lights"

set_state() {
    for light in $LIGHTS; do
        curl -sSf -m 5 -o /dev/null -X PUT -d "$1" "$base/$light/state"
    done
}

if [ "$reset" = 1 ]; then
    set_state "$WHITE"
else
    i=0
    while [ "$i" -lt "$FLASHES" ]; do
        set_state "$RED"
        sleep "$INTERVAL"
        set_state "$DARK"
        sleep "$INTERVAL"
        i=$((i + 1))
    done

    set_state "$RED"
fi
