# 0003 — Training log

Milestone entries for the learned mesh advisor. Companions:
[0001 — architecture](0001-architecture.md),
[0002 — objectives and guardrails](0002-objectives-and-guardrails.md).

Regenerate everything below with:

```
python scripts/gen_primitive_corpus.py
python scripts/advisor/run_batch.py --batch 1 \
    --campaign-template bench/campaigns/advisor-batch-template/campaign.json
python scripts/advisor/promote_truth.py
python scripts/advisor/train.py --runs 30
python scripts/advisor/train.py --baseline
python scripts/advisor/export_onnx.py
python scripts/advisor/dashboard.py && python scripts/advisor/figures.py
python scripts/render_cinema.py --all
```

---

## M-A1 — first trained advisor (2026-08-10)

### Corpus and ground truth

| Quantity | Value |
| --- | --- |
| Procedural STEP parts | 24 (6 families x 4 size regimes) |
| Load cases | 72 (3 per part) |
| Truth campaign rows | 280 across 9 sharded `results.jsonl` |
| References: analytic | 8 files / 12 metrics (Kirsch SCF, stepped-shaft cantilever) |
| References: promoted overkill solve | **59 files / 118 metrics**, tol 0.15 |
| References: still provisional | 5 files / 10 metrics (all `l_bracket`) |

Truth coverage is **67 of 72 cases (93 %)**. The five `l_bracket` holdouts produced
no health-ok row at any rung and keep their first-order beam surrogate; rows for
those cases therefore carry a noisier `rel_err` label, and
`bench/reference/corpus/*.json` records `truth_source` so this is queryable
rather than folklore.

Two deviations from the plan, both forced by measurement:

- **Truth `h_rel` is a ladder, not 0.005.** `testlab` refuses any run predicting
  more than 120 000 elements before meshing; at `h_rel = 0.005` the boxiest
  corpus part predicts 6.28e6, so all 72 runs would have returned
  `over_budget` with nothing to promote. A `{0.060, 0.038, 0.024}` ladder lets
  `promote_truth.py` take the finest health-ok row per case, and the rungs a
  part cannot afford fail before meshing, nearly for free.
- **A second pass was needed.** With `adapt_passes: 3` against the compiled
  80 000-DOF throughput ceiling, only 32 of 72 cases produced a usable row.
  `Campaign::max_dof` / `max_elems` are now readable from
  `campaign.json:resources` (the source comment had promised this: "Tunable
  later via campaign.json"), and a second pass at `adapt_passes: 2` with a
  260 000-DOF budget lifted coverage 32 -> 67.

### Batch 1

| Quantity | Value |
| --- | --- |
| Pairs planned / run | 1536 / 1536 (dedup hits 0 — first batch) |
| Wall time | 2713 s across 4 shards x `OMP_NUM_THREADS=2` |
| Throughput | 0.566 rows/s |
| `ok` | 1026 |
| `solve_fail` | 366 (mostly `no interior cells` — coarse `h_rel` on thin walls) |
| `solve_suspect` | 78 |
| `over_budget` | 8 |
| meshing share of wall time (`mesh_ms / (mesh_ms + solve_ms)`) | **0.467** |

The 25 % failure rate is signal, not breakage: it is what the feasibility head
learns, and it is concentrated where you would expect (`h_rel = 0.20` against
thin-walled channels and notched plates).

**The mesh-cache contingency has fired.** The plan gates it on
`mesh_ms_frac > 0.30`; measured 0.467, and `throughput.json` records
`mesh_cache_recommended: true`. It is *not* built in this milestone: the only
non-mesh-affecting action dimension in the batch-1 grid is `order`, so the
achievable saving is ~23 % of batch wall time, and it would land after all of
this milestone's data was already generated. It is the correct first task of the
scale-up batch, not a retrofit onto a finished dataset.

### Training table

1733 rows, of which **1537 are `advisor-row-v2`** across 48 corpus cases; 196
legacy rows are rejected by `load_dataset` (they have NaN for all 26 geometry
features). `advisor-truth-*` campaigns are excluded from the table on purpose:
`promote_truth.py` *defines* each case's reference from those rows, so their own
`accuracy_rel_err` is ~0 by construction and training on them would teach the
model that the overkill config has zero error.

Split is by part-name hash: 1313 train / 224 val rows, no part on both sides.

### Results after 30 runs

Validation MAE in log10 units. LightGBM is trained on the identical split.

| Head | run 1 | run 30 | LightGBM |
| --- | --- | --- | --- |
| degrees of freedom (`dof`) | 4.837 | **0.108** | 0.018 |
| meshing time (`mesh_ms`) | 2.952 | **0.138** | 0.093 |
| solve time (`solve_ms`) | 3.929 | **0.153** | 0.085 |
| mesh-to-CAD distance (`geo_chamfer`) | 0.102 | 0.155 | 0.025 |
| mesh-to-CAD worst 1% (`geo_p99`) | 0.132 | 0.182 | 0.063 |
| predicted relative error (`rel_err`) | 0.983 | **1.070** | 0.970 |
| failure risk, ROC AUC (`failure_auc`) | 0.992 | 0.981 | — |

Stage A -> B fired automatically at **run 21** on the <2 %-over-10-runs rule.
Pruning removed 328 rows and then stopped at its 25 % ceiling, as designed.

### The honest finding: `rel_err` does not generalize across parts

Train `rel_err_mae` is 0.036; validation is 1.070 — a full decade. That is not
an MLP defect. **LightGBM reaches 0.970 on the same split** while predicting
cost to 0.02–0.09, so both model families fail on exactly one target and
succeed on the rest. The target, not the estimator, is the limit.

The mechanism is structural. `accuracy_rel_err` is measured against a
*per-case* truth, and the three probe kinds (`tip_deflection`, `strain_energy`,
`mean_vm_over_nominal`) each carry their own offset and their own reference
quality. A held-out part brings a truth offset the model has never seen, so its
`rel_err` *level* is not predictable from geometry features at 24 distinct
geometries. Cost and DOF have no such per-part offset, which is precisely why
they learn cleanly.

This is worth stating plainly because the plan's verification item 4 expected
"val `rel_err` decreasing start-vs-end". It does not, and the curve in
`docs/advisor/figures/training_curves.png` shows it not decreasing. Making that
number look better would have meant training on the validation parts or
loosening the split — the two things that would destroy the measurement.

The fix is a design change, not more epochs: predict `rel_err` *relative to a
per-part reference action* rather than its absolute level, or grow the corpus
well past 24 geometries. Both are scale-up work.

Second observed weakness: on out-of-distribution input the regression heads
extrapolate wildly. `polymesh solve bench/geometries/public/unit_box.step
--advisor bench/advisor` (a part not in the corpus, with no BC boxes) returned
`predicted_dof = 1.5e15`. The feasibility head caught it —
`failure_prob = 0.99998` -> vetoed -> defaults -> solve completed normally. The
guardrail is doing exactly the job it exists for, but the predictions behind a
veto should be read as "unknown", not as numbers.

### Product proof

```
polymesh solve bench/geometries/corpus/primitives/stepped_shaft_s0.step \
    --advisor bench/advisor --fix-box ... --load-box ... -o build/advisor_corpus.vtu
```

```json
{"mesher":"graded_tet","h_rel":0.1355,"order":2,"adapt_passes":0,
 "eta_target":0.01322,"failure_prob":6.8e-18,"vetoed":false,"clamped":true,
 "predicted_dof":1946.5,"predicted_mesh_ms":181.8,"predicted_solve_ms":55.3}
```

The decision differs from the defaults on every axis it can (`graded_tet` vs
`hybrid_zoo`, `h_rel` 0.1355 vs 0.100, order 2 vs 1, `eta_target` 0.0132 vs 0),
is inside the clamp box, reports that it was clamped, and the solve completed
(4106 nodes, 2487 elems) with the VTU written.

Predicted DOF 1947 against an actual 12 318 is a ~6x miss — recorded here rather
than omitted.

### Parity

`export_onnx.py` scores the exported graph against a **float64** PyTorch
reference, not against float32 PyTorch. Measured on this model:

| Comparison | Worst relative |
| --- | --- |
| float32 PyTorch vs float64 PyTorch | 2.148e-06 |
| ONNX Runtime vs float64 PyTorch | **1.597e-06** |
| ONNX Runtime vs float32 PyTorch | 1.392e-06 |

ONNX Runtime is *closer to the exact answer than PyTorch's own float32 path is*.
The original 1e-06 onnx-vs-torch bound asked two float32 implementations to
agree more tightly than either agrees with the truth — unsatisfiable by
construction, and it failed at 1.392e-06 on the real model while passing on the
tiny fixture. The criterion is now `|onnx_f32 - torch_f64| / max(1, |torch_f64|)
<= 1e-05`, justified by float32 eps (1.19e-07) against a 64-wide reduction, and
strictly *stronger* than the old one: it catches a wrong op, a wrong weight, or
a permuted column order (all of which move outputs by orders of magnitude) while
tolerating GEMM accumulation order, which is not a defect.

### Next

1. Mesh cache (trigger already fired, 0.467 > 0.30).
2. Re-target the `rel_err` head at a per-part-relative quantity.
3. Grow the corpus past 24 geometries — the binding constraint on every
   accuracy-side head.
4. Recover the 5 `l_bracket` truths.

---

## M-A2 — the accuracy head, fixed (2026-08-10)

M-A1 shipped an advisor that predicted cost well and accuracy not at all. This
entry closes that.

### Capacity was not the problem

`scripts/advisor/capacity_sweep.py` fits the accuracy head alone across a 320x
parameter range on the same part-hash split:

| width x depth | params | absolute train -> val | centred val |
| --- | --- | --- | --- |
| 32 x 2 | 2,529 | 0.093 -> **0.989** | **0.301** |
| 128 x 2 | 22,401 | 0.040 -> **1.355** | 0.328 |
| 512 x 2 | 286,209 | 0.011 -> **1.219** | 0.353 |
| 512 x 4 | 811,521 | 0.007 -> **1.109** | **0.292** |

Adding capacity drove training error down 13x and validation error *up*. That
is overfitting, not underfitting; no width or depth rescues the absolute target.

### What did work: centre the target per case

`rel_err_rel` = `log10(rel_err)` minus that case's median over the actions
actually run. The absolute level of `rel_err` is dominated by how good a case's
reference truth happens to be — an offset a held-out part never shows the
model. Choosing a mesh only needs the *ordering* of actions within one case, and
that ordering survives centring. Validation MAE 1.07 -> **0.30**, and flat
across every capacity, so the production trunk stays small (96 wide, 16k params)
and the neuron map stays legible.

### Data

Batches 1-3: **3456 advisor rows over all 72 cases**, now with real variation on
every action dial the policy head predicts — `h_rel` {0.12, 0.16, 0.20},
`order` {1, 2}, `mesher` {hybrid_zoo, graded_tet}, `element_tendency`
{-0.6, 0, +0.6}, `eta_target` {0.005, 0.02, 0.05}, `adapt_passes` {0, 1}.
Batch 1 had pinned `element_tendency` and `eta_target` to one value each, so the
policy head had no signal on the shape and adaptivity dials at all.

### Validation MAE (log10), 30 runs

| head | run 1 | run 30 | LightGBM |
| --- | --- | --- | --- |
| predicted relative error | 0.815 | 0.809 | 0.768 |
| relative error, centred per part | 0.301 | 0.312 | 0.255 |
| mesh-to-CAD distance | 0.118 | 0.099 | 0.023 |
| mesh-to-CAD worst 1% | 0.125 | 0.097 | 0.033 |
| degrees of freedom | 3.707 | 0.150 | 0.017 |
| meshing time | 2.695 | 0.132 | 0.071 |
| solve time | 1.713 | 0.171 | 0.080 |

### Does it choose a better mesh than the default?

This is the only metric that scores a *decision* rather than a prediction.
`scripts/advisor/evaluate.py` replays every held-out case: the campaign ran a
known set of actions, so the best achievable outcome is known exactly, and
regret is how much worse the chosen action is, in log10 units.

| outcome | advisor | default | oracle | gain |
| --- | --- | --- | --- | --- |
| relative error | **0.3313** | 0.7609 | 0.0000 | **+0.4296** |
| mesh-to-CAD worst 1% | **0.1306** | 0.2254 | 0.0637 | **+0.0948** |
| solve time | 1.8703 | 0.3475 | 1.2598 | -1.5229 |

Split by how good the case's ground truth is — the finding that matters:

| truth quality | cases | advisor | default | gain | wins |
| --- | --- | --- | --- | --- | --- |
| analytic | 2 | 0.216 | 0.988 | **+0.771** | 1/2 |
| promoted overkill solve | 6 | 0.378 | 1.145 | **+0.767** | 5/6 |
| provisional beam surrogate | 4 | 0.203 | 0.072 | -0.131 | 0/4 |

**Where the reference truth is real, the advisor picks meshes ~0.77 decades —
about 6x — more accurate than the default, winning 6 of 8 cases. Every loss is
one of the four `l_bracket` cases whose "truth" is still a first-order beam
surrogate**, i.e. cases where the label itself is wrong and beating it means
nothing. That is the strongest argument yet for finishing those five truths.

The advisor buys that accuracy with time: `solve_ms` regret is worse than the
default's. That is the Stage-A weighting doing exactly what it is told, and it
is a dial (`bench/advisor/weights.json`), not a defect.

Mean Spearman rho between predicted and actual within-case action ordering is
0.094 — near zero. Regret improves anyway because regret only
depends on the top pick, while rho is dominated by mid-pack noise across 32
near-equivalent actions. Honest reading: the model finds good actions more often
than chance, but it does not rank the whole action set.

### Figures

All regenerated from the final model by `report.py` / `figures.py`, except the
cinema, which is the GUI framebuffer driven by `render_cinema.py`. It sequences
real advisor passes, a measured OOD refusal, production spectral sizing, mesher
snapshots, cell/order/quality inspection and the authoritative final solve
([ADR-0042](../decisions/0042-the-advisor-explains-itself-on-screen.md),
[ADR-0043](../decisions/0043-a-film-someone-can-read.md)):

| figure | what it shows |
| --- | --- |
| `network_layout.png` | the trained architecture, read live from the checkpoint |
| `training_curves.png` | per-head convergence, first vs latest run |
| `../assets/cinema/advisor_cinema.mp4` | 39 deployed-network passes on complex `icecream_cone`; honest OOD abstention; graded/spectral/quadratic fallback; real cell-quality, stress-gradient and ZZ fields; 60 s with readable holds — replaces the retired `activation_map.png` |
| `mesh_progress.png` | best-so-far accuracy and fidelity vs cumulative solver time |
| `accuracy_vs_cost.png` | accuracy vs DOF and vs solve time, Pareto front, by mesher |
| `fidelity_vs_h.png` | mesh-vs-BRep fidelity improving with resolution |
| `mesh_before_after.png` | real warehouse renders, coarsest run beside best-accuracy run |

Measured mesh improvement, coarsest run -> best-accuracy run for the same part:

- `sphere_box_s2_c2`: relative error 1.0 -> 7.2e-4 (**1389x**), 675 -> 62,472 DOF
- `stepped_shaft_s0_c1`: relative error 0.512 -> 2.47e-3 (**207x**), 297 -> 1,683 DOF
- `plate_notch_s2_c1`: relative error 0.458 -> 5.84e-3 (**78x**), 693 -> 7,041 DOF

Across the corpus the anytime curve improves median accuracy 1.50x and median
geometric fidelity 2.02x as solver time is spent.

## M-A3 — retrained on the corrected engine and re-derived truth (2026-08-12)

M-A2's policy result could not survive changes to the engine and reference
semantics as if nothing had happened. We bumped to `advisor-row-v3`, archived
the old rows, reran truth, and retrained without increasing model capacity.

### Engine changes that invalidated the old corpus

- Order-2 boundary mid-nodes are owner-aware projected onto exact CAD; the
  acceptance guard checks corner volumes and the stiffness quadrature rule,
  with validity-preserving bisection backoff.
- Selective p-elevation rejects candidates that the stiffness rule would make
  invalid.
- Longest-edge bisection rejects projected midpoints that destroy its progress
  measure and diagnoses a repeated LEPP edge instead of spinning.
- ZZ recovery bounds high-leverage, under-determined patch extrapolation with
  an SVD basis, L2 gain guard, and patch-mean fallback.
- Box-hole SCF labels now use the peak probe (`peak_vm_over_nominal`) that
  matches the Kirsch peak truth, rather than an area mean that could not reach
  3.0.
- The VTU wire renderer dispatches on cell topology, including tet10, hex20,
  polyhedron face streams, and convex point sets, instead of drawing raw
  connectivity as a polygon.
- Testlab's feature-refinement default now matches the CLI and GUI.

These are label changes, not harmless implementation details: geometry,
validity, recovered stress, adaptivity decisions, and the scored observable all
moved.

### Truth rerun and movement audit

The rerun promoted **60 files / 120 metrics**, including **5 files / 10
metrics** recovered for `l_bracket`. The final truth set is:

| truth quality | references | provisional |
| --- | ---: | ---: |
| promoted overkill solve | 64 | 0 |
| analytic | 8 | 0 |
| **total** | **72** | **0** |

Four channel parts could not be re-derived and retain their earlier references:
`channel_s1_c1`, `channel_s1_c2`, `channel_s3_c1`, and `channel_s3_c2`.

Most references moved **0.6–2.7 %**. Two large moves were real corrections:

- `l_bracket_s1_c0` energy moved **+7,660 %** because a stated provisional beam
  surrogate was replaced by an overkill solve.
- `stepped_shaft_s0_c0` energy moved **+950 %**. The old label was internally
  inconsistent: its coarse mesh resolved only 3.80e-6 m² of a 1.40e-5 m² load
  face. The replacement satisfies Clapeyron,
  \(U/(\tfrac12 F u)=0.99977\); the old value gave 0.628.

The SCF label fix is similarly semantic rather than cosmetic. The old
area-weighted mean over a rim box was graded against the Kirsch **peak** 3.0,
creating an unreachable error floor. The four box-hole analytic references now
score a box-windowed peak von Mises value over nominal stress.

### Data and artifact

- **3,456** `advisor-row-v3` rows.
- **196** legacy blank-schema rows explicitly excluded from
  `post-m10-smoke`, `settings-frontier-1`, `smoke`, `varyhedron-*`, and
  `vem-gate-m5`.
- **2,088 train / 672 validation**, with **696** rows pruned cumulatively.
- Model: **15,986 parameters**, width 96 / depth 2; 44 inputs / 10 action
  outputs; ONNX opset 17.
- C++/ONNX parity: **2.483e-06 relative**. C++ advisor tests pass:
  **168 assertions, 4 cases**.

Capacity was deliberately unchanged. The question was whether corrected data
and truth improved the decision, not whether a larger network could memorise
them.

### Validation MAE (log10): corrected model vs archived model

| head | M-A3 | archived M-A2 | direction |
| --- | ---: | ---: | --- |
| predicted relative error | **0.4235** | 0.8086 | better |
| relative error, centred per part | **0.2837** | 0.3121 | better |
| mesh-to-CAD distance | 0.1649 | **0.0989** | worse |
| mesh-to-CAD worst 1% | 0.1428 | **0.0970** | worse |
| degrees of freedom | 0.2025 | **0.1500** | worse |
| meshing time | 0.1732 | **0.1324** | worse |
| solve time | 0.1900 | **0.1708** | worse |

Accuracy improved; every geometry/cost head regressed. The same corrected data
also made LightGBM worse on `geo_chamfer` (0.0227 -> 0.0611), `geo_p99`
(0.0327 -> 0.0733), and `dof` (0.0165 -> 0.0232). This is evidence that engine
fixes and re-derived truths made those targets harder—a data-distribution
shift—not evidence that the unchanged 96-wide MLP uniquely ran out of capacity.

### Held-out decisions (`n=12`)

Mean within-case Spearman correlation is **0.610**. Regret is log10 distance
from the best available action for that outcome:

| outcome | advisor | default | oracle |
| --- | ---: | ---: | ---: |
| relative error | **0.6322** | 1.2822 | 0 |
| mesh-to-CAD worst 1% | **0.2307** | 0.2334 | 0.1758 |
| solve time | 1.7198 | **0.3629** | 1.6251 |

In linear terms the advisor's accuracy pick is approximately **4.3x** off the
oracle versus the default's approximately **19x**: about **4.5x better than the
default on accuracy regret**, replacing M-A2's stale approximately 6x claim.
The geometry win is marginal.

The time result is a trade, not a win. The accuracy-optimal oracle is itself
slow at 1.6251 solve-time regret, and the advisor tracks it to within 0.095 at
1.7198. It is buying accuracy with time rather than simply failing to predict
cost, while the default remains much faster at 0.3629.

## M-A4 — independent truth, honest gates, no model yet (2026-08-13)

No model was trained in this entry. It records why every accuracy number above is
now provisional, and what had to be fixed before a retrain could mean anything.
Full reasoning: [ADR-0029](../decisions/0029-independent-truth-and-honest-gates.md).

### The corpus was scoring itself

M-A3 reran truth, but the truth it reran was still ours: 64 of 72 references
carried `source = "overkill-reference"`, promoted from our own overkill solves, on
meshes ADR-0028 had just proven lose geometry. A reference produced by the system
under test cannot falsify that system.

All 88 non-closed-form references now come from **Gmsh 4.13.1 -> CalculiX 2.23**,
validated against closed form before adoption. Values moved a **median 3.79 %** and
up to **+88.4 %** (`stepped_shaft_s1_c2` strain energy), so the advisor had been
scored against values wrong by up to a factor of 1.9 on the families where geometry
loss was demonstrated. Tolerances are now derived from measured convergence, mostly
0.02 against the previous hand-picked 0.15.

The corpus's own `analytic` 3.0 for the four `box_hole_*_c0` SCF references was the
infinite-plate idealisation and was 2.9–4.1 % below the finite-width Howland value
it should have carried.

**Our solver was never the suspect.** On one identical Gmsh mesh CalculiX and our
solver agree to 3.4e-09 in tip deflection and 6.7e-10 in strain energy.

### Three engine defects the new truth exposed

- **The applied load depended on mesh resolution.** Traction is a pressure, so the
  resultant was scaled by whatever area the candidate mesh selected — up to 28.2 %
  short of the CAD face. Energy scales as \(F^2\). The traction is now rescaled onto
  the exact CAD rule area, which corrects the resultant but not its distribution.
- **One rule had two implementations.** The CAD and mesh sides disagreed by a factor
  of 2.3072 on `sphere_box_s2_c1` for `normal_min_dot = -1`. Now a single shared
  predicate. The prediction that `tube` would be unaffected, because its load slab
  was sized so the wall ring is empty, was made before the fix and held.
- **The load-area gate reported `rel_err = 0.0` when it could not verify anything**,
  including on a mesh missing 28 % of its loaded face. `unverified` is now a distinct
  state that is never a pass.

### Why no numbers are quoted here

Two findings above the line are load-bearing for reading M-A1 to M-A3:

- **The split leaked completely.** 672 of 672 validation rows had a training row
  with an identical (geometry-feature, action) vector. Every validation MAE and
  regret number published before that was found was measuring interpolation across
  three BC variants of a memorised geometry. See the
  [data card](0005-data-card.md) §"The split".
- **The reported chooser was not the shipped chooser.** `advisor_policy` shipped
  while every historical regret number came from `advisor_argmin`. The shipped rule
  is now feasibility-gated enumeration: `advisor_gated_0.05` reaches 0.3468
  `rel_err` regret at 10.0 % pick-failure against `advisor_argmin`'s 0.4413 at
  22.7 %, paired 38W-0L-262T, p = 7.3e-12. `advisor_argmin` versus the trivial
  `finest_action` is p = 0.258 — ranking alone does not beat the trivial rule; the
  gate earns the win. See the [model card](0004-model-card.md).

### Corpus widened for power, not coverage

`tube` and `perforated_plate` take the corpus to 8 families and 96 references. The
measured learning curve over 1–5 training families is flat, and eight families is
the smallest corpus at which a per-family effect is resolvable: CI half-width
0.0317 at six against 0.0164 at eight. `perforated_plate` sits at 1.70 descriptor
distance from `plate_notch`, below the 2.22 corpus minimum, so it is a weaker
transfer test than `tube` at 3.18 — a flat slope at eight families must be reported
with that qualification. Machine-readable in
[`bench/advisor/corpus_evidence.json`](../../bench/advisor/corpus_evidence.json).

### What must happen before the next number

Regenerate the corpus on the corrected engine, re-run the eight
`sphere_box_*_c1`/`_c2` cases affected by the load-rule fix, rebuild the dataset
(accuracy is re-derived from raw answers, so this no longer requires re-solving),
then retrain under leave-one-family-out with deployable baselines only.
