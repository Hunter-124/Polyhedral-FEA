#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh with the CMake presets and stage an install tree; --bundle also
# creates the Linux release payload.
# Usage: ./build.sh [--bundle] [Release|Debug]
#        Release -> preset `release` (build/), Debug -> preset `debug` (build-debug/)
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
case "${1:-Release}" in
  [Dd]ebug) PRESET=debug; BUILD_DIR=build-debug ;;
  *)        PRESET=release; BUILD_DIR=build ;;
esac
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

echo "[polymesh] configure (preset $PRESET)..."
cmake --preset "$PRESET" -DPOLYMESH_STATIC_RUNTIME="$STATIC_RUNTIME"

echo "[polymesh] build (jobs=$JOBS)..."
cmake --build --preset "$PRESET" --target "${BUILD_TARGETS[@]}" -j"$JOBS"

echo "[polymesh] install → $PREFIX"
cmake --install "$BUILD_DIR" --prefix "$PREFIX"
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
