#!/usr/bin/env bash
# Mesh public fixtures with the product CLI (auto h0 unless POLYMESH_H is set).
# Usage: ./examples/run_mesh_public.sh [fixture] [mesher]
#   fixture: unit_box | plate_hole | all (default)
#   mesher: tet | hex | graded | hexpyr | hexvem  (default: graded)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${POLYMESH_BUILD_DIR:-$ROOT/build}"
if [[ -n "${POLYMESH_BIN:-}" ]]; then
  POLYMESH="$POLYMESH_BIN"
elif command -v polymesh >/dev/null 2>&1; then
  POLYMESH="$(command -v polymesh)"
else
  POLYMESH="$BUILD/apps/cli/polymesh"
fi
GEOM="${POLYMESH_GEOM_DIR:-$ROOT/bench/geometries/public}"
OUT="${POLYMESH_EXAMPLES_OUT:-$ROOT/examples/out}"
MESHER="${2:-graded}"
TARGET="${1:-all}"

if [[ ! -x "$POLYMESH" ]]; then
  echo "error: polymesh not found (set POLYMESH_BIN, install it, or build it)" >&2
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
  local fixture
  case "$base" in
    unit_box) fixture="unit_box.step" ;;
    plate_hole) fixture="plate+hole.step" ;;
    *)
      echo "error: unknown fixture '$base' (want unit_box or plate_hole)" >&2
      exit 1
      ;;
  esac
  local cad="$GEOM/$fixture"
  if [[ ! -f "$cad" ]]; then
    echo "error: missing $cad" >&2
    exit 1
  fi
  local vtu="$OUT/${base}_${MESHER}_mesh.vtu"
  echo "==> check $base"
  "$POLYMESH" check "$cad"
  echo "==> mesh $base (mesher=$MESHER, auto-h unless POLYMESH_H set)"
  "$POLYMESH" mesh "$cad" "${H_ARGS[@]}" --mesher "$MESHER" -o "$vtu"
  echo "    wrote $vtu"
}

if [[ "$TARGET" == "all" ]]; then
  for base in unit_box plate_hole; do
    run_one "$base"
  done
else
  run_one "$TARGET"
fi

echo "==> mesh examples OK → $OUT"
