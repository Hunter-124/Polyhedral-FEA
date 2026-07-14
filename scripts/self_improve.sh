#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Self-improvement / healing loop: run a brief diagnostics battery on a couple of
# CAD parts, then hand the measured report to an LLM CLI (omp or grok) asking it
# to improve the meshing / variation / geometry-representation and solver code so
# the mesher tracks the true geometry with less deviation and solves faster.
#
# The LLM edits the repo in place (headless, auto-approved). Review the diff and
# re-run this script to measure the effect — the loop tightens each pass.
#
# Usage:
#   ./scripts/self_improve.sh [--backend omp|grok] [--parts "a.step b.step"]
#                             [--max-turns N] [--dry-run]
# Env:
#   SELF_IMPROVE_BACKEND   default backend (omp|grok); overridden by --backend
#
# The GUI "self-improve" button invokes this same script.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BACKEND="${SELF_IMPROVE_BACKEND:-omp}"
PARTS="tests/fixtures/parts/pipe.step tests/fixtures/parts/cylinder.step"
MAX_TURNS=120
DRY_RUN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) BACKEND="$2"; shift 2;;
    --parts)   PARTS="$2"; shift 2;;
    --max-turns) MAX_TURNS="$2"; shift 2;;
    --dry-run) DRY_RUN=1; shift;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

CLI="build/apps/cli/polymesh"
if [[ ! -x "$CLI" ]]; then
  echo "[self-improve] building CLI..."
  cmake -S . -B build -G Ninja -DPOLYMESH_WITH_OCC=ON -DPOLYMESH_WITH_GUI=OFF \
    -DPOLYMESH_BUILD_TESTS=OFF >/dev/null
  cmake --build build --target polymesh -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="bench/self_improve/$STAMP"
mkdir -p "$OUT"
PROMPT="$OUT/PROMPT.md"

# --- measure: run diagnostics on each part ---------------------------------
REPORTS=()
for part in $PARTS; do
  if [[ ! -f "$part" ]]; then
    echo "[self-improve] skip missing part: $part" >&2
    continue
  fi
  base="$(basename "$part" .step)"
  json="$OUT/${base}.json"
  echo "[self-improve] diag $part"
  if ! "$CLI" diag "$part" --json "$json" >/dev/null 2>"$OUT/${base}.err"; then
    # Capture failures too — a crash / mesh-failure is exactly what we want fixed.
    echo "{ \"part\": \"$base\", \"error\": \"$(tr '\n' ' ' < "$OUT/${base}.err" | sed 's/"/\\"/g')\" }" > "$json"
  fi
  REPORTS+=("$json")
done

if [[ ${#REPORTS[@]} -eq 0 ]]; then
  echo "[self-improve] no parts measured; aborting" >&2
  exit 1
fi

# --- compose the improvement prompt ----------------------------------------
{
  cat <<'HDR'
# PolyMesh self-improvement pass

You are improving an adaptive **CAD → variable-polyhedral mesh → FEA solver**
pipeline (goal: a Cubit-class automatic mesher that represents geometry with
minimal deviation and solves accurately and fast, adapting element order/shape
to geometry curvature and the simulation setup / boundary conditions).

## Objective (in priority order)
1. **Geometry fidelity first.** Imported curved surfaces (pipes, fillets, holes)
   must be represented in 3D with minimal deviation from the true BRep. Improve
   tessellation / surface projection / curvature-adaptive sizing so `quality_min`
   rises and curved walls track the CAD radius.
2. **No mesh failures.** Any `error` field below is a bug to fix at the source
   (e.g. degenerate/sliver elements, non-positive volumes) — never by loosening
   a validity check.
3. **Adaptive variation.** Pack higher element density/order near edges, corners,
   curvature, and the BC/load regions; keep the bulk coarse. Use
   `pipeline::build_refinement_plan` (geometry + BC seed fusion) as the hook.
4. **Speed.** Reduce `timing_ms.mesh` / `timing_ms.solve` at equal or better
   accuracy; raise `mesh_throughput_elem_per_s`.

## Hard constraints
- CAD-only inputs (STEP/BREP); do not reintroduce STL as an input.
- Do not weaken `check_tet_fill_geometry` / `check_validity` / patch tests to pass.
- Keep the build warning-clean (`-Werror`) and `ctest` green. Run only the
  targeted tests you touch plus `test_geometry_fidelity`, `test_refinement_plan`,
  `test_local_refine`; do NOT run the whole suite or campaigns.
- Double precision only. Do not add heavy new dependencies.

## Key source files
- Geometry import / tessellation: `src/geom/src/cad_model.cpp`
- Curvature/thin-wall + BC sizing: `src/adapt/src/sizing_field.cpp`,
  `src/adapt/src/graded_sizing.cpp`, `pipeline::build_refinement_plan`
  (`src/pipeline/src/scene.cpp`)
- Variable-polyhedral mesher: `src/mesh/src/varyhedron_fill.cpp`,
  `src/mesh/src/local_refine.cpp`, `src/mesh/src/surface_project.cpp`,
  `src/mesh/src/wall_project.cpp`
- Solver / recovery: `src/fea/src/solve.cpp`, `src/fea/src/zz.cpp`

## Measured diagnostics (this pass)
Fields: import.{vertices,triangles,bbox_diag}, mesh.{h,nodes,elements,
quality_min,quality_mean,geometry_seeds,bc_seeds}, timing_ms, solve.{...}.
HDR
  echo
  for r in "${REPORTS[@]}"; do
    echo '```json'
    cat "$r"
    echo '```'
    echo
  done
  cat <<'FTR'
## Deliverable
Make focused source edits that measurably improve the objective above, then
report: what you changed, why, and the expected effect on the metrics. After
editing, the human re-runs `scripts/self_improve.sh` to confirm the metrics moved
the right way (the healing loop).
FTR
} > "$PROMPT"

echo "[self-improve] report + prompt written to $OUT/"
echo "[self-improve] backend=$BACKEND  prompt=$PROMPT"

# --- invoke the LLM CLI ----------------------------------------------------
case "$BACKEND" in
  omp)
    CMD=(omp -p --cwd "$ROOT" --auto-approve "@$PROMPT")
    ;;
  grok)
    CMD=(grok -p --yolo --permission-mode bypassPermissions --cwd "$ROOT"
         --max-turns "$MAX_TURNS"
         --deny 'Bash(git push --force*)' --deny 'Bash(git push -f*)'
         --prompt-file "$PROMPT")
    ;;
  *)
    echo "unknown backend: $BACKEND (want omp|grok)" >&2; exit 2;;
esac

echo "+ ${CMD[*]}"
if [[ "$DRY_RUN" == "1" ]]; then
  echo "[self-improve] dry-run: not invoking $BACKEND"
  exit 0
fi
if ! command -v "$BACKEND" >/dev/null 2>&1; then
  echo "[self-improve] $BACKEND CLI not found on PATH" >&2
  exit 1
fi
exec "${CMD[@]}"
