# Progress

Short summary of the most recent period. Current status and open work:
[`STATUS.md`](STATUS.md). Full dated chronology through `ef464dc`:
[`archive/progress-history.md`](archive/progress-history.md).

## 2026-09

| Date | Change | Evidence |
|---|---|---|
| 09-11 | **CAD integration dependency release** (`ef464dc`, branch `cad-fill-progress`, from the public `9554579` baseline). Graded-fill progress observer for the CAD program: same geometry with observation on/off, same-thread callbacks, propagated cancellation, nested-observer restoration. Three integration repairs: SparseLU status queried only after numeric factorization (assertion-enabled Eigen aborted; `direct_sparselu_cantilever` regression); CHOLMOD rung declaration made conditional for strict builds without SuiteSparse; an early graded-refinement ceiling carries its count through `RefinementLimitError`, so auto sizing coarsens and explicit sizing refuses. CHOLMOD/advisor CLI solved the STEP plate-with-hole: 31,808 tet10, 146,205 free DOF, supernodal rung, every VTU array finite. | `test_mesh_budget.cpp` |
| 09-05 | **`polymesh solve` usable as a cross-check lane.** Quadratic preflight reserve removed (183 s → 0.02 s on `smoke_bar`); single solve when promotion is unconditional; triplet-free, thread-count-deterministic assembly; SuiteSparse direct ladder `CholmodSupernodalLLT` → `SimplicialLDLT(AMD)` → `SparseLU(COLAMD)` with an 8-thread cap. Default `plate_hole` h = 6 mm: 31.6 s / 1.30 GiB / 4.20 s factorize; simplicial rung factorizes 56× slower; CG had not finished after 48 min. New `--solver`, `--threads`, `phases:` line. Meshing is now the largest phase (open, see STATUS). | [`solver-core.md` §6](solver-core.md#6-what-the-linear-solve-costs) |

## 2026-08 (second half)

| Date | Change | Evidence |
|---|---|---|
| 08-22 – 08-24 | **Portable-cost advisor shipped.** Solver cost telemetry (`fea::analyze_solve_cost`), `polymesh calibrate` host file, 13 new inputs (75 total), 15-family / 300-case corpus with independent Gmsh + CalculiX truths, 36,010-row dataset, Stage-B run 144, `--advisor-objective efficiency`. Four-branch architecture did not pass the paired gate; shared trunk kept. | [`advisor/0012`](advisor/0012-portable-cost-retrain.md) |
| 08-21 | GLM evaluated and rejected as a math library (ADR-0044); showcase film rebuilt for legibility (ADR-0043). | [`bench/mathlib-probe.txt`](bench/mathlib-probe.txt), [`assets/cinema/NOTES.md`](assets/cinema/NOTES.md) |
| 08-20 | Field-level verification of the shipped solves: 33 pointwise stress/displacement checks, all inside band. | [`validation/field-verification.md`](validation/field-verification.md) |
| 08-19 – 08-20 | Boundary-condition correctness: box selection is a region (ADR-0037), fixtures act on the boundary (ADR-0038), stranded boundary nodes rescued (ADR-0039), BCs name the exact closure of a CAD face so GUI and CLI fields agree (ADR-0040), deformed renders carry the undeformed outline (ADR-0041). | [`decisions/`](decisions/README.md) |
| 08-18 | v7 retrain on authoritative curved CAD geometry; superseded by the portable-cost cycle. | [`advisor/0011`](advisor/0011-v7-curved-geometry-retrain.md) |
| 08-16 – 08-17 | Curved quadratic solve geometry becomes the CAD default; exact-BRep boundary and exterior conformity (ADR-0035); spectral sizing, coarsening, budget-feasible advisor, CG equilibration (ADR-0034). | [`archive/progress-history.md`](archive/progress-history.md) |

Earlier entries (July – mid-August 2026, including the frozen measure-first
board and the ADR-0033 mesher-quality wave) are in the archive.
