#!/bin/sh
# Offline: compiles real overview.c with server calls stubbed; no X libraries linked.
set -eu
cd "$(dirname "$0")/.."
. ./test/toolchain.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Exercise the real unmanage body without pulling in unrelated client machinery.
python3 - "$tmp/unmanage.h" <<'PY'
import pathlib, sys
source = pathlib.Path('src/client.c').read_text()
start = source.index('void unmanage(Window w) {')
body = source[start:]
assert body.rstrip().endswith('}')
pathlib.Path(sys.argv[1]).write_text(body)
PY
# Stub macros intentionally discard arguments from the production drawing code.
$CC $CFLAGS -std=c11 $(pkg-config --cflags xft) -Isrc -I"$tmp" test/test-overview.c -o "$tmp/overview"
"$tmp/overview"
