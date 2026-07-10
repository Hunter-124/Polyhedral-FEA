#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh (CLI + GUI) and copy binaries into the repo root.
# Usage: ./build.sh            # Release
#        ./build.sh Debug
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
BUILD_TYPE="${1:-Release}"

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OCC=OFF \
  -DPOLYMESH_WITH_CUDA=OFF
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cp -f build/apps/cli/polymesh "$ROOT/polymesh"
if [[ -x build/apps/gui/polymesh-gui ]]; then
  cp -f build/apps/gui/polymesh-gui "$ROOT/polymesh-gui"
fi

echo
echo "[polymesh] done. Binaries in repo root:"
echo "  $ROOT/polymesh"
[[ -x "$ROOT/polymesh-gui" ]] && echo "  $ROOT/polymesh-gui"
echo
echo "Try:"
echo "  ./polymesh-gui bench/geometries/public/unit_box.stl"
echo "  ./polymesh mesh bench/geometries/public/unit_box.stl -o box.vtu"
