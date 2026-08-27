#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh and stage an install tree; --bundle also creates the Linux release payload.
# Usage: ./build.sh [--bundle] [Release|Debug]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
BUNDLE=0
if [[ "${1:-}" == "--bundle" ]]; then
  BUNDLE=1
  shift
fi
if [[ "$#" -gt 1 ]]; then
  echo "Usage: $0 [--bundle] [Release|Debug]" >&2
  exit 2
fi
BUILD_TYPE="${1:-Release}"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if [[ "$NPROC" -gt 4 ]]; then
  NPROC=4
fi
JOBS="${POLYMESH_JOBS:-$NPROC}"
PREFIX="${POLYMESH_INSTALL_PREFIX:-$ROOT/dist/polymesh}"
BUNDLE_DIR="${POLYMESH_BUNDLE_DIR:-$ROOT/dist/polymesh-bundle}"
STATIC_RUNTIME=OFF
BUILD_TARGETS=(polymesh polymesh-gui)
if [[ "$BUNDLE" -eq 1 ]]; then
  STATIC_RUNTIME=ON
  BUILD_TARGETS+=(polymesh-webd)
fi

echo "[polymesh] configure ($BUILD_TYPE)..."
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OCC=ON \
  -DPOLYMESH_WITH_CUDA=OFF \
  -DPOLYMESH_WITH_OPENMP=ON \
  -DPOLYMESH_NATIVE_ARCH=OFF \
  -DPOLYMESH_ENABLE_LTO=OFF \
  -DPOLYMESH_STATIC_RUNTIME="$STATIC_RUNTIME"

echo "[polymesh] build (jobs=$JOBS)..."
cmake --build build --target "${BUILD_TARGETS[@]}" -j"$JOBS"

echo "[polymesh] install → $PREFIX"
cmake --install build --prefix "$PREFIX"
RUN_PREFIX="$PREFIX"
if [[ "$BUNDLE" -eq 1 ]]; then
  echo "[polymesh] bundle → $BUNDLE_DIR"
  "$ROOT/scripts/bundle_linux.sh" "$PREFIX" "$BUNDLE_DIR"
  RUN_PREFIX="$BUNDLE_DIR"
fi

echo
"$RUN_PREFIX/bin/polymesh" --version
echo "[polymesh] staged release tree:"
echo "  CLI: $RUN_PREFIX/bin/polymesh"
echo "  GUI: $RUN_PREFIX/bin/polymesh-gui"
if [[ "$BUNDLE" -eq 1 ]]; then
  echo "  Web: $RUN_PREFIX/bin/polymesh-webd"
fi
echo
echo "Try:"
echo "  $RUN_PREFIX/bin/polymesh-gui $RUN_PREFIX/share/polymesh/examples/unit_box.step"
echo "  $RUN_PREFIX/bin/polymesh mesh $RUN_PREFIX/share/polymesh/examples/unit_box.step -o box.vtu"
