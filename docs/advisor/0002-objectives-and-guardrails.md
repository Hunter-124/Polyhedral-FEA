# 0002 — Objectives and guardrails

Status: implemented (2026-08-10). Companion to
[0001 — architecture](0001-architecture.md) and
[0003 — training log](0003-training-log.md).

## Why the objectives are separate

The heads are trained with **per-head losses and explicit weights**, never one
collapsed scalar. Accuracy, geometric fidelity, DOF, meshing time, solve time
and feasibility are not commensurable; adding them with fixed coefficients bakes
an exchange rate into the model that nobody can later inspect or change. Keeping
them separate means the weight vector is a readable configuration file
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
`geo_chamfer`, `geo_p99`, `failure`, `policy`. The cost heads are switched off
entirely. A model that has not yet learned *whether a mesh is right* has no
business trading accuracy against milliseconds.

**Stage B** — cost blended in. `dof`, `mesh_ms` and `solve_ms` ramp linearly
from 0 to their target weight over 5 training runs, so the trunk is not yanked
by three new gradients arriving at full strength in one step.

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
| cell size, as a fraction of the part (`h_rel`) | [0.005, 0.2] |
| error target (`eta_target`) | [0.0, 0.3] |
| refinement passes (`adapt_passes`) | [0, 6] |
| element order (`order`) | one of `order_choices` (argmax over logits) |
| quadratic elements (`p_elevate`) | {false, true} (logit sign) |
| mesher | one of `mesher_choices` (argmax over logits) |

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

### 3. Feasibility veto — at inference

`failure_prob = sigmoid(failure_logit)` evaluated at the **recommended** action
(pass 2, see [0001](0001-architecture.md#two-pass-inference)). Above
`clamps.json:veto_threshold` (0.5) the recommendation is discarded, the clamp-box
defaults are returned, `vetoed = true` is set, and the predictions that caused
the veto are still reported so the operator can see why.

## What the tests actually prove

`build/tests/polymesh_tests.exe "[advisor]"` — 152 assertions, 3 cases:

- **Unusable model directory throws.** A missing model is a configuration error
  the operator must see, not something to paper over with silent defaults.
- **Parity with the exporting PyTorch graph.** The fixture
  `tests/fixtures/advisor_tiny/` carries the graph, the normalization/clamp
  artifacts, and PyTorch's own float64 outputs for four inputs. `evaluate`
  applies no policy of its own, so replaying those inputs is a genuine
  single-forward-pass comparison.

  The tolerance is **relative**, `|onnx - torch| / max(1, |torch|) <= 1e-6`, and
  it ships inside `parity.json` rather than being hardcoded in the test. ONNX
  Runtime and PyTorch use different float32 GEMM kernels and accumulation
  orders, so absolute error scales with output magnitude: the fixture's
  `failure_logit` of +6.5 lands 4.8e-6 away in absolute terms and 3.9e-7 away in
  relative terms. An absolute 1e-6 assertion would have been unsatisfiable and
  would have been "fixed" by loosening it until it passed.
- **Both guardrails, on every fixture case.** The decision is inside the clamp
  box; `clamped` matches an independently recomputed projection of the raw
  policy; `vetoed` matches an independently recomputed
  `sigmoid(failure_logit) > threshold` at the reconstructed pre-veto action; a
  veto returns exactly the defaults; a non-veto returns exactly the clamped
  policy. The four fixture cases are forced by least-squares-fitting the head
  layer — `nominal`, `clamped_low_h_rel` (cell size 5e-4 of the part, below the
  floor), `vetoed_failure` (logit +6.5), and `imputed_defaults` (five columns
  omitted so the C++ impute path is what is under test) — so the guardrail
  branches are reached by construction, not by luck.

The fixture's rows carry the clamp-box default action in their action columns.
That is load-bearing: `recommend` queries the policy head at the default action,
so a fixture whose action columns were arbitrary would have had its forced policy
values attached to a row the C++ never evaluates.
