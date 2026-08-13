#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Land the advisor contract cutover as ONE coherent change, inside one build
# window. Run from the repository root.
#
#   bash scripts/advisor/land_contract_cutover.sh [--build-dir build]
#
# WHY A SCRIPT. The cutover is not a single edit: the C++ expects a 43-input /
# 7-output contract, while the artifacts on disk are still 44/10. Between those
# two states the advisor tests fail by construction, so the window has to be
# short and the order is not negotiable. Anything done by hand at 5 steps is a
# chance to land half of it.
#
# WHAT IS ALREADY DONE (no build needed, already on disk):
#   - dataset.py drops p_elevate and trims ORDER_CHOICES to [1,2]
#   - clamps.json gains candidate_grid; advisor.cpp parses and enumerates it
#   - advisor.cpp recommend() is the gated enumerate-and-argmin chooser
#   - the action_dims width check is 3 + orders + meshers
#
# WHAT THIS SCRIPT DOES, in order, stopping at the first failure:
#   1. retrain so the checkpoint matches the new schema
#   2. re-export model.onnx + normalization.json + clamps.json from it
#   3. regenerate the tiny parity fixture
#   4. configure + build
#   5. run the advisor tests
#   6. measure recommend() latency for the record (G9)
#
# STILL A MANUAL PREREQUISITE, deliberately not automated because it needs
# judgement and cannot be verified from here:
#   - tests/test_advisor_inference.cpp:123-232 re-derives the SINGLE-SHOT clamp/
#     argmax/veto math and cross-checks it against recommend(). That derivation
#     is of the old rule and WILL fail against the enumerating one. It must be
#     rewritten to re-derive the enumeration first. Step 5 will fail loudly if
#     it has not been.
set -euo pipefail

BUILD_DIR="build"
if [[ "${1:-}" == "--build-dir" ]]; then BUILD_DIR="${2:?--build-dir needs a value}"; fi

# WHICH INPUT SET SHIPS. This is a decision, not a default, so it is stated here
# rather than inherited from whatever happens to be on disk.
#
# The 15 offline CAD descriptors are EXCLUDED from the shipped model. They were
# measured to make held-out-family regret worse (q0.5 gated 0.3468 -> 0.4390),
# because a 1-NN classifier recovers the family from them alone at 32/32: on a
# corpus this narrow they are family identifiers, not transferable physics.
# Shipping them would enlarge the contract to 58 inputs to carry a signal that
# measurably hurts.
#
# They are still used, but for abstention rather than prediction: the OOD gate
# in bench/advisor/ood.json is fitted over them and flags 100% of held-out-family
# rows at a 1% in-sample false-alarm rate. Wiring that gate into C++ needs those
# descriptors computed at inference time in C++ (they are OCP/Python today), so
# it is NOT part of this window and the 0.5 feasibility veto stays in place until
# it is.
#
# Set ADVISOR_SHIP_GEOMETRY_FEATURES=1 to override and ship the 58-input contract.
if [[ -z "${ADVISOR_SHIP_GEOMETRY_FEATURES:-}" ]]; then
    export ADVISOR_NO_GEOMETRY_FEATURES=1
    echo "shipping the 43-input contract (offline CAD descriptors excluded)"
else
    echo "shipping the 58-input contract (offline CAD descriptors INCLUDED)"
fi

say() { printf '\n=== %s ===\n' "$1"; }
require() { [[ -f "$1" ]] || { printf 'missing %s\n' "$1" >&2; exit 1; }; }

say "0. preconditions"
require bench/advisor/dataset.csv
require src/advisor/src/advisor.cpp
python - <<'PY'
import sys; sys.path.insert(0, "scripts")
from advisor.dataset import load_dataset
d = load_dataset()
assert d.n_actions == 3 + len(d.order_choices) + len(d.mesher_choices), d.n_actions
assert "p_elevate" not in d.input_columns, "p_elevate still in the input vector"
assert d.order_choices == [1, 2], d.order_choices
grid = d.clamps.get("candidate_grid")
assert grid and grid.get("n_candidates"), "clamps payload carries no candidate_grid"
print(f"python side ready: {d.n_inputs} inputs, {d.n_actions} actions, "
      f"{grid['n_candidates']} candidates")
PY

say "1. retrain on the new schema (fresh, no warm start across a schema change)"
rm -f bench/advisor/runs/latest.pt
python scripts/advisor/train.py --runs 30

say "2. re-export the graph and the artifacts it must agree with"
python scripts/advisor/export_onnx.py

say "3. regenerate the parity fixture"
python scripts/advisor/export_onnx.py --tiny-fixture

say "4. configure and build"
cmake -S . -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" --parallel

say "5. advisor tests"
ctest --test-dir "$BUILD_DIR" -R advisor --output-on-failure

say "6. latency for the record (G9)"
python - <<'PY'
import json, pathlib
clamps = json.loads(pathlib.Path("bench/advisor/clamps.json").read_text())
grid = clamps.get("candidate_grid", {})
onnx = pathlib.Path("bench/advisor/model.onnx")
print(f"candidates per recommend(): {grid.get('n_candidates')} "
      f"(was 2 forward passes under the single-shot rule)")
print(f"model.onnx: {onnx.stat().st_size / 1024:.1f} KiB")
print("Run the C++ side with --advisor-bench once that flag exists; until then "
      "this records the candidate count and model size only, which is the honest "
      "state rather than an estimate presented as a measurement.")
PY

say "done"
cat <<'EOF'
Landed as one change. Verify before committing:
  - bench/advisor/{model.onnx,normalization.json,clamps.json} all regenerated together
  - normalization.json input_columns has no p_elevate
  - clamps.json order_choices == [1,2] and action_dims width == 7
  - tests/fixtures/advisor_tiny/* regenerated in the same run
  - the checkpoint carries a provenance block naming the dataset sha256
EOF
