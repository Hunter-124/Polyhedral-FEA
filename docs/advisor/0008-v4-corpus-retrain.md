# 0008 — The v4 corpus, the retrain, and the metric that punished being right

Status: measured 2026-08-14 on `hunter-pc` and `livingroom-pc`, both gcc.
Corpus `bench/advisor/dataset.csv`, 2,752 rows, sha256 `c015b57eeb61a5aa0081…`
after the §4.1 label fix. Supersedes the numbers in
[0006-clean-data-retrain.md](0006-clean-data-retrain.md) and answers the open
question it ended on. The tolerance verdict in
[0007-tolerance-selector.md](0007-tolerance-selector.md) is re-measured here;
the gap narrows sharply but the verdict stands.

## 1. Why there is a v4 at all

[ADR-0032](../decisions/0032-stl-order-determinism.md): the mesher's output
depended on `unordered_map` iteration order, and every row on record had been
labelled on the laptop's MSVC build, which disagreed with the fixed mesher on
part of the corpus. The v3 rows are archived whole under
`bench/campaigns/archive-v4/`, with their dataset at
`bench/advisor/archive-v4/dataset-v3.csv` (2,896 rows, sha256 `8a402e910141f5…`)
so every number in 0006 and 0007 stays reproducible.

3,528 pairs re-measured across two machines: `hunter-pc` (gcc 15, 6C/12T) took
stage 1 plus box_hole and channel, `livingroom-pc` (gcc 16, 8T) took plate_notch,
sphere_box, stepped_shaft and l_bracket. Every stage now plans to `to run: 0`.

**The split produced a determinism proof nobody planned.** 488 pairs were solved
on both hosts. `status`, `n_elems`, `n_nodes`, `n_dof`, both geometry errors and
`accuracy_rel_err` agree **exactly** on all 488 — two compiler versions, two
machines, two thread counts. ADR-0032's guarantee holds at corpus scale, not
just on the four CI fixtures. `build_advisor_dataset.same_outcome` now
recognises that case instead of reporting it as an unorderable collision.

One thing the regeneration removed rather than added: the truth campaign. Every
corpus reference now carries an `analytic` or `external-gmsh-mesh+calculix-solver`
source, all of which promotion refuses by design, so its 288 order-2 adaptive
solves could not change a single reference value — measured mid-run as
`promoted=0, protected_refused=188`. The gate asks `--check-promotable` first
now and skips. Dropping it took `hunter-pc` from 72 truth rows in 76 minutes to
1,034 batch rows in 5.

## 2. The retrain

Trained twice. The first pass (150 runs, best validation `rel_err_mae` 0.6893)
is superseded: §4 found that one part carried a metric that cannot converge, and
the model had learned from it. The shipped model is the second pass, on the
corrected labels — 150 runs from scratch, no warm start from a v3 checkpoint,
because a regeneration retires the optimisation trajectory with the labels.

| | bad `smoke_bar` labels | corrected |
| --- | --- | --- |
| best validation `rel_err_mae` | 0.6893 (run 70) | **0.5201** (run 49) |
| shipped `best.pt` | run 102 | run 143 |
| `plate_hole` through `--advisor` | hybrid_zoo | **graded_tet**, cell size 0.1 of the part, order 2 |

ONNX re-exported at 62 columns, parity 2.2e-06 relative against onnxruntime;
calibration and the OOD gate rebuilt on the corrected rows.

Deployed behaviour, checked end to end rather than asserted:

| check | v3 | v4 |
| --- | --- | --- |
| `plate_hole` through `--advisor` | graded_tet | graded_tet, exit 0, not vetoed |
| `unit_box` OOD veto | fires, distance 61.8 | fires, distance 44.76 |
| advisor C++ suite | 10/10 | 10/10 |

The `HANDOFF-3080ti.md` §2a acceptance line ("plate_hole → graded_tet, exit 0")
holds on v4. It did *not* hold on the bad-label model, which is the single most
useful thing that acceptance line has ever done.

## 3. Decision quality: the advisor now leads on every reference it should

12-fold family-held-out cross-validation, 5 seeds, 40 epochs
(`bench/advisor/crossval_v4.json`), paired sign tests at the median budget, on
the corrected corpus:

| comparison | v4 W–L–T | p | v3 W–L–T | p |
| --- | --- | --- | --- | --- |
| `advisor_argmin` vs `finest_action` | 85–40–35 | **0.0001** | 75–40–65 | 0.0014 |
| `advisor_argmin` vs `constant_config` | 74–35–51 | **0.0002** | 86–44–50 | 0.0003 |
| `advisor_argmin` vs `default` | 66–53–41 | 0.2712 | 72–52–56 | 0.0876 |
| `advisor_argmin` vs `spend_budget` | 56–59–45 | 0.8522 | 53–56–71 | 0.8482 |
| `advisor_efficiency` vs `finest_action` | 88–34–38 | **0.0000** | 74–49–57 | 0.0300 |
| `advisor_efficiency` vs `default` | 69–42–49 | **0.0132** | 69–54–57 | 0.2066 |
| `advisor_policy` vs `default` | 36–49–75 | 0.1928 | 59–36–85 | 0.0235 |

Two honest readings, and they point the same way. The enumerating choosers beat
both deployable trivial rules more strongly than on the stale corpus, and
`advisor_efficiency` — argmin of predicted `rel_err` + predicted `dof`, which
needs no new head and no ONNX change — is the only chooser that also separates
from `default`. Nobody beats `spend_budget`, which ranks by *measured* cost and
is hindsight no deployed policy could have.

The `advisor_policy` head slipped from a 59–36 win over `default` to a 36–49
non-result. It is not the deployed rule — `advisor.cpp:764-790` enumerates the
38-action `candidate_grid` from `clamps.json` and takes the argmin of
`rel_err_rel` behind the failure gate — so this is the legacy single-shot path
degrading, not the product. Switching the deployed score from `rel_err_rel` to
the efficiency sum is the obvious follow-up and is **not** taken here: head to
head the two are 32–26–102, p = 0.51, and changing a shipped decision rule on a
tie is tuning, not evidence.

Macro-mean regret at the median budget, both populations (see §4 for why two):

| chooser | folds ≥3 cases | all folds |
| --- | --- | --- |
| `advisor_efficiency` | **0.689** | 0.616 |
| `advisor_argmin` | 0.726 | 0.601 |
| `spend_budget` (hindsight) | 0.741 | 0.564 |
| `random` | 0.828 | 0.687 |
| `advisor_policy` | 0.879 | 0.762 |
| `default` | 0.893 | 0.721 |
| `constant_config` | 1.049 | 0.776 |
| `finest_action` | 1.080 | 0.820 |

## 4. The open question from 0006 §4, answered: one fold was eating the mean

0006 reported that macro-mean regret ranks the learned choosers *below* `random`
and could not separate "the folds got harder" from "the advisor's failure mode
got worse". Neither is the answer. Per-fold, median over seeds, at the median
budget:

| fold | family | cases | v3 advisor | v3 random | v4 advisor | v4 random |
| --- | --- | --- | --- | --- | --- | --- |
| 8 | **smoke_bar** | **1** | **3.009** | 0.909 | **2.961** | 0.909 |
| 1 | cantilever | 1 | 1.231 | 1.143 | 1.293 | 1.143 |
| 10 | sphere_box | 6 | 1.166 | 1.098 | 1.061 | 1.213 |
| 5 | l_bracket | 6 | 0.626 | 0.682 | 0.854 | 0.765 |
| 0 | box_hole | 3 | 0.362 | 0.590 | 0.934 | 0.963 |
| 7 | plate_notch | 6 | 0.584 | 0.690 | 0.373 | 0.567 |
| 2 | channel | 6 | 0.267 | 0.355 | 0.462 | 0.631 |
| others | cylinder, icecream_cone, plate_hole, sphere | 1 each | — | — | — | — |

(Both v4 columns are the *pre-fix* run, i.e. the state that raised the question.
§4.1 has the numbers after the label was corrected.)

`smoke_bar` is a **one-case fold** on which the advisor appeared to pick
catastrophically: ~3.0 decades of regret against random's 0.909. In an
unweighted macro mean over folds ranging from 1 to 6 cases, that single case
shifted the advisor-minus-random gap by **+0.19 decades** — and the entire
observed gap was +0.12 (v3) and +0.10 (v4). It also survived the corpus
regeneration unchanged (+0.19 → +0.19), which is what a label problem does and
what a difficulty problem would not.

### 4.1 The advisor was right and the label was wrong

`smoke_bar` is a prismatic bar in uniaxial tension whose reference scored
`sigma_max` with probe kind `max_von_mises` against the hand-calc uniform value
of 1 MPa. The case clamps **all three DOFs** at `x ≈ 0`, which suppresses
Poisson contraction and puts a stress singularity on the clamped edge, so the
nodal maximum does not converge to 1 MPa — it grows as the mesh resolves the
corner. Measured over the part's 288 rows:

| | scored `max_von_mises` | scored `strain_energy` |
| --- | --- | --- |
| Spearman(degrees of freedom, relative error) | **+0.70** (more DOF, "worse") | +0.18 |
| order-1 median / max | 0.011 / 0.070 | 0.009 / 0.021 |
| order-2 median / max | **2.72 / 12.05** | 0.070 / 0.205 |
| worst measured value | σ = 1.3e7 Pa vs 1e6 reference | — |

So the advisor chose a richer discretisation, which is correct engineering, and
a divergent functional charged it three decades for being right. Every model
trained on those rows learned that order 2 is catastrophic.

This was already the project's rule and was simply never enforced. ADR-0023 and
`hand-calcs.md#cylinder` both say raw nodal σ_vm^max is a diagnostic and
prohibited as a score; `evaluate_probe` in `apps/testlab/main.cpp` even carries
the comment "references should not score this". Two legacy fixture references
still did: `smoke_bar` and `icecream_cone`, the latter documented in the same
file as "not a validated truth for this geometry".

Fixed, in the order that makes it stay fixed:

1. `load_metrics` now **rejects** any reference scoring `max_von_mises`/`max_vm`.
   The rule is enforced where references are read, not asserted in prose.
2. `smoke_bar` scores `strain_energy` = 2.5e-5 J (derived in
   `hand-calcs.md#smoke-bar`, mirroring the cylinder) plus `tip_deflection`.
3. `icecream_cone` scores nothing — the doc already says it gates BRep validity
   and mesh fidelity, not solver accuracy. Its rows stay in the campaign as
   unscored.
4. Both parts relabelled (576 pairs), corpus rebuilt, model retrained.

Result: the `smoke_bar` fold's advisor regret falls **2.961 → 1.114** (random
0.909 → 0.497), validation `rel_err_mae` improves 0.6893 → **0.5201**, and the
advisor now leads `random` on the raw macro mean too — 0.601 vs 0.687 — which is
the number 0006 flagged as damning. The prohibited probe was the whole story.

### 4.2 The macro mean still needs a fold-size guard

Independently of the bad label, an unweighted mean over folds of 1 and folds of
6 is not the number it reads as. `regret.MIN_ACTIONS` already guards cases with
too few actions; `crossval.MIN_FOLD_CASES = 3` now guards folds with too few
cases. Nothing is dropped: `macro_mean_regret` still covers every fold,
`macro_mean_regret_scored` covers the comparable ones, the tables print both,
and the ranking uses the comparable column. On this corpus 5 folds qualify and
7 (the single-case families) do not.

## 5. The tolerance selector: much closer, still not deliverable

0007 re-measured on the corrected corpus
(`bench/advisor/tolerance_selector_v4.json`), same folds, same scorer. At
`rel_err ≤ 0.1`, against `finest_action`'s 14.8 % violation rate:

| chooser | v3 | v4, bad labels | v4, corrected |
| --- | --- | --- | --- |
| `finest_action` (reference) | 14.8 % | 14.8 % | 14.8 % |
| `net_tol_0.01_cal` | 27.8 % | 42.6 % | **24.1 %** |
| `net_tol_0.1` | 35.2 % | 46.3 % | 31.5 % |
| `lgbm_tol_0.1` | 38.9 % | 31.5 % | 44.4 % |

The calibrated net now misses the tolerance 24.1 % of the time while spending
0.817 decades over the cheapest satisfying action, against `finest_action`'s
14.8 % and 1.097 — cheaper by 1.9× and non-compliant by 1.6×. The gap closed
from 3× to 1.6× on labels alone, and the "net or LightGBM" answer flipped back
to the net, which is consistent with 0007 §3 locating the limit in the label.

It still loses on the number the feature promises, so nothing user-facing ships.
The remaining gap is now small enough that a conservative selector at a *stated*
violation rate — "cheapest mesh that meets 0.1 in three cases out of four" — is
an honest product; a selector claiming the tolerance is not.

## 6. Provenance

- dataset `bench/advisor/dataset.csv`, 2,752 rows
- 3,528 labelled pairs under `bench/campaigns/advisor-batch-*-{hunter-pc,livingroom-pc}/`
- shipped checkpoint `bench/advisor/runs/best.pt` (run 143 of 150, cold start on
  corrected labels), ONNX parity 2.2e-06
- `bench/advisor/crossval_v4.json` — 12 folds x 5 seeds, 40 epochs
- `bench/advisor/tolerance_selector_v4.json` — 12 folds, seed 1234
- superseded rows: `bench/campaigns/archive-v4/prohibited-probe-rows/` (the 576
  `smoke_bar`/`icecream_cone` rows scored on the prohibited probe)
- v3 preserved: `bench/campaigns/archive-v4/`, `bench/advisor/archive-v4/`
  (rows, dataset and the whole 150-run v3 training history)
