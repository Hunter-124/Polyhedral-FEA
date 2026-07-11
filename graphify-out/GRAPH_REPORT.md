# Graph Report - subagent-019f4fae-16ae-7732-b673-8d72553e95f4  (2026-07-11)

## Corpus Check
- 248 files · ~202,107 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2774 nodes · 4514 edges · 357 communities (190 shown, 167 thin omitted)
- Extraction: 93% EXTRACTED · 7% INFERRED · 0% AMBIGUOUS · INFERRED: 334 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `8b3f670a`
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
- test_lame_cylinder.cpp
- test_product_mesh_tier1.cpp
- schema_version
- wall_time_s
- FaceOrient
- version
- test_vtu.cpp
- mesh_preview.py
- ClosestOnFeature
- HpDriverPolicy
- Material
- HpDriverPlan
- ElementHpSignal
- run_one
- Campaign
- Backend
- ElementHpDecision
- hp_driver.cpp
- GradedTetFillOutput
- Checkpoint
- CantileverSetup
- Config
- PartCase
- element_stiffness
- test_local_refine.cpp
- LiveProgress
- dorfler_mark
- recover_nodal_stress
- recover_zz
- LocalRefineStats
- MetricSpec
- 4. Engineering standards (non-negotiable)
- make_hp_signals
- suggest_refine
- test_spmv.cpp
- Building (options)
- Element
- elem_quad
- solve
- timestamp
- pick_region

## God Nodes (most connected - your core abstractions)
1. `NodalMesh` - 66 edges
2. `TriSurface` - 64 edges
3. `Viewport` - 58 edges
4. `FeaError` - 44 edges
5. `App` - 43 edges
6. `Material` - 40 edges
7. `TestLabState` - 39 edges
8. `SolveJob` - 39 edges
9. `Palette` - 38 edges
10. `MixedFillOutput` - 30 edges

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

## Communities (357 total, 167 thin omitted)

### Community 0 - "VEM & Nodal Mesh"
Cohesion: 0.07
Nodes (84): Fun, function, kP2Mono, kP2Vec, FeaError, runtime_error, Vector3d, QuadraturePoint (+76 more)

### Community 1 - "Grid Classification"
Cohesion: 0.12
Nodes (19): ClosestPoint, distance, point, triangle, ConformityStats, count, max_distance, mean_distance (+11 more)

### Community 2 - "Element Assembly Core"
Cohesion: 0.15
Nodes (21): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+13 more)

### Community 3 - "GUI Viewport GL"
Cohesion: 0.05
Nodes (42): DisplayMode, uint32_t, vector, Vector3d, VectorXd, Viewport, background_program_, background_vao_ (+34 more)

### Community 4 - "Example Geometries"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "Hybrid Graded Fill"
Cohesion: 0.19
Nodes (20): CartesianGrid, cell, nx, ny, nz, origin, Vector3d, cells_for_extent() (+12 more)

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
Cohesion: 0.09
Nodes (26): CellId, CellKind, FaceId, Cell, faces, kind, Face, neighbour (+18 more)

### Community 11 - "GUI App State"
Cohesion: 0.07
Nodes (29): App, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, job, lmb_drag_px (+21 more)

### Community 12 - "Gmsh MSH Import"
Cohesion: 0.18
Nodes (16): runtime_error, ValidityError, array, uint32_t, vector, Vector3d, face_corners(), octa_fill_surface() (+8 more)

### Community 13 - "Surface Traction"
Cohesion: 0.13
Nodes (26): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+18 more)

### Community 14 - "Pipeline Scene Jobs"
Cohesion: 0.14
Nodes (22): Model, bbox_max, bbox_min, name, region_count, surface, triangle_region, checkpoint (+14 more)

### Community 15 - "Structured Mesh Tests"
Cohesion: 0.14
Nodes (20): check_validity, box_hex_mesh(), box_tet_mesh(), cell_corners(), array, uint32_t, uint64_t, Vector3d (+12 more)

### Community 16 - "Surface Projection"
Cohesion: 0.08
Nodes (35): CheckpointState, ImVec4, path, string, Checkpoint, string, time_point, vector (+27 more)

### Community 17 - "Shape Functions"
Cohesion: 0.29
Nodes (14): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, Vector3d, eval_hex20() (+6 more)

### Community 18 - "Adapt Loop Seeds"
Cohesion: 0.20
Nodes (9): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+1 more)

### Community 19 - "Mixed Cell Fill"
Cohesion: 0.07
Nodes (52): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, MixedCellKind, array, size_t, uint32_t (+44 more)

### Community 20 - "FEA Solve Methods"
Cohesion: 0.14
Nodes (18): Dirichlet, dof_values, Index, map, SolveMethod, SolveOptions, cg_max_iters, cg_threshold (+10 more)

### Community 21 - "Scene Solve Result"
Cohesion: 0.09
Nodes (23): array, map, uint32_t, vector, VectorXd, SolveResult, boundary_quads, displacement (+15 more)

### Community 22 - "GUI Widgets"
Cohesion: 0.17
Nodes (20): CollectOffendersFn, closest_on_surface(), closest_on_surface_brute(), closest_on_triangle(), array, size_t, span, uint32_t (+12 more)

### Community 23 - "MMS Convergence Tests"
Cohesion: 0.36
Nodes (7): ElementType, Expectation, name, order, type, make_mesh(), solve_mms_error()

### Community 24 - "Solve Job Pipeline"
Cohesion: 0.08
Nodes (26): State, time_point, load, SolveJob, cancel_, clear_failure, error_, mesh_only_ (+18 more)

### Community 25 - "D6 Tier3 Bench"
Cohesion: 0.09
Nodes (39): add_node(), array, int64_t, json, map, string, uint32_t, vector (+31 more)

### Community 26 - "Viewport Camera"
Cohesion: 0.16
Nodes (17): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+9 more)

### Community 27 - "Sizing Field Blend"
Cohesion: 0.15
Nodes (18): ClosestOnFeature, distance, point, uint32_t, Vector3d, SharpEdge, dihedral, v0 (+10 more)

### Community 28 - "Transition Fill"
Cohesion: 0.11
Nodes (20): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+12 more)

### Community 29 - "CMake Project Build"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "Scene Model Bounds"
Cohesion: 0.28
Nodes (12): _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre., Emit one facet with outward-ish normal (right-hand a->b->c, flipped if needed)., Axis-aligned box [ox,ox+lx] x [oy,oy+ly] x [oz,oz+lz], 12 triangles. (+4 more)

### Community 31 - "Geom Indicators"
Cohesion: 0.19
Nodes (15): vector, VertexCurvature, kappa, VertexThickness, thickness, size_t, uint32_t, Vector3d (+7 more)

### Community 32 - "STL Geometry IO"
Cohesion: 0.22
Nodes (15): GeomError, runtime_error, byte, path, size_t, Soup, span, T (+7 more)

### Community 33 - "TriSurface Geometry"
Cohesion: 0.13
Nodes (15): array, uint32_t, vector, Vector3d, TriSurface, triangles, validate, vertices (+7 more)

### Community 34 - "Sim Setup Loads"
Cohesion: 0.12
Nodes (16): Vector3d, RegionLoad, force, SimSetup, adapt_leb_waves, adapt_passes, element_tendency, eta_target (+8 more)

### Community 35 - "Kirsch Graded Tests"
Cohesion: 0.16
Nodes (15): RadialMap, array, int64_t, Matrix3d, size_t, Vector3d, kirsch_stress(), KirschRun (+7 more)

### Community 36 - "P-Elevate Elements"
Cohesion: 0.13
Nodes (18): Vector3d, NodalMesh, elements, nodes, ElementTypeCounts, hex20, hex8, other (+10 more)

### Community 37 - "Tet Quality Metrics"
Cohesion: 0.12
Nodes (26): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+18 more)

### Community 38 - "Feature Sizing Field"
Cohesion: 0.08
Nodes (23): FeatureSizing, blend_, h_max_, h_min_, size_at, GeometrySizing, blend_, curv_frac_ (+15 more)

### Community 39 - "GUI Study Panels"
Cohesion: 0.18
Nodes (10): cantilever, Finite-domain note, Hand-calculated reference truths, How to add a part, Infinite-plate Kirsch solution, kirsch-plate, smoke-bar, Stress (uniaxial tension) (+2 more)

### Community 40 - "JSON Schema Props"
Cohesion: 0.14
Nodes (14): additionalProperties, properties, required, type, description, type, accuracy, name (+6 more)

### Community 41 - "Tier1 Verification Cases"
Cohesion: 0.22
Nodes (8): ADR-0009: Tier-1 analytical verification setups (Kirsch / Goodier / L-domain), Alternatives, Decision, Gmsh import, Goodier cavity (SCF = 3(9−5ν)/(2(7−5ν))), Kirsch plate (SCF = 3), L-domain (Williams λ ≈ 0.5445), Why

### Community 42 - "Boundary Faces"
Cohesion: 0.13
Nodes (22): element_num_nodes(), ElementType, uint32_t, vector, NodalElement, faces, nodes, type (+14 more)

### Community 43 - "Schema Mesh Solve"
Cohesion: 0.15
Nodes (13): description, minimum, type, mesh, total, wall_time_s, description, minimum (+5 more)

### Community 44 - "Mesh Face Headers"
Cohesion: 0.09
Nodes (21): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+13 more)

### Community 45 - "Competitive Peer Solvers"
Cohesion: 0.05
Nodes (37): Agent procedure (blind), Goals, Holdout geometry audits, Optional peer cross-check (owner), Owner supply (private), Result JSON (metrics only), Roles, Status (+29 more)

### Community 47 - "Geometry Sizing"
Cohesion: 0.05
Nodes (50): FreeFace, CircularFeature, axis_dir, axis_point, radius, select_band, CurvedMeshMetrics, composite_score (+42 more)

### Community 48 - "Reference Case Bench"
Cohesion: 0.07
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 49 - "FEA Colormap Display"
Cohesion: 0.19
Nodes (18): traction, assemble_traction_load(), Dynamic, FaceType, Matrix, vector, VectorXd, eval_face_shape() (+10 more)

### Community 50 - "Schema Type Props"
Cohesion: 0.18
Nodes (11): description, type, description, type, properties, case_id, host, schema_version (+3 more)

### Community 53 - "Cantilever Iterative"
Cohesion: 0.15
Nodes (8): uint32_t, constant_strain_max_error(), Matrix3d, sample_strain_gradient(), unit_box_surface(), patch_max_error(), path, unique_temp_vtu()

### Community 56 - "CUDA SpMV Kernel"
Cohesion: 0.08
Nodes (19): vector, array, uint32_t, vector, PolyCell, faces, nodes, array (+11 more)

### Community 57 - "Feature Graded Error"
Cohesion: 0.22
Nodes (8): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector

### Community 58 - "Hex Fill Output"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 59 - "Local Refine Tests"
Cohesion: 0.31
Nodes (8): size_t, array, uint32_t, vector, Vector3d, hex8_jac_det(), hex_fill_surface(), hex_inverted()

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
Cohesion: 0.06
Nodes (55): Entity, HpMode, edge_odd, entity, entity_index, index0, index1, index2 (+47 more)

### Community 66 - "Geom Features Extract"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "L-Domain Solve Tests"
Cohesion: 0.23
Nodes (25): checkpoint_state_cstr(), count_result_lines(), Checkpoint, CheckpointState, json, optional, path, string (+17 more)

### Community 68 - "Solve Energy Output"
Cohesion: 0.18
Nodes (15): fit, array, DisplayMode, uint32_t, vector, Vector3d, fea_colormap(), bake_result (+7 more)

### Community 69 - "Root Schema JSON"
Cohesion: 0.25
Nodes (7): additionalProperties, description, $id, required, $schema, title, type

### Community 71 - "Lame Cylinder Tests"
Cohesion: 0.22
Nodes (10): count_kinds(), size_t, KindCounts, hex, other, pyr, tet, vem (+2 more)

### Community 72 - "Backend Bench Smoke"
Cohesion: 0.22
Nodes (7): graphify, Polyhedral-FEA — agent notes, Benchmark table, Current phase, Done, Open issues, PROGRESS

### Community 73 - "Resolved Mesh Size"
Cohesion: 0.29
Nodes (13): bbox_of(), boundary_edges(), cell_faces(), detect_hole_roi(), draw_line(), face_key(), main(), parse_vtu_ascii() (+5 more)

### Community 76 - "ZZ Recovery API"
Cohesion: 0.28
Nodes (8): VectorXd, LSolve, energy, mesh, peak_vm_at_corner, u, make_l_hex_mesh(), solve_l()

### Community 77 - "STEP Geometry Load"
Cohesion: 0.21
Nodes (18): pair, assemble_hp(), ElementType, edge_slot(), elem_edge_oriented(), hex_face_orient(), hex_face_slot(), is_hex() (+10 more)

### Community 78 - "Kirsch Plate Tests"
Cohesion: 0.26
Nodes (19): FreeFaceKey, bisect_tet(), array, EdgeKey, size_t, span, uint32_t, vector (+11 more)

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
Cohesion: 0.11
Nodes (18): join_worker, set_status, optional, runtime_error, size_t, string, JobCancelled, Model::load() (+10 more)

### Community 90 - "Schema DOFs Field"
Cohesion: 0.50
Nodes (4): description, minimum, type, dofs

### Community 91 - "Schema Version Field"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face.      Fac, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "Schema Wall Time"
Cohesion: 0.09
Nodes (23): AccuracyInfo, metric, rel_err, truth, value, QualityInfo, M1max, M2max (+15 more)

### Community 94 - "Schema Label Field"
Cohesion: 0.20
Nodes (17): size_t, string, draw_colorbar(), draw_column_splitter(), draw_frame(), draw_study_panel(), draw_viewport_content(), drop_callback() (+9 more)

### Community 95 - "Schema Notes Field"
Cohesion: 0.67
Nodes (3): description, type, notes

### Community 96 - "Schema Solver Field"
Cohesion: 0.67
Nodes (3): solver, description, type

### Community 97 - "Schema Version Meta"
Cohesion: 0.15
Nodes (15): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+7 more)

### Community 100 - "Region Pick Optional"
Cohesion: 0.12
Nodes (17): atomic, mutex, size_t, string, JobProgress, elapsed_ms, pass, pass_count (+9 more)

### Community 119 - "Verification"
Cohesion: 0.20
Nodes (10): P0 — Decisions & scaffolding, P1 — Reference solver on standard elements (the trustworthy baseline), P2 — Mesh core + tet meshing + validity, P3 — Geometric feature analysis → a priori hybrid meshing, P4 — Polyhedral elements (VEM) [parallel with P3 after P2], P5 — Adaptive loop (the product), P6.5 — GUI (ADR-0006), P6 — Performance engineering (+2 more)

### Community 120 - "Context"
Cohesion: 0.10
Nodes (22): CampaignSummary, dir, has_campaign_json, has_checkpoint, has_results, name, result_count, state (+14 more)

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "unit_box"
Cohesion: 0.17
Nodes (12): Benchmark scoreboard, CLI examples (public unit box), Clone, configure, build, test, Dependencies, GUI, Layout (short), License, Performance build notes (+4 more)

### Community 125 - "cell_stamp.hpp"
Cohesion: 0.21
Nodes (12): BenchError, map, runtime_error, string, ReferenceCase, citation, name, values (+4 more)

### Community 126 - "vector"
Cohesion: 0.18
Nodes (9): Public geometry fixtures, Usage, Manual one-liners, Mesh only (auto h0), Notes, PolyMesh examples, Prerequisites, Scripts (+1 more)

### Community 127 - "optional"
Cohesion: 0.14
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
Cohesion: 0.21
Nodes (13): dist_, blend_to_max(), clamp_size(), DistanceFn, vector, Vector3d, FeatureSizing::FeatureSizing(), FeatureSizing::size_at() (+5 more)

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
Cohesion: 0.20
Nodes (16): size_t, span, vector, Vector3d, flat_idx(), stamp_ball(), stamp_curvature_cells(), stamp_feature_cells() (+8 more)

### Community 138 - "transition_fill_surface"
Cohesion: 0.15
Nodes (13): Index, SparseMatrix, HpSystem, k, local_sign, local_to_global, mode_nodes, n_modes (+5 more)

### Community 139 - "BENCHMARKS — Verification Suite & Anti-Cheat Design"
Cohesion: 0.29
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
Cohesion: 0.11
Nodes (17): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, array, int64_t (+9 more)

### Community 145 - "test_d6_bench_smoke.cpp"
Cohesion: 0.47
Nodes (4): string, run_cmd(), slurp(), temp_out_path()

### Community 146 - "apply_theme"
Cohesion: 0.47
Nodes (4): apply_theme(), ThemeId, make_interwebz_palette(), make_slate_palette()

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
Cohesion: 0.17
Nodes (9): Agent bootstrap — overnight / autonomous work on the DAG, 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, Test-lab interfaces (normative) (+1 more)

### Community 305 - "mesh_preview.py"
Cohesion: 0.11
Nodes (19): CampaignResources, max_mem_gb, max_threads, CampaignScoreWeights, accuracy, mesh_ms, solve_ms, CampaignSpec (+11 more)

### Community 306 - "Element"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "8. Graphify (for agents)"
Cohesion: 0.61
Nodes (7): check_prism_fill_geometry(), Vector3d, pick_sweep_axis(), prism_fill_surface(), prism_signed_volume(), prism_signed_volume_impl(), tet_signed_volume_impl()

### Community 308 - "Implementation notes (for coding agents)"
Cohesion: 0.22
Nodes (9): Index, map, uint32_t, uint8_t, vector, Vector3d, homogeneous_boundary(), modes_on_boundary() (+1 more)

### Community 309 - "PROGRESS"
Cohesion: 0.18
Nodes (10): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, Decision policy (v1, `adapt::drive_hp`), The adaptive solver core, explained, What is implemented (node `fe-vem-assembly`) (+2 more)

### Community 310 - "solve"
Cohesion: 0.29
Nodes (6): EdgeKey, size_t, edge_key(), EdgeKeyHash, elem_edge(), TriKeyHash

### Community 311 - "timestamp"
Cohesion: 0.32
Nodes (6): count_zero_modes(), Dynamic, Matrix, MatrixXd, unit_hex_coords(), unit_tet_coords()

### Community 312 - "run_mesher_scoreboard.py"
Cohesion: 0.83
Nodes (3): main(), Path, run_one()

### Community 313 - "Verification"
Cohesion: 0.46
Nodes (7): cell_inverted(), array, vector, Vector3d, hex8_jacobian_det(), tet_signed_vol(), transition_fill_surface()

### Community 314 - "case_id"
Cohesion: 0.25
Nodes (8): ElementTendencyPlan, label, mesher, native_poly_transitions, remapped, skin_layers, tendency, VolumeMesher

### Community 315 - "Context"
Cohesion: 0.40
Nodes (6): array, uint32_t, elem_tri(), map_tet_face_mode(), tri_key(), TriKey

### Community 316 - "test_lame_cylinder.cpp"
Cohesion: 0.20
Nodes (13): path, string, vector, find_testlab_binary(), State, string, is_executable_file(), ProcessRunner (+5 more)

### Community 317 - "test_product_mesh_tier1.cpp"
Cohesion: 0.22
Nodes (7): cantilever_setup(), path, write_box_stl(), box_end_regions(), path, write_box_stl(), thread

### Community 318 - "schema_version"
Cohesion: 0.20
Nodes (15): cfg_id_of(), cmd_pause_status(), json, expand_grid(), geom_class_of(), main(), parse_box(), parse_mesher() (+7 more)

### Community 319 - "wall_time_s"
Cohesion: 0.35
Nodes (16): atomic_write(), completed_keys(), path, set, string, load_campaign(), load_case(), load_checkpoint() (+8 more)

### Community 320 - "FaceOrient"
Cohesion: 0.50
Nodes (4): FaceOrient, sign0, sign1, swap

### Community 321 - "version"
Cohesion: 0.67
Nodes (3): version, description, type

### Community 322 - "test_vtu.cpp"
Cohesion: 0.15
Nodes (16): Box3, hi, lo, Vector3d, face_centroid(), free_faces_as_surface(), LoadSpec, box (+8 more)

### Community 323 - "mesh_preview.py"
Cohesion: 0.53
Nodes (5): main(), parse_mesh_stdout(), Path, Best-effort wireframe via pure-Python exterior edges, then meshio., try_render_png()

### Community 324 - "ClosestOnFeature"
Cohesion: 0.12
Nodes (15): size_t, OctaFillOutput, h, mesh, n_boundary_pyramids, n_octahedra, array, uint32_t (+7 more)

### Community 325 - "HpDriverPolicy"
Cohesion: 0.12
Nodes (16): HpDriverPolicy, cost_h, cost_p, cost_shape, dorfler_theta, eta_rel_floor, geometry_force_h, h_min (+8 more)

### Community 326 - "Material"
Cohesion: 0.14
Nodes (9): Material, d_matrix, poissons_ratio, youngs_modulus, d, Matrix, Material::d_matrix(), path (+1 more)

### Community 327 - "HpDriverPlan"
Cohesion: 0.14
Nodes (14): HpDriverPlan, decisions, global_shape, h_mark, h_suggestion, n_h, n_none, n_p (+6 more)

### Community 328 - "ElementHpSignal"
Cohesion: 0.15
Nodes (12): ElementHpSignal, eta, h, hex_fit, kappa, p, p_max, poly_fit (+4 more)

### Community 329 - "run_one"
Cohesion: 0.20
Nodes (12): BcSpec, box, fix, compute_probes(), array, VectorXd, evaluate_probe(), make_dirichlet() (+4 more)

### Community 330 - "Campaign"
Cohesion: 0.17
Nodes (12): Campaign, grid, name, parts, tiers, w_accuracy, w_mesh_ms, w_solve_ms (+4 more)

### Community 331 - "Backend"
Cohesion: 0.27
Nodes (10): Backend, active_backend(), backend_description(), string, init_runtime_performance(), openmp_default_threads(), openmp_enabled(), openmp_max_threads() (+2 more)

### Community 332 - "ElementHpDecision"
Cohesion: 0.17
Nodes (11): HpAction, ElementHpDecision, action, h_next, p_next, reason, shape, utility_h (+3 more)

### Community 333 - "hp_driver.cpp"
Cohesion: 0.30
Nodes (11): best_shape_vote(), clamp01(), ShapeTendency, string, decide_element(), geometry_severity(), is_thin_wall(), shape_awkwardness() (+3 more)

### Community 334 - "GradedTetFillOutput"
Cohesion: 0.17
Nodes (11): GradedTetFillOutput, h_coarse, h_fine, mesh, n_coarse_cells, n_feature_cells, n_fine_cells, n_seed_cells (+3 more)

### Community 335 - "Checkpoint"
Cohesion: 0.18
Nodes (11): Checkpoint, campaign, completed_runs, started_utc, state, survivors, tier, updated_utc (+3 more)

### Community 336 - "CantileverSetup"
Cohesion: 0.22
Nodes (10): CantileverSetup, bc, length, loads, mesh, nfree, Index, VectorXd (+2 more)

### Community 337 - "Config"
Cohesion: 0.20
Nodes (10): Config, curvature_turn_deg, element_tendency, feature_refine, id, mesher, order, snap_boundary (+2 more)

### Community 338 - "PartCase"
Cohesion: 0.20
Nodes (10): PartCase, bcs, E, geometry, loads, metrics, nu, part (+2 more)

### Community 339 - "element_stiffness"
Cohesion: 0.36
Nodes (9): assemble_stiffness(), b_matrix(), Dynamic, Matrix, MatrixXd, SparseMatrix, element_coords(), element_stiffness() (+1 more)

### Community 340 - "test_local_refine.cpp"
Cohesion: 0.40
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 341 - "LiveProgress"
Cohesion: 0.22
Nodes (9): LiveProgress, cfg_id, cg_iter, cg_resid, elapsed_ms, part, phase, phase_frac (+1 more)

### Community 342 - "dorfler_mark"
Cohesion: 0.31
Nodes (8): size_t, vector, Vector3d, dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth(), Vector3d, drive_hp()

### Community 343 - "recover_nodal_stress"
Cohesion: 0.25
Nodes (8): ElementType, vector, reference_nodes(), Stress, vector, VectorXd, recover_nodal_stress(), von_mises()

### Community 344 - "recover_zz"
Cohesion: 0.33
Nodes (8): Dynamic, Matrix, Stress, Vector3d, VectorXd, element_centroid(), recover_zz(), stress_at()

### Community 345 - "LocalRefineStats"
Cohesion: 0.22
Nodes (8): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets, n_surface_mids

### Community 346 - "MetricSpec"
Cohesion: 0.25
Nodes (8): MetricSpec, derivation, name, probe, tol, ProbeSpec, kind, nominal

### Community 347 - "4. Engineering standards (non-negotiable)"
Cohesion: 0.29
Nodes (7): 4. Engineering standards (non-negotiable), Anti-cheat (sacred), CUDA, Eigen traps, Git identity (owner agents), Language & build, License

### Community 348 - "make_hp_signals"
Cohesion: 0.48
Nodes (7): at_or_broadcast(), at_or_broadcast_int(), size_t, span, vector, estimate_surplus_from_zz(), make_hp_signals()

### Community 349 - "suggest_refine"
Cohesion: 0.57
Nodes (6): span, vector, Vector3d, marked_centroids(), suggest_refine(), suggest_uniform_refine()

### Community 350 - "test_spmv.cpp"
Cohesion: 0.33
Nodes (6): SparseMatrix, vector, VectorXd, make_spd_test_matrix(), max_abs_diff(), random_vector()

### Community 351 - "Building (options)"
Cohesion: 0.33
Nodes (6): Building (options), CUDA backends (`POLYMESH_WITH_CUDA`), Linear solve (direct / CG), Mesh path caveat, OpenMP assembly (`POLYMESH_WITH_OPENMP`), STEP / OpenCASCADE (`POLYMESH_WITH_OCC`)

### Community 352 - "Element"
Cohesion: 0.33
Nodes (4): Element, num_nodes, order, stiffness

### Community 353 - "elem_quad"
Cohesion: 0.50
Nodes (4): QuadKey, elem_quad(), quad_key(), QuadKeyHash

### Community 354 - "solve"
Cohesion: 0.50
Nodes (4): solve, description, minimum, type

### Community 355 - "timestamp"
Cohesion: 0.50
Nodes (4): timestamp, description, format, type

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **1147 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+1142 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **167 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `Viewport` connect `GUI Viewport GL` to `Solve Energy Output`, `pick_region`, `GUI App State`, `GL Shader Bind`, `CUDA SpMV Kernel`, `Viewport Camera`?**
  _High betweenness centrality (0.053) - this node is a cross-community bridge._
- **Why does `Material` connect `Material` to `Element`, `ZZ Stress Recovery`, `Schema Version Meta`, `VEM & Nodal Mesh`, `recover_zz`, `Element Assembly Core`, `TriSurface Geometry`, `run_one`, `CLI Mesh Solve`, `STEP Geometry Load`, `Quickstart (Ubuntu)`, `element_stiffness`, `FEA Solve Methods`, `test_local_refine.cpp`, `Cantilever Iterative`, `recover_nodal_stress`, `CUDA SpMV Kernel`, `schema_version`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._
- **Why does `TriSurface` connect `TriSurface Geometry` to `VolumeMesher`, `sizing_field.cpp`, `Hybrid Graded Fill`, `CLI Mesh Solve`, `prism_fill_surface`, `Gmsh MSH Import`, `Pipeline Scene Jobs`, `Mixed Cell Fill`, `GUI Widgets`, `Sizing Field Blend`, `Geom Indicators`, `STL Geometry IO`, `Feature Sizing Field`, `Mesh Face Headers`, `Geometry Sizing`, `8. Graphify (for agents)`, `Cantilever Iterative`, `CUDA SpMV Kernel`, `Verification`, `Feature Graded Error`, `Local Refine Tests`, `Lame Cylinder Tests`, `Kirsch Plate Tests`, `Tet Fill Tests`, `GroupBox Frame UI`?**
  _High betweenness centrality (0.034) - this node is a cross-community bridge._
- **Are the 42 inferred relationships involving `FeaError` (e.g. with `promote_tet4_to_tet10()` and `solve_l_mesh()`) actually correct?**
  _`FeaError` has 42 INFERRED edges - model-reasoned connections that need verification._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _1181 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `VEM & Nodal Mesh` be split into smaller, more focused modules?**
  _Cohesion score 0.0686641697877653 - nodes in this community are weakly interconnected._