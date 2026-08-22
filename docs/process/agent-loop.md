# Agent loop — harness rules for finishing PolyMesh

## Source of truth
1. **`docs/advisor/0003-training-log.md`** + **`docs/training/HANDOFF-3080ti.md`** —
   active learned-mesh-advisor corpus/retrain program (ADRs 0026–0027,
   0028–0033; advisor docs `docs/advisor/0001–0008`)  
2. **`docs/plans/advisor-measure-first-program.md`** — measure-first
   methodology (scorecard, anti-cheat); its M/G program board is complete and
   frozen (2026-08-16)  
3. **`docs/ROADMAP.md`** — full epic DAG and exit criteria  
4. **`docs/progress.md`** — done log + open issues  
5. **In-session todos** — active epic only (≤12 items)  
6. **`docs/phases.md`** — formal gates (do not skip ⛔ without owner)

**Packing / CVT loops:** the **M9 baseline freeze** and the Geogram CVT lane
(M10, G0–G4) are **done** (2026-07-13), so this gate no longer blocks work;
the M5 VEM gate concluded with its verdict in
`bench/campaigns/vem-gate-m5/GATE.md`. The reward signal is still the
five-number **scorecard + accuracy probes** in `results.jsonl` / `scorecard`
— never wire PNG, residual alone, or raw nodal max stress. See the plan and
`docs/dag/interfaces.md`.

## Session start checklist
```bash
git status && git log -3 --oneline
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
# read ROADMAP “Recommended order”, pick next unblocked ID
```

## One iteration = one ROADMAP ID (or one vertical story)

| Step | Action |
|------|--------|
| Plan | State acceptance check from ROADMAP table |
| Build | Code + Catch2 test (or GUI pipeline test) |
| Verify | Full `ctest` green; GUI smoke if DISPLAY set |
| Commit | `Hunter-124`, no AI attribution, push `master` |
| Log | One bullet under PROGRESS Done |

## Parallelism
- **Safe parallel:** GUI presentation (A*) vs mesh algorithms (B/C) vs benches (E)  
- **Serial:** Anything touching frozen GATE-1 assembly/solve formulation  
- Prefer sequential on `master` for this owner workflow (no long-lived feature branches unless CI forces it)

## Stuck protocol
After 3 failed attempts on the same ID:
1. Document blocker in PROGRESS Open issues  
2. List 2–3 alternatives with trade-offs  
3. Move to next unblocked ID on the critical path  
4. Do not delete/loosen tests to force green  

## GUI verification (DISPLAY may be missing)
- Always: pipeline tests that produce `VolumeMeshOutput` / `SolveResult` the GUI consumes  
- When DISPLAY available: `build/apps/gui/polymesh-gui fixtures/...` manual smoke  
- Never require a human-only display test as the sole gate for a mesh/solver change  

## Per-change checks
Every iteration, in order: `clang-format` on the sources you touched,
`cmake --build build -j$(nproc)`, `ctest --test-dir build --output-on-failure`,
plus `scripts/check_cross_stdlib_mesh.sh` whenever mesh ordering can change.
Physics epics additionally require a prediction stated up front and a
comparison against the benchmark of record.
