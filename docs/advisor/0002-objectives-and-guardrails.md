# 0002 — Objectives and guardrails

Status: implemented (2026-08-10); weights, clamps and thresholds below are the
current values in `bench/advisor/{weights,clamps}.json` from the portable-cost
cycle ([0012](0012-portable-cost-retrain.md)). Companion to
[0001 — architecture](0001-architecture.md) and
[0003 — training log](0003-training-log.md) (historical).

## Why the objectives are separate

The heads are trained with **per-head losses and explicit weights**, never one
collapsed scalar. Accuracy, geometric fidelity, portable cost and feasibility
are not commensurable; adding them with fixed coefficients bakes an exchange
rate into the model that nobody can later inspect or change. Keeping them
separate means the weight vector is a readable configuration file
(`bench/advisor/weights.json`) and the dashboard can show each head's validation
metric moving on its own axis.

## Loss

Per head, masked Huber (delta = 1.0) in log space:

```
L = sum_h  w_h * Huber( yhat_h - y_h )   over rows where head h has a target
  + w_failure * BCE( failure_logit, failure )
  + w_policy  * MSE( policy, best_feasible_action )
  + beta * penalty_barrier(policy)
```

Log space because every regression target spans orders of magnitude
(`solve_ms` from ~1 to ~10^5, `rel_err` from 10^-4 to 10^0). Huber because the
campaign contains genuine outliers — a mesher recovery cascade can multiply
`mesh_ms` by 30 — and squared error would let a handful of them own the
gradient.

**Masking is per head, per row.** A row whose solve failed still carries a
`failure = 1` label and still trains the feasibility head; it contributes to no
regression head. A row with no `geo_fidelity` (STL part, or a build without
OpenCASCADE) trains everything except the two geometry heads. Missing data is
never imputed into a target.

## Staged curriculum

**Stage A** — accuracy and feasibility only. Nonzero weights: `rel_err`,
`rel_err_rel`, `geo_chamfer`, `geo_p99`, `failure`, `policy`. The cost heads are
switched off entirely. A model that has not yet learned *whether a mesh is right*
has no business trading accuracy against cost.

**Stage B** — cost blended in. The portable cost heads `solve_flops`,
`solve_bytes` and `mesh_work` ramp linearly from 0 to their `stage_b_targets`
weight over 5 training runs, so the trunk is not yanked by three new gradients
arriving at full strength in one step. The host-dependent `dof`, `mesh_ms` and
`solve_ms` heads are still exported but carry weight 0 in both stages
([0012](0012-portable-cost-retrain.md): host wall time is not an objective).

**The transition is detected, not scheduled.** Stage A -> B fires when the
relative improvement of the validation `rel_err_mae` over the last 10 runs is
below 2 %, compared against the 10 runs before that. Fewer than 20 runs of
history means no transition. The flip is sticky, recorded as
`"stage_transition": true` on the run where it happens, and drawn as a vertical
marker in the dashboard.

`scripts/advisor/train.py --self-test` asserts the trigger fires at exactly the
first run satisfying the rule on a synthetic plateau, and never fires on a
synthetic series that is still improving. It runs entirely in a temp directory.

## Outlier pruning

After each run, the union of the worst 5 % of training-split absolute residuals
across the three accuracy heads (`rel_err`, `geo_chamfer`, `geo_p99`) is dropped
and recorded in `bench/advisor/runs/pruned_rows.json`, so pruning accumulates
across runs rather than being re-decided each time. Counts are reported per run
in `history.jsonl` and plotted in the dashboard.

Accumulation compounds: each run drops 5 % of what is *left*, so an uncapped
30-run schedule would leave roughly 21 % of the corpus and the val curves would
be measuring the pruner rather than the model. The ledger is therefore capped at
**25 % of the original training split** (`prune.MAX_LEDGER_FRACTION`). Once the
ceiling is reached pruning stops entirely; the run that crosses it keeps only
its worst residuals, up to the remaining allowance. `prune_ceiling` and
`prune_cap_reached` are recorded per run in `history.jsonl`.

**A `failure == 1` row is never pruned.** Those rows are the entire training
signal for the feasibility head, and they are exactly the rows a residual-based
rule would throw away first.

## Guardrails

Three layers, deliberately redundant, because each one fails differently.

### 1. Penalty barrier — during training

```
beta * mean_rows sum_dims relu( |policy_dim| - halfwidth_dim )
```

over the three continuous policy dims, with `halfwidth = max(|lo|, |hi|)` of the
clamp interval. This is a soft push, not a constraint: it shapes the policy head
toward the feasible box so that clamping at inference is rare rather than
routine. The value is reported per run as `penalty` in `history.jsonl` — if it
stops falling, the policy is fighting the box and the box is probably wrong.

### 2. Hard clamps — at inference

`bench/advisor/clamps.json` is the single source of truth, read by both Python
and C++. Nothing in the clamp table is duplicated as a C++ constant.

| Dimension | Box |
| --- | --- |
| cell size, as a fraction of the part (`h_rel`) | [0.005, 0.28] |
| error target (`eta_target`) | [0.0, 0.3] |
| refinement passes (`adapt_passes`) | [0, 6] |
| element order (`order`) | one of `order_choices` = `[1, 2]` (argmax over logits) |
| mesher | one of `mesher_choices` (argmax over logits) |

The `h_rel` ceiling was raised from 0.20 to 0.28 in the portable-cost cycle
because the measured campaign included h_rel = 0.28
([0012](0012-portable-cost-retrain.md#training-and-shipped-graph)). There is no
`p_elevate` dimension: `order >= 2` is the same actuator.

Two corrections to the planned table, both forced by the codebase:

- **`eta_target` floor is 0.0, not 0.005.** In the harness `eta_target = 0.0`
  means *no adaptive error target*, which is a legal and extremely common action
  — every `adapt_passes = 0` row uses it, and it is the default. A floor of
  0.005 excluded the clamp box's own default, so the "safe fallback" would
  itself have been out of box and clamping would have silently switched
  adaptivity on.
- **`mesher_choices` uses the canonical `testlab` vocabulary** (`graded_tet`,
  `hybrid_zoo`, `hex`, `hybrid_vem`, ...) restricted to the meshers present in
  the training data. The planned `{hybrid, tet, mixed}` names do not exist in
  this codebase.

`clamp_table()` additionally projects the default action onto the box and onto
the live vocabularies, so a veto can never return an action the clamp table
itself would reject.

The C++ side records whether it had to clamp (`AdvisorDecision::clamped`) and
says so in the logged decision. Being overruled is reported, not hidden.

### 3. Gate, OOD refusal and veto — at inference

`failure_prob = sigmoid(failure_logit)` is used twice
(see [0001](0001-architecture.md#inference)):

- **Gate** — before ranking, candidates with `failure_prob` above
  `clamps.json:gate_threshold` (0.05) are dropped. The key is required; the C++
  refuses a `clamps.json` without it rather than inheriting the veto value.
- **Veto** — after ranking, the chosen action is re-scored; above
  `clamps.json:veto_threshold` (0.5) the recommendation is discarded.

Before the veto, the OOD test in `ood.json` refuses parts unlike the training
corpus. Every refusal returns the clamp-box defaults, sets `vetoed = true`,
records the reason in the decision note, and suppresses the `predicted_*`
values and `failure_prob` to NaN; only `ood_distance` is kept.

## What the tests actually prove

The `[advisor]` tests (`polymesh_tests "[advisor]"`) are split across
`tests/test_advisor_config.cpp` (model directory, `gate_threshold`, `ood.json`),
`test_advisor_inference.cpp` (head parity, latency, descriptor dump),
`test_advisor_explain.cpp` (activation taps, explain) and
`test_advisor_guardrails.cpp` (gated enumeration, veto, `max_dof` budget). They
cover, among others:

- **Unusable model directory throws.** A missing model is a configuration error
  the operator must see, not something to paper over with silent defaults.
- **`gate_threshold` and `ood.json` are required.** A `clamps.json` without the
  gate key is rejected; a missing OOD descriptor refuses rather than imputes.
- **Parity with the exporting PyTorch graph.** The fixture
  `tests/fixtures/advisor_tiny/` carries the graph, the normalization/clamp/OOD
  artifacts, and PyTorch's own float64 outputs. `evaluate` applies no policy of
  its own, so replaying those inputs is a genuine single-forward-pass
  comparison. `advisor_explain/` does the same for the activation taps.

  The tolerance is **relative**, `|onnx - torch| / max(1, |torch|)`, and it ships
  inside `parity.json` (currently `1e-05`) rather than being hardcoded in the
  test. ONNX Runtime and PyTorch use different float32 GEMM kernels and
  accumulation orders, so absolute error scales with output magnitude; an
  absolute bound is not attainable for outputs above about 8 in magnitude and
  would have been "fixed" by loosening it until it passed.
- **Guardrails on every fixture case.** Gated enumeration over the fixture's
  `candidate_grid` stays inside the clamp box and is re-derived independently of
  the code under test; the veto returns exactly the defaults; a `max_dof` budget
  excludes over-budget candidates. The fixture cases — `nominal`,
  `clamped_low_h_rel`, `vetoed_failure`, `imputed_defaults` (columns omitted so
  the C++ impute path is under test) — are forced by least-squares-fitting the
  head layer, so the guardrail branches are reached by construction, not by
  luck.
