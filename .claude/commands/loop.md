---
description: Autonomous plan → build → verify → improve loop for the current phase
---

You are running the autonomous improvement loop for the current phase of
PolyMesh. Read CLAUDE.md, docs/spec.md, docs/phases.md, docs/benchmarks.md,
and docs/progress.md first. Identify the current phase and its exit criteria.
Then loop:

## 1. PLAN
Build/refresh a DAG of tasks for this phase in docs/progress.md: nodes = concrete
tasks with acceptance checks, edges = dependencies. Pick the highest-leverage
unblocked node. State in one paragraph: what you'll change, what benchmark(s)
it should move, and your predicted effect. **Write the prediction down before
implementing** — it's checked in step 3.

## 2. BUILD
Implement the node. Follow CLAUDE.md rules strictly (no hardcoded reference
values, patch test sacred, SAFETY comments, units documented). Add/extend unit
tests alongside.

## 3. VERIFY
- `clang-format -i` on every touched `*.cpp` / `*.hpp` / `*.cu` under `apps/`,
  `src/`, `tests/` (repo `.clang-format`; CI pins clang-format 18.1.8 and
  gates with `clang-format --dry-run --Werror`).
- `cmake --build build -j$(nproc) && ctest --test-dir build -j$(nproc) --output-on-failure`
  — the full suite must stay green.
- Run the Tier-0 gates, then the benchmark tiers relevant to this phase
  (Catch2 cases by name, e.g. `ctest --test-dir build -R 'patch test|MMS'
  --output-on-failure`; Tier-3 performance via `python3 bench/d6/run_tier3.py`),
  and record the report under `bench/reports/`.
- If the change can affect mesh ordering (unordered containers, smoothing,
  DOF numbering): `./scripts/check_cross_stdlib_mesh.sh` must pass — meshes
  byte-identical across libstdc++/libc++ builds (two-tree setup in the script
  header).
- Compare against the previous report AND against your step-1 prediction.
  - Prediction matched → mark node done in docs/progress.md, commit with a
    message citing the benchmark deltas.
  - Improved but for reasons you didn't predict → investigate before
    committing; unexplained improvements are how cheating hides.
  - Regressed or Tier-0 failure → diagnose, fix or revert. Never comment out,
    loosen tolerances of, or delete a failing test to make the suite pass —
    tolerance changes require a written justification in docs/decisions/.

## 4. LOOP OR STOP
- If phase exit criteria are not met and budget remains: go to 1.
- If exit criteria ARE met: run `/audit`. Only after the audit passes, write a
  phase summary in docs/progress.md (what was built, final benchmark table,
  trade-offs, open issues) and STOP for the ⛔ GATE — do not start the next
  phase without explicit human approval.
- If stuck (3 consecutive iterations with no benchmark movement): STOP, write
  up the blocker with 2–3 alternative approaches and their trade-offs, and ask.

Hard rules for the loop:
- One node per iteration. No drive-by refactors bundled with physics changes.
- Every commit leaves the tree green on Tier-0.
- Never modify anything in bench/reference/ or the frozen P1 baseline solver.
- Track cumulative wall-time/DOF metrics; report the trend, not just the latest.
