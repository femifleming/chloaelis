#!/bin/sh
# Build the Chloaelis package for the desktop emulator (the same work-arounds as tools/fw-make.sh).
#   tools/pkg-make.sh chloaelis     -> testing/<darwin|linux>/chloaelis-<version>.pkg
HERE="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-4}"
if [ "$(uname -s)" != Darwin ]; then
  cd "$HERE" && exec make -j"$JOBS" "$@" SWIG="$HERE/tools/swig-compat.sh"
fi
GCCV="${GCC_VERSION:-$(ls /opt/homebrew/bin /usr/local/bin 2>/dev/null | sed -n 's/^gcc-\([0-9][0-9]*\)$/\1/p' | sort -n | tail -1)}"
export SDKROOT="$(xcrun --show-sdk-path)"
mkdir -p "$HERE/tools/gcc-shim"
printf '/* Homebrew GCC ships a "fixed" stdio.h made for an older macOS SDK; go straight to the real one. */\n#include "%s/usr/include/stdio.h"\n' "$SDKROOT" > "$HERE/tools/gcc-shim/stdio.h"
cd "$HERE" && exec make -j"$JOBS" "$@" GCC_VERSION="$GCCV" SWIG="$HERE/tools/swig-compat.sh" \
  CC="gcc-$GCCV -isystem $HERE/tools/gcc-shim -fmax-errors=8" CPP="g++-$GCCV -isystem $HERE/tools/gcc-shim -fmax-errors=8"
