# 0006 — The clean-data retrain, and what it cost the advisor's claims

Status: measured 2026-08-14 at `b27b0e6`, on the campaign regenerated after the
mesher untangle (`798ef79`). Companion to
[0003-training-log.md](0003-training-log.md) and
[0004-model-card.md](0004-model-card.md); everything here supersedes numbers in
those files that were measured on the pre-untangle rows.

## 1. The old labels were a different mesher, not a stale one

3,672 rows re-measured from the STEP files. Over the 1,944 `(part, cfg_id)`
pairs present in both campaigns:

| change | pairs |
|---|---|
| refusal → success | 162 |
| fill-stage guard → feature-unresolved refusal | 48 |
| success → refusal | 12 |
| class unchanged | 1,696 |

Of the 1,036 pairs that succeeded in both campaigns, **1,012 (97.7 %) report a
different geometry volume error** — 305 better, 141 worse, 590 within 0.1 %.
Corpus-wide, successes went 54.7 % → 70.0 %, fill-stage guard refusals
28.4 % → 19.2 %, and adaptive-solve failures 85 → 15.

One family moved the wrong way and is recorded rather than buried:
`channel_s0` at `h_rel = 0.12` with `feature_refine` off (673 elements) went
0.0168 → 0.0363. With feature refinement on, the same part at the same `h`
measures 0.00728 through the CLI. The overlap carve deletes cells the coarse
unrefined fill cannot spare; the advisor's job is to not choose that config,
but the mechanism is unexplained and is the first thing to look at if coarse
volume error matters later.

## 2. `latest.pt` was never the model worth shipping

It is the trainer's resume point — the last run — and validation oscillates.
On the first clean retrain the final run was 13 % worse than the best on
`rel_err_rel`, 9 % on `geo_p99`, 81 % on `solve_ms`, and `evaluate.py`,
`export_onnx.py` and the model card all read it. `best.pt` now tracks the best
validation `rel_err_rel` within the current stage; `latest.pt` keeps its job,
because freezing it at the best run would make warm starting a greedy hill
climb.

## 3. The cost heads were missing the scale law

`n ~ volume / h^3`, the cost heads are log10 targets — and `h` is not an input.
Only the dimensionless `h_rel` is, and `volume` spans decades, which
standardisation compresses. Four derived inputs (`log10_volume`, `log10_diag`,
`log10_h`, `log10_cells = log10(volume) - 3·log10(h)`) are now computed in
`dataset.py:derived_features` and mirrored in `advisor.cpp:apply_action` —
mirrored, because they depend on the action through `h_rel`, and a missing
column is imputed to the training median, i.e. every part scored as
average-sized.

Validation MAE in log10, family-held-out fold 0:

| head | LightGBM | net, no scale | net, with scale |
|---|---|---|---|
| rel_err | 0.6553 | 0.6048 | **0.5587** |
| rel_err_rel | 0.3848 | **0.3463** | 0.3541 |
| geo_chamfer | **0.3740** | 0.9059 | 0.4316 |
| geo_p99 | **0.3573** | 0.8159 | 0.3814 |
| dof | **0.0661** | 0.6574 | 0.1567 |
| mesh_ms | **0.1001** | 0.8312 | 0.2405 |
| solve_ms | **0.1161** | 0.3184 | 0.1774 |

The net leads on accuracy and is the only model that also emits a policy and a
failure head, so it ships — but **LightGBM still predicts DOF 2.4× better and
mesh time 1.7× better**, and no claim that the net is the best available cost
estimator is supportable.

## 4. What the retrain did to the product claim

12-fold family-held-out cross-validation, 5 seeds, 40 epochs
(`bench/advisor/crossval_v3.json`), paired per-case sign tests at the primary
budget quantile:

| comparison | W–L–T | p |
|---|---|---|
| advisor_argmin vs constant_config | 86–44–50 | 0.0003 |
| advisor_argmin vs finest_action | 75–40–65 | 0.0014 |
| advisor_argmin vs default | 72–52–56 | 0.088 |
| advisor_argmin vs spend_budget | 53–56–71 | 0.85 |

So case by case the advisor still beats picking one config for everything and
beats picking the finest action, and ties the budget-spender.

**But the macro-mean regret ranking says the opposite**, and it is the number
that changed most against us. At the median budget the learned choosers rank
*below* `random` (advisor_argmin 0.798 vs random 0.675 in log10 regret); on the
stale rows the same chooser led at 0.340 vs 0.408. A handful of folds where the
advisor picks a catastrophic action dominate the mean. Two readings are
consistent with the evidence — the corpus grew to 96 parts and the folds are
harder, or the advisor's failure mode got worse — and this document does not
choose between them, because nothing measured here separates them. The next
measurement is per-fold regret against fold difficulty on both datasets.

The failure head remains near chance (validation AUC 0.51–0.75 across runs,
0.64 on the shipped checkpoint). It was near chance before the regeneration
too; it is not a casualty of the clean data.

### 4.1 The obvious fix for it was tried and is wrong

`build_advisor_dataset.py` drops every row testlab marks
`advisor_training_eligible: false` — a resolution refusal that produced no
mesh. On the regenerated campaign that is **776 of the 1,101 refusals**, so the
model was trained on a corpus in which the engine's most common refusal never
happens. Restoring them as failure-head-only rows (`dataset.py` already masks
every failure row out of every regression head) is the obvious repair, and it
is a regression on every axis. Same held-out fold, same masked rows, train
failure share 8 % → 28 %:

| head | refusals excluded | refusals kept |
|---|---|---|
| rel_err | **0.559** | 0.711 |
| rel_err_rel | **0.354** | 0.477 |
| dof | **0.157** | 0.299 |
| mesh_ms | **0.241** | 0.475 |
| failure AUC | **0.72** | 0.53 |

The failure head got *worse* with 3.5× more failure examples, which rules out
"not enough examples" and points at the label. Trained directly on it,
LightGBM scores AUC 0.874 and 0.780 on two held-out families, 0.432 on a
third, and **0.372 on box_hole — worse than chance**, mean 0.614 over the four
folds with both classes present. Refusal is family-local: what other families
teach about it is actively misleading on some parts. Until a feature carries
the refusal boundary itself — most plausibly the ratio of the smallest
resolvable feature to the requested `h`, which the guard actually tests — these
rows are noise with a label attached, and the exclusion stands as a measured
result rather than an oversight.

## 5. Provenance

- dataset `bench/advisor/dataset.csv`, 2,896 rows, sha256 `d911eb4f7af9a7cb…`
- shipped checkpoint `bench/advisor/runs/best.pt`, run 149, stage B
- ONNX contract 62 columns (was 58), parity 1.5e-06 relative vs onnxruntime
- calibration and OOD gate rebuilt on the same rows
- pre-regeneration campaign preserved whole at `bench/campaigns/archive-v3/`
  (32 directories, 3,352 rows) and the pre-scale-feature training run at
  `bench/advisor/archive-v3/runs-v3-noscale/`
