# Graph Report - subagent-019f4f59-e485-7f21-b58c-b4e0189a4752  (2026-07-11)

## Corpus Check
- 222 files · ~176,362 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2289 nodes · 3615 edges · 331 communities (164 shown, 167 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 295 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `65a0f3ac`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- VEM & Nodal Mesh
- Grid Classification
- Element Assembly Core
- GUI Viewport GL
- Example Geometries
- Hybrid Graded Fill
- GUI Theme Palette
- Bench Emit Scripts
- SpMV CSR Backend
- CLI Mesh Solve
- Poly Mesh Topology
- GUI App State
- Gmsh MSH Import
- Surface Traction
- Pipeline Scene Jobs
- Structured Mesh Tests
- Surface Projection
- Shape Functions
- Adapt Loop Seeds
- Mixed Cell Fill
- FEA Solve Methods
- Scene Solve Result
- GUI Widgets
- MMS Convergence Tests
- Solve Job Pipeline
- D6 Tier3 Bench
- Viewport Camera
- Sizing Field Blend
- Transition Fill
- CMake Project Build
- Scene Model Bounds
- Geom Indicators
- STL Geometry IO
- TriSurface Geometry
- Sim Setup Loads
- Kirsch Graded Tests
- P-Elevate Elements
- Tet Quality Metrics
- Feature Sizing Field
- GUI Study Panels
- JSON Schema Props
- Tier1 Verification Cases
- Boundary Faces
- Schema Mesh Solve
- Mesh Face Headers
- Competitive Peer Solvers
- Roadmap Phases CUDA
- Geometry Sizing
- Reference Case Bench
- FEA Colormap Display
- Schema Type Props
- Analytical Tier1 Cases
- Geometry Kernel ADR
- Cantilever Iterative
- D6 Scoreboard Bench
- Element Formulations ADR
- CUDA SpMV Kernel
- Feature Graded Error
- Hex Fill Output
- Local Refine Tests
- Audit Bench Policy
- Mesh Spec Layers
- Gate1 Baseline Freeze
- Hybrid Mesher ADR
- HP Mesh Co-Design
- ZZ Stress Recovery
- Geom Features Extract
- L-Domain Solve Tests
- Solve Energy Output
- Root Schema JSON
- Polyhedral Ideas P4P5
- Lame Cylinder Tests
- Backend Bench Smoke
- Resolved Mesh Size
- CUDA Backend ADR
- Adapt Remesh ADR
- ZZ Recovery API
- STEP Geometry Load
- Kirsch Plate Tests
- Theme Apply Palettes
- GL Shader Bind
- Mesh Structure ADR
- GUI Phase ADR
- Mesher Solver Co-Design
- Agent Loop Process
- Quadrature Tests
- Shape Function Tests
- STL IO Tests
- Tet Fill Tests
- GroupBox Frame UI
- Schema DOFs Field
- Schema Version Field
- Schema Wall Time
- Adaptivity Pipeline
- Schema Label Field
- Schema Notes Field
- Schema Solver Field
- Schema Version Meta
- Public Mesh Scripts
- Public Solve Scripts
- Region Pick Optional
- Polymesh Smoke Script
- MMS Convergence Orders
- OpenCASCADE Option
- OpenMP Parallelism
- Tier0 Patch Tests
- BSD-3 License ADR
- polymesh-gui Executable
- Verification
- Context
- ROADMAP — Get PolyMesh off the ground
- unit_box
- bench_harness library
- User-Paintable Region Override (GUI)
- cell_stamp.hpp
- vector
- optional
- string
- VectorXd
- VolumeMesher
- CLAUDE.md — PolyMesh (working name)
- sizing_field.cpp
- ADR-0012: Hybrid meshing — graded tet + true mixed zoo
- ADR-0016: True local h-refine — longest-edge bisection (no hanging nodes)
- ADR-0017: VEM k=2 with serendipity edge midpoints
- SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver
- prism_fill_surface
- transition_fill_surface
- BENCHMARKS — Verification Suite & Anti-Cheat Design
- ADR-0010: Mesh data structure — keep face-based; edge index optional
- string
- run_calculix_cantilever.py
- ADR-0011: VEM k=1 for polyhedra
- Quickstart (Ubuntu)
- test_d6_bench_smoke.cpp
- apply_theme
- D6 Tier-3 — L-domain uniform tet10 vs graded tet10
- ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available
- ADR-0002: License — BSD-3-Clause
- ADR-0003: Element formulations — unified trait over isoparametric FEM (p=1..4) + VEM
- ADR-0004: Mesh data structure — face-based owner/neighbour
- ADR-0005: Benchmark baseline — own uniform tet10 path + CalculiX audit cross-check
- ADR-0006: GUI — after the solver core, as phase P6.5
- ADR-0007: Implementation language — C++20
- ADR-0008: CUDA backend for parallelizable kernels
- ADR-0014: A posteriori Dörfler seed remesh
- Idea harvest: fea-madness (Grok-generated spec, 2026-07-10)
- GroupBoxFrame
- Recommended approach (not all alternatives)
- README.md
- Holdout Geometry Audit Protocol
- CalculiX First Peer Solver Priority
- Code_Aster Third Peer Solver
- Elmer Second Peer Solver
- Competitive Benchmark Harness
- Edge-Case Mesh Fixtures Suite
- Shared-Edge Ray-Parity Grid Fill Fix
- Public Geometry Fixtures Suite
- unit_box.stl Public Fixture
- SimplicialLDLT Direct Sparse Solver
- External Contributor PR Policy
- Double-Only Solver Math
- GUI Presentation-Only Rule
- P1 Solver Baseline Frozen
- Graded tet10 path
- L-domain re-entrant corner case
- Tier-3 targets (≥5× DOF, ≥3× wall time)
- Uniform tet10 baseline path
- CalculiX peer solver
- kirsch-plate case
- PolyMesh solver
- timoshenko-cantilever case
- ZZ Estimator Honesty (Effectivity [0.5, 2])
- Holdout Geometry Anti-Cheat
- Kirsch Plate Circular-Hole Case
- L-Shaped Domain Singularity Case
- Lamé Thick-Walled Cylinder Case
- Tier 0 Correctness Gates
- Tier 1 Analytical Solutions
- Tier 2 Method of Manufactured Solutions
- Timoshenko Cantilever Case
- OpenCASCADE B-rep/STEP
- POLYMESH_WITH_OCC CMake option
- STL path (always compiled)
- ADR-0002 License BSD-3-Clause
- BSD-3-Clause license
- hp-adaptivity (order + size)
- Isoparametric FEM p=1..4
- Virtual Element Method k=1,2
- Wachspress/mean-value polyhedral FEM
- Face-based owner/neighbour mesh
- Half-face/half-edge alternative
- ADR-0005 Benchmark baseline
- CalculiX audit cross-check
- Own uniform tet10 baseline (frozen GATE 1)
- ADR-0006 GUI phase P6.5
- Desktop GUI (GLFW + Dear ImGui + OpenGL)
- Draft voxel mesher v0
- Interwebz v2 GUI theme
- ADR-0007 Language C++20
- C++20 only (CMake + Ninja)
- ADR-0008 CUDA backend
- Batched element stiffness (CUDA target)
- CUDA optional backend
- fea/backend.hpp dispatch layer
- SpMV in CG iterative solves
- ADR-0009 Tier-1 verification setups
- C5 equal-DOF logarithmic grading
- Goodier cavity
- Kirsch plate (SCF=3)
- L-domain Williams singularity
- ADR-0010 Edge vs face mesh store
- Derived edge adjacency index
- Edge-primary topology (rejected)
- ADR-0011 VEM k=1
- ElementType::kPolyVem
- VEM k=1 formulation
- ADR-0012 Hybrid graded tet + mixed zoo
- graded_tet_fill
- Kuhn 6-tet hex split
- mixed_fill / VolumeMesher::kHybrid
- ADR-0013 Hex core + pyramid skin
- expand_hex_core_to_pyramids
- VolumeMesher::kHexPyramid
- Dörfler seed remesh
- ZZ error recovery
- ADR-0015 Cartesian grid-fill limits
- Cartesian grid-fill meshers
- make_bbox_grid (AABB-fitted lattice)
- Ray parity shared-edge dedupe
- Staircasing boundary artifact
- Constrained Delaunay (deferred B1)
- ADR-0016 Local h-refine LEB
- Hanging-node MPCs (deferred)
- mesh::local_refine_tets API
- Rivara longest-edge bisection
- ADR-0017 VEM k=2
- Hex k=2 coincides with hex20 FEM
- VEM k=2 serendipity edge midpoints
- Theme tokens (theme.hpp/cpp)
- Goal-Oriented (Adjoint) Adaptivity
- Seed-Based Voronoi/Laguerre Polyhedral Meshing
- GATE 1 Baseline Freeze
- Phase P0 Decisions & Scaffolding
- Phase P1 Reference Solver Baseline
- Phase P2 Mesh Core + Tet + Validity
- Phase P3 Geometric Feature Hybrid Meshing
- Phase P4 Polyhedral VEM Elements
- Phase P5 Adaptive Loop Product
- Phase P6 Performance Engineering
- Phase P6.5 GUI
- Phase P7 OSS Release Readiness
- Agent loop harness rules
- One iteration = one ROADMAP ID
- ROADMAP/progress/phases source of truth
- CLAUDE.md / agents notes (moved)
- Convergence rate is the metric
- Eigen .inverse() include gotcha
- Hunter-124 commit attribution policy
- src modules geom/mesh/adapt/fea/bench/cli
- No hardcoded benchmark values in solver
- Patch test is sacred
- Mesher-solver co-design
- ADR-0001 OpenCASCADE STEP/B-rep Option
- ADR-0003 Unified Element Interface
- ADR-0004 Face-Based Mesh DS
- ADR-0005 Benchmark Baseline (Uniform Tet10 + CalculiX)
- ADR-0006 GUI In Scope (P6.5)
- ADR-0008 Optional CUDA Backend
- Dörfler Marking
- Geometry-Driven A Priori Sizing
- hp-Adaptive Mesh Co-Design
- Hybrid Element Zoo
- Linear Elastostatics (3D)
- North Star: Heterogeneous hp Mesh + FEA Co-Optimization
- Architecture Pipeline geom→mesh→fea→adapt
- Solution-Driven A Posteriori Adaptivity
- Tier-3 Win Targets (≥5× DOF, ≥3× Wall Time)
- Virtual Element Method (VEM)
- Zienkiewicz–Zhu Superconvergent Patch Recovery
- Cantilever-style boundary conditions
- Cartesian grid product fills (ADR-0015)
- cylinder_prism.stl
- l_domain.stl
- Mesher options (tet|hex|graded|hexpyr|hexvem)
- plate.stl
- polymesh product CLI
- run_mesh_public.sh
- run_solve_public.sh
- unit_box.stl
- VTU output (displacement, von Mises)
- Auto CG Above 8000 Free DOFs
- Element Types: tet/hex/prism/pyramid/VEM polyhedra
- mesh_preview.py
- Element
- 8. Graphify (for agents)
- Implementation notes (for coding agents)
- PROGRESS
- solve
- timestamp
- run_mesher_scoreboard.py
- Verification
- case_id
- Context
- Test-lab interfaces (normative)
- GradedTetFillOutput
- Competitive benchmark harness
- ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)
- The adaptive solver core, explained
- Holdout geometry audits
- README.md
- test_hierarchical.cpp
- poly_mesh.cpp
- string
- hp_element_stiffness
- HexFace
- schema_version
- wall_time_s
- version

## God Nodes (most connected - your core abstractions)
1. `TriSurface` - 63 edges
2. `NodalMesh` - 59 edges
3. `Viewport` - 58 edges
4. `FeaError` - 44 edges
5. `App` - 42 edges
6. `Palette` - 38 edges
7. `Material` - 38 edges
8. `MixedFillOutput` - 30 edges
9. `mixed_fill_surface()` - 26 edges
10. `Model` - 26 edges

## Surprising Connections (you probably didn't know these)
- `vem_body_load()` --calls--> `body`  [INFERRED]
  src/fea/src/vem.cpp → tests/support/mms.hpp
- `sizing_field` --semantically_similar_to--> `resolve_mesh_size`  [INFERRED] [semantically similar]
  src/adapt/CMakeLists.txt → examples/README.md
- `merge_unique()` --references--> `NodalMesh`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp
- `make_l_hex_mesh()` --calls--> `add_node()`  [INFERRED]
  tests/test_l_domain.cpp → apps/bench/d6_tier3.cpp
- `promote_tet4_to_tet10()` --calls--> `FeaError`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp

## Import Cycles
- None detected.

## Communities (331 total, 167 thin omitted)

### Community 0 - "VEM & Nodal Mesh"
Cohesion: 0.07
Nodes (82): Fun, function, kP2Mono, kP2Vec, FeaError, runtime_error, Vector3d, QuadraturePoint (+74 more)

### Community 1 - "Grid Classification"
Cohesion: 0.26
Nodes (19): FreeFaceKey, bisect_tet(), array, EdgeKey, size_t, span, uint32_t, vector (+11 more)

### Community 2 - "Element Assembly Core"
Cohesion: 0.09
Nodes (16): Material, d_matrix, poissons_ratio, youngs_modulus, Matrix, Material::d_matrix(), VectorXd, energy_norm_error() (+8 more)

### Community 3 - "GUI Viewport GL"
Cohesion: 0.05
Nodes (41): DisplayMode, uint32_t, Vector3d, VectorXd, Viewport, background_program_, background_vao_, baked_max_ (+33 more)

### Community 4 - "Example Geometries"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "Hybrid Graded Fill"
Cohesion: 0.16
Nodes (15): element_num_nodes(), ElementType, uint32_t, vector, NodalElement, faces, nodes, type (+7 more)

### Community 6 - "GUI Theme Palette"
Cohesion: 0.06
Nodes (36): ImVec4, Palette, accent, accent_dim, accent_mid, accent_soft, accent_soft_top, axis_x (+28 more)

### Community 7 - "Bench Emit Scripts"
Cohesion: 0.31
Nodes (9): fmt_num(), load_results(), main(), Any, Path, Markdown-friendly ASCII sparkline; skips None., Minimal inline SVG polyline for accuracy trend (lower often better)., sparkline() (+1 more)

### Community 8 - "SpMV CSR Backend"
Cohesion: 0.18
Nodes (14): CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t, vector (+6 more)

### Community 9 - "CLI Mesh Solve"
Cohesion: 0.11
Nodes (27): cmd_check(), cmd_mesh(), cmd_solve(), span, string, string_view, VolumeMesher, load_surface() (+19 more)

### Community 10 - "Poly Mesh Topology"
Cohesion: 0.11
Nodes (20): CellId, CellKind, FaceId, Cell, faces, kind, Face, neighbour (+12 more)

### Community 11 - "GUI App State"
Cohesion: 0.07
Nodes (29): App, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, job, lmb_drag_px (+21 more)

### Community 12 - "Gmsh MSH Import"
Cohesion: 0.18
Nodes (16): CartesianGrid, cell, nx, ny, nz, origin, Vector3d, size_t (+8 more)

### Community 13 - "Surface Traction"
Cohesion: 0.05
Nodes (55): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+47 more)

### Community 14 - "Pipeline Scene Jobs"
Cohesion: 0.14
Nodes (24): join_worker, set_status, optional, size_t, span, string, Vector3d, VectorXd (+16 more)

### Community 15 - "Structured Mesh Tests"
Cohesion: 0.15
Nodes (25): Vector3d, NodalMesh, check_validity, elements, nodes, box_hex_mesh(), box_tet_mesh(), cell_corners() (+17 more)

### Community 16 - "Surface Projection"
Cohesion: 0.17
Nodes (12): GeometrySizing, blend_, curv_frac_, edges_, h_max_, h_min_, kappa_, size_at (+4 more)

### Community 17 - "Shape Functions"
Cohesion: 0.23
Nodes (17): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+9 more)

### Community 18 - "Adapt Loop Seeds"
Cohesion: 0.12
Nodes (21): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+13 more)

### Community 19 - "Mixed Cell Fill"
Cohesion: 0.07
Nodes (52): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, MixedCellKind, array, size_t, uint32_t (+44 more)

### Community 20 - "FEA Solve Methods"
Cohesion: 0.12
Nodes (23): solve_l_mesh(), Dirichlet, dof_values, Index, map, SolveMethod, SolveOptions, cg_max_iters (+15 more)

### Community 21 - "Scene Solve Result"
Cohesion: 0.09
Nodes (23): array, map, uint32_t, vector, VectorXd, SolveResult, boundary_quads, displacement (+15 more)

### Community 22 - "GUI Widgets"
Cohesion: 0.08
Nodes (39): CollectOffendersFn, ClosestPoint, distance, point, triangle, ConformityStats, count, max_distance (+31 more)

### Community 23 - "MMS Convergence Tests"
Cohesion: 0.21
Nodes (19): Dynamic, Matrix, VectorXd, HpShape, dn, n, build_hex(), build_tet() (+11 more)

### Community 24 - "Solve Job Pipeline"
Cohesion: 0.12
Nodes (19): atomic, mutex, load, SolveJob, clear_failure, error_, mesh_only_, result_ (+11 more)

### Community 25 - "D6 Tier3 Bench"
Cohesion: 0.32
Nodes (13): string, vector, geometric_layers(), graded_line(), main(), make_l_graded(), make_l_uniform(), make_row() (+5 more)

### Community 26 - "Viewport Camera"
Cohesion: 0.17
Nodes (17): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+9 more)

### Community 27 - "Sizing Field Blend"
Cohesion: 0.20
Nodes (13): ClosestOnFeature, distance, point, Vector3d, closest_on_features(), size_t, span, vector (+5 more)

### Community 28 - "Transition Fill"
Cohesion: 0.10
Nodes (27): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+19 more)

### Community 29 - "CMake Project Build"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "Scene Model Bounds"
Cohesion: 0.12
Nodes (16): set_model, update_overlays, Model, bbox_max, bbox_min, name, region_count, surface (+8 more)

### Community 31 - "Geom Indicators"
Cohesion: 0.19
Nodes (15): vector, VertexCurvature, kappa, VertexThickness, thickness, size_t, uint32_t, Vector3d (+7 more)

### Community 32 - "STL Geometry IO"
Cohesion: 0.22
Nodes (15): GeomError, runtime_error, byte, path, size_t, Soup, span, T (+7 more)

### Community 33 - "TriSurface Geometry"
Cohesion: 0.12
Nodes (16): array, uint32_t, vector, Vector3d, TriSurface, triangles, validate, vertices (+8 more)

### Community 34 - "Sim Setup Loads"
Cohesion: 0.12
Nodes (16): Vector3d, VolumeMesher, RegionLoad, force, SimSetup, adapt_leb_waves, adapt_passes, eta_target (+8 more)

### Community 35 - "Kirsch Graded Tests"
Cohesion: 0.16
Nodes (15): RadialMap, array, int64_t, Matrix3d, size_t, Vector3d, kirsch_stress(), KirschRun (+7 more)

### Community 36 - "P-Elevate Elements"
Cohesion: 0.25
Nodes (7): ElementTypeCounts, hex20, hex8, other, tet10, tet4, size_t

### Community 37 - "Tet Quality Metrics"
Cohesion: 0.12
Nodes (26): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+18 more)

### Community 38 - "Feature Sizing Field"
Cohesion: 0.15
Nodes (11): FeatureSizing, blend_, h_max_, h_min_, size_at, DistanceFn, Vector3d, SizingField (+3 more)

### Community 39 - "GUI Study Panels"
Cohesion: 0.16
Nodes (20): Matrix, uint64_t, Vector3d, array, map, ManufacturedSolution, body, body_force (+12 more)

### Community 40 - "JSON Schema Props"
Cohesion: 0.14
Nodes (14): additionalProperties, properties, required, type, description, type, accuracy, name (+6 more)

### Community 41 - "Tier1 Verification Cases"
Cohesion: 0.22
Nodes (8): ADR-0009: Tier-1 analytical verification setups (Kirsch / Goodier / L-domain), Alternatives, Decision, Gmsh import, Goodier cavity (SCF = 3(9−5ν)/(2(7−5ν))), Kirsch plate (SCF = 3), L-domain (Williams λ ≈ 0.5445), Why

### Community 42 - "Boundary Faces"
Cohesion: 0.27
Nodes (13): add_face(), array, FaceKey, map, uint32_t, vector, emit_element_faces(), extract_boundary_faces() (+5 more)

### Community 43 - "Schema Mesh Solve"
Cohesion: 0.15
Nodes (13): description, minimum, type, mesh, solve, total, description, minimum (+5 more)

### Community 44 - "Mesh Face Headers"
Cohesion: 0.13
Nodes (11): array, set, array, span, uint32_t, vector, Vector3d, graded_tet_fill_surface() (+3 more)

### Community 45 - "Competitive Peer Solvers"
Cohesion: 0.14
Nodes (13): Accuracy vs labeled commits, All runs, Benchmark scoreboard, `cantilever_smoke` — `smoke_ran`, How to refresh, `kirsch-plate` — `scf_rel_err_pct`, `l-domain-d6-baseline` — `energy_deficit_pct`, `l-domain-d6-graded` — `energy_deficit_pct` (+5 more)

### Community 47 - "Geometry Sizing"
Cohesion: 0.05
Nodes (50): FreeFace, CircularFeature, axis_dir, axis_point, radius, select_band, CurvedMeshMetrics, composite_score (+42 more)

### Community 48 - "Reference Case Bench"
Cohesion: 0.17
Nodes (12): Critical files, File ownership (to avoid merge thrash), First concrete commits after approval, Goals / success metrics, Monolithic DAG (parallel execution), Node specs, Out of scope (explicit), Parallel subagent playbook (handoff) (+4 more)

### Community 49 - "FEA Colormap Display"
Cohesion: 0.20
Nodes (15): size_t, runtime_error, ValidityError, array, uint32_t, vector, Vector3d, face_corners() (+7 more)

### Community 50 - "Schema Type Props"
Cohesion: 0.18
Nodes (11): description, type, description, type, properties, case_id, host, timestamp (+3 more)

### Community 53 - "Cantilever Iterative"
Cohesion: 0.22
Nodes (10): CantileverSetup, bc, length, loads, mesh, nfree, Index, VectorXd (+2 more)

### Community 56 - "CUDA SpMV Kernel"
Cohesion: 0.17
Nodes (18): array, uint32_t, vector, Vector3d, PrismFillOutput, boundary_max_distance, boundary_quads, h (+10 more)

### Community 57 - "Feature Graded Error"
Cohesion: 0.13
Nodes (18): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector (+10 more)

### Community 58 - "Hex Fill Output"
Cohesion: 0.14
Nodes (17): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+9 more)

### Community 59 - "Local Refine Tests"
Cohesion: 0.40
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 60 - "Audit Bench Policy"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "Mesh Spec Layers"
Cohesion: 0.12
Nodes (16): 0. Contributing with AI agents (quick start), 10. Quick “I am lost” paths, 1. What this repo is, 2. Directory layout (keep it), 3. Where to change what, 5. Documentation standards (no slop), 6. How to add a feature (agent checklist), 7. GUI rules (Interwebz) (+8 more)

### Community 62 - "Gate1 Baseline Freeze"
Cohesion: 0.40
Nodes (4): 1. PLAN, 2. BUILD, 3. VERIFY, 4. LOOP OR STOP

### Community 63 - "Hybrid Mesher ADR"
Cohesion: 0.22
Nodes (8): ADR-0013: Hex core + pyramid skin transition mesher, Decision, Jacobian safety (B3), Nonconformity (why native hex8 + pyramid5 fails the patch test), Orientation, Product FE path (C1 honesty), Surface snap, Why

### Community 64 - "HP Mesh Co-Design"
Cohesion: 0.15
Nodes (12): ASCII convergence (log₂ energy error vs refinement step), GATE 1 — P1 baseline convergence report, Goodier SCF bar, Infrastructure landed with GATE 1, Kirsch SCF bar, L-domain energy self-convergence, Open items deferred past GATE 1, Owner action (+4 more)

### Community 65 - "ZZ Stress Recovery"
Cohesion: 0.33
Nodes (8): Dynamic, Matrix, Stress, Vector3d, VectorXd, element_centroid(), recover_zz(), stress_at()

### Community 66 - "Geom Features Extract"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "L-Domain Solve Tests"
Cohesion: 0.22
Nodes (9): Spec, h0, layers, n, n_bg, nz, path, rho (+1 more)

### Community 68 - "Solve Energy Output"
Cohesion: 0.18
Nodes (15): fit, array, DisplayMode, optional, uint32_t, vector, Vector3d, fea_colormap() (+7 more)

### Community 69 - "Root Schema JSON"
Cohesion: 0.25
Nodes (7): additionalProperties, description, $id, required, $schema, title, type

### Community 71 - "Lame Cylinder Tests"
Cohesion: 0.25
Nodes (8): SolveOut, energy, free_dofs, mesh_s, nelems, nnodes, solve_s, total_s

### Community 73 - "Resolved Mesh Size"
Cohesion: 0.29
Nodes (13): bbox_of(), boundary_edges(), cell_faces(), detect_hole_roi(), draw_line(), face_key(), main(), parse_vtu_ascii() (+5 more)

### Community 76 - "ZZ Recovery API"
Cohesion: 0.11
Nodes (12): uint32_t, constant_strain_max_error(), Matrix3d, sample_strain_gradient(), unit_box_surface(), uint32_t, uint8_t, vector (+4 more)

### Community 77 - "STEP Geometry Load"
Cohesion: 0.17
Nodes (13): Backend, active_backend(), backend_description(), string, openmp_enabled(), openmp_max_threads(), performance_description(), SparseMatrix (+5 more)

### Community 78 - "Kirsch Plate Tests"
Cohesion: 0.22
Nodes (8): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets, n_surface_mids

### Community 79 - "Theme Apply Palettes"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "GL Shader Bind"
Cohesion: 0.47
Nodes (6): bind_line_attr(), compile(), link(), init, GLenum, GLuint

### Community 82 - "GUI Phase ADR"
Cohesion: 0.50
Nodes (3): Adding a colorscheme, GUI theme & layout, Rules

### Community 84 - "Agent Loop Process"
Cohesion: 0.22
Nodes (8): Agent loop — harness rules for finishing PolyMesh, GUI verification (DISPLAY may be missing), `/loop` vs this file, One iteration = one ROADMAP ID (or one vertical story), Parallelism, Session start checklist, Source of truth, Stuck protocol

### Community 85 - "Quadrature Tests"
Cohesion: 0.27
Nodes (18): begin_field(), button(), checkbox(), ImVec4, draw_accent_fill(), draw_box(), draw_centered_label(), end_group_box() (+10 more)

### Community 86 - "Shape Function Tests"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "STL IO Tests"
Cohesion: 0.50
Nodes (4): as_bytes(), byte, string_view, vector

### Community 88 - "Tet Fill Tests"
Cohesion: 0.40
Nodes (4): Case, path, volume, unit_box()

### Community 89 - "GroupBox Frame UI"
Cohesion: 0.10
Nodes (20): vector, Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, Stress (+12 more)

### Community 90 - "Schema DOFs Field"
Cohesion: 0.50
Nodes (4): description, minimum, type, dofs

### Community 91 - "Schema Version Field"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face.      Fac, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "Schema Wall Time"
Cohesion: 0.57
Nodes (7): add_node(), array, int64_t, map, uint32_t, fill_block_tets(), push_hex_as_tets()

### Community 94 - "Schema Label Field"
Cohesion: 0.20
Nodes (14): size_t, string, draw_colorbar(), draw_frame(), draw_study_panel(), draw_viewport_content(), drop_callback(), is_geometry_path() (+6 more)

### Community 95 - "Schema Notes Field"
Cohesion: 0.67
Nodes (3): description, type, notes

### Community 96 - "Schema Solver Field"
Cohesion: 0.67
Nodes (3): solver, description, type

### Community 97 - "Schema Version Meta"
Cohesion: 0.29
Nodes (7): 4. Engineering standards (non-negotiable), Anti-cheat (sacred), CUDA, Eigen traps, Git identity (owner agents), Language & build, License

### Community 100 - "Region Pick Optional"
Cohesion: 0.25
Nodes (8): size_t, string, ResolvedMeshSize, auto_chosen, h, min_feature_length, n_sharp_edges, note

### Community 119 - "Verification"
Cohesion: 0.20
Nodes (10): P0 — Decisions & scaffolding, P1 — Reference solver on standard elements (the trustworthy baseline), P2 — Mesh core + tet meshing + validity, P3 — Geometric feature analysis → a priori hybrid meshing, P4 — Polyhedral elements (VEM) [parallel with P3 after P2], P5 — Adaptive loop (the product), P6.5 — GUI (ADR-0006), P6 — Performance engineering (+2 more)

### Community 120 - "Context"
Cohesion: 0.32
Nodes (7): count_element_types(), size_t, span, vector, p_elevate(), p_elevate_impl(), promote_to_quadratic()

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "unit_box"
Cohesion: 0.11
Nodes (18): Benchmark scoreboard, Building (options), CLI examples (public unit box), Clone, configure, build, test, CUDA backends (`POLYMESH_WITH_CUDA`), Dependencies, GUI, Layout (short) (+10 more)

### Community 125 - "cell_stamp.hpp"
Cohesion: 0.24
Nodes (10): BenchError, runtime_error, ReferenceCase, citation, name, values, path, string (+2 more)

### Community 126 - "vector"
Cohesion: 0.18
Nodes (9): Public geometry fixtures, Usage, Manual one-liners, Mesh only (auto h0), Notes, PolyMesh examples, Prerequisites, Scripts (+1 more)

### Community 127 - "optional"
Cohesion: 0.20
Nodes (10): Agent system prompt (paste this), CHANGES.md — Agent instructions for external PRs, Correct clone (do this first — most failures start here), Hard rules, Merge responsibility, Mission, Open the PR, Start work (every session) (+2 more)

### Community 128 - "string"
Cohesion: 0.20
Nodes (10): ADR-0015: Cartesian grid-fill limits (not Delaunay / frontal), Consequences, Context, Decision, Follow-on, Lattice construction (bbox-fitted), Ray parity (shared-edge dedupe), Staircasing and when they fail (+2 more)

### Community 129 - "VectorXd"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "VolumeMesher"
Cohesion: 0.43
Nodes (6): bounding_diagonal(), path, Soup, load_step(), triangulate_shape(), TopoDS_Shape

### Community 131 - "CLAUDE.md — PolyMesh (working name)"
Cohesion: 0.22
Nodes (8): CLAUDE.md — PolyMesh (working name), Definitions of done, Git & attribution, Language & tooling, Licensing, Moved, Non-negotiable engineering rules, Workflow

### Community 132 - "sizing_field.cpp"
Cohesion: 0.31
Nodes (8): dist_, blend_to_max(), clamp_size(), DistanceFn, Vector3d, FeatureSizing::FeatureSizing(), FeatureSizing::size_at(), GeometrySizing::size_at()

### Community 133 - "ADR-0012: Hybrid meshing — graded tet + true mixed zoo"
Cohesion: 0.18
Nodes (10): ADR-0012: Hybrid meshing — graded tet + true mixed zoo, Alternatives rejected for now, Amendment: curvature-driven refinement + boundary finishing (both meshers), Amendment: graded tet S4 cap collapse + S5 void carve, Amendment (v4): conforming 2:1 fan transitions, Context, Decision (v1 — graded all-tet), Decision (v2 — SPEC hybrid zoo) (+2 more)

### Community 134 - "ADR-0016: True local h-refine — longest-edge bisection (no hanging nodes)"
Cohesion: 0.25
Nodes (7): ADR-0016: True local h-refine — longest-edge bisection (no hanging nodes), Alternatives considered, API, Context, Decision, Guarantees / limits, Related

### Community 135 - "ADR-0017: VEM k=2 with serendipity edge midpoints"
Cohesion: 0.25
Nodes (7): Acceptance (ROADMAP C4), ADR-0017: VEM k=2 with serendipity edge midpoints, Alternatives considered, API, Decision, General polyhedra (incremental), Hex serendipity path (ROADMAP C4 acceptance)

### Community 136 - "SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver"
Cohesion: 0.25
Nodes (7): Architecture (pinned), Decisions — ratified at GATE 0 (2026-07-09; full rationale in docs/decisions/), Goals, Key technical positions (pinned unless a phase proves otherwise), Non-goals (v1), Problem statement, SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver

### Community 137 - "prism_fill_surface"
Cohesion: 0.29
Nodes (7): map, array, int64_t, Matrix3d, Vector3d, kirsch_stress(), param_key()

### Community 138 - "transition_fill_surface"
Cohesion: 0.12
Nodes (16): Entity, HpMode, edge_odd, entity, entity_index, order, uint8_t, HexRecipe (+8 more)

### Community 139 - "BENCHMARKS — Verification Suite & Anti-Cheat Design"
Cohesion: 0.18
Nodes (6): Anti-cheat / adversarial audit design, BENCHMARKS — Verification Suite & Anti-Cheat Design, Tier 0 — Correctness gates (must pass exactly, every commit), Tier 1 — Analytical solutions (energy-norm + peak-stress error targets), Tier 2 — Method of Manufactured Solutions (MMS), Tier 3 — Performance benchmarks (the "win" metric)

### Community 140 - "ADR-0010: Mesh data structure — keep face-based; edge index optional"
Cohesion: 0.29
Nodes (6): ADR-0010: Mesh data structure — keep face-based; edge index optional, Alternatives, Context, Decision, Rejected for now, Why face-based wins for PolyMesh

### Community 141 - "string"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 142 - "run_calculix_cantilever.py"
Cohesion: 0.53
Nodes (5): ccx_version(), main(), Path, Write coarse C3D8 cantilever deck. Returns (nnodes, n_fixed_nodes)., write_inp()

### Community 143 - "ADR-0011: VEM k=1 for polyhedra"
Cohesion: 0.33
Nodes (5): ADR-0011: VEM k=1 for polyhedra, Alternatives, Decision, Formulation, Why

### Community 144 - "Quickstart (Ubuntu)"
Cohesion: 0.13
Nodes (17): Index, SparseMatrix, HpSystem, k, local_to_global, mode_nodes, n_modes, ndof (+9 more)

### Community 145 - "test_d6_bench_smoke.cpp"
Cohesion: 0.47
Nodes (4): string, run_cmd(), slurp(), temp_out_path()

### Community 146 - "apply_theme"
Cohesion: 0.60
Nodes (4): apply_theme(), make_interwebz_palette(), make_slate_palette(), ThemeId

### Community 147 - "D6 Tier-3 — L-domain uniform tet10 vs graded tet10"
Cohesion: 0.40
Nodes (4): D6 Tier-3 — L-domain uniform tet10 vs graded tet10, Headline (equal strain-energy match, 0.01% tol), Ratios (uniform / graded), Reproduce

### Community 148 - "ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available"
Cohesion: 0.40
Nodes (4): ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available, Alternatives, Decision, Why

### Community 149 - "ADR-0002: License — BSD-3-Clause"
Cohesion: 0.40
Nodes (4): ADR-0002: License — BSD-3-Clause, Alternatives considered, Decision, Why

### Community 150 - "ADR-0003: Element formulations — unified trait over isoparametric FEM (p=1..4) + VEM"
Cohesion: 0.40
Nodes (4): ADR-0003: Element formulations — unified trait over isoparametric FEM (p=1..4) + VEM, Alternatives, Decision, Why

### Community 151 - "ADR-0004: Mesh data structure — face-based owner/neighbour"
Cohesion: 0.40
Nodes (4): ADR-0004: Mesh data structure — face-based owner/neighbour, Alternatives, Decision, Why

### Community 152 - "ADR-0005: Benchmark baseline — own uniform tet10 path + CalculiX audit cross-check"
Cohesion: 0.40
Nodes (4): ADR-0005: Benchmark baseline — own uniform tet10 path + CalculiX audit cross-check, Alternatives, Decision, Why

### Community 153 - "ADR-0006: GUI — after the solver core, as phase P6.5"
Cohesion: 0.40
Nodes (4): ADR-0006: GUI — after the solver core, as phase P6.5, Alternatives, Decision, Why

### Community 154 - "ADR-0007: Implementation language — C++20"
Cohesion: 0.40
Nodes (4): ADR-0007: Implementation language — C++20, Alternatives, Decision, Why

### Community 155 - "ADR-0008: CUDA backend for parallelizable kernels"
Cohesion: 0.40
Nodes (4): ADR-0008: CUDA backend for parallelizable kernels, Alternatives, Constraints acknowledged, Decision

### Community 156 - "ADR-0014: A posteriori Dörfler seed remesh"
Cohesion: 0.40
Nodes (4): ADR-0014: A posteriori Dörfler seed remesh, CLI / GUI, Decision, Why

### Community 157 - "Idea harvest: fea-madness (Grok-generated spec, 2026-07-10)"
Cohesion: 0.40
Nodes (4): Assessment, Idea harvest: fea-madness (Grok-generated spec, 2026-07-10), Rejected / deferred, Worth incorporating

### Community 158 - "GroupBoxFrame"
Cohesion: 0.50
Nodes (4): GroupBoxFrame, start, title, width

### Community 159 - "Recommended approach (not all alternatives)"
Cohesion: 0.33
Nodes (6): Graded tet (highest priority accuracy), Hybrid zoo (highest priority product quality), Octahedral experiment (low priority, parallel), Recommended approach (not all alternatives), Shared infrastructure (unlocks both), Solver (after mesh paths stable)

### Community 305 - "mesh_preview.py"
Cohesion: 0.43
Nodes (6): json, main(), parse_mesh_stdout(), Path, Best-effort wireframe via pure-Python exterior edges, then meshio., try_render_png()

### Community 306 - "Element"
Cohesion: 0.33
Nodes (4): Element, num_nodes, order, stiffness

### Community 307 - "8. Graphify (for agents)"
Cohesion: 0.22
Nodes (14): assemble_hp(), EdgeKey, ElementType, FaceKey, size_t, uint32_t, edge_key(), EdgeKeyHash (+6 more)

### Community 308 - "Implementation notes (for coding agents)"
Cohesion: 0.40
Nodes (5): Assembly change for H2, G1 template detail (minimum viable), H2 conformity diagonal policy, Implementation notes (for coding agents), Profiling commands (agent verify)

### Community 309 - "PROGRESS"
Cohesion: 0.40
Nodes (5): Benchmark table, Current phase, Done, Open issues, PROGRESS

### Community 310 - "solve"
Cohesion: 0.15
Nodes (15): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+7 more)

### Community 311 - "timestamp"
Cohesion: 0.12
Nodes (15): size_t, OctaFillOutput, h, mesh, n_boundary_pyramids, n_octahedra, array, uint32_t (+7 more)

### Community 312 - "run_mesher_scoreboard.py"
Cohesion: 0.83
Nodes (3): main(), Path, run_one()

### Community 313 - "Verification"
Cohesion: 0.50
Nodes (4): Anti-cheat, Epic exit (E1), Per-node, Verification

### Community 314 - "case_id"
Cohesion: 0.34
Nodes (13): cells_for_extent(), classify_cells_inside(), classify_cells_inside_axis(), classify_impl(), vector, Vector3d, dedupe_crossings(), fit_cell_budget() (+5 more)

### Community 315 - "Context"
Cohesion: 0.67
Nodes (3): Constraints (do not break), Context, Why hex currently wins (root causes)

### Community 316 - "Test-lab interfaces (normative)"
Cohesion: 0.17
Nodes (9): Agent bootstrap — overnight / autonomous work on the DAG, 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, Test-lab interfaces (normative) (+1 more)

### Community 317 - "GradedTetFillOutput"
Cohesion: 0.17
Nodes (11): GradedTetFillOutput, h_coarse, h_fine, mesh, n_coarse_cells, n_feature_cells, n_fine_cells, n_seed_cells (+3 more)

### Community 318 - "Competitive benchmark harness"
Cohesion: 0.20
Nodes (10): Adding a labeled PolyMesh point, CalculiX peer (E1), Competitive benchmark harness, D6 Tier-3 (uniform tet10 vs graded), Holdout audits (E3), Peer priority (headless practicality), PolyMesh labeled points (E2), PolyMesh smoke (internal) (+2 more)

### Community 319 - "ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 320 - "The adaptive solver core, explained"
Cohesion: 0.20
Nodes (9): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, The adaptive solver core, explained, What is implemented (node `fe-vem-assembly`), What "order" costs (+1 more)

### Community 321 - "Holdout geometry audits"
Cohesion: 0.22
Nodes (9): Agent procedure (blind), Goals, Holdout geometry audits, Optional peer cross-check (owner), Owner supply (private), Result JSON (metrics only), Roles, Status (+1 more)

### Community 322 - "README.md"
Cohesion: 0.29
Nodes (5): CalculiX peer runner, Cases to port next, Common install paths (documentation only), Run (CI-safe), Runner contract

### Community 323 - "test_hierarchical.cpp"
Cohesion: 0.32
Nodes (6): count_zero_modes(), Dynamic, Matrix, MatrixXd, unit_hex_coords(), unit_tet_coords()

### Community 324 - "poly_mesh.cpp"
Cohesion: 0.38
Nodes (6): check_validity, Vector3d, face_centroid(), PolyMesh::check_geometry(), PolyMesh::check_validity(), tet_volume()

### Community 325 - "string"
Cohesion: 0.33
Nodes (3): string, path, unique_temp_vtu()

### Community 326 - "hp_element_stiffness"
Cohesion: 0.70
Nodes (5): b_matrix(), Dynamic, Matrix, MatrixXd, hp_element_stiffness()

### Community 327 - "HexFace"
Cohesion: 0.40
Nodes (5): HexFace, fixed_axis, fixed_val, vary0, vary1

### Community 328 - "schema_version"
Cohesion: 0.50
Nodes (4): schema_version, const, description, type

### Community 329 - "wall_time_s"
Cohesion: 0.50
Nodes (4): wall_time_s, additionalProperties, required, type

### Community 330 - "version"
Cohesion: 0.67
Nodes (3): version, description, type

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **929 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+924 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **167 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `TriSurface` connect `TriSurface Geometry` to `Grid Classification`, `VolumeMesher`, `CLI Mesh Solve`, `Gmsh MSH Import`, `Pipeline Scene Jobs`, `Surface Projection`, `Mixed Cell Fill`, `GUI Widgets`, `Sizing Field Blend`, `Transition Fill`, `Scene Model Bounds`, `Geom Indicators`, `STL Geometry IO`, `Mesh Face Headers`, `Geometry Sizing`, `FEA Colormap Display`, `CUDA SpMV Kernel`, `Feature Graded Error`, `Hex Fill Output`, `case_id`, `string`, `ZZ Recovery API`, `Tet Fill Tests`?**
  _High betweenness centrality (0.068) - this node is a cross-community bridge._
- **Why does `Viewport` connect `GUI Viewport GL` to `Solve Energy Output`, `Poly Mesh Topology`, `GUI App State`, `Mesh Face Headers`, `GL Shader Bind`, `GroupBox Frame UI`, `Viewport Camera`, `Scene Model Bounds`?**
  _High betweenness centrality (0.046) - this node is a cross-community bridge._
- **Why does `Palette` connect `GUI Theme Palette` to `apply_theme`, `Schema Label Field`?**
  _High betweenness centrality (0.045) - this node is a cross-community bridge._
- **Are the 42 inferred relationships involving `FeaError` (e.g. with `promote_tet4_to_tet10()` and `solve_l_mesh()`) actually correct?**
  _`FeaError` has 42 INFERRED edges - model-reasoned connections that need verification._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _960 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `VEM & Nodal Mesh` be split into smaller, more focused modules?**
  _Cohesion score 0.06920326864147089 - nodes in this community are weakly interconnected._