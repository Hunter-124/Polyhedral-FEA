# 0010 — The v6 corpus: the geometry objective stopped discriminating

Status: **landed**, 2026-08-17. Supersedes the corpus of
[0009](0009-v5-mesher-conformity-retrain.md); the deployed decision rule is
again unchanged.

## 1. Why there is a v6

ADR-0035's second wave changed mesher output on every part with a curved face:
the exterior conformity gate, the assembly-truth integrability predicate, the
conforming hex relief and the facet-kink phase all move nodes or cells in the
mesh that ships. Every advisor label is a *measurement of a mesh*, so every
label taken against the previous mesher is stale. This is ADR-0032's rule, and
it fires whether or not the change was an improvement.

The v5 generation is archived whole: `bench/campaigns/archive-v6/` (the 24
campaign directories) and `bench/advisor/archive-v6/{dataset-v5.csv,
dataset_schema-v5.json,runs-v5/}`.

## 2. The retrain

Regenerated with `scripts/advisor/regenerate_campaign.py --archive v6
--host-tag hunter-pc --shards 6 --omp-threads 2`, four stages, 3,528 pairs,
97.9 min wall. Dataset: **2,660 rows**, sha256 `9489f0d3b275340164…`.

Trained from a fresh run series (`bench/advisor/runs/` removed first, so run 001
is cold and each later run warm-starts from its predecessor), 150 runs,
`--threads 12`. Shipped run **126**, val `rel_err` MAE 0.3473. Exported at
opset 17, D = 62, A = 9, 17,617 parameters, onnxruntime parity 1.917e-06
relative.

One operational note worth recording: `train.py`'s own improvement stop ended
the first invocation at run 019, so the 150 runs took two invocations (19 + 131).
The run series is continuous and `runs/history.jsonl` carries all 150.

## 3. Prediction improved; the decision is still a tie

Macro mean regret, unconstrained level, v5 → v6:

| head | oracle | advisor_argmin | default | finest_action | spend_budget |
|---|---|---|---|---|---|
| rel_err | 0 → 0 | 0.917 → **0.869** | 1.031 → 0.970 | 0.596 → 0.661 | 0.618 → 0.645 |
| efficiency | 0.362 → 0.426 | 1.194 → 1.141 | 0.923 → 0.966 | 1.063 → 1.267 | 1.541 → 1.708 |
| geo_p99 | 0.698 → **0.288** | 1.356 → **0.491** | 1.286 → **0.146** | 0.086 → 0.047 | 0.058 → 0.059 |
| solve_ms | 2.049 → 1.962 | 2.033 → 1.886 | 1.230 → 1.236 | 2.300 → 2.405 | 2.977 → 3.079 |

Paired pooled, `advisor_argmin` against each baseline:

| comparison | record | p |
|---|---|---|
| vs `default` | 54W-58L-33T | 0.777 |
| vs `constant_config` | 86W-45L-14T | **0.00043** |
| vs `finest_action` | 50W-60L-35T | 0.391 |
| vs `spend_budget` | 35W-68L-42T | **0.0015** |

The headline is the same non-result 0008 and 0009 reported: the advisor beats a
single constant configuration decisively and does not beat `default`
(54W-58L, p = 0.78 — v5 was 53W-59L, p = 0.64). Predictions did improve on the
accuracy head (0.917 → 0.869), which is what a cleaner corpus should buy.

## 4. The interesting result: conformity removed a decision problem

Look at the `geo_p99` row. `default`'s macro regret fell **1.286 → 0.146** and
the oracle's fell 0.698 → 0.288. That is not the model getting better at
geometry; it is the *mesher* getting better at geometry on every action, so the
spread between actions on the geometry objective collapsed. When every action
lands boundary nodes on the exact BRep, there is very little left for a chooser
to choose about geometry — the default action is already within 15% of oracle.

This is worth stating plainly because it inverts the usual reading: an advisor
head whose regret goes to zero has not become smart, it has run out of decision
to make. The accuracy and cost heads still discriminate; `geo_p99` mostly no
longer does, and any future claim built on it should be checked against that.

## 5. The deployed rule is unchanged, and one deployed bug was fixed

`advisor.cpp` still ranks the 38-action grid by argmin of the centred `rel_err`
score. Changing the shipped score on a corpus regeneration would be tuning, not
evidence — the same call 0008 and 0009 made, on the same tie.

The retrain did expose a real defect in the deployed path. The v6 model picks
`h_rel = 0.2` on plate_hole, the Cartesian fill refuses that mesh outright
("feature unresolved … a hole/void smaller than that level can disappear"), and
`polymesh solve --advisor` therefore **exited 1 on the flagship fixture**. The
model is not wrong to want a coarse mesh — `h_rel` is a scale-free fraction of
the bounding diagonal and carries no knowledge of this part's hole — so the fix
belongs at the boundary between the decision and the engine.

Two scalar pre-flight proxies were tried and both over-clamp:

- 2 × shortest CAD feature length takes the cylinder from h = 24.5 mm to
  9.8 mm, though it meshes cleanly at 24.5 mm.
- smallest CAD face size takes the cone from 17.7 mm to 10.6 mm, likewise.
- clamping to the auto h0 is worse still: sphere 17.3 mm → 3.4 mm and plate_hole
  past a 300 s timeout, because that default is far finer than any advisor
  action. A "fix" that erases the advisor's purpose is not a fix.

So the verdict is left to the engine: the advisor path probe-meshes at the
chosen h and, when the fill refuses on feature resolution, refines by the factor
the guard itself recommends (0.6) and probes again, at most three times. Every
refinement is printed. Measured over all seven CAD fixtures: only plate_hole is
touched (0.0448 → 0.0269 m, exactly the guard's own recommendation), and all
seven exit 0.

`bench/results/advisor-budget-sweep.json` (21 runs) is regenerated against v6.
Calibration and OOD are regenerated: group conformal 0.9 = ±1.979 decades
achieving 84.3%, 0.95 = ±2.764 decades achieving 91.2%.

## 6. The tolerance selector is still not deliverable

`tolerance_selector.json` regenerated. The best net selector at
`rel_err ≤ 0.05` still violates the tolerance on 35.7% of calibration folds
(lgbm 28.6%), against the standing rule that a selector ships only if it
violates no more often than `finest_action`. Unchanged verdict, unchanged
reason.

## 7. Provenance

- dataset `bench/advisor/dataset.csv`, 2,660 rows, sha256 `9489f0d3b275340164…`
- campaigns `bench/campaigns/advisor-batch-{1..4}-s*-hunter-pc`, 3,528 pairs
- retired generation `bench/campaigns/archive-v6/`,
  `bench/advisor/archive-v6/{dataset-v5.csv,dataset_schema-v5.json,runs-v5/}`
- decision numbers `bench/advisor/crossval_v6.json`; tolerance
  `bench/advisor/tolerance_selector_v6.json`; calibration/OOD
  `bench/advisor/{calibration,ood}.json`
- shipped model run 126, `bench/advisor/{model.onnx,normalization.json,clamps.json}`
- references untouched: every corpus reference carries an `analytic` or
  `external-gmsh-mesh+calculix-solver` source and promotion refuses them by
  design, so the truth gate had nothing to promote.
