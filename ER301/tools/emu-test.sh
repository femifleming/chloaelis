#!/bin/sh
# Run the headless-emulator tests in tests/emu (TAP output). Needs tools/fw-make.sh emu core
# and tools/pkg-make.sh chloaelis to have been run. Screenshots go to build/shots.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ARCH_DIR="${ARCH_DIR:-$(ls -d "$HERE"/er-301/testing/* 2>/dev/null | head -1 | xargs basename)}"
PKGS="$HERE/testing/$ARCH_DIR"
cp -f "$HERE"/er-301/testing/$ARCH_DIR/mods/core-*.pkg "$PKGS/" 2>/dev/null
# the tests name the screenshot folder as @SHOTS@; skip tests of packages that are not built
RUN="$HERE/build/emu-tests"
rm -rf "$RUN"; mkdir -p "$RUN" "$HERE/build/shots"
for t in "$HERE"/tests/emu/*.test; do
  ok=1
  for p in $(sed -n 's/^!packages //p' "$t"); do ls "$PKGS"/$p-*.pkg >/dev/null 2>&1 || ok=0; done
  [ $ok = 1 ] && sed "s|@SHOTS@|$HERE/build/shots|g" "$t" > "$RUN/$(basename "$t")"
done
cd "$HERE/er-301" && STOL_EMU_BIN="$PWD/testing/$ARCH_DIR/emu/emu.elf" STOL_EMU_TESTS_DIR="$RUN" \
  STOL_EMU_TEST_TIMEOUT="${STOL_EMU_TEST_TIMEOUT:-180}" STOL_EMU_GOLDEN_DIR="$HERE/tests/emu/goldens" STOL_EMU_PKG_DIR="$PKGS" exec python3 tools/emu_test.py "$@"
