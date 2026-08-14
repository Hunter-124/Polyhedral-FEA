# Training handoff — 3080 Ti box bring-up and run plan

Written 2026-08-13, at HEAD `91e08b4`. Read this before the first training run
starts. The mesher is now stable enough to label against: the tangle fix
(S7 overlapped-sheet carve, `6822ea7` + `798ef79`) was the last known
geometry-changing defect, and every fix this cycle changed graded_tet output,
so **nothing labelled before `798ef79` is trustworthy for graded_tet rows**.

## 0. What the box needs (bring-up checklist)

1. Windows or Linux both fine; Linux preferred for long unattended runs.
2. Install: git, CMake ≥ 3.26, a C++20 toolchain (MSVC 2022 or gcc-13),
   Python 3.11 + numpy + pyvista, CUDA toolkit 12.x (3080 Ti = sm_86).
3. Clone the repo; build `polymesh` + `polymesh_testlab` exactly as CI does.
   Windows: `cmd /c "scripts\msvcbuild.bat cmake --build build -j <n> --target polymesh polymesh_testlab"`.
4. Smoke test before any labelling: mesh `sphere_box_s0.step -h 0.0036
   --mesher graded` and REQUIRE `rel_err≈1.1e-04, open=0 nonmanifold=0`, and
   `icecream_cone.step -h 0.010` completing without the "buried" refusal.
   If either differs from this document, STOP — the binaries don't match the
   labels this plan assumes.
5. SSH: enable sshd, put the main workstation's key in authorized_keys.
   The driving session reaches it as `ssh://<box>/...`.

## 1. Campaign regeneration (decided: everything, after the tangle fix)

- 3,548 rows, all meshers. The 76 known-mislabelled rows and the 1,684
  graded_tet rows are the reason; per-part geometry deltas measured up to
  +653 % volume error change on sphere_box_s2 in earlier audits, and the S7
  carve changed sphere_box/channel/stepped_shaft geometry again.
- Shard by part family across both machines (this workstation + 3080 Ti);
  campaign labelling is CPU-bound, so the GPU box's 3080 Ti is idle during
  this phase — start GPU training only after its shard finishes.
- Keep `bench/results/gmsh-peer.json` untouched (Gmsh-meshed, unaffected).
- Acceptance: row counts identical to the old campaign, zero rows carrying a
  `fill-stage guard` refusal that now succeeds (spot-check the 16 configs that
  flipped in the earlier audit).

### 1.1 v4 regeneration — in flight since 2026-08-14, gcc only

The v3 rows were superseded again by ADR-0032: the mesher no longer depends on
the standard library's hash order, and every row on record was labelled on the
laptop's MSVC build, which disagrees with the fixed mesher on part of the
corpus. `bench/campaigns/advisor-*` moved whole into `archive-v4/`, and
`bench/advisor/archive-v4/dataset-v3.csv` preserves the 2,896-row dataset every
number in `docs/advisor/0006` and `0007` was measured on. `dataset.csv` is
**empty** until the campaigns land.

Scope: 3,528 batch pairs (stages 1–4) plus a **288**-pair truth campaign — the
committed truth campaign had been the stale 72-case version, which is what the
"216/288, 3 configs incomplete" gate message meant; it now covers all 96 cases,
including the tube and perforated_plate families that had no reference solve.

Split, both hosts on gcc, both verified to write byte-identical meshes
(`plate_hole` at h = 4 mm, md5 `9f13124199…`, gcc 15 here and gcc 16 there):

| host | threads | owns |
| --- | --- | --- |
| `hunter-pc` | 4 shards x 3 | the truth campaign, stage 1 (6 fixture parts x 288 cfgs), then box_hole, channel, l_bracket |
| `livingroom-pc` | 4 shards x 2 | plate_notch, sphere_box, stepped_shaft (stages 2–4) |

```sh
# hunter-pc — owns truth, so no --skip-truth
python3 scripts/advisor/regenerate_campaign.py --archive v4 \
    --host-tag hunter-pc --shards 4 --omp-threads 3 --from-stage 1
# either host, per family
python3 scripts/advisor/regenerate_campaign.py --archive v4 \
    --host-tag <host> --shards 4 --omp-threads N --from-stage 2 --skip-truth \
    --parts-glob 'bench/geometries/corpus/primitives/<family>_s[02]_c[012].case.json'
```

Resume is free: `run_batch` skips any `(part, cfg_id)` already recorded under
`bench/campaigns/advisor-*`, so re-running after a crash or a reboot re-plans
against whatever landed.

**Merging is manual and is not optional.** `--host-tag` keeps the two hosts'
campaign directories apart; nothing merges them. When `livingroom-pc` finishes,
commit and push its `bench/campaigns/advisor-*-livingroom-pc/` directories
(results only — `runs/` is per-run artifacts and is not committed), pull them on
`hunter-pc`, and rebuild:

```sh
python3 scripts/build_advisor_dataset.py    # union of every advisor-* directory
python3 scripts/advisor/promote_truth.py --require-all
```

Only then is a retrain measuring the whole corpus. A retrain on one host's half
is a family-truncated corpus, which is exactly the fold contamination the
generation split exists to prevent.

## 2. Training tracks (all four selected, in dependency order)

### 2a. Retrain the current advisor (first, cheap, de-risks the pipeline)
- Existing 44-feature classifier, existing training script, clean campaign.
- Checkpoint cadence: every epoch; keep the OOD veto (mahalanobis) —
  unit_box must still veto with distance ≈ 61.8.
- Acceptance: advisor smoke tests unchanged (plate_hole → graded_tet,
  exit 0), held-out accuracy ≥ old model on the clean labels.
- **DONE 2026-08-14** (`b27b0e6`, `dada547`). Measured in
  `docs/advisor/0006-clean-data-retrain.md`, including where it now loses: the
  net still trails LightGBM on DOF by 2.4×, and macro-mean regret ranks the
  learned choosers below `random` at the median budget.

### 2b. Learned error estimator / h-selector (second)
- Label: (part features, mesher, h) → measured rel_err and DOF from the fresh
  campaign; regression, not classification.
- Deliverable: "cheapest mesh meeting tolerance X" — directly demoable.
- **MEASURED AND NOT DELIVERABLE, 2026-08-14** —
  `docs/advisor/0007-tolerance-selector.md`. The selector needs no new head, and
  on 12 family-held-out folds it is 2–5× cheaper than "ask for the finest mesh"
  while missing the tolerance 1.3–2.6× more often, on both the net and LightGBM.
  A conservative margin does not fix it: the sweep saturates at 2.0 decades
  without reaching a 10 % violation rate. The scorer
  (`regret.cost_at_tolerance`) is now wired into `crossval.py` and
  `evaluate.py`, so any future attempt reports compliance next to regret. No
  user-facing query path was built, deliberately.

### 2c. Per-region size field GNN (the flagship, 1–3 day runs)
- Needs a NEW label pipeline: adaptive solves (`local_refine`) as ground
  truth for where refinement paid off. Design the label schema BEFORE
  generating: (surface patch graph, curvature, feature distance) →
  target h per patch.
- 1–3 days on the 3080 Ti with checkpoints every 2 h and auto-resume;
  validate on the 9-part matrix by meshing with the predicted size field and
  comparing rel_err/DOF against uniform-h at equal budget.

### 2d. Learned repair policy (research-grade, LAST)
- Do not start until 2a–2c are delivered. The S4/S6/S7 failure modes this
  cycle (pull-vs-carve, snap re-tangling) are the training curriculum:
  imitation targets from the deterministic repair passes first, RL only if
  imitation plateaus.

## 3. Run discipline

- 1–3 day ceiling per run, checkpoints mandatory, resume tested BEFORE the
  long run (kill it at 10 min and resume once).
- Every run logs: git SHA of the labelling binary, campaign snapshot hash,
  seed. A model whose provenance can't be replayed is discarded.
- Report negative results plainly; do not tune thresholds to make a run look
  finished.
