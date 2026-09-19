#!/bin/bash

WALLPAPER_FILE="$HOME/.config/awesome/wallpaper.txt"
DEFAULT_WALLPAPER="/home/dani/Pictures/Wallpapers/christmas-tree-christmas-decoration-xmas-background-3085x2182-1198.jpg"

if [ ! -f "$WALLPAPER_FILE" ]; then
    echo "$DEFAULT_WALLPAPER" > "$WALLPAPER_FILE"
fi

if [ $# -eq 0 ]; then
    WALLPAPER=$(cat "$WALLPAPER_FILE")
else
    WALLPAPER="$1"
    echo "$WALLPAPER" > "$WALLPAPER_FILE"
fi

if [ -n "$WAYLAND_DISPLAY" ] && command -v swaybg >/dev/null 2>&1; then
    pkill swaybg
    swaybg -i "$WALLPAPER" &
elif command -v feh >/dev/null 2>&1; then
    feh --bg-scale "$WALLPAPER"
else
    echo "No supported wallpaper setter found. Install swaybg or feh." >&2
    exit 1
fi
