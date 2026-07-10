# PROGRESS

## Current phase
**Master plan:** [`docs/ROADMAP.md`](ROADMAP.md) · **Agent loop:**
[`docs/process/agent-loop.md`](process/agent-loop.md)

**Active:** Track **H** mesher honesty/perf overhaul in progress
([`docs/plans/mesher-solver-overhaul.md`](plans/mesher-solver-overhaul.md)).
**141 tests** green (3 skip without OCC/CUDA). Remaining owner-facing gate:
**A9** theme polish ⛔ GATE 6.5. Open H nodes: true hybrid hex+pyramid FE (H2),
octa experiment (O1), CG precond (V1).

GATE 1 deliverables ready:
- Full Tier-0 + Tier-1 suite (Lamé, Timoshenko, Kirsch, Goodier, L-domain)
- MMS convergence orders matching theory
- Gmsh `.msh` v2.2 ASCII import
- Convergence report: `bench/reports/p1-gate1-convergence.md`
- ADR-0009 (Tier-1 verification setups)

GATE 0 was approved by owner on 2026-07-09.

## Done
- 2026-07-10: **Graded tet coarse-primary lattice** — Recovered WIP after agent
  crash: classify at target \(h\) (same cost class as tet/hybrid), then local
  \(2×2×2\) Kuhn only on skin/feature/seed cells (bulk≈\(h\), fine≈\(h/2\)).
  Replaces fine-global lattice + coarse-block aggregation. Boundary quads
  emitted per exterior coarse/fine face. **138/138** Catch2 green on related +
  full suite.
- 2026-07-10: **Graphify shared workflow** — Rebuilt `graphify-out/` (AST +
  docs); gitignore machine-local artifacts; CONTRIBUTING §8 + `CLAUDE.md`
  document clone setup, `graphify update`, hooks, merge driver for concurrent
  graph.json updates.
- 2026-07-10: **Graded tet fix (size + speed + RAM)** — Dropped global \(h/4\)
  lattice when features/seeds active (was bulk only \(h/2\), 8× cells, thin plates
  fully fine → slow mesh + FEA OOM). Always **2:1** (bulk≈\(h\), fine≈\(h/2\));
  feature/seed stamp via rasterized balls (not O(blocks·seeds)); skin depth
  capped by interior thickness; snap Jacobian only on boundary-touching tets;
  pipeline seeds sparse (≤192, 85th-κ, band≈1.25\(h\)); p-elevate skipped when
  nodes>40k. GUI: skin=2, p-elev opt-in. Tests updated (subdiv always 2).
- 2026-07-10: **Performance build** — Release defaults to **-O3**; OpenMP ON for assembly, mesh classify (uint8 mask, not vector<bool>), ZZ, stress, SpMV; Eigen kept serial to avoid nested OpenMP hangs; no -ffast-math; LTO/native-arch OFF (Eigen miscompile risk). `polymesh backend` reports thread stack. 133 tests green.
- 2026-07-10: **Results viewport + geo-hybrid mesh** — pan/orbit fixed in von
  Mises/deflection/error (Image hover captured before colorbar child);
  auto-exaggerated deformation (max |u| → ~12% of model diagonal, true-scale
  checkbox); graded fill targets **h/4** near feature/seed bands (subdiv=4)
  so curved edges densify vs bulk h/2; more aggressive κ/thin seeds + thicker
  skin default; pre-solve **geo-hp** bulk p-elevate (tet10 interior, linear
  near surface); GUI defaults graded+feature+adapt+p-elev. 132 tests green.
- 2026-07-10: **GUI layout + mesher product pass** — group-box right padding
  (content child reserves both sides); single workspace tiles study|splitter|
  viewport (no purple gap); fixtures: CAD face list + click-to-select without
  orbit fight + “show CAD” when in mesh mode; mesh preview checkerboard + dark
  wireframe with depth bias; multi-pass surface snap ≤0.55h on tet/graded/
  hexpyr; graded feature path seeds curvature (cylinder/hole) + thin-wall
  bands; 131 tests green.
- 2026-07-10: **Graded tet “grid too fine” fix + full-adapt product path** —
  `make_bbox_grid` / `make_bbox_grid_even` auto-coarsen under the 512k cell
  budget (no hard fail); graded fill pre-floors \(h\) for the fine \(h/2\)
  lattice; adapt loop uses multi-wave LEB, grid-aware \(h\) floor, graded seed
  remesh; GUI defaults graded tet + 3 adapt passes + η=0.12. Catch2 tiny-h
  graded + grid budget tests.
- 2026-07-10: **Mesh gap fix** — shared-edge ray-parity double-count punched
  diagonal tunnels through cubes/plates (cells with \(c_x\approx c_y\) outside);
  bbox-fitted anisotropic lattice so nodes hit AABB faces. Shared
  `mesh/grid_classify` used by tet/hex/graded/transition/prism. Unit box volume
  exact (6000 tet @ h=0.1); edge fixtures thin plate / slender / offset / sphere;
  Catch2 regressions. ADR-0015 updated.
- 2026-07-10: Fix GUI mesh-only freeze — stop corner geometry-sizing from shrinking global h 8×; O(n) element-type colors in viewport; live meshing status; `build.bat`/`build.sh` copy CLI+GUI to repo root.
- 2026-07-10: D6 Tier-3 instrument — L-domain uniform tet10 vs geometric graded
  tet10 (same solver, ADR-0005). Harness: `apps/bench/polymesh-d6-tier3` +
  `bench/d6/run_tier3.py`; raw `bench/d6/out/…-raw.json`, scoreboard rows
  `bench/results/polymesh-d6-l-domain.json`; writeup `docs/bench/d6-tier3.md`;
  label `d6-tier3`. Measured (full suite): **5.12× DOF** and **12.2× wall time**
  at matched strain energy (graded `h0=w/8_rho2` 1248 free DOFs / 0.23 s vs
  uniform n6 6384 DOFs / 2.76 s; energy match tol 0.01%). Catch2 smoke for
  script --help / JSON schema (not multi-minute bench). ROADMAP D6 closed on
  this instrument; product-mesh Tier-3 on full public geometry suite still open.
- 2026-07-10: F3 CUDA SpMV scaffolding — `fea/spmv.hpp` CSR + `spmv_cpu` (always),
  `try_spmv_cuda` / device kernel in `backend_cuda.cu` when `POLYMESH_WITH_CUDA=ON`,
  Catch2 CPU vs Eigen + CUDA-vs-CPU parity (SKIP without toolkit/device). Default
  CI remains CPU-only. README CUDA enable notes. ROADMAP F3 closed.
- 2026-07-10: C3 prism sweep volume fill — `prism_fill_surface` (Cartesian
  lattice, each inside voxel → 2× prism6 along longest bbox axis); pipeline
  `VolumeMesher::kPrismSweep`; CLI `--mesher prism|sweep`; GUI mesher combo;
  Catch2 validity + constant-strain patch + solve smoke; ADR-0015 updated.
  Honesty: not CAD extrusion detection (same grid-fill limits as tet/hex).
- 2026-07-10: C4 VEM k=2 — serendipity edge midpoints on `kPolyVem` (order
  inferred: nv→k=1, nv+ne→k=2); hex path = isoparametric hex20 (ADR-0017);
  patch test + degree-2 exact + MMS energy-norm order ≈2 ±0.2; k=1 unchanged.
- 2026-07-10: D4 true local h-refine — ADR-0016 Rivara longest-edge bisection
  (LEPP, no hanging nodes); `mesh::local_refine_tets`; Catch2 single-tet +
  multi-tet center mark (validity, +volume, volume conserve) + solve smoke;
  pipeline adapt tries LEB on tet/graded-tet before seed remesh (ADR-0014).
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
| Tier 3 performance | L-domain instrument PASS: 5.12× DOF, 12.2× time (d6-tier3); full public-suite product path still open |

## Open issues
- GATE 1 frozen; see `bench/reports/p1-gate1-convergence.md`.
- License closed: BSD-3-Clause (ADR-0002); no CLA process.
- `POLYMESH_WITH_OCC` wired in `src/geom` (`load_step`, CMake find + stub
  when OFF); exact B-rep feature queries still deferred to P3 (ADR-0001).
- CUDA SpMV scaffolding landed (F3); enable with `POLYMESH_WITH_CUDA=ON` +
  toolkit on PATH (`nvcc`). CI stays CPU-only. Batched Ke kernels still open.
  RTX 3080 Ti present (ADR-0008).
- Geometric validity: boundary manifold + tet volume checks; limited surface
  snap with Jacobian unsnap (B3) on tet and hex+pyramid fills. True Delaunay
  deferred (B1 = ADR-0015 documented limits). CAD feature queries still open.
- Goodier: exact continuum-field BCs + ZZ recovery would tighten SCF further
  (ADR-0009); P1 bar is 12% with Saint-Venant Dirichlet + nodal averaging.
