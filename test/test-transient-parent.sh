#!/bin/sh
set -eu
exec python3 "$(dirname "$0")/test-transient-parent.py"
