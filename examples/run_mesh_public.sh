#!/usr/bin/env bash
# Mesh public fixtures with the product CLI (auto h0 unless POLYMESH_H is set).
# Usage: ./examples/run_mesh_public.sh [fixture_basename] [mesher]
#   fixture_basename: unit_box | l_domain | plate | cylinder_prism | all (default)
#   mesher: tet | hex | graded | hexpyr | hexvem  (default: tet)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${POLYMESH_BUILD_DIR:-$ROOT/build}"
POLYMESH="${POLYMESH_BIN:-$BUILD/apps/cli/polymesh}"
GEOM="${POLYMESH_GEOM_DIR:-$ROOT/bench/geometries/public}"
OUT="${POLYMESH_EXAMPLES_OUT:-$ROOT/examples/out}"
MESHER="${2:-tet}"
TARGET="${1:-all}"

if [[ ! -x "$POLYMESH" ]]; then
  echo "error: polymesh not found at $POLYMESH" >&2
  echo "  build first: cmake -S . -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

if [[ ! -d "$GEOM" ]]; then
  echo "error: geometry dir missing: $GEOM" >&2
  exit 1
fi

mkdir -p "$OUT"

H_ARGS=()
if [[ -n "${POLYMESH_H:-}" ]]; then
  H_ARGS=(-h "$POLYMESH_H")
fi

run_one() {
  local base="$1"
  local stl="$GEOM/${base}.stl"
  if [[ ! -f "$stl" ]]; then
    echo "error: missing $stl" >&2
    exit 1
  fi
  local vtu="$OUT/${base}_${MESHER}_mesh.vtu"
  echo "==> check $base"
  "$POLYMESH" check "$stl"
  echo "==> mesh $base (mesher=$MESHER, auto-h unless POLYMESH_H set)"
  "$POLYMESH" mesh "$stl" "${H_ARGS[@]}" --mesher "$MESHER" -o "$vtu"
  echo "    wrote $vtu"
}

if [[ "$TARGET" == "all" ]]; then
  for base in unit_box l_domain plate cylinder_prism; do
    run_one "$base"
  done
else
  run_one "$TARGET"
fi

echo "==> mesh examples OK → $OUT"
