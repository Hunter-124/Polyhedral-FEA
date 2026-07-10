#!/usr/bin/env bash
# Solve public fixtures with cantilever-style BCs (auto h0 unless POLYMESH_H is set).
# Usage: ./examples/run_solve_public.sh [fixture_basename] [mesher]
#   fixture_basename: unit_box | l_domain | plate | cylinder_prism | all (default: unit_box + plate)
#   mesher: tet | hex | graded | hexpyr | hexvem  (default: tet)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${POLYMESH_BUILD_DIR:-$ROOT/build}"
POLYMESH="${POLYMESH_BIN:-$BUILD/apps/cli/polymesh}"
GEOM="${POLYMESH_GEOM_DIR:-$ROOT/bench/geometries/public}"
OUT="${POLYMESH_EXAMPLES_OUT:-$ROOT/examples/out}"
MESHER="${2:-tet}"
TARGET="${1:-smoke}"

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

# Defaults match CLI: steel-ish E, nu.
E="${POLYMESH_E:-200e9}"
NU="${POLYMESH_NU:-0.3}"

run_one() {
  local base="$1"
  local stl="$GEOM/${base}.stl"
  if [[ ! -f "$stl" ]]; then
    echo "error: missing $stl" >&2
    exit 1
  fi
  local vtu="$OUT/${base}_${MESHER}_solve.vtu"
  echo "==> solve $base (mesher=$MESHER, E=$E Pa, nu=$NU)"
  "$POLYMESH" solve "$stl" "${H_ARGS[@]}" --mesher "$MESHER" \
    -E "$E" -nu "$NU" -o "$vtu"
  echo "    wrote $vtu"
}

case "$TARGET" in
  all)
    for base in unit_box l_domain plate cylinder_prism; do
      run_one "$base"
    done
    ;;
  smoke)
    # Fast default: box + thin plate only.
    run_one unit_box
    run_one plate
    ;;
  *)
    run_one "$TARGET"
    ;;
esac

echo "==> solve examples OK → $OUT"
