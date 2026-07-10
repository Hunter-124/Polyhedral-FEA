#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build PolyMesh (CLI + GUI) and copy binaries into the repo root.
# Usage: ./build.sh            # Release
#        ./build.sh Debug
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
BUILD_TYPE="${1:-Release}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[polymesh] configure ($BUILD_TYPE)..."
# Performance defaults: Release ⇒ -O3; OpenMP ON; host CPU tuning; LTO.
# Do not force POLYMESH_BUILD_TESTS=OFF — that poisons the CMake cache for
# later ctest runs. --target below still builds only the apps we install.
# Accuracy: never pass -ffast-math / -Ofast (patch tests + Tier-1 stay exact).
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OCC=OFF \
  -DPOLYMESH_WITH_CUDA=OFF \
  -DPOLYMESH_WITH_OPENMP=ON \
  -DPOLYMESH_NATIVE_ARCH=OFF \
  -DPOLYMESH_ENABLE_LTO=OFF

# Build only the apps we install to the repo root (not the full test suite).
echo "[polymesh] build (jobs=$JOBS)..."
cmake --build build --target polymesh polymesh-gui -j"$JOBS"

# Locate executables across Ninja / multi-config / legacy src/ layouts.
find_exe() {
  local name="$1"
  shift
  local p
  for p in "$@"; do
    if [[ -x "$p" && -f "$p" ]]; then
      printf '%s\n' "$p"
      return 0
    fi
  done
  return 1
}

CLI="$(find_exe polymesh \
  "build/apps/cli/polymesh" \
  "build/apps/cli/${BUILD_TYPE}/polymesh" \
  "build/src/cli/polymesh" \
  "build/polymesh" || true)"

if [[ -z "${CLI}" ]]; then
  echo "[polymesh] error: built CLI binary not found under build/" >&2
  echo "  expected e.g. build/apps/cli/polymesh" >&2
  exit 1
fi

cp -f "$CLI" "$ROOT/polymesh"
chmod +x "$ROOT/polymesh"

GUI="$(find_exe polymesh-gui \
  "build/apps/gui/polymesh-gui" \
  "build/apps/gui/${BUILD_TYPE}/polymesh-gui" \
  "build/src/gui/polymesh-gui" \
  "build/polymesh-gui" || true)"

if [[ -n "${GUI}" ]]; then
  cp -f "$GUI" "$ROOT/polymesh-gui"
  chmod +x "$ROOT/polymesh-gui"
else
  echo "[polymesh] warning: polymesh-gui not found (GUI may be disabled)" >&2
fi

echo
echo "[polymesh] done. Binaries in repo root:"
echo "  $ROOT/polymesh  (from $CLI)"
[[ -n "${GUI}" && -x "$ROOT/polymesh-gui" ]] && echo "  $ROOT/polymesh-gui  (from $GUI)"
echo
echo "Try:"
echo "  ./polymesh-gui bench/geometries/public/unit_box.stl"
echo "  ./polymesh mesh bench/geometries/public/unit_box.stl -o box.vtu"
