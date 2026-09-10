#!/bin/sh
# find_display.sh — source this to get a free Xvfb display number in $D
# Usage: . "$(dirname "$0")/find_display.sh"
# Sets:  D  (e.g. ":107")
find_free_display() {
    for n in $(seq 50 199); do
        [ -f "/tmp/.X${n}-lock" ] && continue
        [ -S "/tmp/.X11-unix/X${n}" ] && continue
        echo ":${n}"
        return 0
    done
    echo ":99"
}
D=$(find_free_display)
