#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh (CLI + GUI) with the CMake presets and copy the binaries into
# the repo root.
# Usage: ./build.sh            # preset `release` -> build/
#        ./build.sh Debug      # preset `debug`   -> build-debug/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

case "${1:-Release}" in
  [Dd]ebug) PRESET=debug; BIN=build-debug ;;
  *)        PRESET=release; BIN=build ;;
esac
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[polymesh] configure (preset $PRESET)..."
cmake --preset "$PRESET"

# Only the two apps copied below; the preset keeps POLYMESH_BUILD_TESTS ON so a
# later `ctest --preset $PRESET` works against the same cache.
echo "[polymesh] build (jobs=$JOBS)..."
cmake --build --preset "$PRESET" --target polymesh polymesh-gui -j"$JOBS"

cp -f "$BIN/apps/cli/polymesh" "$ROOT/polymesh"
cp -f "$BIN/apps/gui/polymesh-gui" "$ROOT/polymesh-gui"
chmod +x "$ROOT/polymesh" "$ROOT/polymesh-gui"

echo
echo "[polymesh] done. Binaries in repo root:"
echo "  $ROOT/polymesh  (from $BIN/apps/cli/polymesh)"
echo "  $ROOT/polymesh-gui  (from $BIN/apps/gui/polymesh-gui)"
echo
echo "Try:"
echo "  ./polymesh-gui bench/geometries/public/unit_box.step"
echo "  ./polymesh mesh bench/geometries/public/unit_box.step -o box.vtu"
