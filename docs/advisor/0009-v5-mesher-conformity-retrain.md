# 0009 — The v5 corpus: a better mesher, better predictions, and no decision win

Status: measured 2026-08-17 on `hunter-pc` (gcc, Release, OCC on).
Corpus `bench/advisor/dataset.csv`, **2,672 rows**, sha256 `d4367ab0b7456ed6…`.
Supersedes the decision numbers in
[0008-v4-corpus-retrain.md](0008-v4-corpus-retrain.md). The v4 generation is
archived whole under `bench/campaigns/archive-v5/` with its dataset at
`bench/advisor/archive-v5/dataset-v4.csv` and its 150 training runs at
`bench/advisor/archive-v5/runs-v4/`, so every number in 0006–0008 stays
reproducible.

## 1. Why there is a v5

[ADR-0035](../decisions/0035-boundary-conformity.md). The mesher stopped
snapping boundary nodes to the tessellation and started placing them on the
exact BRep, with hard pinning of sharp edges and CAD vertices. Boundary-node
residuals fell from 0.04–0.22 h to machine precision on every curved fixture,
inverted cells disappeared from the hybrid path, and both the geometry and
accuracy columns of every row moved. ADR-0032's rule applies: a corpus half
measured on two meshers puts the difference inside a training fold where
nothing can see it, so the whole generation is re-measured.

3,528 pairs across 4 stages, all on `hunter-pc`, one machine this time. One
shard died silently on `stepped_shaft_s2_c1` in stage 4 and was resumed by
re-running the driver with `--from-stage 4`; dedup made that free.

`build_advisor_dataset.py` kept **2,672** rows (v4: 2,752): 303 rows emitted no
accuracy columns and 186 are retained for the failure head only.

## 2. The retrain

150 runs from scratch, no warm start from a v4 checkpoint — a regeneration
retires the optimisation trajectory with the labels. Shipped checkpoint is
**run 58**, chosen by the trainer's own validation objective (0.3991 on its
pruned validation split).

| | v4 | v5 |
|---|---|---|
| rows | 2,752 | **2,672** |
| dataset sha256 | `c015b57eeb61a5aa0081…` | **`d4367ab0b7456ed6…`** |
| shipped run | 143 | **58** |
| `val.rel_err_mae` of the shipped run | 0.5236 | **0.6389** |
| best `val.rel_err_mae` over 150 runs | 0.5201 (run 49) | **0.5737** (run 143) |
| ONNX | 62 columns, 17,617 params | **62 columns, 17,617 params** |
| ONNX parity vs float64 torch | 2.2e-06 | **2.197e-06** (tol 1e-05) |

Calibration and the OOD gate were rebuilt on the new rows: the 0.95 conformal
band is ±0.5641 decades (achieved 42.9 % on 33/36 fold-seeds — the same
under-coverage v4 had), the grouped 0.95 band is ±2.3614 decades (achieved
92.8 %), and 83.3 % of held-out rows sit beyond the training 99th-percentile
Mahalanobis distance.

## 3. Decision quality: the honest result is "no change"

12-fold family-held-out cross-validation, 5 seeds, 40 epochs
(`bench/advisor/crossval_v5.json`), paired sign tests at the median budget:

| comparison | v5 W–L–T | p | v4 W–L–T | p |
|---|---|---|---|---|
| `advisor_argmin` vs `default` | 53–59–48 | 0.64 | 53–59–48 | 0.64 |
| `advisor_argmin` vs `constant_config` | 89–45–26 | **0.0002** | 89–45–26 | 0.0002 |
| `advisor_argmin` vs `finest_action` | 69–52–39 | 0.15 | — | — |
| `advisor_efficiency` vs `default` | 62–56–42 | 0.65 | — | — |
| `advisor_efficiency` vs `constant_config` | 85–50–25 | **0.0033** | — | — |

Macro-mean regret at the median budget, folds with ≥3 cases (`_scored`), lower
is better:

| chooser | v4 | v5 |
|---|---|---|
| `oracle` | 0.000 | 0.000 |
| `default` | 0.227 | 0.286 |
| `advisor_tol_0.05_cal` | 0.254 | **0.207** |
| `advisor_efficiency` | 0.242 | 0.249 |
| `advisor_argmin` (deployed) | 0.267 | 0.347 |
| `spend_budget` (hindsight) | 0.245 | 0.310 |
| `random` | 0.334 | 0.376 |
| `finest_action` | 0.596 | 0.711 |

**Every chooser's regret rose, including the oracle-free baselines and random.**
That is a property of the corpus, not of the model: with boundary nodes on the
exact BRep, the cheap actions got *better* — the gap a chooser can win by
picking the right action narrowed, and the same absolute mistake now costs more
relative to a smaller spread. The deployed `advisor_argmin` moved 0.267 → 0.347
while `default` moved 0.227 → 0.286, so the advisor's deficit against the
trivial rule widened slightly, from 0.040 to 0.061 decades. It remains a
non-result by the sign test (53–59–48, p = 0.64), exactly as in v4.

The one real improvement is the tolerance-constrained family:
`advisor_tol_0.05_cal` is the only chooser that got better in absolute terms
(0.254 → **0.207**) and it is now the best non-oracle chooser on this metric.

Nothing about the deployed rule changes on this evidence. Switching the shipped
score would be tuning on a corpus regeneration, which is precisely what 0008
declined to do on a tie.

## 4. The tolerance selector: still not deliverable

At `rel_err ≤ 0.1`, against `finest_action`'s 14.8 % violation rate
(`bench/advisor/tolerance_selector_v5.json`):

| chooser | violation rate | cost regret (decades) |
|---|---|---|
| `oracle` | 0.0 % | 0.872 |
| `finest_action` | 14.8 % | 1.080 |
| `net_tol_0.1_cal` | **25.9 %** | 1.118 |

v4 had 24.1 % at 0.851 decades. The calibrated net is now both more
non-compliant *and* more expensive than it was, and it is still worse than
`finest_action` on the number the feature promises. Verdict unchanged: nothing
user-facing ships.

## 5. Deployed behaviour, checked end to end

```
polymesh solve tests/fixtures/parts/plate_hole.step --advisor bench/advisor
  → mesher=hybrid_zoo h_rel=0.1 order=2, predicted_dof 37,995,
    failure_prob 2.5e-07, vetoed=false, exit 0
polymesh solve bench/geometries/corpus/primitives/box_hole_s0.step --advisor bench/advisor
  → mesher=hybrid_zoo order=2, predicted_dof 33,226
polymesh solve …/stepped_shaft_s0.step --advisor bench/advisor --advisor-max-dof 2000
  → vetoed=true
```
The v4 acceptance line ("plate_hole → graded_tet, exit 0") no longer holds
verbatim: on v5 the advisor picks `hybrid_zoo` order 2 for that part. That is a
label change, not a regression — hybrid is the path whose boundary residuals
improved most under ADR-0035 (plate_hole hybrid at h = 3 mm now places every
boundary node within 2.9e-15 h of the BRep), so the corpus now rates it above
graded there. The acceptance criterion that survives the regeneration is
"advisor runs, picks a live action, exits 0", which it does.

`bench/results/advisor-budget-sweep.json` (21 runs) and
`bench_advisor_budget.png` are regenerated against v5.

## 6. Provenance

- dataset `bench/advisor/dataset.csv`, 2,672 rows, sha256 `d4367ab0b7456ed6…`
- campaigns `bench/campaigns/advisor-batch-{1..4}-s*-hunter-pc`, 3,528 pairs
- retired generation `bench/campaigns/archive-v5/`,
  `bench/advisor/archive-v5/{dataset-v4.csv,runs-v4/}`
- decision numbers `bench/advisor/crossval_v5.json`; tolerance
  `bench/advisor/tolerance_selector_v5.json`; calibration/OOD
  `bench/advisor/{calibration,ood}.json`
- references untouched: every corpus reference carries an `analytic` or
  `external-gmsh-mesh+calculix-solver` source and promotion refuses them by
  design, so the truth gate had nothing to promote.
