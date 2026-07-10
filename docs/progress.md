# PROGRESS

## Current phase
**Master plan:** [`docs/ROADMAP.md`](ROADMAP.md) · **Agent loop:**
[`docs/process/agent-loop.md`](process/agent-loop.md)

**Active:** Track A (GUI usable) → M1 study app. 70 tests green. Solver core
(GATE 1) frozen. Mesh/adapt product path advancing (graded, hexpyr, seed remesh).

GATE 1 deliverables ready:
- Full Tier-0 + Tier-1 suite (Lamé, Timoshenko, Kirsch, Goodier, L-domain)
- MMS convergence orders matching theory
- Gmsh `.msh` v2.2 ASCII import
- Convergence report: `bench/reports/p1-gate1-convergence.md`
- ADR-0009 (Tier-1 verification setups)

GATE 0 was approved by owner on 2026-07-09.

## Done
- 2026-07-10: CI green again — clang-format 18.1.8 pinned in workflow (was drift vs local), full tree reformat; rename `namespace pipe` alias in test_transition_fill (POSIX `pipe()` collision on Ubuntu).
- 2026-07-10: Master ROADMAP + agent-loop protocol; GUI M1 path — argv open,
  mesh-only job + element-type preview, ZZ error field + colorbar, failure
  dismiss, public `unit_box.stl` fixture. (in progress / this commit)
- 2026-07-10: A posteriori adapt seeds — Dörfler centroids → graded fine balls;
  `suggest_refine`; pipeline adapt remesh; CLI `solve --adapt n`. ~70 tests.
- 2026-07-10: Graded tet feature band (sharp-edge distance), pipeline
  `feature_refine`, CLI `--feature`, feature-block stats in mesher notes. 68 tests.
- 2026-07-10: FeatureSizing field + feature-aware solve h; pyramid5 patch test
  (pure pyramid lattice); pyramid base orientation for +Jacobian; Duffy product
  quadrature; documented hex–pyramid hybrid nonconformity (ADR-0013). 66 tests.
- 2026-07-10: Hex+pyramid boundary snap (0.35h), pipeline residual distance note,
  CLI solve/mesh `--mesher` + `--skin`, pyramid tet-split stiffness (flip-safe
  scatter), ADR-0013 snap notes. 63 tests.
- 2026-07-10: Graded tet fill (fine skin / coarse core), surface conformity
  metrics, ADR-0012 (hybrid = graded all-tet until pyramids). 58 tests.
- 2026-07-10: Prism6 wedges; hex-VEM hybrid; quality metrics; CalculiX smoke.
- 2026-07-10: Hex grid fill option + GUI mesher selector; tet quality notes.
- 2026-07-10: VEM k=1 polyhedra (patch test + 6 RBM), adapt_passes in pipeline,
  feature grading, CalculiX smoke peer, GUI adapt/feature controls. 50/50 tests.
- 2026-07-10: Product batch — VTU export, ZZ recovery + Dörfler marking,
  sharp-edge features + graded sizing, limited surface snap on tet fill,
  CLI `mesh`/`solve`, GUI STEP paths + theme switch + VTU export button,
  linguist fix (graphify HTML vendored). 47/47 tests green.
- 2026-07-10: Optional OpenCASCADE STEP path — `geom::load_step`, CMake
  `POLYMESH_WITH_OCC` finds OCCT (TKDESTEP + BRepMesh), stub throws when OFF;
  Catch2 tests + unit-cube fixture.
- 2026-07-10: G1 — ADR-0010 keep face-based mesh; geometric validity;
  `mesh::tet_fill_surface` (tet4 grid fill); pipeline/GUI use tet4 path
  (replaces draft voxel hex8). 39/39 tests green.
- 2026-07-10: Campaign G0 — branch `master`, BSD-3-Clause, apps/src split,
  pipeline vs GUI separation, CONTRIBUTING/CHANGES, docs under docs/.
- 2026-07-09: D1–D5 + GUI scope ratified with owner (ADR-0001..0006).
- 2026-07-09: License BSD-3-Clause applied; process docs live under docs/.
- 2026-07-09: Owner switched language to C++ (ADR-0007) and made CUDA a
  first-class optional backend (ADR-0008). C++ scaffold the same day:
  CMake/Ninja workspace (geom, mesh, adapt, fea, bench, cli), STL loader with
  welding, face-based mesh structure with structural validity checker,
  Material/D-matrix, backend dispatch (cpu/cuda), reference-case loader,
  CLI `check`/`backend` subcommands, Catch2 tests green.
  CI: warnings-as-errors build + ctest + clang-format + grep audit.
- 2026-07-10: P1 Tier-1 completion — Kirsch plate (SCF 3.056 vs 3), Goodier
  cavity (SCF 1.902 vs 2.045), L-domain singularity energy-gap order 1.265
  vs 2λ=1.089, Gmsh v2.2 import, GATE 1 convergence report, ADR-0009.
  37/37 tests green.

## Benchmark table
| Case | Status |
|---|---|
| Tier 0 patch test (all 4 element types, distorted meshes) | PASS, max error < 1e-12 m |
| Tier 0 rigid-body modes | PASS (< 1e-12 relative) |
| Tier 0 single-element eigenvalues (6 zero modes) | PASS |
| Tier 1 Timoshenko cantilever (hex20, gravity load) | PASS, tip err 1.50% (tol 3%) |
| Tier 1 Lamé cylinder (hex20 sector) | PASS, u_r 0.0068%, hoop 1.36% |
| Tier 1 Kirsch plate (hex20, exact field BC) | PASS, SCF 3.056 vs 3 (1.87%) |
| Tier 1 Goodier cavity (hex20 shell, b/a=15) | PASS, SCF 1.902 vs 2.045 (7.0%) |
| Tier 1 L-domain (hex20, energy-gap order) | PASS, order 1.265 vs 1.089 (±0.35) |
| Tier 2 MMS convergence | PASS: tet4 0.997, hex8 0.997, tet10 2.000, hex20 2.000 (theory 1/1/2/2, tol ±0.2) |
| Tier 2 MMS exact-representation sanity (p=2, quadratic field) | PASS (< 1e-9 relative) |
| Tier 3 performance | not yet (needs P2+ adaptive path) |

## Open issues
- GATE 1 frozen; see `bench/reports/p1-gate1-convergence.md`.
- License closed: BSD-3-Clause (ADR-0002); no CLA process.
- `POLYMESH_WITH_OCC` wired in `src/geom` (`load_step`, CMake find + stub
  when OFF); exact B-rep feature queries still deferred to P3 (ADR-0001).
- CUDA toolkit not installed on dev machine; `POLYMESH_WITH_CUDA` untested
  until it is (ADR-0008). RTX 3080 Ti present.
- Geometric validity: boundary manifold + tet volume checks in place; limited
  surface snap on tet and hex+pyramid fills. True Delaunay / CAD feature
  queries still open.
- Goodier: exact continuum-field BCs + ZZ recovery would tighten SCF further
  (ADR-0009); P1 bar is 12% with Saint-Venant Dirichlet + nodal averaging.
