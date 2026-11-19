#!/bin/sh
# Run a make target in the ER-301 firmware tree (er-301/, see tools/fetch-firmware.sh).
#   tools/fw-make.sh emu      the emulator
#   tools/fw-make.sh core     the core unit package the emulator needs
# On macOS it works round two problems of building with Homebrew GCC: its stale "fixed" stdio.h
# (tools/gcc-shim, made here) and a flag newer SWIG no longer accepts (tools/swig-compat.sh).
HERE="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-4}"
if [ "$(uname -s)" != Darwin ]; then
  cd "$HERE/er-301" && exec make -j"$JOBS" "$@" SWIG="$HERE/tools/swig-compat.sh"
fi
GCCV="${GCC_VERSION:-$(ls /opt/homebrew/bin /usr/local/bin 2>/dev/null | sed -n 's/^gcc-\([0-9][0-9]*\)$/\1/p' | sort -n | tail -1)}"
export SDKROOT="$(xcrun --show-sdk-path)"
mkdir -p "$HERE/tools/gcc-shim"
printf '/* Homebrew GCC ships a "fixed" stdio.h made for an older macOS SDK; go straight to the real one. */\n#include "%s/usr/include/stdio.h"\n' "$SDKROOT" > "$HERE/tools/gcc-shim/stdio.h"
cd "$HERE/er-301" && exec make -j"$JOBS" "$@" GCC_VERSION="$GCCV" SWIG="$HERE/tools/swig-compat.sh" \
  CC="gcc-$GCCV -isystem $HERE/tools/gcc-shim -fmax-errors=5" CPP="g++-$GCCV -isystem $HERE/tools/gcc-shim -fmax-errors=5"
