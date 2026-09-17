#!/bin/sh
# toolchain.sh — vá cờ build helper khi CC là gcc nix.
# gcc nix không thấy header/lib hệ thống (/usr/include, /usr/lib64) và
# linker nix bỏ qua RUNPATH/cache hệ thống -> helper build xong vẫn
# "cannot open shared object" lúc chạy.
# Dùng: . "$(dirname "$0")/toolchain.sh" trước khi build helper bằng
# ${CC} ${CFLAGS}. Tôn trọng CC/CFLAGS có sẵn (vd CC=/usr/bin/gcc).
TC_EXTRA=""
[ -f /usr/include/X11/Xlib.h ] && TC_EXTRA="$TC_EXTRA -isystem /usr/include"
[ -f /usr/lib64/libX11.so ] && TC_EXTRA="$TC_EXTRA -L/usr/lib64"
if command -v "${CC:-cc}" >/dev/null 2>&1; then
    case "$(readlink -f "$(command -v "${CC:-cc}")" 2>/dev/null)" in
        /nix/*) TC_EXTRA="$TC_EXTRA -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2" ;;
    esac
fi
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}$TC_EXTRA"
export CC CFLAGS
