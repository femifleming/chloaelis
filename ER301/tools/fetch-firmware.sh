#!/bin/sh
# Fetches the ER-301 firmware this package is built against: the stolmine fork, at the release
# the package targets. Nothing of it is kept in this repository (it is MIT-licensed, (c) Brian
# Clarkson / Orthogonal Devices and stolmine: see its LICENSE.md).
#
#   tools/fetch-firmware.sh            both trees
#   tools/fetch-firmware.sh release    er-301-release/  headers for the module build (make ARCH=am335x)
#   tools/fetch-firmware.sh emu        er-301/          the tree the desktop emulator is built from
#
# The emulator tree is the same source; on macOS it gets one small file the upstream tree lacks
# (tools/macos-emulator-link-fix.patch), without which the macOS emulator does not link.
set -e
URL=https://github.com/stolmine/er-301-stolmine
TAG=v0.7.0-stolmine.9.7.0
COMMIT=2fbb7ce11dbef608ef0ade6a551fb0cf3cd90bde
HERE="$(cd "$(dirname "$0")/.." && pwd)"
want="${1:-both}"

fetch() {
  dir="$HERE/$1"
  if [ -d "$dir/.git" ]; then
    echo "$1: already here ($(git -C "$dir" rev-parse --short HEAD))"
  else
    git clone --depth 1 --branch "$TAG" "$URL" "$dir"
  fi
  have="$(git -C "$dir" rev-parse HEAD)"
  [ "$1" != er-301-release ] || [ "$have" = "$COMMIT" ] || { echo "$1 is at $have, expected $COMMIT ($TAG)" >&2; exit 1; }
  # the firmware names its version after the tag it finds
  git -C "$dir" describe --tags >/dev/null 2>&1 || git -C "$dir" tag "$TAG"
}

case "$want" in release|both) fetch er-301-release ;; esac
case "$want" in emu|both)
  fetch er-301
  if [ "$(uname -s)" = Darwin ] && [ ! -f "$HERE/er-301/arch/darwin/hal/dynload.cpp" ]; then
    git -C "$HERE/er-301" apply "$HERE/tools/macos-emulator-link-fix.patch"
    echo "er-301: applied the macOS emulator link fix"
  fi ;;
esac
