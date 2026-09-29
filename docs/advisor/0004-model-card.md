# 0004 — Model card: learned mesh advisor

Companions: [0001 — architecture](0001-architecture.md),
[0002 — objectives and guardrails](0002-objectives-and-guardrails.md),
[0003 — training log](0003-training-log.md),
[0005 — data card](0005-data-card.md),
[ADR-0027](../decisions/0027-learned-mesh-advisor.md).

## Current generation

The shipped artifact is the portable-cost cycle,
[0012](0012-portable-cost-retrain.md): Stage-B run 144, trained on
`bench/advisor/dataset.csv` with **36,010 rows** (SHA-256 `f0a5c150c275…`, 15
families; see the [data card](0005-data-card.md)), **75 input columns**,
**19,156 parameters**, ONNX parity `3.063e-06` relative for the contract outputs
and `1.951e-06` for the activation taps (both under `1e-05`). Its held-out-family
head MAEs, failure AUC (0.8921) and OOD/missed-failure rates are published in
[0012](0012-portable-cost-retrain.md#training-and-shipped-graph) and are not
repeated here. The artifact set is listed in the [series index](README.md).

Machine-readable evidence: calibration and OOD for the current graph are in
`bench/advisor/calibration.json` / `ood.json`; the portable-cost descriptor gate
in `bench/advisor/evidence/corpus_evidence.json`; the earlier corpus-design
claims (descriptor distances, tube load-slab derivation, 1-NN family recovery,
learning curve, power table) in `bench/advisor/corpus_evidence.json`, regenerated
by `scripts/advisor/corpus_evidence.py`. These carry a `provenance` block and
win over any table in this card.

## Scope of the tables below

Unless a line says otherwise, the evaluation sections below are the **v2-era
measurement** on the rebuilt corpus: 2,412 rows, dataset SHA-256
`3c0d6bd7a7d3…`, references from an independent Gmsh + CalculiX chain (88
external, 8 closed-form) with evidence-derived tolerances near 0.02, 43 input
columns and 15,591 parameters. Decision numbers come from
`bench/advisor/crossval_final.json`. They are kept because they established the
decision rule, the gate threshold and the OOD design that still ship; 0012 does
not republish the chooser-regret tables. Later cycle reports supersede them in
order: [0006](0006-clean-data-retrain.md) (scale-law features),
[0007](0007-tolerance-selector.md), [0008](0008-v4-corpus-retrain.md),
[0009](0009-v5-mesher-conformity-retrain.md),
[0010](0010-v6-exterior-conformity-retrain.md),
[0011](0011-v7-curved-geometry-retrain.md), [0012](0012-portable-cost-retrain.md).
Where a figure below conflicts with a later report, the later report wins.

## What it is

A compact multi-head MLP that predicts the outcome of a candidate meshing action
for a given CAD part and load case, so a chooser can rank actions before any of
them is run.

| | current (0012) | v2-era (tables below) |
|---|---|---|
| Inputs | **75** standardized columns (39 `CaseFeatures`, 15 exact-BRep descriptors, 7 case, 8 action, 4 scale-law, 2 categorical; see [0001](0001-architecture.md#feature-and-action-schema)) | 43 (26 geometry/BC, 7 case, 8 action, 2 categorical) |
| Outputs | 10 regression heads, 1 feasibility logit, 1 policy vector of width **9** | 7 regression heads, 1 feasibility logit, policy width 7 |
| Trunk | `Linear → GELU → Linear → GELU`, width 96, 4-dim embeddings for `order_idx` and `mesher_idx` | same |
| Parameters | **19,156** | 15,591 |
| Export | ONNX opset 17, single-threaded CPU FP32, deterministic; parity `3.063e-06` relative (tolerance 1e-05) | parity 2.158e-06 |
| Runtime | ONNX Runtime via the C API, no Python at inference | same |

`p_elevate` was **deleted** from the action space — it was redundant, not merely
unvaried: `polymesh solve` computes
`p_elevate = decision.p_elevate || order >= 2`, the same actuator. The order
vocabulary was trimmed from `[1,2,3,4]` to the reachable `[1,2]`;
`fea::promote_to_quadratic` is a single linear→quadratic step and the CLI warned
and downgraded anything higher, so the policy head had been spending two of its
ten outputs on actions the engine cannot perform.

## Intended use

Propose a starting mesh configuration for a linear-elastostatic solve on a CAD
solid, inside PolyMesh, where the downstream estimator, health gates and resource
guards all still run. **The advisor proposes; the estimator disposes**
(ADR-0027 §8).

## Out-of-scope use

- Any part unlike the training corpus — 15 procedural families today — and on
  an unseen family the regression heads can be numerically meaningless.
- Loading conditions beyond the geometric BC/load-region descriptors the model
  sees ([0001](0001-architecture.md#feature-families)).
- Non-structural physics, non-linear material, contact, dynamics.
- As a substitute for the solver, the error estimator, or the health gates.
- Reporting `predicted_*` values to a user as estimates. On an unseen family they
  are not.

## The shipped decision rule

**Gated enumeration over measured candidates.** Score every action in
`clamps.json:candidate_grid`, drop those whose predicted failure probability
exceeds the gate, take the argmin of predicted per-case accuracy over the
survivors. The wholesale veto is a separate concern — the gate improves a
choice, the veto refuses one — and if the gate rejects every candidate the
best-ranked action is returned and left to the veto rather than the chooser
abstaining silently.

With `--advisor-objective efficiency` and a host calibration, survivors within
5% of the best accuracy score are ranked by predicted mesh-plus-roofline time
instead ([0012](0012-portable-cost-retrain.md#deployed-selection-behavior));
accuracy remains the default objective.

The candidate set is an **explicit list of action tuples the campaign actually
ran** — 108 in the current `clamps.json` (20 in the v2-era grid measured below) —
not a cross product of per-dial levels. A cross product invents combinations: the
v2-era corpus ran `adapt_passes=0` only at `h_rel=0.12`, so crossing the dials
manufactured "no adaptivity at `h_rel=0.08`" and asked the heads to extrapolate
to it. It also lets provably inert dials collapse — `order` has no effect at
`adapt_passes>0` (264 of 264 matched pairs bit-identical), so those duplicates
are dropped, while `eta_target` is **not** collapsed because it is inert in only
193 of 237 pairs.

Three choosers were compared and they are **not** equivalent:

| chooser | rule | status |
|---|---|---|
| `advisor_policy` | one pass at the default action, read the policy head | **retired** |
| `advisor_argmin` | enumerate, argmin the predicted outcome | intermediate |
| `advisor_gated` | filter by predicted feasibility, then argmin | **shipped** |

## Decision quality

Leave-one-family-out over 8 families (7 of 8 folds scorable), 5 seeds, failing
actions offered and charged, DOF-primary budget. `rel_err` regret in log10,
lower is better.

| chooser | q0.5 regret | ±fold | ±seed | pick-failure |
|---|---:|---:|---:|---:|
| `advisor_gated_0.2` *(best regret)* | **0.3233** | 0.244 | 0.071 | 31.3 % |
| `advisor_gated_0.05` **(shipped)** | 0.3338 | 0.238 | 0.077 | **27.5 %** |
| `spend_budget` *(hindsight — not deployable)* | 0.3358 | 0.190 | 0 | 0.0 % |
| `advisor_argmin` | 0.3400 | 0.238 | 0.090 | 36.4 % |
| `default` | 0.3796 | 0.223 | 0 | 19.0 % |
| `advisor_efficiency` | 0.3853 | 0.260 | 0.087 | 35.9 % |
| `random` | 0.4076 | 0.238 | 0.098 | 41.7 % |
| `advisor_policy` | 0.4272 | 0.228 | 0.092 | 27.9 % |
| `constant_config` | 0.4379 | 0.321 | 0 | 27.4 % |
| `finest_action` | 0.4409 | 0.294 | 0 | 23.8 % |

**At a median DOF budget the advisor now beats `spend_budget`** — the
hindsight-flavoured baseline that previously beat everything. `spend_budget` is
**not a fair opponent**: it ranks by *measured* DOF, so it can never select an
action that failed, which shows up as an impossible 0.0 % pick-failure rate at
every budget. `finest_action` is the honest form of the same idea.

### The gate threshold: chosen, not inherited

**Shipped value: `gate_threshold = 0.05`**, set explicitly in `clamps.json` and
read strictly by `Advisor::Impl::load_clamps`. The shipped chooser is therefore
**`advisor_gated_0.05`: q0.5 regret 0.3338 (±0.238 fold, ±0.077 seed, 7 folds),
pick-failure 27.5 %.**

**Why it needed choosing.** `clamps.json` previously carried no `gate_threshold`
at all and the C++ fell back to `veto_threshold`, so the gate ran at 0.5 — the
*weakest* member of its own measured sweep — while looking like a deliberate
setting. The gate and the veto are different decisions: the gate filters
candidates *before* ranking to improve a choice; the veto refuses the whole
recommendation *after* the fact. Sharing one number was an accident, and a
fallback that silently reuses one for the other is a value that looks considered
and is not. **The C++ now rejects a `clamps.json` that omits the key** rather than
defaulting, and a test asserts both the strict read and the rejection.

**Why 0.05 rather than 0.2.** Regret does not single out a threshold: across
0.05–0.8 the whole gated family spans 0.3233–0.3350, so the *best* choice by
regret (`0.2`, at 0.3233) is 0.012 decades ahead of the worst — inside the ±0.238
fold spread, i.e. not a distinguishable difference. Pick-failure does separate
them, and by more than the regret gap is worth:

| threshold | q0.5 regret | pick-failure @ q0.5 | pick-failure, all levels |
|---|---:|---:|---:|
| **0.05 (shipped)** | 0.3338 | **27.5 %** | **30.6 %** |
| 0.1 | 0.3249 | 29.0 % | 31.9 % |
| 0.2 *(best regret)* | **0.3233** | 31.3 % | 33.3 % |
| 0.5 *(the old accidental value)* | 0.3350 | 31.2 % | 33.3 % |
| 0.8 | 0.3272 | 30.7 % | 33.1 % |
| *ungated `advisor_argmin`* | 0.3400 | 36.4 % | 39.5 % |

**Two different comparisons, both real, and they must not be confused.** Gating at
all buys **8.9 points** of pick-failure against ungated ranking (36.4 % → 27.5 %),
and that figure is basis-independent — it is 39.5 % → 30.6 %, the same 8.9 points,
when averaged over all thirteen budget levels instead of q0.5 alone. Choosing
*this* threshold rather than the regret-optimal one buys a further **3.8 points**
(31.3 % → 27.5 %). Rates are quoted at the **q0.5 budget level** unless the column
says otherwise; the all-levels mean is uniformly ~2–3 points higher for every
chooser, because tight budgets leave fewer feasible actions to choose from.
Trading 0.0105 decades of median regret for 3.8 points of pick-failure is the
right direction for a user-facing default: **a rule that avoids doomed picks is
worth more than a hair of median accuracy that sits inside the fold noise.** The
failure head's calibration is mediocre (ECE 0.263), which argues the same way — if
the probability is only roughly right, gate conservatively and let the ranking do
the discriminating.

Because the threshold is read after inference and touches no tensor, retuning it
needs **no retrain**: `export_onnx.py` now separates the deployment keys
(`veto_threshold`, `gate_threshold`) from the graph-affecting payload, so the
operating point can move while the checkpoint's vocabularies, boxes, defaults and
candidate grid stay pinned.

Paired sign tests, pooled over folds and seeds, at q0.5:

- `advisor_gated_0.05` vs `finest_action`: 107W-55L-198T, **p=5.4e-05**
- vs `constant_config`: 109W-65L-186T, **p=1.1e-03**
- vs `default`: 101W-61L-198T, **p=2.1e-03**
- vs `spend_budget`: 87W-91L-182T, p=0.82 (tie)
- vs `advisor_argmin`: 21W-16L-323T, **p=0.51 — not significant**
- `advisor_argmin` vs `finest_action`: 108W-64L-188T, **p=9.9e-04**
- `advisor_policy` vs `constant_config` p=1.00, vs `default` p=0.20, vs
  `finest_action` p=0.62 — the retired rule is indistinguishable from a
  zero-parameter baseline.

### The "strictly dominant" claim is retired

An earlier version of this card stated that the gate was **strictly dominant**
over ungated ranking, on the basis of 38W-0L-262T, p=7.3e-12. **That claim is
withdrawn.** On the rebuilt corpus the same comparison is 21W-16L-323T, p=0.51.

The gate stopped being the differentiator because **ranking alone became
significant**: `advisor_argmin` vs `finest_action` moved from 117W-136L
(p=0.258, *losing*) to 108W-64L (p=9.9e-04, *winning*). The cause is the labels,
not the model. The references are now independent and the tolerances about five
times tighter, which made `rel_err_rel` a learnable target where it previously
was not. **The model was never the bottleneck; the labels were.**

The gate still ships, on its honest basis: the gated family occupies the whole top
of the deployable ranking at q0.5 — every threshold from 0.05 to 0.8 beats
`default`, `constant_config`, `finest_action` and `random` — and it cuts the rate
of recommending an action that then fails from 36.4 % to 27.5 % at the best
threshold, which is now the shipped one. It is no longer strictly dominant.

## What the model has and has not learned

Holding cost nearly constant (a narrow DOF band, so only judgement separates the
choosers). These bands are over **6 scorable folds**, one fewer than q0.5:

| chooser | band 0.4–0.6 | band 0.7–0.9 |
|---|---:|---:|
| `spend_budget` *(hindsight)* | 0.1673 | 0.1694 |
| `default` | **0.1724** | **0.2098** |
| `advisor_gated_0.05` *(shipped)* | **0.2061** | 0.2714 |
| `advisor_gated_0.5` *(the old accidental value)* | 0.2260 | 0.2661 |
| `advisor_argmin` | 0.2425 | 0.3191 |
| `random` | 0.2576 | 0.3531 |
| `constant_config` | 0.2637 | 0.3706 |
| `finest_action` | 0.2690 | 0.3689 |
| `advisor_policy` | 0.2927 | 0.3095 |

The advisor is **no longer the worst chooser at matched cost** — previously it
ranked below random in both bands. In the 0.4–0.6 band the shipped gate at 0.2061
beats `random` (0.2576), `constant_config` (0.2637) and `finest_action` (0.2690);
the accidental 0.5 value managed only 0.2260, so setting the threshold explicitly
improved this band too. The shipped `default` at 0.1724
wins both bands. So **most of the advisor's value is spend allocation and
feasibility filtering, and per-case judgement now weakly exists rather than being
absent.**

## Per-head accuracy

Leave-one-family-out, 8 folds × 3 seeds, all heads weighted. On every head the
standard deviation is comparable to or larger than the mean, because one or two
folds blow up — **quote the medians**.

| head | median | mean | sd |
|---|---:|---:|---:|
| predicted relative error (`rel_err`) | 0.8045 | 1.1693 | 1.2526 |
| relative error, centred per part (`rel_err_rel`) | 0.6357 | 0.7539 | 0.4866 |
| mesh-to-CAD distance (`geo_chamfer`) | 0.6921 | 3.8252 | 7.8202 |
| mesh-to-CAD worst 1% (`geo_p99`) | 0.7390 | 3.6906 | 7.4704 |
| degrees of freedom (`dof`) | 0.5185 | 1.3036 | 2.5764 |
| meshing time (`mesh_ms`) | 0.5701 | 0.9846 | 1.5953 |
| solve time (`solve_ms`) | 0.6684 | 1.0308 | 1.8415 |
| failure risk, ROC AUC (`failure_auc`) | 0.8657 | 0.8062 | 0.2160 |

Every head is **worse** than the previously published figures (relative error
0.809, degrees of freedom 0.150, meshing time 0.132, solve time 0.171). That is
expected and correct, not a regression: those came from a **leaky** part-hash
split with warm-started training, these from leave-one-family-out with
independent labels. Prediction accuracy got worse and decision quality got
better, at the same time, from the same cause.

## Uncertainty, calibration and abstention

**Current artifact.** `ood.json` is fit on the 36,010-row portable-cost table
over 44 raw part columns: the 16 mesh-derived geometry features, the 13
proximity/crease/singularity columns added in 0012 (including the load/fix
feature distances and multiaxiality), and the 15 exact-BRep descriptors. The
threshold is the training q0.99 recorded in the file. 0012 reports 47.2 % of
held-out rows beyond it and a 17.4 % missed-failure rate at the 0.5 veto
([limitations](0012-portable-cost-retrain.md#honest-limitations)). The bullets
below are the v2-era derivation of the same design.

- **Feasibility head:** mean ECE **0.263** (was 0.4795), Brier 0.2489, base
  failure rate 0.459.
- **The shipped 0.5 veto is not a safety mechanism.** It abstains on 28.1 % of
  queries while still letting 28.5 % of failures through. Holding missed failures
  under 2 % needs a 0.95 threshold and 56.9 % abstention.
- **Split-conformal does not hold across families.** Nominal 90 % achieves
  56.7 % (was 24.6 %). Conformal guarantees coverage only under exchangeability,
  which leave-one-family-out deliberately breaks, so the gap *measures* the
  shift. Group-conformal restores it — 90.5 % at nominal 90 % — but only with a
  **±2.3 decade** band, so an honest interval on an unseen family still says
  little.
- **The OOD gate is distance-based, validated, and wired.** v2-era fit:
  Mahalanobis over **31 part-geometry columns** — the 16 mesh-derived geometry
  features plus the 15 exact-BRep descriptors — with shrunk covariance and the
  threshold at the training 99th percentile (**5.0341**): the fit's own
  leave-one-family-out cross-validation records **83.3 % of held-out-family rows
  flagged over 12 folds** at a **1.0 %** in-sample false-alarm rate. Parameters
  in `bench/advisor/ood.json`, consumed by `Advisor::Impl::load_ood`.

  Measured on the v2-era artifact over all 44 corpus primitives, the gate is
  cleaner than that cross-validated rate suggests, because the rate is per
  *row* while a refusal is per *part*. Training covers 12 corpus geometries —
  regimes s0 and s2 only, of six families — and `polymesh solve <part>
  --advisor bench/advisor` over the whole corpus refuses **20 of 20** parts from
  the five families absent from training (distances 11.36 to 80.19, between
  2.3x and 16x the threshold) while advising **12 of 12** geometries that are
  actually in the training set. The narrowest margin on trained geometry is
  `channel_s2` at 5.03 against the 5.0341 threshold. Of the twelve unseen
  *regimes* of trained families, eleven are advised and `stepped_shaft_s1` is
  refused at 5.12, 1.7 % over — the single marginal call in the sweep.

  **It tests the part, not the load case** (v2-era fit). Boundary-condition
  columns were deliberately excluded. `fit_ood`'s rule is that only an unfamiliar *part* is out
  of distribution — a user is entitled to clamp and load a familiar part
  differently from any campaign case, and that is a legal question about a known
  geometry. This is not a tidy-up: the campaign derives BC features from the
  corpus case definitions while the CLI derives them from its own default slab
  selection, and including them made the deployed gate refuse `box_hole_s0`, a
  *training* part, at distance 35.82 against a 6.50 threshold. A gate that refuses
  its own training geometry is measuring the wrong thing. Excluding them costs no
  detection power — 100 % mean and 100 % min either way — and improves the
  false-alarm rate from 0.92 % to 0.86 %.

  The parameters are held in **raw feature units with their own `center`/`scale`**,
  independent of `normalization.json`. In the v2-era contract fifteen of the
  columns were not model inputs; today every `ood.json` column is also a model
  input, but the encoded row lives in `normalization.json`'s standardized space,
  so the distance is still computed on raw columns. Fitting on raw columns
  directly gave a precision matrix at condition number **2.97e20**, past
  float64's 1/eps; the artifact's own standardizer brought the v2-era fit to
  **7.68e10**, recorded in the file as `precision_condition_number` so a later
  refit that degrades it is visible. The C++ accumulates the quadratic form in
  double.

  **Verified against the Python reference over 32 corpus parts**, at both levels
  that matter. Descriptors: worst relative deviation **2.94e-16**. Distance, on
  the *assembled 31-column vector the C++ actually used*, against
  `calibration.py:ood_scores`: worst **2.00e-15**, no part differing by more than
  1e-12 and none moving across the threshold.

  The distance check is the load-bearing one and it initially was not done
  properly: an earlier version computed both sides in Python from the C++
  descriptors, which tests descriptor agreement propagated through the matrix but
  never exercises the C++ quadratic form at all. `polymesh_tests "advisor
  descriptor dump"` now emits the assembled vector, the resolved column order and
  the C++-computed `ood_distance` for every part, so the comparison is one command
  rather than an argument.

  **The marginal calls are real and visible.** Over the 44 corpus primitives the
  one part the gate refuses from a trained family is `stepped_shaft_s1`, at 5.12
  against the 5.0341 operating point — 1.7 % over, on an unseen *regime* of a
  family whose s0 and s2 regimes are in training. In the other direction
  `channel_s2`, which *is* training geometry, is advised at 5.03, inside the
  threshold by 0.1 %. Two parts within 2 % of the boundary on opposite sides is
  what a q0.99 fit looks like from the inside, and neither is hidden here.

  `tube` sits nowhere near the boundary: absent from training entirely, its four
  regimes score 13.55 to 19.02, so it is refused by the OOD gate on its own
  merits rather than by the *feasibility* gate.

  This is not an isolated result. `tube` is also the family contributing no
  scorable folds and the one Gmsh fails outright on, and those findings share a
  cause: see [`tube` is the hardest family in the
  corpus](0005-data-card.md#tube-is-the-hardest-family-in-the-corpus-and-four-measurements-say-so)
  in the data card, where all four measurements are collected rather than repeated
  a line at a time across three documents.

The 15 offline CAD descriptors *hurt* held-out regret in the v2-era measurement
— a 1-NN classifier recovers the family from them alone at 32/32, so on a corpus
that narrow they are family identifiers rather than transferable physics. That
same property makes them close to ideal for OOD detection, which is why earlier
contracts kept them out of the model inputs. The portable-cost contract includes
them as inputs (see [0001](0001-architecture.md#feature-and-action-schema)) and
still uses them for OOD.

## Known failure modes

- **Extrapolation on an unseen family is catastrophic, not graceful.** The
  geometry-head means exceed their medians roughly fivefold, so at least one fold
  produces numerically meaningless geometry predictions. Regret survives it
  because regret depends only on the argmin, and a ranking tolerates a large
  constant offset.
- Consequently every `predicted_*` value **and** `failure_prob` are **suppressed
  to NaN on a refusal** — implemented, not aspirational. The case that motivated
  the gate reported `predicted_mesh_ms = 1.66e14` (about 5,300 years) for a unit
  box beside a failure probability of 1e-65 claiming near-certain success.
  Printing that to a user is a defect independent of the meshing decision. The
  `ood_distance` is retained, because it is the measurement that caused the
  refusal and it is meaningful by construction.
- **Two refusal causes exist and are deliberately distinguishable.** "out of
  distribution: mahalanobis X exceeds the validated operating point Y" means the
  part was measured and is unlike the training corpus. "out-of-distribution test
  unavailable ..." means the descriptors could not be measured at all — no
  OpenCASCADE, or a model carrying no BRep. The first is a statement about the
  part, the second about our own instrumentation, and they call for different
  actions. A missing descriptor is never imputed: substituting the training median
  would place an unknown part at the *centre* of the training distribution and
  report it as maximally familiar.
- **v2-era: `failure_auc` on the then-shipped checkpoint's own fold was 0.5248 —
  near chance**, against a leave-one-family-out mean of 0.8062 (sd 0.216). The
  gate is built on this head. The current checkpoint's held-out-family failure
  AUC is 0.8921 ([0012](0012-portable-cost-retrain.md#training-and-shipped-graph)).
- v2-era: one of eight folds contributes no scorable cases, so macro means are
  over seven families.
- **`order` is not one knob, and it is not independent of `adapt_passes`.** With
  `adapt_passes = 0` promotion is unconditional and the mesh is uniformly
  quadratic (testlab calls `fea::promote_to_quadratic` on the whole mesh); with
  `adapt_passes > 0` it routes through the adaptive driver's marked p-set,
  falling back to `adapt::mark_smooth(zz.element_eta, 0.3)` in the pipeline's
  adaptive solve path, which never consults `cfg.order`. Measured on the v2-era corpus: of 264 matched
  `order = 1` vs `order = 2` pairs at `adapt_passes = 1`, **all 264 are
  bit-identical** in `n_dof`, `n_nodes` and `rel_err`, while at
  `adapt_passes = 0` the median DOF ratio is 5.52.

  **Do not read any coefficient, importance score or ablation delta on `order` as
  an element-order effect.** It is a weighted average over two regimes and should
  be reported per `adapt_passes` stratum or not at all. Counts and the full
  derivation are in the [data card](0005-data-card.md).

  *Future work, deliberately deferred:* unifying the two promotion paths would
  invalidate the comparability of every row already generated, and the CLI's
  `--p-elevate-uniform` covers the benchmark case that needed parity. The fix is
  either to unify them at the next corpus rebuild, or to rename the dial so the
  two regimes are distinguishable in the schema.

## Deployment cost

Gated enumeration turns roughly 2 forward passes into one per candidate.
Measured on the v2-era graph, single-threaded over 200 calls after warmup:
**p50 1.64 ms, p90 2.00 ms, p99 2.62 ms** at 32 candidates (~51 µs each) —
roughly **0.1 % of a solve** measured in seconds. The current grid has 108
candidates and a larger graph (`model.onnx` 84,388 bytes); its latency is not
re-measured here. The `advisor recommend() latency` test guard fails above
100 ms p99, at which point the candidate list should be pruned via
`max_candidates` rather than the rule abandoned.

## Reproducibility

Every checkpoint and report artifact carries a `provenance` block: git revision
(suffixed `-dirty` when the tree is uncommitted), dataset path and SHA-256, row
count, split mode and fold, held-out groups, input/action widths, the
geometry-descriptor list and its SHA-256, seed, and Python/NumPy/Torch versions.
Inference is single-threaded with a fixed graph.

## Evaluation harness

`scripts/advisor/regret.py` (choosers, budget-constrained regret, sign tests),
`crossval.py` (folds × seeds), `calibration.py` (reliability, conformal, OOD),
`learning_curve.py` (regret vs corpus width), `corpus_evidence.py`,
`evaluate.py` (single checkpoint). `evaluate.py` and `crossval.py` share the
scoring module so they cannot report different things.
