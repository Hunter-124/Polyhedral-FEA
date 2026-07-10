#!/usr/bin/env bash
# PolyMesh competitive smoke: build (if needed) + Tier-0/Tier-1 ctest subset.
# Full peer harness (CalculiX/Elmer decks) lands later under peers/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${POLYMESH_BUILD_DIR:-$ROOT/build}"
CONFIG="${POLYMESH_BUILD_TYPE:-Release}"

cd "$ROOT"

if [[ ! -f "$BUILD/build.ninja" && ! -f "$BUILD/Makefile" ]]; then
  echo "==> configuring $BUILD ($CONFIG)"
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
fi

echo "==> building"
cmake --build "$BUILD"

# Catch2 discovers test names from section titles. Match Tier-0 gates and
# primary Tier-1 analytical cases used on the scoreboard.
# Full peer comparison is not wired yet — this only proves PolyMesh still passes.
echo "==> ctest smoke (Tier-0 + Tier-1 subset)"
ctest --test-dir "$BUILD" --output-on-failure -R \
  'patch test|rigid-body|eigenvalue|cantilever|Lamé|Kirsch|Goodier|L-domain'

echo "==> smoke OK"
echo "    Peer runners: see bench/competitive/peers/ (CalculiX stub first)."
echo "    Refresh scoreboard: python3 bench/competitive/render_scoreboard.py"
