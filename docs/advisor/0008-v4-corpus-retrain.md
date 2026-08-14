# 0008 — The v4 corpus, the retrain, and the fold that was eating the headline

Status: measured 2026-08-14 on `hunter-pc` and `livingroom-pc`, both gcc, at
`6ccc2cf`. Corpus `bench/advisor/dataset.csv`, 2,752 rows, sha256
`5b009fe57b111e0f9a6c…`. Supersedes the numbers in
[0006-clean-data-retrain.md](0006-clean-data-retrain.md) and answers the open
question it ended on. The tolerance verdict in
[0007-tolerance-selector.md](0007-tolerance-selector.md) is re-measured here and
does not change.

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

150 runs from scratch — no warm start from a v3 checkpoint, because a
regeneration retires the optimisation trajectory with the labels. Best
validation `rel_err_mae` 0.6893 at run 70; the shipped `best.pt` is run 102.
ONNX re-exported at 62 columns, parity 2.5e-06 relative against onnxruntime;
calibration and the OOD gate rebuilt on the v4 rows.

Deployed behaviour, checked end to end rather than asserted:

| check | v3 | v4 |
| --- | --- | --- |
| `plate_hole` through `--advisor` | graded_tet | **hybrid_zoo**, h_rel 0.1, order 2, exit 0, not vetoed |
| `unit_box` OOD veto | fires, distance 61.8 | fires, distance 44.76 |
| advisor C++ suite | 10/10 | 10/10 |

The `plate_hole` pick changed. It is a different corpus and a different model, so
the acceptance line in `HANDOFF-3080ti.md` §2a ("plate_hole → graded_tet") is a
v3 fact, not a contract; what the gate should require is exit 0 and no veto,
both of which hold.

## 3. Decision quality: case by case it holds, and slightly improves

12-fold family-held-out cross-validation, 5 seeds, 40 epochs
(`bench/advisor/crossval_v4.json`), paired sign tests at the median budget:

| comparison | v4 W–L–T | p | v3 W–L–T | p |
| --- | --- | --- | --- | --- |
| `advisor_argmin` vs `constant_config` | 83–44–38 | 0.0007 | 86–44–50 | 0.0003 |
| `advisor_argmin` vs `finest_action` | 81–38–46 | **0.0001** | 75–40–65 | 0.0014 |
| `advisor_argmin` vs `default` | 69–50–46 | 0.0985 | 72–52–56 | 0.0876 |
| `advisor_argmin` vs `spend_budget` | 53–59–53 | 0.6368 | 53–56–71 | 0.8482 |
| `advisor_policy` vs `default` | **29–46–90** | 0.0639 | 59–36–85 | 0.0235 |

The argmin chooser's win over the deployable trivial rule is stronger on clean
labels than it was on stale ones. The shipped *policy head* went the other way
and now loses to the default action case by case — it was 59–36 up, it is 29–46
down. That is the honest cost of the regeneration and it is the thing to fix
next, because the policy head is what the C++ advisor actually emits.

Macro-mean regret rose for every chooser including the ones that read no model
(`finest_action` 0.740 → 0.849, `random` 0.675 → 0.749, `constant_config` 0.615
→ 0.912 at the median budget). When the trivial rules move together, the corpus
changed difficulty; it is not a model regression.

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

`smoke_bar` is a **one-case fold** on which the advisor picks catastrophically:
~3.0 decades of regret, a thousand times worse than that case's best action,
against random's 0.909. In an unweighted macro mean over folds ranging from 1 to
6 cases, that single case shifts the advisor-minus-random gap by **+0.19
decades** — and the entire observed gap is +0.12 (v3) and +0.10 (v4).

Remove it and the ranking inverts on both corpora:

| population | v3 advisor / random / finest | v4 advisor / random / finest |
| --- | --- | --- |
| all 11 folds | 0.798 / 0.675 / 0.740 | 0.851 / 0.749 / 0.849 |
| minus `smoke_bar` | **0.577 / 0.652 / 0.814** | **0.640 / 0.733 / 0.934** |
| folds with ≥3 cases | 0.601 / 0.683 / 0.860 | 0.737 / 0.828 / 1.079 |
| median over folds | 0.584 / 0.682 | 0.701 / 0.765 |

The advisor beats random in 7 of 11 folds on both corpora, wins on the median on
both, and wins the macro mean on both as soon as the single-case fold is not
allowed to count as much as a six-case one.

So the headline number in 0006 was an artefact of unweighted macro-averaging
over wildly unequal folds, and it survived the corpus regeneration unchanged
(+0.19 vs +0.19), which is exactly what an artefact does and what a corpus
problem would not. Two things follow, and neither is "the number was wrong":

1. **`smoke_bar` is a real failure.** One case, 3 decades. It is the smallest
   reproducer of the advisor's worst behaviour that exists, and it is cheap to
   run. That is the next thing to look at.
2. **The macro mean over folds needs a minimum fold size**, or a weighting, to
   mean what it is read to mean. `regret.MIN_ACTIONS` already guards cases with
   too few actions; there is no equivalent guard on folds with too few cases.

## 5. The tolerance selector is still not deliverable

0007 re-measured on v4 (`bench/advisor/tolerance_selector_v4.json`), same folds,
same scorer. At `rel_err ≤ 0.1`, violation rate against `finest_action`'s 14.8 %:

| chooser | v3 | v4 |
| --- | --- | --- |
| `finest_action` (reference) | 14.8 % | 14.8 % |
| `lgbm_tol_0.1` | 38.9 % | **31.5 %** |
| `net_tol_0.1` | 35.2 % | 46.3 % |
| `net_tol_0.1_cal` | 29.6 % | 51.9 % |

Clean labels helped LightGBM (38.9 → 31.5 %) and hurt the net (35.2 → 46.3 %),
which sharpens 0007 §3 rather than overturning it: the better error model is the
better selector, and it still misses the tolerance twice as often as asking for
the finest mesh. Nothing user-facing ships.

## 6. Provenance

- dataset `bench/advisor/dataset.csv`, 2,752 rows, sha256 `5b009fe57b111e0f9a6c…`
- 3,528 labelled pairs under `bench/campaigns/advisor-batch-*-{hunter-pc,livingroom-pc}/`
- shipped checkpoint `bench/advisor/runs/best.pt` (run 102 of 150, cold start)
- `bench/advisor/crossval_v4.json` — 12 folds x 5 seeds, 40 epochs
- `bench/advisor/tolerance_selector_v4.json` — 12 folds, seed 1234
- v3 preserved: `bench/campaigns/archive-v4/`, `bench/advisor/archive-v4/`
  (rows, dataset and the whole 150-run v3 training history)
