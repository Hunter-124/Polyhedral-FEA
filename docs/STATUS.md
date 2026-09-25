# Status

The single current-status page for PolyMesh. Updated 2026-09-25 from the
`ef464dc` checkpoint. Dated history is in
[`docs/progress.md`](progress.md) (recent period) and
[`docs/archive/progress-history.md`](archive/progress-history.md) (full
chronology); decisions are indexed in [`docs/decisions/`](decisions/README.md).

## Current state

| Subsystem | State | Evidence |
|---|---|---|
| **Mesher** | CAD-only input (`.step .stp .brep .brp`, OCC on by default). Product default is `graded` with curved solve geometry: tet4/hex8 promoted to tet10/hex20, boundary mid-nodes projected onto the exact BRep behind a positive-Jacobian gate. Boundary conformity pins nodes to exact CAD edges/vertices and gates the true element exterior (`conform_true_exterior`); every gate measures the cell that ships. Other meshers: `hybrid` (hex bulk + pyramid skin), `hybridvem`, `hexpyr`, `hex`, `hexvem`, `tet`, `prism`; `cvt` is experimental. | ADR-0033, ADR-0035, ADR-0039; [`cli.md` mesher names](cli.md#mesher-names) |
| **Varyhedron** | **Core product direction** (ADR-0021): variable-polyhedron packing on protected sharp-edge seeds, interior bubble seeds and a live BRep oracle (`VolumeMesher::kVaryhedron`, CLI/GUI/testlab `varyhedron`). Current output is a CAD-edge-seeded tet scaffold, so its accuracy claims fall under the tet-FE rule; the poly export target is constrained restricted CVT / clipped Voronoi. | ADR-0021, ADR-0023; [`plans/advisor-measure-first-program.md`](plans/advisor-measure-first-program.md) |
| **Octahedral** | Experimental (`kOctahedral`, BCC octahedra split to tet4). Not a product default. | ADR-0019 |
| **Poly-VEM** | Correct where used (stress recovery, `VTK_POLYHEDRON` export) but not the product default: the M5 gate was measured and not promoted. | [`vem-gate-m5/GATE.md`](../bench/campaigns/vem-gate-m5/GATE.md) |
| **Solver** | `--solver auto` takes the sparse direct ladder `CholmodSupernodalLLT` → `SimplicialLDLT(AMD)` → `SparseLU(COLAMD)` whenever the estimated factor fits the memory budget; CHOLMOD is optional (`POLYMESH_WITH_CHOLMOD`, auto-detected) and capped at 8 OpenMP threads. CG (equilibrated, incomplete-Cholesky with Jacobi fallback, bounded iterations) runs only above `SolveOptions::cg_threshold` (50,000 free DOF without CHOLMOD, 1,500,000 backstop with it) or when the factor does not fit. Assembly is deterministic across thread counts. | [`solver-core.md` §6](solver-core.md#6-what-the-linear-solve-costs) |
| **Adapt** | ZZ recovery with dimensionless `global_eta` and `--eta-target`; Dörfler seed remesh, LEB local h-refine, p-elevation, coarsening, spectral size-field smoothing, joint (h, p, shape) driver. | ADR-0014, ADR-0016, ADR-0019, ADR-0034 |
| **Advisor** | Portable-cost cycle shipped: 75-input, 19,156-parameter ONNX graph (Stage-B run 144) predicting accuracy, geometry fidelity, feasibility and hardware-portable solve FLOPs/bytes/mesh work; inference on ONNX Runtime CPU. Artifacts: `bench/advisor/{model.onnx, normalization.json, clamps.json, ood.json, activation_layout.json}`. `--advisor-objective efficiency` uses an optional `polymesh calibrate` host file. Inputs are geometry plus geometric BC features; loading-condition signal is not an input. | [`advisor/0012`](advisor/0012-portable-cost-retrain.md) |
| **GUI** | `polymesh-gui` Studio: CAD open, CAD-face fixtures/loads applied to the exact face closure, mesh preview, solve, von Mises / displacement / η / gradient display on curved quadratic faces, VTU export, headless `--auto` verbs (`savevtu`). Owner theme sign-off (old gate A9) not recorded. | ADR-0040, ADR-0041; [`gui/theme-layout.md`](gui/theme-layout.md) |
| **CLI** | `polymesh check / mesh / solve / diag / render / calibrate / backend`; conservative loads (`--force`, `--traction`, `--load-dir`), box selections (`--fix-box`, `--load-box`), `--solver`, `--threads`, per-phase time and peak RSS. External tools use it as the cross-check lane. | [`cli.md`](cli.md) |
| **CAD integration** | The CAD program pins the internal `ef464dc` checkpoint, which is distinct from the public `master` history. That checkpoint adds a synchronous, per-thread graded-fill progress observer (`mesh::FillProgressScope`, `FillOptions::on_progress`) that never changes the geometry it observes. | [`progress.md`](progress.md) |

Completed early tracks (GUI M1, grid meshers, hybrid honesty, adapt, Tier 0–2
verification, OpenMP/CG/CUDA SpMV, release packaging — former tracks A–G) are
recorded in [`archive/progress-history.md`](archive/progress-history.md); the
retired track plan remains in git history at `ef464dc`.

## Open defects

| Defect | Detail | Next thread |
|---|---|---|
| Graded sliver chain | `cylinder` graded h=0.005 builds a mesh CG cannot solve (min edge 0.004 h). | Graded-snap re-engineering, not a threshold change (ADR-0033). |
| `ellipsoid_boss` boundary tail | Binding constraint is `hex8_shape_quality >= 0.02` against the required wall travel. | The size field, not the snap (ADR-0033). |
| Meshing dominates run time | `perf`: 29.6 % of `polymesh mesh` on `plate_hole` h = 6 mm is in `pipeline::conform_true_exterior` (`edge_pass=10423 ms`), inner loop `element_jacobians_positive`. | Profile-driven; fine graded meshes can also sit for hours in non-interruptible geometry code (advisor/0012). |
| CLI default BCs on curved parts | Box-less CLI selection is an end slab with a normal-aligned fallback; the CLI has no CAD-face selection (the GUI/library does, ADR-0040). | Face-ID selection on the CLI. |
| Advisor generalisation | Family-held-out relative-error MAE 0.62 decades; 47.2 % of held-out rows beyond training q99; 17.4 % missed-failure rate at 0.5. Exhaustive matrix 65.15 % covered at its two-day ceiling. | See Next. |
| Tier-3 product path | D6 Tier-3 (5.12× DOF, 12.2× time) is measured on the L-domain instrument only, not on the full public suite. | — |

## Next

| ID | Item |
|---|---|
| H-H2 | True hex + pyramid + tet FE (isoparametric bulk hex) instead of hex→pyramid expansion. |
| H-O1 | Octahedral experiment: measure against the frozen metrics before any promotion. |
| H-V1 | Mesh-aware / AMG preconditioner for the CG path (IC + Jacobi fallback exists). |
| H-E1 | Mesher scoreboard close-out (`bench/mesher/run_mesher_scoreboard.py`). |
| Varyhedron | Polyhedral export (restricted CVT / clipped Voronoi) replacing the tet scaffold, gated like any other claim. |
| Advisor | More advanced architecture and training (the four-branch design did not pass the paired gate in advisor/0012); feed loading-condition signal — not only geometric BC features — into meshing and the advisor. |
| CUDA | Batched element-stiffness kernels (SpMV parity exists). |

## Evidence index

| Area | Documents |
|---|---|
| Advisor | [`0012` portable-cost retrain](advisor/0012-portable-cost-retrain.md) (current) · [`0004` model card](advisor/0004-model-card.md) · [`0005` data card](advisor/0005-data-card.md) · [`0001` architecture](advisor/0001-architecture.md) · [`0003` training log](advisor/0003-training-log.md) · [training guide](training/README.md) |
| Solver and CLI | [`solver-core.md`](solver-core.md) · [`cli.md`](cli.md) |
| Benchmarks | [`bench/scoreboard.md`](bench/scoreboard.md) · [`bench/d6-tier3.md`](bench/d6-tier3.md) · [`bench/mathlib-probe.txt`](bench/mathlib-probe.txt) · [`benchmarks.md`](benchmarks.md) · [`bench/reports/`](../bench/reports/p1-gate1-convergence.md) (GATE 1 convergence) |
| Validation | [`validation/field-verification.md`](validation/field-verification.md) (33 pointwise checks) · [`validation/hand-calcs.md`](validation/hand-calcs.md) |
| Showcase | [`SHOWCASE.md`](SHOWCASE.md) · [`assets/showcase/OVERVIEW.md`](assets/showcase/OVERVIEW.md) · [`assets/cinema/NOTES.md`](assets/cinema/NOTES.md) |
| Campaign gates | [`vem-gate-m5/GATE.md`](../bench/campaigns/vem-gate-m5/GATE.md) · [`varyhedron-baseline-m9/BASELINE.md`](../bench/campaigns/varyhedron-baseline-m9/BASELINE.md) |

## Methodology in force

From the measure-first program
([`plans/advisor-measure-first-program.md`](plans/advisor-measure-first-program.md),
[ADR-0023](decisions/0023-measure-first-tet-primary-cvt-path.md),
[ADR-0024](decisions/0024-advisor-measure-answers.md)) and the honesty ADRs
([ADR-0029](decisions/0029-independent-truth-and-honest-gates.md),
[ADR-0033](decisions/0033-a-gate-must-measure-what-ships.md)):

- Measure before claiming; a change is a delta against a frozen baseline.
- Tet FE is the accuracy claim; poly-VEM is promoted only by a measured gate. No dual-first, no frame-field core.
- Never score raw nodal max stress; score strain energy, displacement and face-mean probes.
- Truth comes from an independent chain (Gmsh + CalculiX or closed form), never from our own mesher or solver.
- A gate must measure the cell that ships.
