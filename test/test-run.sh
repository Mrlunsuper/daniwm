#!/bin/bash
set -euo pipefail
TDIR=$(cd "$(dirname "$0")" && pwd)
. "$TDIR/toolchain.sh"
H=$(mktemp -d)
trap 'rm -rf "$H"' EXIT
$CC $CFLAGS -D_DEFAULT_SOURCE $(pkg-config --cflags xft) "$TDIR/run-unit.c" \
    -o "$H/run-unit" $(pkg-config --libs xft) -lX11 -lm
HOME="$H/home" XDG_CACHE_HOME="$H/cache/nested" "$H/run-unit"
python3 "$TDIR/run-regression.py"
