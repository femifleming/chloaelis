#!/bin/sh
# Fetch the calibration sources: units from stolmine's er-301-habitat whose CPU load was
# read off a real ER-301 (planning/house-ports-optimization.md and creamcoat-port-plan.md in
# that repository). They are compiled into the benchmark image next to the Chloaelis units so
# the model's output can be compared with hardware. The repository carries no licence file,
# so nothing from it is copied into this tree: it is cloned into build/bench/habitat (ignored).
#
#   bench/fetch-calibrators.sh [destination]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="${1:-$HERE/../build/bench/habitat}"
PIN=ed1d20ce99a04a6a4e9b53a8e16b016326b9f7d6   # v2.8.2, 2026-11-13
if [ ! -d "$DEST/.git" ]; then
  rm -rf "$DEST"
  git clone -q --filter=blob:none --no-checkout https://github.com/stolmine/er-301-habitat "$DEST"
fi
cd "$DEST"
git checkout -q "$PIN" -- mods/house planning/house-ports-optimization.md planning/creamcoat-port-plan.md
# The versions that were on the module when the readings were taken (three of the five were
# reworked later: float storage and arithmetic in place of double).
mkdir -p as-measured
git show 0a89103:mods/house/atoms/KWoodRoom.h > as-measured/KWoodRoom.h   # 29% stereo
git show a2f1db6:mods/house/atoms/WoodenBox.h > as-measured/WoodenBox.h   # 14% stereo
git show 65fb372:mods/house/atoms/Verbity.h   > as-measured/Verbity.h     # 15-20% stereo
echo "calibration sources in $DEST (pinned $PIN)"
