# Graph Report - Polyhedral-FEA  (2026-08-08)

## Corpus Check
- 450 files · ~838,026 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 6057 nodes · 12556 edges · 464 communities (276 shown, 188 thin omitted)
- Extraction: 95% EXTRACTED · 5% INFERRED · 0% AMBIGUOUS · INFERRED: 591 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `03e4882e`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- vem.cpp
- analyze_campaign.py
- CurvedMeshMetrics
- Viewport
- fea library
- ManufacturedSolution
- Palette
- render_scoreboard.py
- render_showcase.py
- vec2
- PolyMesh
- App
- TetFillOutput
- msh.cpp
- Delaunay_psm.cpp
- NodalMesh
- TestLabState
- Advisor measure-first program (canonical agent plan)
- AdaptSuggestion
- MixedFillOutput
- solve_elastostatics
- JobProgress
- snap_boundary_nodes
- ProgressHeartbeat
- SolveJob
- d6_tier3.cpp
- string
- Predicates_psm.cpp
- TransitionFillOutput
- polymesh CMake Project
- gen_part_library.py
- graded_tet_fill_surface
- CadModel
- TriSurface
- SimSetup
- solve_hp
- compute_probes
- Backend
- SizingField
- Hand-calculated reference truths
- accuracy
- vecng<3, T>
- Sign
- null
- NodalElement
- CalculiX peer runner
- POLYMESH_WITH_CUDA
- eval_shape
- Plan: Mesher / Solver Accuracy + Performance Overhaul
- gen_cad_parts.py
- timestamp
- Goodier Spherical Cavity Case
- ADR-0001 Geometry kernel
- Pareto analysis — `varyhedron-baseline-m9`
- D6 Tier-3 L-domain instrument
- fea::Element unified trait
- prism_fill_surface
- cvt_export.cpp
- indicators.cpp
- Delaunay_psm.h
- CI Grep-Audit Anti-Cheat Job
- mixed_fill_surface
- loop.md
- BrepFaceIndex
- run_packing_microbench.py
- hierarchical.cpp
- run_tier3.py
- testlab_data.cpp
- Camera
- schema.json
- fea-madness Idea Harvest Source
- KindCounts
- Pareto analysis — `varyhedron-short-1`
- vtu_wire_png.py
- ADR-0003 Element formulations
- ADR-0014 Dörfler seed remesh
- PeriodicDelaunay3dThread
- hp_assembly.cpp
- assemble_body_load
- ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)
- .end
- ADR-0004 Mesh data structure
- plot_benchmarks.py
- lame-cylinder case
- Material
- widgets.cpp
- interior_points
- recover_nodal_stress
- wall_tangential_project
- scene.cpp
- dofs
- gate1_rows
- ResultRow
- Tier 3 Performance Benchmarks
- vector
- T
- vector
- extract_tet4
- run_mesh_public.sh
- run_solve_public.sh
- GradedSizing
- run_polymesh_smoke.sh
- P1 MMS Convergence Orders (tet4/hex8/tet10/hex20)
- POLYMESH_WITH_OCC
- POLYMESH_WITH_OPENMP
- Patch Test Is Sacred
- ADR-0002 BSD-3-Clause License
- Layer Dependency Direction Rule
- VtuPointData
- index_t
- ROADMAP — Get PolyMesh off the ground
- cad_model.cpp
- bench_harness library
- User-Paintable Region Override (GUI)
- SiteGrid
- PolyMesh Showcase
- evaluate_curved_mesh_quality
- GeomError
- backend_cuda.cu
- gui/main.cpp
- cell_validity.hpp
- local_refine_tets
- SnapStats
- path
- project_point_on_surface
- build_grid
- CartesianGrid
- advisor-measure-first-program.md
- GeometrySizing
- ReferenceCase
- ElementTypeCounts
- main
- ImVec2
- M9 frozen baseline — `varyhedron-baseline-m9`
- .size
- Pareto analysis — `settings-frontier-1`
- Config
- MetricSpec
- required
- png_writer.hpp
- Checkpoint
- .length
- ProjProf
- HandoffInfo
- MeshPreview
- CellQualityStats
- .closest
- Test-lab interfaces (normative)
- ADR-0022: Full experiment warehouse + headless Grok improvement loop
- KirschRun
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
- CampaignSpec
- ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)
- SurfaceFace
- vec3
- The adaptive solver core, explained
- Pareto analysis — `smoke`
- test_hierarchical.cpp
- main
- solve_mms_error
- Decision
- Grok improvement loop
- GuiSettings
- CampaignSummary
- Delaunay3dThread
- ClipBox
- CadEdge
- HpSystem
- ADR-0024: Advisor measure-first answers (normative Q&A)
- main
- ConstrainedSiteSeedResult
- HpDriverPolicy
- T
- HpDriverPlan
- ElementHpSignal
- Pareto analysis — `varyhedron-smoke`
- Campaign
- ClippedVoronoiExportStats
- ElementHpDecision
- hp_driver.cpp
- test_spmv.cpp
- CadTopology
- CantileverSetup
- PeriodicVertexArray3d
- run_one
- testlab/main.cpp
- Geogram / restricted CVT — vendoring study path
- LiveProgress
- dorfler_mark
- ADR-0021: Varyhedron — variable polyhedral packing mesher
- AnswersInfo
- nodal_mesh.hpp
- ProbeAnswers
- .resize
- make_hp_signals
- ScorecardInfo
- ADR-0020: True BRep volume meshing (product path)
- index_t
- ConstrainedLloydParams
- vec4
- FeaError
- Logger
- ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)
- GroupBoxFrame
- param_key
- Feedback loop — campaign → defaults
- FeatureGradedSizing
- Grok improvement handoff — `varyhedron-short-1`
- Varyhedron packing — algorithm survey (V5)
- set_arg
- VaryhedronFillOutput
- Grok improvement handoff — `varyhedron-smoke`
- write_grok_handoff.py
- string
- CvtLloydStats
- viewport.cpp
- structured_mesh.hpp
- AccuracyInfo
- .vertex_ptr
- resolve_campaign
- check_no_product_stl.sh
- intervalBase
- test_fe_vem_assembly.cpp
- LexicoCompare
- stamp_feature_cells
- Grok improvement handoff — `varyhedron-baseline-m9`
- probe_util.hpp
- Polyhedral-FEA — agent notes
- .thread
- PROGRESS
- invoke_grok_improve.sh
- SolveJob::start
- Campaign metrics — normative definitions for agents
- HealthInfo
- Triangle
- Predicates_psm.h
- CDT2d_ConstraintWalker
- manifest.json
- Protecting balls + local feature size (LFS)
- SolveResult
- pointer_
- Matrix
- BRep face-tag BCs / probes (design stub)
- function
- ProjectResult
- Session handoff — M5 accuracy push (PC restart checkpoint)
- declare_arg
- case_id
- FaceKey
- VolumeMeshOutput
- ElementTendencyPlan
- expansion
- Matrix
- expansion
- host
- build.sh
- post-m10-smoke/README.md
- .stellate_cavity
- testlab_selfcheck.cpp
- IndexType
- cli/main.cpp
- percent
- WindowsThreadPoolManager
- ClippedCell
- varyhedron_fill_surface
- PointAlignment<3>
- Environment::find_environment
- HexFillOutput
- FaceConformityStats
- Model
- PointAlignment<4>
- properties
- Spec
- CommandLineDesc
- initialize
- index_t
- compile
- Box
- SharpEdge
- DomainTet
- GeoFaceKey
- wall_time_s
- settings-frontier-1 — campaign-1 close-out
- Geogram subset — what PolyMesh takes
- M5 VEM gate — campaign results (2026-07-13)
- LocalRefineStats
- A1
- A2
- cell_status_t
- CMP
- const_pointer
- const_reference
- FPTR
- matrix_type
- reference
- size_type
- std::vector<bool>
- std::vector<T, Memory::aligned_allocator<T> >
- T1
- test_stl.cpp
- test_d6_bench_smoke.cpp
- mean_lateral_radial_residual
- self_improve.sh
- PointAlignment<2>

## God Nodes (most connected - your core abstractions)
1. `PeriodicDelaunay3dThread` - 125 edges
2. `Delaunay3dThread` - 109 edges
3. `NodalMesh` - 101 edges
4. `TriSurface` - 76 edges
5. `Logger` - 69 edges
6. `Viewport` - 65 edges
7. `vec3` - 59 edges
8. `App` - 56 edges
9. `geo_argused()` - 56 edges
10. `SolveJob` - 49 edges

## Surprising Connections (you probably didn't know these)
- `assemble_traction_load()` --calls--> `traction`  [INFERRED]
  src/fea/src/traction.cpp → apps/testlab/main.cpp
- `vem_body_load()` --calls--> `body`  [INFERRED]
  src/fea/src/vem.cpp → tests/support/mms.hpp
- `sizing_field` --semantically_similar_to--> `resolve_mesh_size`  [INFERRED] [semantically similar]
  src/adapt/CMakeLists.txt → examples/README.md
- `merge_unique()` --references--> `NodalMesh`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp
- `make_l_hex_mesh()` --calls--> `add_node()`  [INFERRED]
  tests/test_l_domain.cpp → apps/bench/d6_tier3.cpp

## Import Cycles
- None detected.

## Communities (464 total, 188 thin omitted)

### Community 0 - "vem.cpp"
Cohesion: 0.14
Nodes (52): Fun, kP2Mono, kP2Vec, b_from_grads(), array, Dynamic, function, Matrix (+44 more)

### Community 1 - "analyze_campaign.py"
Cohesion: 0.12
Nodes (37): accuracy_of(), aggregate_configs(), analyze_one(), CfgAgg, config_label(), factor_breakdown(), _fmt_ms(), _fmt_pct() (+29 more)

### Community 2 - "CurvedMeshMetrics"
Cohesion: 0.06
Nodes (34): CurvedMeshMetrics, composite_score, has_circular, has_tet_aspect, has_volume, m1_max, m1_mean, m2_max (+26 more)

### Community 3 - "Viewport"
Cohesion: 0.04
Nodes (48): array, DisplayMode, uint32_t, vector, Vector3d, VectorXd, Viewport, background_program_ (+40 more)

### Community 4 - "fea library"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "ManufacturedSolution"
Cohesion: 0.15
Nodes (22): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+14 more)

### Community 6 - "Palette"
Cohesion: 0.05
Nodes (41): apply_theme(), ThemeId, ImVec4, make_interwebz_palette(), make_slate_palette(), make_studio_palette(), Palette, accent (+33 more)

### Community 7 - "render_scoreboard.py"
Cohesion: 0.31
Nodes (9): fmt_num(), load_results(), main(), Any, Path, Markdown-friendly ASCII sparkline; skips None., Minimal inline SVG polyline for accuracy trend (lower often better)., sparkline() (+1 more)

### Community 8 - "render_showcase.py"
Cohesion: 0.07
Nodes (60): ndarray, architecture_dot(), build_mesh_tiles(), cli_path(), color_range(), compose(), draw_colorbar(), draw_sci() (+52 more)

### Community 9 - "vec2"
Cohesion: 0.12
Nodes (20): mat2, mat3, angle(), approximate(), CDT2d::create_enclosing_quad(), CDT2d::create_enclosing_triangle(), CDT2d::create_intersection(), cos_angle() (+12 more)

### Community 10 - "PolyMesh"
Cohesion: 0.08
Nodes (27): CellId, CellKind, FaceId, poly_mesh_to_vem(), Cell, faces, kind, Face (+19 more)

### Community 11 - "App"
Cohesion: 0.05
Nodes (40): App, custom_font, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, improve_running (+32 more)

### Community 12 - "TetFillOutput"
Cohesion: 0.07
Nodes (26): GradedTetFillOutput, h_coarse, h_fine, mesh, n_coarse_cells, n_feature_cells, n_fine_cells, n_seed_cells (+18 more)

### Community 13 - "msh.cpp"
Cohesion: 0.13
Nodes (26): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+18 more)

### Community 14 - "Delaunay_psm.cpp"
Cohesion: 0.01
Nodes (89): EMSCRIPTEN_KEEPALIVE, InvalidInput, siginfo_t, abnormal_program_termination(), AdaptiveKdTree::create_kd_tree_recursive(), AdaptiveKdTree::new_node(), aligned_3d(), aligned_3d_exact() (+81 more)

### Community 15 - "NodalMesh"
Cohesion: 0.07
Nodes (46): RadialMap, Vector3d, NodalMesh, check_validity, compact_unused_nodes, elements, nodes, size_t (+38 more)

### Community 16 - "TestLabState"
Cohesion: 0.07
Nodes (43): path, size_t, string, fmt_opt_num(), Checkpoint, string, time_point, vector (+35 more)

### Community 17 - "Advisor measure-first program (canonical agent plan)"
Cohesion: 0.08
Nodes (24): 0. One-sentence strategy, 10. Related files, 1. Substrate (keep forever until proven wrong), 2. Claims (product honesty), 3.1 Five-number scorecard + residual gate, 3.2 What to score vs dashboard (stress), 3.3 Chordal efficiency (edge residual), 3.4 Over-budget diagnosis (+16 more)

### Community 18 - "AdaptSuggestion"
Cohesion: 0.18
Nodes (15): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+7 more)

### Community 19 - "MixedFillOutput"
Cohesion: 0.07
Nodes (30): MixedCellKind, array, size_t, uint32_t, uint8_t, vector, Vector3d, MixedCell (+22 more)

### Community 20 - "solve_elastostatics"
Cohesion: 0.11
Nodes (26): Precond, Dirichlet, dof_values, function, Index, map, SolveMethod, SolveOptions (+18 more)

### Community 21 - "JobProgress"
Cohesion: 0.11
Nodes (18): string, JobProgress, cg_iter, cg_resid, elapsed_ms, n_elems, n_nodes, pass (+10 more)

### Community 22 - "snap_boundary_nodes"
Cohesion: 0.13
Nodes (25): CollectOffendersFn, ClosestPoint, distance, point, triangle, Vector3d, closest_on_surface(), closest_on_surface_brute() (+17 more)

### Community 23 - "ProgressHeartbeat"
Cohesion: 0.10
Nodes (19): atomic, mutex, size_t, time_point, ProgressHeartbeat, cfg_id_, cg_iter_, cg_resid_ (+11 more)

### Community 24 - "SolveJob"
Cohesion: 0.07
Nodes (31): State, time_point, uint64_t, load, SolveJob, cancel_, clear_failure, error_ (+23 more)

### Community 25 - "d6_tier3.cpp"
Cohesion: 0.14
Nodes (30): add_node(), array, int64_t, json, map, string, uint32_t, vector (+22 more)

### Community 26 - "string"
Cohesion: 0.03
Nodes (106): E, Node, absolute_path(), base_name(), BooleanExpression::BooleanExpression(), BooleanExpression::parse_factor(), can_read_directory(), can_write_directory() (+98 more)

### Community 27 - "Predicates_psm.cpp"
Cohesion: 0.04
Nodes (99): coord_index_t, Sign, SOSMode, det_3d(), det_3d_exact(), det_3d_filter(), det_4d(), det_4d_filter() (+91 more)

### Community 28 - "TransitionFillOutput"
Cohesion: 0.09
Nodes (23): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+15 more)

### Community 29 - "polymesh CMake Project"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "gen_part_library.py"
Cohesion: 0.24
Nodes (14): _assert_manifold_facets(), _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre. x ∈…, Parse emitted ASCII facet blocks and require edge multiplicity 2. (+6 more)

### Community 31 - "graded_tet_fill_surface"
Cohesion: 0.13
Nodes (27): Vector3d, runtime_error, ValidityError, classify_cells_inside(), array, uint32_t, vector, Vector3d (+19 more)

### Community 32 - "CadModel"
Cohesion: 0.11
Nodes (17): Impl, shared_ptr, CadModel, bbox_diagonal, compute_bbox, has_brep, impl_, load_brep (+9 more)

### Community 33 - "TriSurface"
Cohesion: 0.07
Nodes (31): array, uint32_t, vector, Vector3d, TriSurface, triangles, validate, vertices (+23 more)

### Community 34 - "SimSetup"
Cohesion: 0.12
Nodes (15): SimSetup, adapt_leb_waves, adapt_passes, bc_grading, element_tendency, eta_target, fixtures, mesh_size (+7 more)

### Community 35 - "solve_hp"
Cohesion: 0.22
Nodes (9): loads, Index, map, solve_hp(), FaceQp, u, v, weight (+1 more)

### Community 36 - "compute_probes"
Cohesion: 0.16
Nodes (23): compute_probes(), compute_scorecard_geom(), compute_solve_health(), count_orphan_nodes(), optional, uint32_t, vector, Vector3d (+15 more)

### Community 37 - "Backend"
Cohesion: 0.11
Nodes (24): Backend, CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t (+16 more)

### Community 38 - "SizingField"
Cohesion: 0.09
Nodes (24): FeatureSizing, blend_, dist_, h_max_, h_min_, size_at, DistanceFn, Vector3d (+16 more)

### Community 39 - "Hand-calculated reference truths"
Cohesion: 0.09
Nodes (22): cantilever, cylinder, Engineering estimate: polar compression as a short column, Finite-domain note, Hand-calculated reference truths, How to add a part, icecream-cone, Infinite-plate Kirsch solution (+14 more)

### Community 40 - "accuracy"
Cohesion: 0.15
Nodes (13): additionalProperties, properties, required, type, description, type, accuracy, name (+5 more)

### Community 41 - "vecng<3, T>"
Cohesion: 0.06
Nodes (16): vector_type, vecng<2, T>, dim, x, y, vecng<3, T>, dim, x (+8 more)

### Community 42 - "Sign"
Cohesion: 0.03
Nodes (115): interval_nt, AdaptiveKdTree::AdaptiveKdTree(), AdaptiveKdTree::get_node(), BalancedKdTree::BalancedKdTree(), BalancedKdTree::get_node(), CDTBase2d::locate(), CDTBase2d::Tedge_is_Delaunay(), coord_index_t (+107 more)

### Community 43 - "null"
Cohesion: 0.14
Nodes (18): description, minimum, type, mesh, solve, total, value, description (+10 more)

### Community 44 - "NodalElement"
Cohesion: 0.16
Nodes (23): CornerEdges, ElementType, uint32_t, vector, NodalElement, faces, nodes, type (+15 more)

### Community 45 - "CalculiX peer runner"
Cohesion: 0.33
Nodes (5): CalculiX peer runner, Cases to port next, Common install paths (documentation only), Run (CI-safe), Runner contract

### Community 47 - "eval_shape"
Cohesion: 0.13
Nodes (27): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+19 more)

### Community 48 - "Plan: Mesher / Solver Accuracy + Performance Overhaul"
Cohesion: 0.07
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 49 - "gen_cad_parts.py"
Cohesion: 0.14
Nodes (23): _edge(), _face_triangle(), _is_valid(), main(), make_cylinder(), make_icecream_cone(), make_plate_hole(), make_sphere() (+15 more)

### Community 50 - "timestamp"
Cohesion: 0.50
Nodes (4): timestamp, description, format, type

### Community 53 - "Pareto analysis — `varyhedron-baseline-m9`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 56 - "prism_fill_surface"
Cohesion: 0.17
Nodes (18): array, uint32_t, vector, Vector3d, PrismFillOutput, boundary_max_distance, boundary_quads, h (+10 more)

### Community 57 - "cvt_export.cpp"
Cohesion: 0.16
Nodes (37): ClipPlane, a, b, c, d, bisector_keep_site(), build_cell(), build_cell_tet_nbr() (+29 more)

### Community 58 - "indicators.cpp"
Cohesion: 0.14
Nodes (23): vector, VertexCurvature, kappa, VertexThickness, thickness, build_tri_grid(), array, size_t (+15 more)

### Community 59 - "Delaunay_psm.h"
Cohesion: 0.03
Nodes (57): align(), clear(), copy(), Counted(), FPTR, expansion_nt_dot_at(), expansion_nt_sq_dist(), function_pointer_to_generic_pointer() (+49 more)

### Community 60 - "CI Grep-Audit Anti-Cheat Job"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "mixed_fill_surface"
Cohesion: 0.23
Nodes (22): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, closed_poly_volume(), array, span, uint32_t (+14 more)

### Community 62 - "loop.md"
Cohesion: 0.40
Nodes (4): 1. PLAN, 2. BUILD, 3. VERIFY, 4. LOOP OR STOP

### Community 63 - "BrepFaceIndex"
Cohesion: 0.09
Nodes (21): Handle, BrepFaceIndex, adaptors, bins, boxes, cell, epoch, faces (+13 more)

### Community 64 - "run_packing_microbench.py"
Cohesion: 0.23
Nodes (21): boundary_residual_placeholder(), bubble_relax(), _clamp01(), _dedupe(), _dist2(), fill_fraction_proxy(), main(), pack_case() (+13 more)

### Community 65 - "hierarchical.cpp"
Cohesion: 0.06
Nodes (56): Entity, HpMode, edge_odd, entity, entity_index, index0, index1, index2 (+48 more)

### Community 66 - "run_tier3.py"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "testlab_data.cpp"
Cohesion: 0.30
Nodes (18): checkpoint_state_cstr(), CheckpointState, json, string, is_measure_first_baseline(), json_value_to_string(), opt_bool(), opt_double() (+10 more)

### Community 68 - "Camera"
Cohesion: 0.16
Nodes (17): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+9 more)

### Community 69 - "schema.json"
Cohesion: 0.29
Nodes (6): additionalProperties, description, $id, $schema, title, type

### Community 71 - "KindCounts"
Cohesion: 0.22
Nodes (10): count_kinds(), size_t, KindCounts, hex, other, pyr, tet, vem (+2 more)

### Community 72 - "Pareto analysis — `varyhedron-short-1`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 73 - "vtu_wire_png.py"
Cohesion: 0.29
Nodes (13): bbox_of(), boundary_edges(), cell_faces(), detect_hole_roi(), draw_line(), face_key(), main(), parse_vtu_ascii() (+5 more)

### Community 76 - "PeriodicDelaunay3dThread"
Cohesion: 0.04
Nodes (50): condition_variable, Periodic, SFrame, CERRStream, mutex, spinlock, geo_sqr(), ParallelDelaunay3d::set_vertices() (+42 more)

### Community 77 - "hp_assembly.cpp"
Cohesion: 0.09
Nodes (38): QuadKey, assemble_hp(), array, EdgeKey, ElementType, pair, size_t, uint32_t (+30 more)

### Community 78 - "assemble_body_load"
Cohesion: 0.13
Nodes (24): Vector3d, QuadraturePoint, weight, xi, assemble_body_load(), BodyForce, VectorXd, hp_energy_error() (+16 more)

### Community 79 - "ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - ".end"
Cohesion: 0.09
Nodes (25): SparseBits, BooleanExpression::cur_char(), BooleanExpression::next_char(), Instance, Environment::add_observer(), Environment::notify_local_observers(), Environment::remove_observer(), ExactCDT2d::classify_triangles_union_cnstr_operand_bits_is_operand_id() (+17 more)

### Community 82 - "plot_benchmarks.py"
Cohesion: 0.24
Nodes (20): apply_style(), d6_records(), finish(), footer(), gate1_records(), load_json(), main(), parse_mms_elements() (+12 more)

### Community 84 - "Material"
Cohesion: 0.11
Nodes (11): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+3 more)

### Community 85 - "widgets.cpp"
Cohesion: 0.22
Nodes (20): begin_field(), begin_group_box(), begin_group_box_fill(), button(), checkbox(), ImVec4, draw_accent_fill(), draw_box() (+12 more)

### Community 86 - "interior_points"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "recover_nodal_stress"
Cohesion: 0.13
Nodes (22): p, ElementCentroidStress, centroid, element_index, quality, stress, volume, uint32_t (+14 more)

### Community 88 - "wall_tangential_project"
Cohesion: 0.14
Nodes (18): size_t, WallProjectStats, max_surface_residual, mean_surface_residual, n_iters, n_moved, n_reverted, n_wall_nodes (+10 more)

### Community 89 - "scene.cpp"
Cohesion: 0.12
Nodes (20): build_refinement_plan(), runtime_error, size_t, span, string, vector, Vector3d, VolumeMesher (+12 more)

### Community 90 - "dofs"
Cohesion: 0.40
Nodes (5): description, minimum, type, dofs, integer

### Community 91 - "gate1_rows"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face. Face…, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "ResultRow"
Cohesion: 0.09
Nodes (22): QualityInfo, M1max, M2max, M6, score, ResultRow, accuracy, answers (+14 more)

### Community 94 - "vector"
Cohesion: 0.09
Nodes (8): set, array, string, vector, array, vector, optional, map

### Community 95 - "T"
Cohesion: 0.05
Nodes (54): A, B, A1, DIM, T1, T2, det4x4(), expansion_nt (+46 more)

### Community 96 - "vector"
Cohesion: 0.04
Nodes (64): COORD, IT, MESH, MeshElementsFlags, MeshOrder, Base_ccmp, Base_fcmp, Base_tcmp (+56 more)

### Community 97 - "extract_tet4"
Cohesion: 0.39
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 100 - "GradedSizing"
Cohesion: 0.07
Nodes (30): int32_t, GradedSizing, build_grid, cell_start_, grid_cell_, grid_nx_, grid_ny_, grid_nz_ (+22 more)

### Community 119 - "VtuPointData"
Cohesion: 0.15
Nodes (19): string, vector, VectorXd, VtuCellData, name, scalars, VtuPointData, name (+11 more)

### Community 120 - "index_t"
Cohesion: 0.16
Nodes (11): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, geo_pause(), atomic, index_t (+3 more)

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "cad_model.cpp"
Cohesion: 0.15
Nodes (14): BRepAdaptor_Surface, BRepExtrema_DistShapeShape, CadModel::compute_bbox(), CadModel::Impl, shape, CadModel::load_brep(), CadModel::load_step(), path (+6 more)

### Community 125 - "SiteGrid"
Cohesion: 0.11
Nodes (15): uint32_t, vector, Vector3d, SiteGrid, build, cell_, cell_start_, inv_cell_ (+7 more)

### Community 126 - "PolyMesh Showcase"
Cohesion: 0.11
Nodes (18): Architecture diagram, `bench_dof_time.png`, `bench_mms.png`, `bench_tier1.png`, Benchmark charts, `gallery_cantilever.png`, `gallery_cylinder.png`, `gallery_icecream_cone.png` (+10 more)

### Community 127 - "evaluate_curved_mesh_quality"
Cohesion: 0.19
Nodes (16): FreeFace, CircularFeature, axis_dir, axis_point, radius, select_band, Vector3d, clamp01() (+8 more)

### Community 128 - "GeomError"
Cohesion: 0.22
Nodes (15): GeomError, runtime_error, byte, path, size_t, Soup, span, T (+7 more)

### Community 129 - "backend_cuda.cu"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "gui/main.cpp"
Cohesion: 0.26
Nodes (15): capture_screenshot(), string, draw_study_panel(), drop_callback(), hexagon_sdf(), is_geometry_path(), load_model(), load_ui_font() (+7 more)

### Community 131 - "cell_validity.hpp"
Cohesion: 0.33
Nodes (16): hex8_jacobian_det(), hex8_min_jacobian(), hex8_shape_quality(), array, Vector3d, max_edge(), prism_min_corner_jacobian(), prism_shape_quality() (+8 more)

### Community 132 - "local_refine_tets"
Cohesion: 0.26
Nodes (19): FreeFaceKey, bisect_tet(), array, EdgeKey, size_t, span, uint32_t, vector (+11 more)

### Community 133 - "SnapStats"
Cohesion: 0.14
Nodes (14): ConformityStats, count, max_distance, mean_distance, size_t, SmoothStats, max_residual, n_moved (+6 more)

### Community 134 - "path"
Cohesion: 0.31
Nodes (13): count_result_lines(), Checkpoint, optional, path, vector, load_campaign(), load_checkpoint(), load_handoff() (+5 more)

### Community 135 - "project_point_on_surface"
Cohesion: 0.33
Nodes (11): duration, gp_Pnt, gp_Vec, optional, size_t, TopoDS_Face, face_normal_at(), project_on_face_brute() (+3 more)

### Community 136 - "build_grid"
Cohesion: 0.26
Nodes (11): build_grid(), load_font(), main(), FreeTypeFont, Image, ImageDraw, Path, Resolve the first available TTF from ``names`` across the font dirs. (+3 more)

### Community 137 - "CartesianGrid"
Cohesion: 0.24
Nodes (18): CartesianGrid, cell, nx, ny, nz, origin, cells_for_extent(), classify_cells_inside_axis() (+10 more)

### Community 139 - "GeometrySizing"
Cohesion: 0.17
Nodes (12): GeometrySizing, blend_, curv_frac_, edges_, h_max_, h_min_, kappa_, size_at (+4 more)

### Community 140 - "ReferenceCase"
Cohesion: 0.23
Nodes (11): BenchError, runtime_error, string, ReferenceCase, citation, name, values, path (+3 more)

### Community 141 - "ElementTypeCounts"
Cohesion: 0.22
Nodes (8): ElementTypeCounts, hex20, hex8, other, tet10, tet4, size_t, count_element_types()

### Community 142 - "main"
Cohesion: 0.53
Nodes (5): ccx_version(), main(), Path, Write coarse C3D8 cantilever deck. Returns (nnodes, n_fixed_nodes)., write_inp()

### Community 143 - "ImVec2"
Cohesion: 0.29
Nodes (11): draw_colorbar(), draw_column_splitter(), draw_frame(), draw_viewport_content(), CheckpointState, ImVec4, draw_results_panel(), draw_testlab_panel() (+3 more)

### Community 144 - "M9 frozen baseline — `varyhedron-baseline-m9`"
Cohesion: 0.17
Nodes (12): Campaign matrix, Case primary accuracy metrics (as wired at freeze), Freeze identity, Known issues frozen-in (not blockers for freeze), M9 frozen baseline — `varyhedron-baseline-m9`, Metric schema version, Outcome summary, Per-run snapshot (+4 more)

### Community 145 - ".size"
Cohesion: 0.06
Nodes (27): Attribute, DWORD, ExactPoint, LPVOID, ostream, barycenter(), CDT2d::incircle(), CDT2d::orient2d() (+19 more)

### Community 146 - "Pareto analysis — `settings-frontier-1`"
Cohesion: 0.12
Nodes (16): `cantilever`, Config ranking (weighted mean score), `curved`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 147 - "Config"
Cohesion: 0.18
Nodes (11): Config, bc_grading, curvature_turn_deg, element_tendency, feature_refine, id, mesher, order (+3 more)

### Community 148 - "MetricSpec"
Cohesion: 0.20
Nodes (10): evaluate_probe(), MetricSpec, derivation, name, probe, tol, ProbeSpec, kind (+2 more)

### Community 149 - "required"
Cohesion: 0.22
Nodes (9): required, accuracy, case_id, dofs, label, solver, timestamp, version (+1 more)

### Community 150 - "png_writer.hpp"
Cohesion: 0.57
Nodes (7): adler32_of(), crc32_of(), size_t, uint32_t, put_chunk(), put_u32be(), write_png_rgba()

### Community 151 - "Checkpoint"
Cohesion: 0.25
Nodes (8): Checkpoint, campaign, completed_runs, started_utc, state, survivors, tier, updated_utc

### Community 152 - ".length"
Cohesion: 0.32
Nodes (7): GEO_NODISCARD, std::string remove_prefix(), std::string remove_suffix(), std::string trim_spaces(), string_ends_with(), string_starts_with(), vecng<DIM, T> normalize()

### Community 153 - "ProjProf"
Cohesion: 0.25
Nodes (7): ProjProf, calls, extrema, normals, t_extrema, t_normal, t_total

### Community 154 - "HandoffInfo"
Cohesion: 0.29
Nodes (7): HandoffInfo, campaign, checkpoint_state, finished_utc, git_head, mode, open_program_nodes

### Community 155 - "MeshPreview"
Cohesion: 0.29
Nodes (7): size_t, uint32_t, MeshPreview, n_elems, nodes, note, quads

### Community 156 - "CellQualityStats"
Cohesion: 0.29
Nodes (6): CellQualityStats, mean, min, n_measured, n_unmeasured, size_t

### Community 157 - ".closest"
Cohesion: 0.38
Nodes (6): box_lower_bound(), Vector3d, FaceBox, hi, lo, Winner

### Community 158 - "Test-lab interfaces (normative)"
Cohesion: 0.10
Nodes (20): 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp` (+12 more)

### Community 159 - "ADR-0022: Full experiment warehouse + headless Grok improvement loop"
Cohesion: 0.12
Nodes (14): Campaign warehouse, Directory layout, git-LFS, Short-campaign defaults (Lane V), Wireframe PNGs (`wire.png`), ADR-0022: Full experiment warehouse + headless Grok improvement loop, Alternatives rejected, Consequences (+6 more)

### Community 160 - "KirschRun"
Cohesion: 0.29
Nodes (7): size_t, KirschRun, free_dofs, n_elements, n_nodes, scf, scf_rel_err

### Community 305 - "CampaignSpec"
Cohesion: 0.11
Nodes (19): CampaignResources, max_mem_gb, max_threads, CampaignScoreWeights, accuracy, mesh_ms, solve_ms, CampaignSpec (+11 more)

### Community 306 - "ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "SurfaceFace"
Cohesion: 0.10
Nodes (40): Sink, ConsistentLoad, area, conservation_error, resultant, face_num_nodes(), FaceType, uint32_t (+32 more)

### Community 308 - "vec3"
Cohesion: 0.07
Nodes (49): COORD_T, ConvexCell::barycenter(), ConvexCell::compute_mg(), ConvexCell::compute_triangle_point(), ConvexCell::squared_inner_radius(), ConvexCell::squared_radius(), ConvexCell::volume(), POINT (+41 more)

### Community 309 - "The adaptive solver core, explained"
Cohesion: 0.14
Nodes (11): Showcase asset index, 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, Decision policy (v1, `adapt::drive_hp`), The adaptive solver core, explained (+3 more)

### Community 310 - "Pareto analysis — `smoke`"
Cohesion: 0.14
Nodes (13): Config ranking (weighted mean score), Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run, Pareto analysis — `smoke`, Pareto by geometric class (+5 more)

### Community 311 - "test_hierarchical.cpp"
Cohesion: 0.32
Nodes (6): count_zero_modes(), Dynamic, Matrix, MatrixXd, unit_hex_coords(), unit_tet_coords()

### Community 312 - "main"
Cohesion: 0.83
Nodes (3): main(), Path, run_one()

### Community 313 - "solve_mms_error"
Cohesion: 0.33
Nodes (6): ElementType, Expectation, name, order, type, solve_mms_error()

### Community 314 - "Decision"
Cohesion: 0.14
Nodes (14): 1. Substrate (keep, do not replace), 2. Element technology claims, 3. Packing evolution, 4. CAD edge classification (normative), 5. Measurement order (two-week horizon), 6. License landscape (core vs plugin), 7. Sizing field, 8. p-order (+6 more)

### Community 315 - "Grok improvement loop"
Cohesion: 0.25
Nodes (7): Autonomous vs supervised answers, Grok improvement loop, Handoff contents, Headless invoke (default), Manual interactive (optional), Safety, When it runs

### Community 316 - "GuiSettings"
Cohesion: 0.08
Nodes (29): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+21 more)

### Community 317 - "CampaignSummary"
Cohesion: 0.10
Nodes (22): CampaignSummary, dir, has_campaign_json, has_checkpoint, has_results, name, result_count, state (+14 more)

### Community 318 - "Delaunay3dThread"
Cohesion: 0.03
Nodes (44): Delaunay3d::find_conflict_zone_iterative(), Delaunay3d::show_list(), Delaunay3d::show_tet(), Delaunay3d::show_tet_adjacent(), Delaunay3dThread, b_hint_, cavity_, cond_ (+36 more)

### Community 319 - "ClipBox"
Cohesion: 0.14
Nodes (32): CvtSite, fixed, pos, density_from_size(), ClipBox, max, min, bisector_keep_site() (+24 more)

### Community 320 - "CadEdge"
Cohesion: 0.07
Nodes (31): CadEdgeFeature, CadSurfaceKind, CadEdge, dihedral_rad, feature, id, kappa_samples, length (+23 more)

### Community 321 - "HpSystem"
Cohesion: 0.07
Nodes (36): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+28 more)

### Community 322 - "ADR-0024: Advisor measure-first answers (normative Q&A)"
Cohesion: 0.17
Nodes (12): ADR-0024: Advisor measure-first answers (normative Q&A), Compressed path (do not invent another), Q10 — High-dimensional traps, Q1 — 1e20 von Mises with 1e-13 residual, Q2 — Next 3–5 days order, Q3 — Geogram, Q4 — Chordal efficiency e ~ 100 at h_scale=5, Q5 — Cylinder truth (+4 more)

### Community 323 - "main"
Cohesion: 0.53
Nodes (5): main(), parse_mesh_stdout(), Path, Best-effort wireframe via pure-Python exterior edges, then meshio., try_render_png()

### Community 324 - "ConstrainedSiteSeedResult"
Cohesion: 0.10
Nodes (21): ConstrainedLloydResult, lloyd_stats, project_stats, seed_stats, sites, ConstrainedSiteSeedResult, n_interior_free, n_seams_skipped (+13 more)

### Community 325 - "HpDriverPolicy"
Cohesion: 0.12
Nodes (16): HpDriverPolicy, cost_h, cost_p, cost_shape, dorfler_theta, eta_rel_floor, geometry_force_h, h_min (+8 more)

### Community 326 - "T"
Cohesion: 0.05
Nodes (42): A1, A2, DIM, DIM2, index_t, initializer_list, std::vector<T, Memory::aligned_allocator<T> >, T (+34 more)

### Community 327 - "HpDriverPlan"
Cohesion: 0.14
Nodes (14): HpDriverPlan, decisions, global_shape, h_mark, h_suggestion, n_h, n_none, n_p (+6 more)

### Community 328 - "ElementHpSignal"
Cohesion: 0.18
Nodes (11): ElementHpSignal, eta, h, hex_fit, kappa, p_max, poly_fit, surplus (+3 more)

### Community 329 - "Pareto analysis — `varyhedron-smoke`"
Cohesion: 0.13
Nodes (14): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+6 more)

### Community 330 - "Campaign"
Cohesion: 0.12
Nodes (16): Campaign, grid, max_pack_wall_s, max_run_wall_s, name, on_finish_analyze, on_finish_grok, parts (+8 more)

### Community 331 - "ClippedVoronoiExportStats"
Cohesion: 0.12
Nodes (18): ClippedVoronoiExport, mesh, site_to_cell, stats, ClippedVoronoiExportStats, domain_clip_used, geogram_ok, n_boundary_faces (+10 more)

### Community 332 - "ElementHpDecision"
Cohesion: 0.18
Nodes (11): HpAction, ElementHpDecision, action, h_next, p_next, reason, shape, utility_h (+3 more)

### Community 333 - "hp_driver.cpp"
Cohesion: 0.30
Nodes (11): best_shape_vote(), clamp01(), ShapeTendency, string, decide_element(), geometry_severity(), is_thin_wall(), shape_awkwardness() (+3 more)

### Community 334 - "test_spmv.cpp"
Cohesion: 0.33
Nodes (6): SparseMatrix, vector, VectorXd, make_spd_test_matrix(), max_abs_diff(), random_vector()

### Community 335 - "CadTopology"
Cohesion: 0.09
Nodes (41): CadEdgeClassCounts, n_seam, n_sharp, n_smooth, CadTopology, edges, faces, vertices (+33 more)

### Community 336 - "CantileverSetup"
Cohesion: 0.15
Nodes (14): Cantilever, mesh, u, VectorXd, CantileverSetup, bc, length, loads (+6 more)

### Community 337 - "PeriodicVertexArray3d"
Cohesion: 0.14
Nodes (11): AdaptiveKdTree::plane_split(), Hilbert_vcmp_periodic<COORD, false, PeriodicVertexMesh3d>, Hilbert_vcmp_periodic<COORD, true, PeriodicVertexMesh3d>, PeriodicVertexArray3d, base_, nb_real_vertices_, nb_vertices_, stride_ (+3 more)

### Community 338 - "run_one"
Cohesion: 0.09
Nodes (22): BcSpec, box, fix, array, detect_hmin_features(), HminFeatureReport, feature_flags, n_features_below_h_min (+14 more)

### Community 339 - "testlab/main.cpp"
Cohesion: 0.13
Nodes (41): atomic_write(), Box3, hi, lo, cfg_id_of(), cmd_pause_status(), completed_keys(), json (+33 more)

### Community 340 - "Geogram / restricted CVT — vendoring study path"
Cohesion: 0.17
Nodes (12): 1. Why Geogram BSD-3 (not clean-room clipped Voronoi), 2.1 Vendor from Geogram (BSD-3), 2.2 We write ourselves, 2.3 Dual hard-block, 2. What to vendor vs what we write, 3. Dependency order (do not invent another), 4. Packing context (how this sits in varyhedron), 5. Suggested `third_party/` layout (not full CMake yet) (+4 more)

### Community 341 - "LiveProgress"
Cohesion: 0.18
Nodes (11): LiveProgress, cfg_id, cg_iter, cg_resid, elapsed_ms, n_elems, n_nodes, part (+3 more)

### Community 342 - "dorfler_mark"
Cohesion: 0.31
Nodes (8): size_t, vector, Vector3d, dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth(), Vector3d, drive_hp()

### Community 343 - "ADR-0021: Varyhedron — variable polyhedral packing mesher"
Cohesion: 0.29
Nodes (6): ADR-0021: Varyhedron — variable polyhedral packing mesher, Alternatives rejected, Consequences, Context, Decision, Research anchors

### Community 344 - "AnswersInfo"
Cohesion: 0.33
Nodes (6): AnswersInfo, load_area_rel_err, load_face_area, sigma_face_mean, strain_energy, tip_deflection

### Community 345 - "nodal_mesh.hpp"
Cohesion: 0.33
Nodes (4): element_num_nodes(), size_t, NodalMesh::check_validity(), NodalMesh::compact_unused_nodes()

### Community 346 - "ProbeAnswers"
Cohesion: 0.10
Nodes (20): ProbeAnswers, dominant_load_axis, free_residual_rel, load_area_ok, load_area_rel_err, load_face_area, mean_u_component, mean_ux (+12 more)

### Community 347 - ".resize"
Cohesion: 0.08
Nodes (23): size_t, BalancedKdTree::build_tree(), Cavity(), CDT2d::insert(), CDTBase2d::create_enclosing_quad(), CDTBase2d::create_enclosing_triangle(), CellStatusArray, capacity_ (+15 more)

### Community 348 - "make_hp_signals"
Cohesion: 0.48
Nodes (7): at_or_broadcast(), at_or_broadcast_int(), size_t, span, vector, estimate_surplus_from_zz(), make_hp_signals()

### Community 349 - "ScorecardInfo"
Cohesion: 0.18
Nodes (11): ScorecardInfo, accuracy_rel_err, chordal_efficiency_max, edge_hausdorff_over_h, has_health_ok, health_ok, min_element_quality, n_dof (+3 more)

### Community 350 - "ADR-0020: True BRep volume meshing (product path)"
Cohesion: 0.33
Nodes (5): ADR-0020: True BRep volume meshing (product path), Alternatives rejected, Consequences, Context, Decision

### Community 351 - "index_t"
Cohesion: 0.09
Nodes (16): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, Delaunay2d(), geo_pause(), atomic (+8 more)

### Community 352 - "ConstrainedLloydParams"
Cohesion: 0.12
Nodes (17): CvtLloydParams, h_floor, max_iters, move_tol_rel, size_at, SizeFieldFn, ConstrainedLloydParams, lloyd (+9 more)

### Community 353 - "vec4"
Cohesion: 0.07
Nodes (33): ConvexCellFlags, Frame, global_index_t, begin_task(), cancel(), ConvexCell::clip_by_plane(), ConvexCell::clip_by_plane_fast(), ConvexCell::compute_geometry() (+25 more)

### Community 354 - "FeaError"
Cohesion: 0.14
Nodes (21): FeaError, runtime_error, uint32_t, vector, PolyCell, faces, nodes, assemble_stiffness() (+13 more)

### Community 355 - "Logger"
Cohesion: 0.09
Nodes (37): CDT2d::save(), Environment::instance(), ExactCDT2d::save(), expansion::show_all_stats(), geo_abort(), geo_assertion_failed(), geo_breakpoint(), geo_range_assertion_failed() (+29 more)

### Community 356 - "ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)"
Cohesion: 0.20
Nodes (10): 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3), 2. Dual hard-block (ADR-0024 Q8), 3. `third_party/` plan, 4. Order (do not invent another), ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block), Alternatives rejected, Consequences, Context (+2 more)

### Community 357 - "GroupBoxFrame"
Cohesion: 0.40
Nodes (5): GroupBoxFrame, fixed_content_h, start, title, width

### Community 358 - "param_key"
Cohesion: 0.33
Nodes (6): array, int64_t, Matrix3d, Vector3d, kirsch_stress(), param_key()

### Community 359 - "Feedback loop — campaign → defaults"
Cohesion: 0.40
Nodes (5): Feedback loop — campaign → defaults, Procedure after campaign finishes, Provisional findings (settings-frontier-1, partial), Tooling, When to change product defaults

### Community 360 - "FeatureGradedSizing"
Cohesion: 0.25
Nodes (8): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector

### Community 361 - "Grok improvement handoff — `varyhedron-short-1`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-short-1`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 362 - "Varyhedron packing — algorithm survey (V5)"
Cohesion: 0.07
Nodes (28): 0. Normative ranking (ADR-0023 / plan — do not ignore), 1. Goals (from ADR-0021), 2. Bubble / sphere packing → Delaunay, 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage), 4. Field-aligned hex-dominant (PGP3D-class), 5. CAD edge protecting balls / PLC constraints, 6. Licensing notes (core vs plugin), 7. Decision: v1 algorithm (+20 more)

### Community 363 - "set_arg"
Cohesion: 0.05
Nodes (53): ArgType, Arg, desc, flags, arg_is_declared(), arg_matches(), name, type (+45 more)

### Community 364 - "VaryhedronFillOutput"
Cohesion: 0.08
Nodes (24): size_t, VaryhedronFillOutput, edge_chordal_efficiency_max, edge_hausdorff_over_h, edge_profile_hausdorff_max, edge_profile_rel, h_coarse, h_fine (+16 more)

### Community 365 - "Grok improvement handoff — `varyhedron-smoke`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-smoke`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 366 - "write_grok_handoff.py"
Cohesion: 0.44
Nodes (8): collect_shots(), git_head(), load_results(), main(), open_program_nodes(), Path, resolve_campaign(), trend_table()

### Community 367 - "string"
Cohesion: 0.04
Nodes (50): CreatorType, int16, int8, Param1, Registry, create(), Factory, Factory0 (+42 more)

### Community 368 - "CvtLloydStats"
Cohesion: 0.12
Nodes (16): CvtLloydResult, positions, stats, CvtLloydStats, converged, domain_diag, geogram_ok, max_move (+8 more)

### Community 369 - "viewport.cpp"
Cohesion: 0.13
Nodes (19): fea_colormap(), fit, array, DisplayMode, optional, uint32_t, vector, Vector3d (+11 more)

### Community 370 - "structured_mesh.hpp"
Cohesion: 0.07
Nodes (23): Stress, box_faces(), box_nodes(), ElementType, uint32_t, vector, Vector3d, one_cell() (+15 more)

### Community 371 - "AccuracyInfo"
Cohesion: 0.40
Nodes (5): AccuracyInfo, metric, rel_err, truth, value

### Community 372 - ".vertex_ptr"
Cohesion: 0.10
Nodes (22): Delaunay2d::check_combinatorics(), Delaunay2d::check_geometry(), Delaunay2d::create_first_triangle(), Delaunay2d::find_conflict_zone(), Delaunay2d::insert(), Delaunay2d::locate(), Delaunay2d::locate_inexact(), Delaunay2d::nearest_vertex() (+14 more)

### Community 373 - "resolve_campaign"
Cohesion: 0.67
Nodes (3): main(), Path, resolve_campaign()

### Community 375 - "intervalBase"
Cohesion: 0.06
Nodes (16): Sign2, control_add(), control_check(), control_mul(), control_set(), control_sub(), vec2HE, incircle_2d_SOS() (+8 more)

### Community 376 - "test_fe_vem_assembly.cpp"
Cohesion: 0.50
Nodes (4): constant_strain_max_error(), Matrix3d, sample_strain_gradient(), unit_box_surface()

### Community 377 - "LexicoCompare"
Cohesion: 0.40
Nodes (3): GEOGRAM_API SOS_sort(), LexicoCompare, dim_

### Community 378 - "stamp_feature_cells"
Cohesion: 0.40
Nodes (9): size_t, span, vector, Vector3d, flat_idx(), stamp_ball(), stamp_curvature_cells(), stamp_feature_cells() (+1 more)

### Community 379 - "Grok improvement handoff — `varyhedron-baseline-m9`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-baseline-m9`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 380 - "probe_util.hpp"
Cohesion: 0.25
Nodes (9): dominant_axis(), face_mean_displacement_component(), face_mean_displacement_mag(), global_max_displacement_mag(), size_t, uint32_t, vector, Vector3d (+1 more)

### Community 381 - "Polyhedral-FEA — agent notes"
Cohesion: 0.67
Nodes (3): Active program (do not skip), graphify, Polyhedral-FEA — agent notes

### Community 382 - ".thread"
Cohesion: 0.19
Nodes (8): Delaunay::update_neighbors(), parallel_for(), PeriodicDelaunay3d::compress(), PeriodicDelaunay3d::handle_periodic_boundaries(), PeriodicDelaunay3d::handle_periodic_boundaries_phase_I(), PeriodicDelaunay3d::handle_periodic_boundaries_phase_II(), PeriodicDelaunay3d::update_v_to_cell(), pop_count()

### Community 383 - "PROGRESS"
Cohesion: 0.33
Nodes (6): Active (read this first), Background / older phases, Benchmark table, Done, Open issues, PROGRESS

### Community 388 - "SolveJob::start"
Cohesion: 0.18
Nodes (16): empty, checkpoint, join_worker, publish_live_mesh, report, reset_control_flags, set_status, solve_options_with_progress (+8 more)

### Community 389 - "Campaign metrics — normative definitions for agents"
Cohesion: 0.22
Nodes (9): 1. Score vs dashboard vs gate, 2. Minimum scorecard (five numbers + residual gate), 3. Case-specific accuracy scores, 4. Chordal efficiency \(e\), 5. Gates and kills (not scores), 6. Displacement probes, 7. Agent checklist before claiming a campaign “win”, Campaign metrics — normative definitions for agents (+1 more)

### Community 390 - "HealthInfo"
Cohesion: 0.25
Nodes (8): HealthInfo, free_residual_rel, has_load_area_ok, load_area_ok, n_orphans, ok, present, reaction_sum_err

### Community 391 - "Triangle"
Cohesion: 0.25
Nodes (10): ConvexCell::connect_triangles(), ushort, make_triangle(), make_triangle_with_flags(), Triangle, i, j, k (+2 more)

### Community 392 - "Predicates_psm.h"
Cohesion: 0.07
Nodes (40): aligned_3d(), aligned_3d_exact(), det2x2(), det3x3(), det4x4(), geo_argused(), geo_clamp(), geo_cmp() (+32 more)

### Community 393 - "CDT2d_ConstraintWalker"
Cohesion: 0.09
Nodes (25): DList, CDT2d_ConstraintWalker, i, j, t, t_prev, v, v_cnstr (+17 more)

### Community 394 - "manifest.json"
Cohesion: 0.50
Nodes (3): generated_utc, git_rev, images

### Community 395 - "Protecting balls + local feature size (LFS)"
Cohesion: 0.33
Nodes (6): 1. Role, 2. CDS radius formula (must-change), 3. Reference, 4. Risk cases, 5. Agent checklist, Protecting balls + local feature size (LFS)

### Community 396 - "SolveResult"
Cohesion: 0.09
Nodes (22): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, VectorXd, SolveResult (+14 more)

### Community 397 - "pointer_"
Cohesion: 0.06
Nodes (31): function_pointer, aligned_allocator, ALIGNMENT, aligned_free(), aligned_malloc(), A2, const_pointer, const_reference (+23 more)

### Community 398 - "Matrix"
Cohesion: 0.17
Nodes (13): result, mat4, FT, initializer_list, matrix_type, create_scaling_matrix(), create_translation_matrix(), FT (+5 more)

### Community 399 - "BRep face-tag BCs / probes (design stub)"
Cohesion: 0.33
Nodes (6): BRep face-tag BCs / probes (design stub), Exit criteria (future work item), Measured icecream instability (why face tags), Out of scope for this stub, Target model (sketch), Why boxes are temporary

### Community 400 - "function"
Cohesion: 0.12
Nodes (17): ConvexCell::for_each_Voronoi_vertex(), function, Thread, parallel_for_slice(), ParallelForSliceThread, from_, func_, to_ (+9 more)

### Community 401 - "ProjectResult"
Cohesion: 0.50
Nodes (4): ProjectResult, distance, normal, point

### Community 402 - "Session handoff — M5 accuracy push (PC restart checkpoint)"
Cohesion: 0.25
Nodes (8): Code levers currently in tree (`scene.cpp` kCvtPoly + `vem.cpp`), Done this arc, Failed / regressing (do not retry blindly), M5 state (do not claim done), Next for M5, Open board, Session handoff — M5 accuracy push (PC restart checkpoint), Verify

### Community 403 - "declare_arg"
Cohesion: 0.16
Nodes (29): ArgFlags, GroupArgs, arg_group(), declare_arg(), declare_arg_group(), declare_arg_percent(), Group, args (+21 more)

### Community 404 - "case_id"
Cohesion: 0.67
Nodes (3): description, type, case_id

### Community 405 - "FaceKey"
Cohesion: 0.15
Nodes (23): add_face(), collect_element_loops(), array, map, uint32_t, vector, emit_element_faces(), extract_boundary_faces() (+15 more)

### Community 406 - "VolumeMeshOutput"
Cohesion: 0.15
Nodes (13): array, map, uint32_t, note_mesh_stats, VolumeMeshOutput, boundary_node_region, boundary_quads, mesh (+5 more)

### Community 407 - "ElementTendencyPlan"
Cohesion: 0.25
Nodes (8): ElementTendencyPlan, label, mesher, native_poly_transitions, remapped, skin_layers, tendency, VolumeMesher

### Community 408 - "expansion"
Cohesion: 0.19
Nodes (20): compress_expansion(), expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 409 - "Matrix"
Cohesion: 0.27
Nodes (6): FT, matrix_type, Matrix, coeff_, dim, mult()

### Community 410 - "expansion"
Cohesion: 0.17
Nodes (20): expansion, compress_expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 411 - "host"
Cohesion: 0.67
Nodes (3): description, type, host

### Community 414 - ".stellate_cavity"
Cohesion: 0.51
Nodes (7): Delaunay3d::stellate_cavity(), facet_facet(), facet_tet(), facet_vertex(), get_facet_neighbor_tets(), nb_facets(), set_facet_tet()

### Community 416 - "IndexType"
Cohesion: 0.21
Nodes (8): IndexType, KeepOrderType, basic_bindex, indices, basic_quadindex, indices, basic_trindex, indices

### Community 417 - "cli/main.cpp"
Cohesion: 0.10
Nodes (45): BcSelection, face_fallback, faces, fallback_band, from_box, nodes, slab_nodes, BoxSel (+37 more)

### Community 418 - "percent"
Cohesion: 0.67
Nodes (3): int64, percent(), PredicateStats::show_stats()

### Community 419 - "WindowsThreadPoolManager"
Cohesion: 0.13
Nodes (13): LONG, PTP_CALLBACK_INSTANCE, PTP_CLEANUP_GROUP, PTP_POOL, PTP_WORK, PVOID, WindowsThreadPoolManager, cbe_ (+5 more)

### Community 420 - "ClippedCell"
Cohesion: 0.13
Nodes (18): ClippedCell, barycenter, empty, n_planes, n_triangles, volume, size_t, clip_convex_cell() (+10 more)

### Community 421 - "varyhedron_fill_surface"
Cohesion: 0.35
Nodes (14): boundary_nodes(), bubble_relax_volume(), span, uint32_t, vector, Vector3d, far_enough(), far_enough_protect() (+6 more)

### Community 423 - "Environment::find_environment"
Cohesion: 0.22
Nodes (7): AssertMode, assert_mode(), Environment::find_environment(), Environment::get_value(), Environment::set_value(), ProcessEnvironment, set_assert_mode()

### Community 424 - "HexFillOutput"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 425 - "FaceConformityStats"
Cohesion: 0.12
Nodes (27): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+19 more)

### Community 426 - "Model"
Cohesion: 0.10
Nodes (22): optional, size_t, vector, Vector3d, Model, bbox_max, bbox_min, cad (+14 more)

### Community 428 - "properties"
Cohesion: 0.12
Nodes (17): description, type, description, type, properties, label, notes, schema_version (+9 more)

### Community 429 - "Spec"
Cohesion: 0.22
Nodes (9): Spec, h0, layers, n, n_bg, nz, path, rho (+1 more)

### Community 430 - "CommandLineDesc"
Cohesion: 0.25
Nodes (8): Args, GroupNames, Groups, CommandLineDesc, args, argv0, group_names, groups

### Community 431 - "initialize"
Cohesion: 0.09
Nodes (24): ProgressClient, android_get_number_of_cores(), enable_cancel(), enable_FPE(), enable_multithreading(), GeogramLibSingleton, initialize(), max_threads() (+16 more)

### Community 432 - "index_t"
Cohesion: 0.05
Nodes (39): IncidentTetrahedra, KeepInitialValues, local_index_t, NearestNeighbors, AdaptiveKdTree::build_tree(), AdaptiveKdTree::split_kd_node(), BalancedKdTree::best_splitting_coord(), BalancedKdTree::split_kd_node() (+31 more)

### Community 433 - "compile"
Cohesion: 0.47
Nodes (6): bind_line_attr(), compile(), link(), init, GLenum, GLuint

### Community 434 - "Box"
Cohesion: 0.22
Nodes (8): bbox_union(), bboxes_overlap(), Box, Box2d, xy_max, xy_min, xyz_max, xyz_min

### Community 435 - "SharpEdge"
Cohesion: 0.13
Nodes (26): ClosestOnFeature, distance, point, uint32_t, Vector3d, SharpEdge, dihedral, v0 (+18 more)

### Community 436 - "DomainTet"
Cohesion: 0.29
Nodes (7): DomainTet, centroid, v0, v1, v2, v3, Vector3d

### Community 438 - "GeoFaceKey"
Cohesion: 0.29
Nodes (5): GeoFaceHash, GeoFaceKey, cx, cy, cz

### Community 439 - "wall_time_s"
Cohesion: 0.29
Nodes (7): wall_time_s, additionalProperties, required, type, mesh, solve, total

### Community 441 - "settings-frontier-1 — campaign-1 close-out"
Cohesion: 0.33
Nodes (5): Caveat, Product default decision (feedback-loop), settings-frontier-1 — campaign-1 close-out, Survivors (tier-2 keep), Tooling top-score cfg (global ranking)

### Community 442 - "Geogram subset — what PolyMesh takes"
Cohesion: 0.18
Nodes (10): CMake, Geogram (vendored subset), Layout, Normative docs, Dual hard-block, Geogram subset — what PolyMesh takes, How PolyMesh consumes it, Included (+2 more)

### Community 445 - "M5 VEM gate — campaign results (2026-07-13)"
Cohesion: 0.25
Nodes (7): Campaign, Hard-learned (do not re-open without new evidence), History, M5 VEM gate — campaign results (2026-07-13), Next to flip M5 → done, Results (latest: VEM τ=0.08 + plate-only wall + cylinder shell sites + OCC snap), What landed (code)

### Community 447 - "LocalRefineStats"
Cohesion: 0.22
Nodes (9): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets, n_skipped_slivers (+1 more)

### Community 461 - "test_stl.cpp"
Cohesion: 0.50
Nodes (4): as_bytes(), byte, string_view, vector

### Community 468 - "test_d6_bench_smoke.cpp"
Cohesion: 0.47
Nodes (4): string, run_cmd(), slurp(), temp_out_path()

### Community 469 - "mean_lateral_radial_residual"
Cohesion: 0.33
Nodes (5): array, uint32_t, vector, Vector3d, mean_lateral_radial_residual()

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **1844 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+1839 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **188 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `SiteGrid` connect `SiteGrid` to `cvt_export.cpp`, `ClippedCell`, `vector`, `ClipBox`?**
  _High betweenness centrality (0.029) - this node is a cross-community bridge._
- **Why does `TriSurface` connect `TriSurface` to `GeomError`, `local_refine_tets`, `CartesianGrid`, `GeometrySizing`, `snap_boundary_nodes`, `graded_tet_fill_surface`, `CadModel`, `varyhedron_fill_surface`, `SizingField`, `Model`, `SharpEdge`, `prism_fill_surface`, `cvt_export.cpp`, `indicators.cpp`, `mixed_fill_surface`, `KindCounts`, `scene.cpp`, `vector`, `GradedSizing`, `FeatureGradedSizing`, `test_fe_vem_assembly.cpp`, `stamp_feature_cells`, `evaluate_curved_mesh_quality`?**
  _High betweenness centrality (0.022) - this node is a cross-community bridge._
- **Why does `set` connect `vector` to `vem.cpp`, `cli/main.cpp`, `SimSetup`, `TriSurface`, `Delaunay_psm.h`, `PolyMesh`, `TransitionFillOutput`, `Delaunay_psm.cpp`, `NodalMesh`, `testlab/main.cpp`, `prism_fill_surface`, `scene.cpp`, `wall_tangential_project`, `evaluate_curved_mesh_quality`, `mixed_fill_surface`, `graded_tet_fill_surface`?**
  _High betweenness centrality (0.020) - this node is a cross-community bridge._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _1844 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `vem.cpp` be split into smaller, more focused modules?**
  _Cohesion score 0.1444121915820029 - nodes in this community are weakly interconnected._
- **Should `analyze_campaign.py` be split into smaller, more focused modules?**
  _Cohesion score 0.11517165005537099 - nodes in this community are weakly interconnected._