#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh and stage the same relocatable install tree that CPack ships.
# Usage: ./build.sh [Release|Debug]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
BUILD_TYPE="${1:-Release}"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if [[ "$NPROC" -gt 4 ]]; then
  NPROC=4
fi
JOBS="${POLYMESH_JOBS:-$NPROC}"
PREFIX="${POLYMESH_INSTALL_PREFIX:-$ROOT/dist/polymesh}"

echo "[polymesh] configure ($BUILD_TYPE)..."
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OCC=ON \
  -DPOLYMESH_WITH_CUDA=OFF \
  -DPOLYMESH_WITH_OPENMP=ON \
  -DPOLYMESH_NATIVE_ARCH=OFF \
  -DPOLYMESH_ENABLE_LTO=OFF

echo "[polymesh] build (jobs=$JOBS)..."
cmake --build build --target polymesh polymesh-gui -j"$JOBS"

echo "[polymesh] install → $PREFIX"
cmake --install build --prefix "$PREFIX"

echo
"$PREFIX/bin/polymesh" --version
echo "[polymesh] staged release tree:"
echo "  CLI: $PREFIX/bin/polymesh"
echo "  GUI: $PREFIX/bin/polymesh-gui"
echo
echo "Try:"
echo "  $PREFIX/bin/polymesh-gui $PREFIX/share/polymesh/examples/unit_box.step"
echo "  $PREFIX/bin/polymesh mesh $PREFIX/share/polymesh/examples/unit_box.step -o box.vtu"
