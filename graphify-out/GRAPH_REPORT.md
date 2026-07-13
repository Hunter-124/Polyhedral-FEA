# Graph Report - Polyhedral-FEA  (2026-07-12)

## Corpus Check
- 396 files · ~327,340 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 3611 nodes · 5848 edges · 412 communities (241 shown, 171 thin omitted)
- Extraction: 94% EXTRACTED · 6% INFERRED · 0% AMBIGUOUS · INFERRED: 370 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `7541c89d`
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
- GeometrySizing
- LocalRefineStats
- MetricSpec
- element_stiffness
- make_hp_signals
- suggest_refine
- test_spmv.cpp
- Competitive benchmark harness
- SharpEdge
- elem_quad
- FeaError
- test_local_refine.cpp
- Spec
- GroupBoxFrame
- elem_quad
- Feedback loop — campaign → defaults
- Checkpoint
- Holdout geometry audits
- schema_version
- README.md
- VaryhedronFillOutput
- pick_region
- write_grok_handoff.py
- Recommended approach (not all alternatives)
- p2_strain_basis
- TetFillOutput
- solve_l
- MatrixXd
- cad_topology.cpp
- OctaFillOutput
- varyhedron_fill_surface
- compile
- HexFace
- 2. Bubble / sphere packing → Delaunay
- 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage)
- 4. Field-aligned hex-dominant (PGP3D-class)
- PROGRESS
- Polyhedral-FEA — agent notes
- test_quadrature.cpp
- PROGRESS
- invoke_grok_improve.sh
- size_t
- uint32_t
- elem_edge
- MeshPreview
- Checkpoint
- WallProjectStats
- HandoffInfo
- Protecting balls + local feature size (LFS)
- p2_strain_basis
- MatrixXd
- mean_lateral_radial_residual
- BRep face-tag BCs / probes (design stub)
- ClosestOnFeature
- Geogram (vendored subset) — placeholder
- pick_region
- accuracy
- solve
- timestamp
- FaceOrient
- case_id
- test_vtu.cpp
- ClosestOnFeature
- notes
- README.md

## God Nodes (most connected - your core abstractions)
1. `NodalMesh` - 76 edges
2. `TriSurface` - 67 edges
3. `Viewport` - 58 edges
4. `SolveJob` - 49 edges
5. `TestLabState` - 48 edges
6. `App` - 45 edges
7. `FeaError` - 45 edges
8. `Material` - 42 edges
9. `Palette` - 38 edges
10. `Model` - 36 edges

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

## Communities (412 total, 171 thin omitted)

### Community 0 - "VEM & Nodal Mesh"
Cohesion: 0.23
Nodes (36): kP2Mono, kP2Vec, b_from_grads(), char_length(), Dynamic, function, Matrix, MatrixXd (+28 more)

### Community 1 - "Grid Classification"
Cohesion: 0.12
Nodes (37): accuracy_of(), aggregate_configs(), analyze_one(), CfgAgg, config_label(), factor_breakdown(), _fmt_ms(), _fmt_pct() (+29 more)

### Community 2 - "Element Assembly Core"
Cohesion: 0.05
Nodes (50): FreeFace, CircularFeature, axis_dir, axis_point, radius, select_band, CurvedMeshMetrics, composite_score (+42 more)

### Community 3 - "GUI Viewport GL"
Cohesion: 0.04
Nodes (43): array, DisplayMode, uint32_t, vector, Vector3d, VectorXd, Viewport, background_program_ (+35 more)

### Community 4 - "Example Geometries"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "Hybrid Graded Fill"
Cohesion: 0.15
Nodes (21): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+13 more)

### Community 6 - "GUI Theme Palette"
Cohesion: 0.05
Nodes (40): apply_theme(), ThemeId, ImVec4, make_interwebz_palette(), make_slate_palette(), Palette, accent, accent_dim (+32 more)

### Community 7 - "Bench Emit Scripts"
Cohesion: 0.31
Nodes (9): fmt_num(), load_results(), main(), Any, Path, Markdown-friendly ASCII sparkline; skips None., Minimal inline SVG polyline for accuracy trend (lower often better)., sparkline() (+1 more)

### Community 8 - "SpMV CSR Backend"
Cohesion: 0.05
Nodes (51): cmd_check(), cmd_mesh(), cmd_solve(), span, string, string_view, VolumeMesher, load_surface() (+43 more)

### Community 9 - "CLI Mesh Solve"
Cohesion: 0.13
Nodes (18): ElementCentroidStress, centroid, element_index, quality, stress, volume, uint32_t, Vector3d (+10 more)

### Community 10 - "Poly Mesh Topology"
Cohesion: 0.09
Nodes (22): optional, CellId, CellKind, FaceId, Cell, faces, kind, Face (+14 more)

### Community 11 - "GUI App State"
Cohesion: 0.06
Nodes (32): App, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, job, live_mesh_seen_gen (+24 more)

### Community 12 - "Gmsh MSH Import"
Cohesion: 0.14
Nodes (22): size_t, runtime_error, ValidityError, array, span, uint32_t, vector, Vector3d (+14 more)

### Community 13 - "Surface Traction"
Cohesion: 0.12
Nodes (26): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+18 more)

### Community 14 - "Pipeline Scene Jobs"
Cohesion: 0.16
Nodes (18): empty, checkpoint, join_worker, publish_live_mesh, report, reset_control_flags, set_status, solve_options_with_progress (+10 more)

### Community 15 - "Structured Mesh Tests"
Cohesion: 0.14
Nodes (20): check_validity, box_hex_mesh(), box_tet_mesh(), cell_corners(), array, uint32_t, uint64_t, Vector3d (+12 more)

### Community 16 - "Surface Projection"
Cohesion: 0.07
Nodes (48): CheckpointState, ImVec4, path, size_t, string, draw_results_panel(), fmt_opt_num(), Checkpoint (+40 more)

### Community 17 - "Shape Functions"
Cohesion: 0.08
Nodes (24): 0. One-sentence strategy, 10. Related files, 1. Substrate (keep forever until proven wrong), 2. Claims (product honesty), 3.1 Five-number scorecard + residual gate, 3.2 What to score vs dashboard (stress), 3.3 Chordal efficiency (edge residual), 3.4 Over-budget diagnosis (+16 more)

### Community 18 - "Adapt Loop Seeds"
Cohesion: 0.20
Nodes (9): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+1 more)

### Community 19 - "Mixed Cell Fill"
Cohesion: 0.07
Nodes (52): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, MixedCellKind, array, size_t, uint32_t (+44 more)

### Community 20 - "FEA Solve Methods"
Cohesion: 0.11
Nodes (25): CG, Dirichlet, dof_values, function, Index, map, SolveMethod, SolveOptions (+17 more)

### Community 21 - "Scene Solve Result"
Cohesion: 0.08
Nodes (28): array, map, uint32_t, vector, VectorXd, note_mesh_stats, SolveResult, boundary_quads (+20 more)

### Community 22 - "GUI Widgets"
Cohesion: 0.17
Nodes (20): CollectOffendersFn, closest_on_surface(), closest_on_surface_brute(), closest_on_triangle(), array, size_t, span, uint32_t (+12 more)

### Community 23 - "MMS Convergence Tests"
Cohesion: 0.09
Nodes (20): atomic, mutex, size_t, thread, time_point, ProgressHeartbeat, cfg_id_, cg_iter_ (+12 more)

### Community 24 - "Solve Job Pipeline"
Cohesion: 0.07
Nodes (31): State, time_point, uint64_t, load, SolveJob, cancel_, clear_failure, error_ (+23 more)

### Community 25 - "D6 Tier3 Bench"
Cohesion: 0.09
Nodes (39): add_node(), array, int64_t, json, map, string, uint32_t, vector (+31 more)

### Community 26 - "Viewport Camera"
Cohesion: 0.17
Nodes (17): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+9 more)

### Community 27 - "Sizing Field Blend"
Cohesion: 0.17
Nodes (12): GeometrySizing, blend_, curv_frac_, edges_, h_max_, h_min_, kappa_, size_at (+4 more)

### Community 28 - "Transition Fill"
Cohesion: 0.12
Nodes (19): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+11 more)

### Community 29 - "CMake Project Build"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "Scene Model Bounds"
Cohesion: 0.24
Nodes (14): _assert_manifold_facets(), _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre., Parse emitted ASCII facet blocks and require edge multiplicity 2. (+6 more)

### Community 31 - "Geom Indicators"
Cohesion: 0.33
Nodes (5): vector, VertexCurvature, kappa, VertexThickness, thickness

### Community 32 - "STL Geometry IO"
Cohesion: 0.15
Nodes (12): Impl, shared_ptr, CadModel, bbox_diagonal, compute_bbox, has_brep, impl_, name_ (+4 more)

### Community 33 - "TriSurface Geometry"
Cohesion: 0.10
Nodes (27): array, uint32_t, vector, Vector3d, TriSurface, triangles, validate, vertices (+19 more)

### Community 34 - "Sim Setup Loads"
Cohesion: 0.08
Nodes (26): ElementTendencyPlan, label, mesher, native_poly_transitions, remapped, skin_layers, tendency, atomic (+18 more)

### Community 35 - "Kirsch Graded Tests"
Cohesion: 0.13
Nodes (16): Index, SparseMatrix, HpSystem, k, local_sign, local_to_global, mode_nodes, n_modes (+8 more)

### Community 36 - "P-Elevate Elements"
Cohesion: 0.11
Nodes (36): compute_probes(), compute_scorecard_geom(), compute_solve_health(), count_orphan_nodes(), uint32_t, vector, Vector3d, VectorXd (+28 more)

### Community 37 - "Tet Quality Metrics"
Cohesion: 0.15
Nodes (27): boundary_residual_placeholder(), bubble_relax(), _clamp01(), _dedupe(), _dist2(), fill_fraction_proxy(), main(), pack_case() (+19 more)

### Community 38 - "Feature Sizing Field"
Cohesion: 0.15
Nodes (11): FeatureSizing, blend_, h_max_, h_min_, size_at, DistanceFn, Vector3d, SizingField (+3 more)

### Community 39 - "GUI Study Panels"
Cohesion: 0.09
Nodes (22): cantilever, cylinder, Engineering estimate: polar compression as a short column, Finite-domain note, Hand-calculated reference truths, How to add a part, icecream-cone, Infinite-plate Kirsch solution (+14 more)

### Community 40 - "JSON Schema Props"
Cohesion: 0.14
Nodes (14): additionalProperties, properties, required, type, description, type, accuracy, name (+6 more)

### Community 41 - "Tier1 Verification Cases"
Cohesion: 0.22
Nodes (8): ADR-0009: Tier-1 analytical verification setups (Kirsch / Goodier / L-domain), Alternatives, Decision, Gmsh import, Goodier cavity (SCF = 3(9−5ν)/(2(7−5ν))), Kirsch plate (SCF = 3), L-domain (Williams λ ≈ 0.5445), Why

### Community 42 - "Boundary Faces"
Cohesion: 0.18
Nodes (11): Config, curvature_turn_deg, element_tendency, feature_refine, id, mesher, order, snap_boundary (+3 more)

### Community 43 - "Schema Mesh Solve"
Cohesion: 0.15
Nodes (13): description, minimum, type, mesh, solve, total, description, minimum (+5 more)

### Community 44 - "Mesh Face Headers"
Cohesion: 0.10
Nodes (19): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+11 more)

### Community 45 - "Competitive Peer Solvers"
Cohesion: 0.05
Nodes (37): Agent procedure (blind), Goals, Holdout geometry audits, Optional peer cross-check (owner), Owner supply (private), Result JSON (metrics only), Roles, Status (+29 more)

### Community 47 - "Geometry Sizing"
Cohesion: 0.23
Nodes (17): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+9 more)

### Community 48 - "Reference Case Bench"
Cohesion: 0.07
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 49 - "FEA Colormap Display"
Cohesion: 0.14
Nodes (23): _edge(), _face_triangle(), _is_valid(), main(), make_cylinder(), make_icecream_cone(), make_plate_hole(), make_sphere() (+15 more)

### Community 50 - "Schema Type Props"
Cohesion: 0.18
Nodes (11): description, type, description, type, properties, case_id, host, timestamp (+3 more)

### Community 53 - "Cantilever Iterative"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 56 - "CUDA SpMV Kernel"
Cohesion: 0.18
Nodes (11): array, uint32_t, vector, Vector3d, PrismFillOutput, boundary_max_distance, boundary_quads, h (+3 more)

### Community 57 - "Feature Graded Error"
Cohesion: 0.29
Nodes (8): gp_Pnt, gp_Vec, optional, TopoDS_Face, face_normal_at(), project_on_face(), project_point_on_surface(), TopoDS_Vertex

### Community 58 - "Hex Fill Output"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 59 - "Local Refine Tests"
Cohesion: 0.43
Nodes (7): array, uint32_t, vector, Vector3d, hex8_jac_det(), hex_fill_surface(), hex_inverted()

### Community 60 - "Audit Bench Policy"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "Mesh Spec Layers"
Cohesion: 0.09
Nodes (23): 0. Contributing with AI agents (quick start), 10. Quick “I am lost” paths, 1. What this repo is, 2. Directory layout (keep it), 3. Where to change what, 4. Engineering standards (non-negotiable), 5. Documentation standards (no slop), 6. How to add a feature (agent checklist) (+15 more)

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
Cohesion: 0.19
Nodes (31): checkpoint_state_cstr(), count_result_lines(), Checkpoint, CheckpointState, json, optional, path, string (+23 more)

### Community 68 - "Solve Energy Output"
Cohesion: 0.13
Nodes (23): bind_line_attr(), fit, compile(), array, DisplayMode, optional, uint32_t, vector (+15 more)

### Community 69 - "Root Schema JSON"
Cohesion: 0.25
Nodes (7): additionalProperties, description, $id, required, $schema, title, type

### Community 71 - "Lame Cylinder Tests"
Cohesion: 0.22
Nodes (10): count_kinds(), size_t, KindCounts, hex, other, pyr, tet, vem (+2 more)

### Community 72 - "Backend Bench Smoke"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 73 - "Resolved Mesh Size"
Cohesion: 0.29
Nodes (13): bbox_of(), boundary_edges(), cell_faces(), detect_hole_roi(), draw_line(), face_key(), main(), parse_vtu_ascii() (+5 more)

### Community 76 - "ZZ Recovery API"
Cohesion: 0.16
Nodes (14): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, Dynamic, Matrix (+6 more)

### Community 77 - "STEP Geometry Load"
Cohesion: 0.22
Nodes (17): pair, assemble_hp(), ElementType, edge_slot(), elem_edge_oriented(), hex_face_slot(), is_hex(), is_tet() (+9 more)

### Community 78 - "Kirsch Plate Tests"
Cohesion: 0.17
Nodes (23): Vector3d, QuadraturePoint, weight, xi, assemble_body_load(), BodyForce, VectorXd, assemble_hp_body_load() (+15 more)

### Community 79 - "Theme Apply Palettes"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "GL Shader Bind"
Cohesion: 0.31
Nodes (12): byte, path, size_t, Soup, span, T, is_ascii_stl(), load_stl() (+4 more)

### Community 82 - "GUI Phase ADR"
Cohesion: 0.50
Nodes (3): Adding a colorscheme, GUI theme & layout, Rules

### Community 84 - "Agent Loop Process"
Cohesion: 0.22
Nodes (8): Agent loop — harness rules for finishing PolyMesh, GUI verification (DISPLAY may be missing), `/loop` vs this file, One iteration = one ROADMAP ID (or one vertical story), Parallelism, Session start checklist, Source of truth, Stuck protocol

### Community 85 - "Quadrature Tests"
Cohesion: 0.22
Nodes (20): begin_field(), begin_group_box(), begin_group_box_fill(), button(), checkbox(), ImVec4, draw_accent_fill(), draw_box() (+12 more)

### Community 86 - "Shape Function Tests"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "STL IO Tests"
Cohesion: 0.50
Nodes (4): as_bytes(), byte, string_view, vector

### Community 88 - "Tet Fill Tests"
Cohesion: 0.09
Nodes (40): add_face(), array, map, uint32_t, vector, emit_element_faces(), extract_boundary_faces(), FaceRecord (+32 more)

### Community 89 - "GroupBox Frame UI"
Cohesion: 0.13
Nodes (18): runtime_error, size_t, span, string, Vector3d, VolumeMesher, JobCancelled, Model::load() (+10 more)

### Community 90 - "Schema DOFs Field"
Cohesion: 0.50
Nodes (4): description, minimum, type, dofs

### Community 91 - "Schema Version Field"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face.      Fac, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "Schema Wall Time"
Cohesion: 0.07
Nodes (28): AnswersInfo, load_area_rel_err, load_face_area, sigma_face_mean, strain_energy, tip_deflection, QualityInfo, M1max (+20 more)

### Community 94 - "Schema Label Field"
Cohesion: 0.18
Nodes (7): array, vector, array, uint32_t, vector, Vector3d, mean_lateral_radial_residual()

### Community 95 - "Schema Notes Field"
Cohesion: 0.15
Nodes (15): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+7 more)

### Community 96 - "Schema Solver Field"
Cohesion: 0.17
Nodes (13): Box3, hi, lo, optional, evaluate_probe(), LoadSpec, box, expected_area (+5 more)

### Community 97 - "Schema Version Meta"
Cohesion: 0.10
Nodes (13): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+5 more)

### Community 100 - "Region Pick Optional"
Cohesion: 0.11
Nodes (19): size_t, string, JobProgress, cg_iter, cg_resid, elapsed_ms, n_elems, n_nodes (+11 more)

### Community 119 - "Verification"
Cohesion: 0.18
Nodes (10): P0 — Decisions & scaffolding, P1 — Reference solver on standard elements (the trustworthy baseline), P2 — Mesh core + tet meshing + validity, P3 — Geometric feature analysis → a priori hybrid meshing, P4 — Polyhedral elements (VEM) [parallel with P3 after P2], P5 — Adaptive loop (the product), P6.5 — GUI (ADR-0006), P6 — Performance engineering (+2 more)

### Community 120 - "Context"
Cohesion: 0.22
Nodes (8): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "unit_box"
Cohesion: 0.11
Nodes (19): Active program (agents — read this first), Benchmark scoreboard, Building (options), CLI examples (public unit box), Clone, configure, build, test, CUDA backends (`POLYMESH_WITH_CUDA`), Dependencies, GUI (+11 more)

### Community 125 - "cell_stamp.hpp"
Cohesion: 0.14
Nodes (17): BenchError, map, runtime_error, string, ReferenceCase, citation, name, values (+9 more)

### Community 126 - "vector"
Cohesion: 0.18
Nodes (9): Public geometry fixtures, Usage, Manual one-liners, Mesh only (auto h0), Notes, PolyMesh examples, Prerequisites, Scripts (+1 more)

### Community 127 - "optional"
Cohesion: 0.20
Nodes (10): Agent system prompt (paste this), CHANGES.md — Agent instructions for external PRs, Correct clone (do this first — most failures start here), Hard rules, Merge responsibility, Mission, Open the PR, Start work (every session) (+2 more)

### Community 128 - "string"
Cohesion: 0.18
Nodes (10): ADR-0015: Cartesian grid-fill limits (not Delaunay / frontal), Consequences, Context, Decision, Follow-on, Lattice construction (bbox-fitted), Ray parity (shared-edge dedupe), Staircasing and when they fail (+2 more)

### Community 129 - "VectorXd"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "VolumeMesher"
Cohesion: 0.26
Nodes (14): string, draw_colorbar(), draw_column_splitter(), draw_frame(), draw_study_panel(), draw_viewport_content(), drop_callback(), is_geometry_path() (+6 more)

### Community 131 - "CLAUDE.md — PolyMesh (working name)"
Cohesion: 0.22
Nodes (8): CLAUDE.md — PolyMesh (working name), Definitions of done, Git & attribution, Language & tooling, Licensing, Moved, Non-negotiable engineering rules, Workflow

### Community 132 - "sizing_field.cpp"
Cohesion: 0.41
Nodes (15): bisect_tet(), array, EdgeKey, span, uint32_t, vector, Vector3d, erase_tet_from_edge() (+7 more)

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
Cohesion: 0.22
Nodes (19): CartesianGrid, cell, nx, ny, nz, origin, cells_for_extent(), classify_cells_inside() (+11 more)

### Community 139 - "BENCHMARKS — Verification Suite & Anti-Cheat Design"
Cohesion: 0.29
Nodes (6): Anti-cheat / adversarial audit design, BENCHMARKS — Verification Suite & Anti-Cheat Design, Tier 0 — Correctness gates (must pass exactly, every commit), Tier 1 — Analytical solutions (energy-norm + peak-stress error targets), Tier 2 — Method of Manufactured Solutions (MMS), Tier 3 — Performance benchmarks (the "win" metric)

### Community 140 - "ADR-0010: Mesh data structure — keep face-based; edge index optional"
Cohesion: 0.29
Nodes (6): ADR-0010: Mesh data structure — keep face-based; edge index optional, Alternatives, Context, Decision, Rejected for now, Why face-based wins for PolyMesh

### Community 141 - "string"
Cohesion: 0.25
Nodes (7): ElementTypeCounts, hex20, hex8, other, tet10, tet4, size_t

### Community 142 - "run_calculix_cantilever.py"
Cohesion: 0.53
Nodes (5): ccx_version(), main(), Path, Write coarse C3D8 cantilever deck. Returns (nnodes, n_fixed_nodes)., write_inp()

### Community 143 - "ADR-0011: VEM k=1 for polyhedra"
Cohesion: 0.33
Nodes (5): ADR-0011: VEM k=1 for polyhedra, Alternatives, Decision, Formulation, Why

### Community 144 - "Quickstart (Ubuntu)"
Cohesion: 0.17
Nodes (12): Campaign matrix, Case primary accuracy metrics (as wired at freeze), Freeze identity, Known issues frozen-in (not blockers for freeze), M9 frozen baseline — `varyhedron-baseline-m9`, Metric schema version, Outcome summary, Per-run snapshot (+4 more)

### Community 145 - "test_d6_bench_smoke.cpp"
Cohesion: 0.47
Nodes (4): string, run_cmd(), slurp(), temp_out_path()

### Community 146 - "apply_theme"
Cohesion: 0.12
Nodes (16): `cantilever`, Config ranking (weighted mean score), `curved`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 147 - "D6 Tier-3 — L-domain uniform tet10 vs graded tet10"
Cohesion: 0.40
Nodes (4): D6 Tier-3 — L-domain uniform tet10 vs graded tet10, Headline (equal strain-energy match, 0.01% tol), Ratios (uniform / graded), Reproduce

### Community 148 - "ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available"
Cohesion: 0.40
Nodes (4): ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP; STL compare/legacy, Alternatives, Decision, Why

### Community 149 - "ADR-0002: License — BSD-3-Clause"
Cohesion: 0.50
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
Cohesion: 0.10
Nodes (20): 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp` (+12 more)

### Community 159 - "Recommended approach (not all alternatives)"
Cohesion: 0.12
Nodes (14): Campaign warehouse, Directory layout, git-LFS, Short-campaign defaults (Lane V), Wireframe PNGs (`wire.png`), ADR-0022: Full experiment warehouse + headless Grok improvement loop, Alternatives rejected, Consequences (+6 more)

### Community 305 - "mesh_preview.py"
Cohesion: 0.08
Nodes (27): AccuracyInfo, metric, rel_err, truth, value, CampaignSummary, dir, has_campaign_json (+19 more)

### Community 306 - "Element"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "8. Graphify (for agents)"
Cohesion: 0.19
Nodes (18): traction, assemble_traction_load(), Dynamic, FaceType, Matrix, vector, VectorXd, eval_face_shape() (+10 more)

### Community 308 - "Implementation notes (for coding agents)"
Cohesion: 0.22
Nodes (9): Index, map, uint32_t, uint8_t, vector, Vector3d, homogeneous_boundary(), modes_on_boundary() (+1 more)

### Community 309 - "PROGRESS"
Cohesion: 0.18
Nodes (10): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, Decision policy (v1, `adapt::drive_hp`), The adaptive solver core, explained, What is implemented (node `fe-vem-assembly`) (+2 more)

### Community 310 - "solve"
Cohesion: 0.14
Nodes (13): Config ranking (weighted mean score), Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run, Pareto analysis — `smoke`, Pareto by geometric class (+5 more)

### Community 311 - "timestamp"
Cohesion: 0.32
Nodes (6): count_zero_modes(), Dynamic, Matrix, MatrixXd, unit_hex_coords(), unit_tet_coords()

### Community 312 - "run_mesher_scoreboard.py"
Cohesion: 0.83
Nodes (3): main(), Path, run_one()

### Community 313 - "Verification"
Cohesion: 0.16
Nodes (15): RadialMap, array, int64_t, Matrix3d, size_t, Vector3d, kirsch_stress(), KirschRun (+7 more)

### Community 314 - "case_id"
Cohesion: 0.14
Nodes (14): 1. Substrate (keep, do not replace), 2. Element technology claims, 3. Packing evolution, 4. CAD edge classification (normative), 5. Measurement order (two-week horizon), 6. License landscape (core vs plugin), 7. Sizing field, 8. p-order (+6 more)

### Community 315 - "Context"
Cohesion: 0.25
Nodes (7): Autonomous vs supervised answers, Grok improvement loop, Handoff contents, Headless invoke (default), Manual interactive (optional), Safety, When it runs

### Community 316 - "test_lame_cylinder.cpp"
Cohesion: 0.20
Nodes (13): path, string, vector, find_testlab_binary(), State, string, is_executable_file(), ProcessRunner (+5 more)

### Community 317 - "test_product_mesh_tier1.cpp"
Cohesion: 0.11
Nodes (17): optional, thread, Model, bbox_max, bbox_min, cad, name, region_count (+9 more)

### Community 318 - "schema_version"
Cohesion: 0.18
Nodes (11): CadSurfaceKind, CadFace, area, edge_ids, id, kind, CadTopology, edges (+3 more)

### Community 319 - "wall_time_s"
Cohesion: 0.14
Nodes (30): atomic_write(), Checkpoint, campaign, completed_runs, started_utc, state, survivors, tier (+22 more)

### Community 320 - "FaceOrient"
Cohesion: 0.12
Nodes (18): CadEdgeClassCounts, n_seam, n_sharp, n_smooth, CadVertex, id, position, ClosestEdgeQuery (+10 more)

### Community 321 - "version"
Cohesion: 0.11
Nodes (19): CampaignResources, max_mem_gb, max_threads, CampaignScoreWeights, accuracy, mesh_ms, solve_ms, CampaignSpec (+11 more)

### Community 322 - "test_vtu.cpp"
Cohesion: 0.17
Nodes (12): ADR-0024: Advisor measure-first answers (normative Q&A), Compressed path (do not invent another), Q10 — High-dimensional traps, Q1 — 1e20 von Mises with 1e-13 residual, Q2 — Next 3–5 days order, Q3 — Geogram, Q4 — Chordal efficiency e ~ 100 at h_scale=5, Q5 — Cylinder truth (+4 more)

### Community 323 - "mesh_preview.py"
Cohesion: 0.53
Nodes (5): main(), parse_mesh_stdout(), Path, Best-effort wireframe via pure-Python exterior edges, then meshio., try_render_png()

### Community 324 - "ClosestOnFeature"
Cohesion: 0.12
Nodes (19): ClosestPoint, distance, point, triangle, ConformityStats, count, max_distance, mean_distance (+11 more)

### Community 325 - "HpDriverPolicy"
Cohesion: 0.12
Nodes (16): HpDriverPolicy, cost_h, cost_p, cost_shape, dorfler_theta, eta_rel_floor, geometry_force_h, h_min (+8 more)

### Community 326 - "Material"
Cohesion: 0.15
Nodes (14): Fun, array, VectorXd, integrate_p2_matrix(), integrate_p2_vector(), P2Projector, dof_eval, fan (+6 more)

### Community 327 - "HpDriverPlan"
Cohesion: 0.14
Nodes (14): HpDriverPlan, decisions, global_shape, h_mark, h_suggestion, n_h, n_none, n_p (+6 more)

### Community 328 - "ElementHpSignal"
Cohesion: 0.15
Nodes (12): ElementHpSignal, eta, h, hex_fit, kappa, p, p_max, poly_fit (+4 more)

### Community 329 - "run_one"
Cohesion: 0.13
Nodes (14): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+6 more)

### Community 330 - "Campaign"
Cohesion: 0.12
Nodes (16): Campaign, grid, max_pack_wall_s, max_run_wall_s, name, on_finish_analyze, on_finish_grok, parts (+8 more)

### Community 331 - "Backend"
Cohesion: 0.40
Nodes (9): size_t, span, vector, Vector3d, flat_idx(), stamp_ball(), stamp_curvature_cells(), stamp_feature_cells() (+1 more)

### Community 332 - "ElementHpDecision"
Cohesion: 0.17
Nodes (11): HpAction, ElementHpDecision, action, h_next, p_next, reason, shape, utility_h (+3 more)

### Community 333 - "hp_driver.cpp"
Cohesion: 0.30
Nodes (11): best_shape_vote(), clamp01(), ShapeTendency, string, decide_element(), geometry_severity(), is_thin_wall(), shape_awkwardness() (+3 more)

### Community 334 - "GradedTetFillOutput"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 335 - "Checkpoint"
Cohesion: 0.30
Nodes (11): classify_edges(), gp_Vec, map, TopoDS_Face, TopoDS_Shape, uint32_t, dihedral_from_normals(), extract_topology() (+3 more)

### Community 336 - "CantileverSetup"
Cohesion: 0.22
Nodes (10): CantileverSetup, bc, length, loads, mesh, nfree, Index, VectorXd (+2 more)

### Community 337 - "Config"
Cohesion: 0.67
Nodes (3): version, description, type

### Community 338 - "PartCase"
Cohesion: 0.13
Nodes (15): MetricSpec, derivation, name, probe, tol, PartCase, bcs, E (+7 more)

### Community 339 - "element_stiffness"
Cohesion: 0.12
Nodes (27): BcSpec, box, fix, cfg_id_of(), array, json, detect_hmin_features(), expand_grid() (+19 more)

### Community 340 - "test_local_refine.cpp"
Cohesion: 0.17
Nodes (12): 1. Why Geogram BSD-3 (not clean-room clipped Voronoi), 2.1 Vendor from Geogram (BSD-3), 2.2 We write ourselves, 2.3 Dual hard-block, 2. What to vendor vs what we write, 3. Dependency order (do not invent another), 4. Packing context (how this sits in varyhedron), 5. Suggested `third_party/` layout (not full CMake yet) (+4 more)

### Community 341 - "LiveProgress"
Cohesion: 0.11
Nodes (18): size_t, uint32_t, LiveProgress, cfg_id, cg_iter, cg_resid, elapsed_ms, n_elems (+10 more)

### Community 342 - "dorfler_mark"
Cohesion: 0.31
Nodes (8): size_t, vector, Vector3d, dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth(), Vector3d, drive_hp()

### Community 343 - "recover_nodal_stress"
Cohesion: 0.29
Nodes (6): ADR-0021: Varyhedron — variable polyhedral packing mesher, Alternatives rejected, Consequences, Context, Decision, Research anchors

### Community 344 - "GeometrySizing"
Cohesion: 0.25
Nodes (9): dominant_axis(), face_mean_displacement_component(), face_mean_displacement_mag(), global_max_displacement_mag(), size_t, uint32_t, vector, Vector3d (+1 more)

### Community 345 - "LocalRefineStats"
Cohesion: 0.27
Nodes (10): vector, GeometrySizing::GeometrySizing(), make_feature_sizing(), make_geometry_sizing(), uint32_t, SharpEdge, dihedral, v0 (+2 more)

### Community 346 - "MetricSpec"
Cohesion: 0.10
Nodes (21): ProbeAnswers, dominant_load_axis, free_residual_rel, load_area_ok, load_area_rel_err, load_face_area, mean_u_component, mean_ux (+13 more)

### Community 347 - "element_stiffness"
Cohesion: 0.28
Nodes (10): check_prism_fill_geometry(), Vector3d, pick_sweep_axis(), prism_fill_surface(), prism_signed_volume(), prism_signed_volume_impl(), tet_signed_volume_impl(), set (+2 more)

### Community 348 - "make_hp_signals"
Cohesion: 0.48
Nodes (7): at_or_broadcast(), at_or_broadcast_int(), size_t, span, vector, estimate_surplus_from_zz(), make_hp_signals()

### Community 349 - "suggest_refine"
Cohesion: 0.18
Nodes (11): ScorecardInfo, accuracy_rel_err, chordal_efficiency_max, edge_hausdorff_over_h, has_health_ok, health_ok, min_element_quality, n_dof (+3 more)

### Community 350 - "test_spmv.cpp"
Cohesion: 0.33
Nodes (5): ADR-0020: True BRep volume meshing (product path), Alternatives rejected, Consequences, Context, Decision

### Community 351 - "Competitive benchmark harness"
Cohesion: 0.29
Nodes (9): Vector3d, cell_inverted(), array, vector, Vector3d, expand_hex_core_to_pyramids(), hex8_jacobian_det(), tet_signed_vol() (+1 more)

### Community 352 - "SharpEdge"
Cohesion: 0.31
Nodes (8): dist_, blend_to_max(), clamp_size(), DistanceFn, Vector3d, FeatureSizing::FeatureSizing(), FeatureSizing::size_at(), GeometrySizing::size_at()

### Community 353 - "elem_quad"
Cohesion: 0.15
Nodes (13): CadEdgeFeature, CadEdge, dihedral_rad, feature, id, kappa_samples, length, samples (+5 more)

### Community 354 - "FeaError"
Cohesion: 0.12
Nodes (21): element_num_nodes(), FeaError, ElementType, runtime_error, uint32_t, vector, NodalElement, faces (+13 more)

### Community 355 - "test_local_refine.cpp"
Cohesion: 0.47
Nodes (10): array, uint32_t, vector, Vector3d, make_face_key(), summarize_tet4_quality(), tet4_aspect_ratios(), tet4_face_conformity() (+2 more)

### Community 356 - "Spec"
Cohesion: 0.20
Nodes (10): 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3), 2. Dual hard-block (ADR-0024 Q8), 3. `third_party/` plan, 4. Order (do not invent another), ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block), Alternatives rejected, Consequences, Context (+2 more)

### Community 357 - "GroupBoxFrame"
Cohesion: 0.40
Nodes (5): GroupBoxFrame, fixed_content_h, start, title, width

### Community 358 - "elem_quad"
Cohesion: 0.17
Nodes (11): GradedTetFillOutput, h_coarse, h_fine, mesh, n_coarse_cells, n_feature_cells, n_fine_cells, n_seed_cells (+3 more)

### Community 359 - "Feedback loop — campaign → defaults"
Cohesion: 0.40
Nodes (5): Feedback loop — campaign → defaults, Procedure after campaign finishes, Provisional findings (settings-frontier-1, partial), Tooling, When to change product defaults

### Community 360 - "Checkpoint"
Cohesion: 0.36
Nodes (9): closest_on_features(), size_t, span, vector, Vector3d, detect_sharp_edges(), distance_to_features(), point_segment_distance() (+1 more)

### Community 361 - "Holdout geometry audits"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-short-1`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 362 - "schema_version"
Cohesion: 0.07
Nodes (28): 0. Normative ranking (ADR-0023 / plan — do not ignore), 1. Goals (from ADR-0021), 2. Bubble / sphere packing → Delaunay, 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage), 4. Field-aligned hex-dominant (PGP3D-class), 5. CAD edge protecting balls / PLC constraints, 6. Licensing notes (core vs plugin), 7. Decision: v1 algorithm (+20 more)

### Community 363 - "README.md"
Cohesion: 0.31
Nodes (10): assemble_stiffness(), b_matrix(), Dynamic, Matrix, MatrixXd, SparseMatrix, element_coords(), element_stiffness() (+2 more)

### Community 364 - "VaryhedronFillOutput"
Cohesion: 0.08
Nodes (24): size_t, VaryhedronFillOutput, edge_chordal_efficiency_max, edge_hausdorff_over_h, edge_profile_hausdorff_max, edge_profile_rel, h_coarse, h_fine (+16 more)

### Community 365 - "pick_region"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-smoke`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 366 - "write_grok_handoff.py"
Cohesion: 0.44
Nodes (8): collect_shots(), git_head(), load_results(), main(), open_program_nodes(), Path, resolve_campaign(), trend_table()

### Community 367 - "Recommended approach (not all alternatives)"
Cohesion: 0.20
Nodes (9): array, uint32_t, vector, Vector3d, TetFillOutput, boundary_quads, h, nodes (+1 more)

### Community 368 - "p2_strain_basis"
Cohesion: 0.22
Nodes (8): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets, n_surface_mids

### Community 369 - "TetFillOutput"
Cohesion: 0.57
Nodes (6): span, vector, Vector3d, marked_centroids(), suggest_refine(), suggest_uniform_refine()

### Community 370 - "solve_l"
Cohesion: 0.11
Nodes (14): Stress, array, int64_t, Matrix3d, Vector3d, kirsch_stress(), param_key(), array (+6 more)

### Community 371 - "MatrixXd"
Cohesion: 0.67
Nodes (3): solver, description, type

### Community 372 - "cad_topology.cpp"
Cohesion: 0.28
Nodes (8): VectorXd, LSolve, energy, mesh, peak_vm_at_corner, u, make_l_hex_mesh(), solve_l()

### Community 373 - "OctaFillOutput"
Cohesion: 0.67
Nodes (3): main(), Path, resolve_campaign()

### Community 375 - "compile"
Cohesion: 0.22
Nodes (9): GeomError, runtime_error, CadModel::compute_bbox(), CadModel::load_brep(), CadModel::load_step(), path, Vector3d, fill_bbox() (+1 more)

### Community 376 - "HexFace"
Cohesion: 0.13
Nodes (15): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+7 more)

### Community 377 - "2. Bubble / sphere packing → Delaunay"
Cohesion: 0.18
Nodes (12): QuadKey, array, EdgeKey, size_t, uint32_t, edge_key(), EdgeKeyHash, elem_edge() (+4 more)

### Community 378 - "3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage)"
Cohesion: 0.29
Nodes (8): ChordalEdgeMetrics, hausdorff, hausdorff_over_h, max_chordal, max_efficiency, n_segments, chordal_edge_metrics(), chordal_edge_metrics_segments()

### Community 379 - "4. Field-aligned hex-dominant (PGP3D-class)"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-baseline-m9`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 380 - "PROGRESS"
Cohesion: 0.29
Nodes (6): size_t, OctaFillOutput, h, mesh, n_boundary_pyramids, n_octahedra

### Community 381 - "Polyhedral-FEA — agent notes"
Cohesion: 0.67
Nodes (3): Active program (do not skip), graphify, Polyhedral-FEA — agent notes

### Community 383 - "PROGRESS"
Cohesion: 0.33
Nodes (6): Active (read this first), Background / older phases, Benchmark table, Done, Open issues, PROGRESS

### Community 388 - "size_t"
Cohesion: 0.22
Nodes (8): size_t, WallProjectStats, max_surface_residual, mean_surface_residual, n_iters, n_moved, n_reverted, n_wall_nodes

### Community 389 - "uint32_t"
Cohesion: 0.22
Nodes (9): 1. Score vs dashboard vs gate, 2. Minimum scorecard (five numbers + residual gate), 3. Case-specific accuracy scores, 4. Chordal efficiency \(e\), 5. Gates and kills (not scores), 6. Displacement probes, 7. Agent checklist before claiming a campaign “win”, Campaign metrics — normative definitions for agents (+1 more)

### Community 390 - "elem_edge"
Cohesion: 0.25
Nodes (8): HealthInfo, free_residual_rel, has_load_area_ok, load_area_ok, n_orphans, ok, present, reaction_sum_err

### Community 391 - "MeshPreview"
Cohesion: 0.20
Nodes (10): 1. What is complete (do not redo), 2. Advisor order (frozen — do not reorder), 3. Next workstream (G1 first), 4. Deferred / side work (not ahead of G1), 5. GUI readiness (current bar), 6. Verify before starting G1, 7. Open PROGRAM nodes after this handoff, 8. Anti-confusion checklist (copy from plan) (+2 more)

### Community 392 - "Checkpoint"
Cohesion: 0.36
Nodes (7): ElementType, Expectation, name, order, type, make_mesh(), solve_mms_error()

### Community 393 - "WallProjectStats"
Cohesion: 0.36
Nodes (10): closest_edge(), closest_edge_impl(), closest_on_segment(), optional, vector, Vector3d, dist2(), edge_passes_filter() (+2 more)

### Community 394 - "HandoffInfo"
Cohesion: 0.29
Nodes (7): HandoffInfo, campaign, checkpoint_state, finished_utc, git_head, mode, open_program_nodes

### Community 395 - "Protecting balls + local feature size (LFS)"
Cohesion: 0.33
Nodes (6): 1. Role, 2. CDS radius formula (must-change), 3. Reference, 4. Risk cases, 5. Agent checklist, Protecting balls + local feature size (LFS)

### Community 396 - "p2_strain_basis"
Cohesion: 0.40
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 397 - "MatrixXd"
Cohesion: 0.50
Nodes (4): elem_tri(), tri_key(), TriKeyHash, TriKey

### Community 398 - "mean_lateral_radial_residual"
Cohesion: 0.33
Nodes (4): FreeFaceKey, size_t, EdgeHash, FreeFaceHash

### Community 399 - "BRep face-tag BCs / probes (design stub)"
Cohesion: 0.33
Nodes (6): BRep face-tag BCs / probes (design stub), Exit criteria (future work item), Measured icecream instability (why face tags), Out of scope for this stub, Target model (sketch), Why boxes are temporary

### Community 400 - "ClosestOnFeature"
Cohesion: 0.33
Nodes (5): load_brep, load_step, load_cad(), path, load_step()

### Community 401 - "Geogram (vendored subset) — placeholder"
Cohesion: 0.67
Nodes (3): Geogram (vendored subset) — placeholder, Normative docs, When G1 lands

### Community 402 - "pick_region"
Cohesion: 0.40
Nodes (4): Case, path, volume, unit_box()

### Community 403 - "accuracy"
Cohesion: 0.40
Nodes (4): ProjectResult, distance, normal, point

### Community 404 - "solve"
Cohesion: 0.40
Nodes (5): CadModel::Impl, shape, Soup, TopoDS_Shape, triangulate_shape()

### Community 405 - "timestamp"
Cohesion: 0.50
Nodes (4): schema_version, const, description, type

### Community 406 - "FaceOrient"
Cohesion: 0.40
Nodes (5): FaceOrient, sign0, sign1, swap, hex_face_orient()

### Community 407 - "case_id"
Cohesion: 0.50
Nodes (4): wall_time_s, additionalProperties, required, type

### Community 408 - "test_vtu.cpp"
Cohesion: 0.25
Nodes (6): uint32_t, constant_strain_max_error(), Matrix3d, sample_strain_gradient(), unit_box_surface(), patch_max_error()

### Community 410 - "ClosestOnFeature"
Cohesion: 0.40
Nodes (4): ClosestOnFeature, distance, point, Vector3d

### Community 412 - "notes"
Cohesion: 0.67
Nodes (3): description, type, notes

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **1567 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+1562 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **171 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `NodalMesh` connect `P-Elevate Elements` to `VEM & Nodal Mesh`, `Element Assembly Core`, `Hybrid Graded Fill`, `SpMV CSR Backend`, `CLI Mesh Solve`, `Checkpoint`, `p2_strain_basis`, `Surface Traction`, `Structured Mesh Tests`, `FEA Solve Methods`, `Scene Solve Result`, `test_vtu.cpp`, `D6 Tier3 Bench`, `8. Graphify (for agents)`, `Implementation notes (for coding agents)`, `Lame Cylinder Tests`, `ZZ Recovery API`, `Kirsch Plate Tests`, `CantileverSetup`, `element_stiffness`, `Tet Fill Tests`, `FeaError`, `README.md`, `cad_topology.cpp`?**
  _High betweenness centrality (0.044) - this node is a cross-community bridge._
- **Why does `TriSurface` connect `TriSurface Geometry` to `Element Assembly Core`, `sizing_field.cpp`, `SpMV CSR Backend`, `prism_fill_surface`, `Gmsh MSH Import`, `ClosestOnFeature`, `pick_region`, `Mixed Cell Fill`, `GUI Widgets`, `test_vtu.cpp`, `Sizing Field Blend`, `STL Geometry IO`, `Local Refine Tests`, `test_product_mesh_tier1.cpp`, `Lame Cylinder Tests`, `Backend`, `GL Shader Bind`, `Tet Fill Tests`, `LocalRefineStats`, `GroupBox Frame UI`, `element_stiffness`, `Competitive benchmark harness`, `Checkpoint`, `compile`, `Context`?**
  _High betweenness centrality (0.037) - this node is a cross-community bridge._
- **Why does `App` connect `GUI App State` to `VolumeMesher`, `GUI Viewport GL`, `Sim Setup Loads`, `Surface Projection`, `Scene Solve Result`, `Solve Job Pipeline`, `test_product_mesh_tier1.cpp`?**
  _High betweenness centrality (0.029) - this node is a cross-community bridge._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _1623 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Grid Classification` be split into smaller, more focused modules?**
  _Cohesion score 0.11517165005537099 - nodes in this community are weakly interconnected._
- **Should `Element Assembly Core` be split into smaller, more focused modules?**
  _Cohesion score 0.05079825834542816 - nodes in this community are weakly interconnected._