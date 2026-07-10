# PROGRESS

## Current phase
**Master plan:** [`docs/ROADMAP.md`](ROADMAP.md) · **Agent loop:**
[`docs/process/agent-loop.md`](process/agent-loop.md)

**Active:** Track A (GUI usable) → M1 study app. Solver core (GATE 1) frozen.
Mesh/adapt product path advancing (graded, hexpyr, seed remesh).
F1/F2 + E1–E3 + B1/B3/B4 + C1 + C2 + C5 + D3 + D5 + E4 + G1–G4 done.

GATE 1 deliverables ready:
- Full Tier-0 + Tier-1 suite (Lamé, Timoshenko, Kirsch, Goodier, L-domain)
- MMS convergence orders matching theory
- Gmsh `.msh` v2.2 ASCII import
- Convergence report: `bench/reports/p1-gate1-convergence.md`
- ADR-0009 (Tier-1 verification setups)

GATE 0 was approved by owner on 2026-07-09.

## Done
- 2026-07-10: D3 p-elevation — `fea::promote_to_quadratic` / `fea::p_elevate`
  (tet4→tet10, hex8→hex20, shared mid-edge map); `adapt::mark_smooth` (Dörfler
  complement); `SimSetup::p_elevate` + auto when `adapt_passes>0`; CLI
  `--p-elevate`; GUI checkbox; Catch2 promote/patch/selective/mark tests.
  test_support wraps product API.
- 2026-07-10: C5 Kirsch equal-DOF graded vs uniform tet — structured annular
  tet10 (ADR-0009 BC setup; not Cartesian product fill — stair-case on hole,
  ADR-0015). Same free DOFs (648); log radial map vs linear: SCF rel err
  **0.70%** vs **3.06%** (analytical SCF=3). Catch2
  `test_kirsch_c5_graded.cpp`. ROADMAP C5 closed (GATE 3 Kirsch leg).
- 2026-07-10: C2 curvature + thin-wall indicators — `geom::estimate_vertex_curvature`
  (dihedral 1-ring |H| proxy) + `estimate_local_thickness` (inward ray cast);
  `adapt::GeometrySizing` / `make_geometry_sizing` mins sharp-edge blend, h≈c/κ,
  h≈f·thickness; pipeline feature-grading samples geometry sizing. Catch2 thin
  plate vs bulk + sphere vs flat. ROADMAP C2 closed.
- 2026-07-10: F2 iterative CG solve — `SolveOptions` / `SolveMethod`
  (`kAuto`|`kDirect`|`kCG`); default auto switches to Eigen
  `ConjugateGradient` + `DiagonalPreconditioner` when free DOFs > 8000
  (else `SimplicialLDLT`). `select_solve_method` for diagnostics. Catch2:
  forced-CG vs direct cantilever + patch, auto CG on ~15k free-DOF hex
  cantilever. README + `solve.hpp` docs. Patch-test direct path unchanged.
- 2026-07-10: G2+G3+G4 — `examples/` README + `run_mesh_public.sh` /
  `run_solve_public.sh` (auto-h CLI on `bench/geometries/public/*.stl`, symlink
  geometries); public-header SI units/doxygen spot-check (SimSetup, volume_mesh,
  write_vtu, sizing, tet/hex/graded/transition fills); CI `actions/checkout@v5`
  (setup-python stays @v5); ROADMAP G2–G4 closed.
- 2026-07-10: F1 OpenMP assembly — CMake `POLYMESH_WITH_OPENMP` (default ON)
  finds OpenMP; when present, `fea` links `OpenMP::OpenMP_CXX` and defines
  `POLYMESH_WITH_OPENMP`. `assemble_stiffness` uses `#pragma omp parallel for`
  with thread-local triplets (critical-free hot loop), then merges; serial if
  OpenMP missing. README notes. Patch/Tier-0 remain green with OpenMP on.
- 2026-07-10: E1/E2/E3 — CalculiX peer `run_calculix_cantilever.py` (skip exit 0
  without ccx; JSON when present); gate1-p1 Lamé/Kirsch/cantilever scoreboard
  + `emit_polymesh_gate1.py` best-effort DOF fill; `audits/README.md` holdout
  protocol (no secret geometries). Scoreboard regenerated.
- 2026-07-10: B1/B3/B4 — ADR-0015 Cartesian grid-fill limits (not Delaunay);
  surface-snap Jacobian safety (unsnap nodes that invert tet / hex J / pyramid
  volume); README OCC enablement (Ubuntu libocct-* + `POLYMESH_WITH_OCC=ON`);
  Catch2 unit-box snap + L-domain fixture validity. B1 = documented limits.
- 2026-07-10: D5+E4+G1 — `resolve_mesh_size` (bbox extent/16 ∩ diagonal/28 +
  sharp-edge density / min feature); pipeline mesh-only+solve and CLI omit `-h`
  use it; mesher_note carries `auto h=…` for GUI. E4 Catch2 product-mesh box
  cantilever (max|u|>0, finite σ_vm) + cylinder_prism smoke (not Lamé tol).
  README quickstart: Ubuntu apt (CI list), cmake/build/ctest, CLI mesh/solve on
  `unit_box.stl`, GUI argv/auto-h note. 81 tests (with C1).
- 2026-07-10: C1 hybrid honesty — product FE path `expand_hex_core_to_pyramids`
  (interior hex → 6 pyramids, matching face diagonals); pipeline kHexPyramid
  always expands; Catch2 hybrid constant-strain patch < 1e-12 on mixed lattice;
  ADR-0013 amended. Pure-pyramid patch unchanged. 77 tests.
- 2026-07-10: B2+B5 — VTU `VtuCellData` + tet4 `quality` cell array on CLI/GUI export;
  Catch2 CellData XML check; public fixtures `l_domain`/`plate`/`cylinder_prism` +
  README; STL load smoke. 74 tests.
- 2026-07-10: GUI A6/A7/A8 — wireframe + undeformed outline toggles (OpenGL
  boundary edges), GLFW drag-drop open (.stl/.step/.stp) with path field
  fallback, mesh_note + DOF (3×nnodes) in sidebar/status after mesh/solve.
- 2026-07-10: D2 global η stopping criterion — `SimSetup::eta_target` (0=off);
  adapt loop early-stops when `global_eta ≤ eta_target`; CLI `--eta-target`;
  GUI η input near adapt passes; Catch2 early-stop + disabled-path tests.
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
- Geometric validity: boundary manifold + tet volume checks; limited surface
  snap with Jacobian unsnap (B3) on tet and hex+pyramid fills. True Delaunay
  deferred (B1 = ADR-0015 documented limits). CAD feature queries still open.
- Goodier: exact continuum-field BCs + ZZ recovery would tighten SCF further
  (ADR-0009); P1 bar is 12% with Saint-Venant Dirichlet + nodal averaging.
