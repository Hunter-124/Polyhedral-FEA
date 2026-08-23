# Portable-cost advisor retrain

**Status:** shipped evidence for the 2026-08 portable-cost cycle.

This cycle removes host wall time from the advisor's optimization objective.
The network now predicts hardware-portable solve FLOPs, structural byte traffic,
and dimensionless mesh work. A host calibration converts those quantities to an
optional report-time estimate; changing hosts does not require retraining.

## Solver instrumentation

`fea::analyze_solve_cost` builds the exact reduced free-DOF pattern without
assembling a numeric factor. It applies Eigen's default AMD ordering, constructs
the Liu elimination forest, and runs Gilbert-Ng-Peyton column counting. It
reports:

- `nfree` and reduced `pattern_nnz`;
- `factor_nnz`, equal to Eigen's stored strict-lower `SimplicialLDLT` factor;
- direct factor FLOPs $\sum_j(|L_j|+1)^2$;
- CG work per iteration
  `2*nnz(K) + 2*nnz(L_ichol) + 10*nfree`;
- CG traffic per iteration `12*nnz(K) + 48*nfree` bytes.

Measured solve results carry method, iteration/restart counts, factor fill,
FLOPs, and bytes. Direct totals add `4*factor_nnz` for two triangular solves;
CG totals multiply the reported progress-callback iteration count by the
per-iteration model. Symbolic analysis is observation-only: a direct solve is
bit-identical to an independently assembled baseline.

Verification:

- hand pattern vs brute-force symbolic elimination;
- hand pattern vs Eigen's actual stored LDLT nonzeros;
- plate-with-hole in the direct regime vs Eigen's factor;
- exact resource estimate drives LDLT/CG preflight;
- direct and CG formulas are checked against runtime method/iteration telemetry.

The production `plate_hole` h=4 mm mesh is about 221k DOF, deliberately beyond
the direct regime; allocating that factor in a unit test would negate the
count-only API and exceeded ten minutes. The exact numeric-factor cross-check
therefore uses the same CAD part at h=20 mm, while the h=4 mm geometry is used
by the committed five-run host mesh calibration.

## Host calibration and reporting

`polymesh calibrate --out host.json` measures a fixed double-FMA loop, STREAM
triad bandwidth, and the median of five `plate_hole` graded-tet meshes at h=4 mm.
The committed Hunter host result is:

| quantity | value |
| --- | ---: |
| compute | 9.405e8 FLOP/s |
| bandwidth | 2.755e10 byte/s |
| reference mesh median | 54,507.1 ms |

Report-time solve cost is the roofline expression
$\max(F/P_{host}, B/W_{host})$. Mesh work is `mesh_ms / ref_mesh_ms`. Neither
host rate is a training label.

## Feature and corpus expansion

Thirteen appended inputs describe the interactions missing from the previous
corpus: inner loops, hole/feature spacing quantiles, dihedral quantiles, minimum
Williams singular exponent, load/fix distance to features, and multiaxiality.
C++ exact-BRep extraction and Python offline extraction agree on three new-family
parts with maximum absolute difference `5.56e-17`; the deterministic two-hole
fixture reports two inner loops and finite spacing.

The procedural corpus now contains 15 families x 4 regimes x 5 archetypes =
300 cases. Four new families add ribs, gussets, near-touching unequal holes, and
near/remote bosses. `c3` applies perpendicular resultants on two disjoint
virtual patches of the free end face; their authored areas are exact BRep clips
and both product and truth chains preserve the resultants. `c4` applies pressure
normal to the largest free planar face.

All 168 c3/c4/new-family truths come from the independent Gmsh 4.13.1 ->
CalculiX 2.23 chain. Scored metrics are strain energy and displacement only;
no internal PolyMesh solve and no peak/max von Mises value is promotable.
Fifty cases passed a 2% two-rung convergence gate and ten c3 cases carry explicit
2.03-5.53% measured numerical uncertainty. Existing 132 protected truth inputs
were not rewritten by generation.

The 60-solid descriptor gate passed for every new family and rejects a deliberate
near duplicate. Evidence is in:

- `bench/advisor/evidence/geometry_features.csv`;
- `bench/advisor/evidence/corpus_evidence.json`;
- `bench/reference/external/*-audit.json`.

## Campaign coverage and dataset

The exhaustive configuration matrix was run on `hunter-pc`. Meshing several
fine graded configurations is non-interruptible and the run exceeded the
approved two-day ceiling, so the explicit contingency was applied rather than
fabricating or extrapolating rows.

| campaign | planned | unique recorded | coverage |
| --- | ---: | ---: | ---: |
| full solves | 57,600 | 37,525 | 65.15% |
| cost-only extension | 4,800 | 4,517 | 94.10% |

Full statuses are 12,809 `ok`, 20,866 `mesh_fail`, 1,196 `solve_suspect`,
2,021 `solve_fail`, and 633 `over_budget`. Cost-extension statuses are 3,201
`cost_only`, 1,268 `mesh_fail`, 45 `solve_fail`, and 64 `over_budget`.
Uncompleted rows remain absent and masked; `campaign_coverage.json` records every
omitted `(part,cfg_id)`. New-family c4 is the only intentionally reduced
archetype; the other omissions are wall-budget exhaustion.

The assembled v3+v4 dataset contains 36,010 rows:

- 15,578 rows supervise each accuracy head;
- 17,707 rows supervise each portable cost head;
- all 36,010 supervise feasibility;
- cost-only rows never supervise accuracy;
- v3 rows remain valid accuracy evidence and have all portable heads masked.

Rows carry `host` and `cost_label_source`; exact duplicate campaigns are
collapsed, and a corrected training-eligible row wins over a prior
resolution-refusal row regardless of directory sort order.

## Precision and architecture experiments

The RTX 3080 Ti benchmark measured:

| network / arithmetic | examples/s |
| --- | ---: |
| shared FP32 | 439,023 |
| shared TF32 | **1,246,521** |
| shared FP16 | 415,843 |
| shared BF16 | 507,811 |
| hybrid QAT FP16 (INT4/INT8 fake quant) | 255,038 |
| hybrid QAT BF16 (INT4/INT8 fake quant) | 354,269 |

Ampere and stock PyTorch provide low-bit forward paths, not native INT4/INT8
backward and optimizer kernels for this MLP. The QAT prototype therefore kept
floating gradients/state and was slower. Production training uses measured-fast
TF32; deployed ONNX inference remains deterministic CPU FP32.

A four-branch accuracy/geometry/cost/feasibility experiment was evaluated over
12 family-held-out folds x 5 seeds. On the final v4 dataset it did not pass the
pre-registered paired promotion gate, so the production shared trunk was kept.
The rejected design and raw paired results remain in
`architecture_benchmark.json`; an unproven deeper graph was not shipped merely
because it was larger.

Public CAD/FEA sources were also evaluated. ABC, Fusion 360 Gallery/Assembly,
Thingi10K, MCB, SimJEB, and ShapeNet either lack a usable redistribution license,
explicit load/BC/material metadata, or independently reproducible truths. None
was silently imported into this supervised dataset.

## Training and shipped graph

Two clean 150-run rehearsals were performed. The first exposed that the measured
campaign included h_rel=0.28 while deployment still clamped at 0.20; that graph
was discarded. The action clamp was corrected to 0.28 and a second clean 150-run
production retrain completed on CUDA TF32.

The shipped checkpoint is Stage-B run 144. Its held-out-family metrics are:

| head | MAE (log10 units unless noted) |
| --- | ---: |
| rel_err | 0.7507 |
| rel_err_rel | 0.6186 |
| geometry chamfer | 0.5811 |
| geometry p99 | 0.3731 |
| solve FLOPs | 0.4159 |
| solve bytes | 0.2859 |
| mesh work | 0.2835 |
| failure AUC | 0.8921 |

The 75-input, 19,156-parameter ONNX graph exports twelve contract outputs plus
three activation taps. ONNX Runtime parity is `3.063e-06` relative for the
contract and `1.951e-06` for taps, both under `1e-05`.

## Deployed selection behavior

Accuracy remains the default objective. `--advisor-objective efficiency` loads
the optional host calibration, finds the best gate-passing relative-accuracy
score, keeps actions within a multiplicative 5% envelope, and selects the lowest
predicted mesh-plus-roofline solve time. Without calibration it falls back to
accuracy and says so.

Live proof on `plate_hole.step` selected `hex`, h_rel=0.12, order 1, with
`failure_prob=4.83e-09`, OOD distance 5.309, predicted solve 2.536e6 FLOPs /
1.943e6 bytes / 2.696 ms, and completed the real solve in 0.81 s. The decision
note was `efficiency objective: lowest calibrated cost within 5% of best
accuracy`.

## Honest limitations

- The exhaustive matrix is intentionally incomplete at its measured two-day
  wall ceiling; coverage and omissions are machine-readable.
- Family-held-out accuracy remains difficult (0.62 decades on the primary
  relative head); the advisor gates and can refuse rather than feign certainty.
- OOD calibration reports 47.2% of held-out rows beyond the training q99 and a
  17.4% missed-failure rate at the 0.5 threshold. These values are published,
  not tuned away after inspection.
- Fine graded meshes can spend hours inside non-interruptible geometry code.
  The campaign records that limitation; it does not substitute a synthetic cost.
