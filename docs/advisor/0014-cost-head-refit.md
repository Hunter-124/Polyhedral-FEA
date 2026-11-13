# Cost-head refit: dof, mesh_ms, solve_ms

**Status:** shipped 2026-11-13 as a head-only edit of the portable-cost graph
([0012](0012-portable-cost-retrain.md)). Trunk, normalization, clamps, OOD
test and every other output are unchanged. Evidence:
`bench/advisor/evidence/cost_head_refit.json`. Tool:
`scripts/advisor/refit_cost_heads.py`.

## Problem

`weights.json` gives `dof`, `mesh_ms` and `solve_ms` loss weight 0.0 in Stage A,
and they are not Stage-B targets. The graph therefore emitted those heads at
their initialization. On the held-out family (`bossed_plate`, fold 0 of the
family split) the `dof` head was low by about three decades (MAE 3.01 log10,
bias −3.01, Spearman −0.21). The typical prediction was a few DOF for a mesh of
thousands, so `--advisor-max-dof` never removed a candidate. `predicted_dof`,
`predicted_mesh_ms` and `predicted_solve_ms` in the decision JSON were just as
far off.

## Fix

Each of the three heads is a 96→1 Gemm on the `trunk_fc2` tap. The tool refits
that Gemm in closed form on the graph's own taps over the train split, which is
the same split the trunk was trained on (here 17,836 cost-supervised rows from 20
families). The ridge strength is chosen by leave-family-group-out inner CV on
those rows.

| head | fit | λ | clip |
| --- | --- | ---: | --- |
| `dof` | Huber ridge (IRLS, δ = 1, the training loss) | 0.1 | none |
| `mesh_ms` | squared ridge | 0.0316 | training-label range [1.125, 5.416] |
| `solve_ms` | Huber ridge | 0.1 | training-label range [−3.0, 5.075] |

`dof` is deliberately not clipped. It feeds the budget filter, and a ceiling at
the largest training mesh would let a larger mesh pass any budget above that
ceiling.

The tool asserts that every other output stays bit-identical on every row,
including all three trunk taps, `mesh_work`, `solve_flops`, `solve_bytes`,
`failure_logit` and `policy`. As a result the accuracy ranking, the feasibility
gate, the veto and `--advisor-objective efficiency` make exactly the same
decisions as before. For the same reason the `rel_err` / `rel_err_rel` /
failure / policy evidence in `crossval_final.json` and 0012 stays valid. Running
the tool on an already-refitted graph rewrites the same weights and clip bounds.

## Validation

**Held-out family, shipped trunk** (`bossed_plate`, 1,930 rows; refit on the
other 20 families):

| head | MAE before | MAE after | Spearman before → after |
| --- | ---: | ---: | --- |
| `dof` | 3.010 | 0.230 | −0.21 → 0.79 |
| `mesh_ms` | 2.515 | 0.264 | 0.08 → 0.79 |
| `solve_ms` | 2.436 | 1.086 | −0.22 → 0.67 |

**Leave-one-family-out, retrained trunks.** 21 folds × 2 seeds = 42 trunks,
each trained with the repo's `train_one_run`, the shipped curriculum weights
and the pruning ledger. These runs used a short schedule, not the production 150
runs. W-L counts folds where the refit beat the untrained head:

| head | variant | MAE before → after | folds won (RMSE / MAE) |
| --- | --- | --- | --- |
| `dof` | Huber ridge | 2.72 → 0.41 | 42–0 / 42–0 |
| `mesh_ms` | ridge + clip | 3.54 → 0.57 | 42–0 / 41–1 |
| `solve_ms` | Huber ridge | 1.96 → 1.39 | 34–8 / 37–5 |

The same comparison found the refits of `rel_err`, `rel_err_rel`, `geo_*` and
`failure` either within noise or carried by a single family, so those heads are
not edited. `mesh_work`, `solve_flops` and `solve_bytes` are trained heads that
drive the efficiency objective, and they are not edited either.

**C++ advisor, `--advisor-max-dof`.** This check loads the graph through
`polymesh::advisor::Advisor` (ONNX Runtime 1.28.0, CPU) and runs it on the
1,930 `bossed_plate` rows (20 cases). Each case's budget is its median measured
DOF. Agreement is computed per row (predicted-affordable == measured-affordable)
and averaged per case. The chooser columns come from `Advisor::explain` /
`recommend` on the 108-action candidate grid, using the part's exact-BRep
descriptors.

| | base graph | refit graph |
| --- | ---: | ---: |
| affordability agreement | 52.6 % | 80.6 % |
| rows predicted affordable | 100 % | 65.8 % |
| candidates pruned by the budget | 0 / 2,160 | 281 / 2,160 |
| cases where the budget pruned | 0 / 20 | 20 / 20 |
| decisions changed by the budget | 0 / 20 | 10 / 20 |
| budget refusals | 0 | 0 |

C++ and Python/ORT `dof` outputs agree exactly on every row. Without a budget,
all 20 decisions are identical to the base graph's. One example is
`bossed_plate_s3_c2` at a 2,292-DOF budget. The unbudgeted pick is
`graded_tet h_rel=0.06 p2`, predicted at about 11.9k DOF. With the budget the
pick becomes `hex h_rel=0.10 p2`, predicted at about 680 DOF.

## Data

The dataset was rebuilt with `scripts/build_advisor_dataset.py` from the
regenerated procedural corpus. `scripts/gen_primitive_corpus.py` needs
`cadquery-ocp==7.8.1`; 8.0 breaks the import. The rebuilt table has 35,060 rows,
against 36,010 for the 0012 cycle, and `normalization.json` reproduces from its
fold-0 train split to within 0.07 std. The SHA-256 of the table is in the
evidence file.

## Limitations and follow-up

- A head refit on a frozen trunk does not learn cost structure that the trunk
  never represented. `solve_ms` stays about one decade off on the held-out
  family, so it is a coarse estimate.
- `export_onnx.py` rewrites `model.onnx` from a checkpoint, which drops this
  edit. Re-run `refit_cost_heads.py` after any export. Alternatively, give the
  three heads a nonzero weight in the next retrain and re-evaluate the accuracy
  heads under that change. Neither has been done yet.
- The budget remains a learned filter, not a guarantee: 19.4 % of held-out rows
  still fall on the wrong side of a median budget.
