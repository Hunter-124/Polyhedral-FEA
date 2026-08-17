# Graph Report - Polyhedral-FEA  (2026-08-16)

## Corpus Check
- 1438 files · ~9,975,327 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 8675 nodes · 18011 edges · 538 communities (353 shown, 185 thin omitted)
- Extraction: 96% EXTRACTED · 4% INFERRED · 0% AMBIGUOUS · INFERRED: 805 edges (avg confidence: 0.79)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `4fd11e14`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- FeaError
- analyze_campaign.py
- CurvedMeshMetrics
- Viewport
- fea library
- ManufacturedSolution
- Palette
- build_advisor_dataset.py
- render_showcase.py
- expansion_nt
- PolyMesh
- App
- TetFillOutput
- msh.cpp
- is_nan
- run_kirsch_annulus
- TestLabState
- Advisor measure-first program (canonical agent plan)
- AdaptSuggestion
- mixed_fill_surface
- solve_elastostatics
- SolveJob::start
- snap_boundary_nodes
- ProgressHeartbeat
- SolveJob
- d6_tier3.cpp
- Delaunay_psm.cpp
- Predicates_psm.cpp
- TransitionFillOutput
- polymesh CMake Project
- gen_part_library.py
- CartesianGrid
- CadModel
- dataset.py
- SimSetup
- thread
- metric_field.cpp
- CsrMatrix
- GeometrySizing
- Hand-calculated reference truths
- accuracy
- index_t
- Stats
- null
- NodalElement
- CalculiX / PolyMesh cantilever cross-validation
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
- SolveResourceEstimate
- Delaunay_psm.h
- CI Grep-Audit Anti-Cheat Job
- HpMode
- loop.md
- BrepFaceIndex
- run_packing_microbench.py
- hierarchical.cpp
- run_tier3.py
- testlab_data.cpp
- viewport.cpp
- train.py
- fea-madness Idea Harvest Source
- KindCounts
- Pareto analysis — `varyhedron-short-1`
- main
- ADR-0003 Element formulations
- ADR-0014 Dörfler seed remesh
- index_t
- hp_assembly.cpp
- assemble_body_load
- ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)
- sparse_bits_flip_bit
- ADR-0004 Mesh data structure
- plot_benchmarks.py
- lame-cylinder case
- Material
- widgets.cpp
- interior_points
- Model
- crossval.py
- scene.cpp
- Grid3d
- gate1_rows
- ResultRow
- Tier 3 Performance Benchmarks
- vector
- T
- .resize
- total_volume
- run_mesh_public.sh
- run_solve_public.sh
- GradedSizing
- run_polymesh_smoke.sh
- P1 MMS Convergence Orders (tet4/hex8/tet10/hex20)
- POLYMESH_WITH_OCC
- POLYMESH_WITH_OPENMP
- Patch Test Is Sacred
- ADR-0002 BSD-3-Clause License
- Camera
- Layer Dependency Direction Rule
- resource_budget.cpp
- index_t
- ROADMAP — Get PolyMesh off the ground
- BRepAdaptor_Surface
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
- report.py
- EffectiveMemoryBudget
- cad_model.cpp
- regret.py
- Sign
- advisor-measure-first-program.md
- CadEdge
- KirschRun
- p_elevate.cpp
- run_calculix_cantilever.py
- run_batch.py
- M9 frozen baseline — `varyhedron-baseline-m9`
- .run
- Pareto analysis — `settings-frontier-1`
- Config
- PartCase
- required
- png_writer.hpp
- plot_evaluation.py
- merge_face_component
- CadTopology
- external_truth.py
- PassTrace
- SurfaceFace
- CaseFeatures
- Test-lab interfaces (normative)
- ADR-0022: Full experiment warehouse + headless Grok improvement loop
- FaceConformityStats
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
- run_gmsh_peer.py
- gen_primitive_corpus.py
- AdvisorNet
- Variable-everything meshing + learned mesh advisor
- NodalMesh
- export_onnx.py
- dashboard.py
- LinearConstraints
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
- TetGrid
- ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)
- traction.cpp
- vec3
- The adaptive solver core, explained
- Pareto analysis — `smoke`
- test_hierarchical.cpp
- json
- BRepGeometryFidelity
- Decision
- Grok improvement loop
- GuiSettings
- testlab_data.hpp
- Advisor::Impl
- lloyd_cvt
- MeshEdgeSegment
- HpSystem
- ADR-0024: Advisor measure-first answers (normative Q&A)
- main
- ClipBox
- HpDriverPolicy
- T
- HpDriverPlan
- ElementHpSignal
- Pareto analysis — `varyhedron-smoke`
- Campaign
- ClippedVoronoiExportStats
- ElementHpDecision
- hp_driver.cpp
- max_abs_diff
- .size
- promote_truth.py
- PeriodicVertexArray3d
- SolveDecision
- testlab/main.cpp
- Geogram / restricted CVT — vendoring study path
- solve_rung
- dorfler_mark
- ADR-0021: Varyhedron — variable polyhedral packing mesher
- M-A1 — first trained advisor (2026-08-10)
- PolyCell
- ProbeAnswers
- Edge
- make_hp_signals
- MixedFillOutput
- ADR-0020: True BRep volume meshing (product path)
- index_t
- ConstrainedLloydParams
- .empty
- backend.cpp
- RuntimeError
- ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)
- VolumeMeshOutput
- JobProgress
- Feedback loop — campaign → defaults
- wall_tangential_project
- Grok improvement handoff — `varyhedron-short-1`
- Varyhedron packing — algorithm survey (V5)
- graded_tet_fill_surface
- VaryhedronFillOutput
- Grok improvement handoff — `varyhedron-smoke`
- write_grok_handoff.py
- string
- CvtLloydStats
- Prior art: ML for mesh generation and adaptive refinement
- structured_mesh.hpp
- run_one
- brep_fidelity.cpp
- resolve_campaign
- check_no_product_stl.sh
- intervalBase
- Checkpoint
- calibration.py
- rebuild_results.py
- Grok improvement handoff — `varyhedron-baseline-m9`
- probe_util.hpp
- progress.md
- Path
- PROGRESS
- invoke_grok_improve.sh
- PredicateStats
- Campaign metrics — normative definitions for agents
- RefinementPlan
- Triangle
- Predicates_psm.h
- TriSurface
- manifest.json
- Protecting balls + local feature size (LFS)
- SolveResult
- pointer_
- operator==
- BRep face-tag BCs / probes (design stub)
- vec4
- ProjectResult
- Variable-everything idea bank
- declare_arg
- Split
- boundary_faces.cpp
- ReferenceCase
- BRepInspection
- expansion
- Matrix
- expansion
- GradedTetFillOutput
- build.sh
- post-m10-smoke/README.md
- Any
- testlab_selfcheck.cpp
- IndexType
- cli/main.cpp
- percent
- compile
- ClipPlane
- varyhedron_fill_surface
- Geometry
- AdvisorDecision
- HexFillOutput
- properties
- advisor.cpp
- GroupBoxFrame
- properties
- figstyle.py
- CommandLineDesc
- test_quadrature.cpp
- coord_index_t
- plot_hole_bug.py
- Box
- SharpEdge
- DomainTet
- main
- plot_truth_independence.py
- wall_time_s
- GeometryDescriptors
- settings-frontier-1 — campaign-1 close-out
- figures.py
- recover_nodal_stress
- test_brep_fidelity.cpp
- M5 VEM gate — campaign results (2026-07-13)
- OmpSettingsGuard
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
- load_area.hpp
- PointAlignment
- PointAlignment<3>
- PointAlignment<6>
- Decision
- 0004 — Model card: learned mesh advisor
- 0005 — Data card: advisor training corpus
- test_d6_bench_smoke.cpp
- mean_lateral_radial_residual
- WindowsThreadPoolManager
- MixedCell
- detect_hole_roi
- Decision
- Decision
- Decision
- SampleDistribution
- self_improve.sh
- FeatureAwareClassification
- test_advisor_inference.cpp
- BcSelection
- ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening
- convergence
- Dirichlet
- 0001 — Advisor architecture
- Top-5 shortlist with implementation sketches
- plot_load_deficit.py
- main
- vtu_wire_png.py
- AdvisorError
- AdvisorRawOutputs
- BrepFidelitySummary
- 0002 — Objectives and guardrails
- LAN access — `hunter-pc` (the 3080 Ti box)
- 2. Training tracks (all four selected, in dependency order)
- VtuPointData
- write_vtu
- build_loads
- CgAttempt
- 0008 — The v4 corpus, the retrain, and the metric that punished being right
- ADR-0033: A gate must measure what ships
- Public CAD corpora for training a mesh advisor
- _healed
- Advisor
- ADR-0031: A jut has a side
- colorbar
- GeometryCompleteness
- ElementTendencyPlan
- BoxSel
- run_artifacts.hpp
- enum
- 0006 — The clean-data retrain, and what it cost the advisor's claims
- check
- MshModel
- 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number
- LogLimits
- LSolve
- refusal
- emit_transition_poly
- HoleROI
- schema_version
- land_contract_cutover.sh
- stage_argv
- DomainClipParams
- summarize_samples
- test_advisor_dataset_rederive.cpp
- Case
- test_truth_guard.cpp
- host
- label
- .fix_node
- same_node_bytes
- archive/README.md
- check_cross_stdlib_mesh.sh
- enroll_lan_keys.sh
- PointAlignment<8>

## God Nodes (most connected - your core abstractions)
1. `NodalMesh` - 147 edges
2. `PeriodicDelaunay3dThread` - 125 edges
3. `Delaunay3dThread` - 109 edges
4. `TriSurface` - 82 edges
5. `Logger` - 69 edges
6. `Viewport` - 65 edges
7. `vec3` - 59 edges
8. `App` - 58 edges
9. `Model` - 56 edges
10. `geo_argused()` - 56 edges

## Surprising Connections (you probably didn't know these)
- `select_exact_cad_load_faces()` --calls--> `target`  [INFERRED]
  apps/testlab/main.cpp → src/mesh/include/mesh/surface_project.hpp
- `run_one()` --calls--> `request_cancel`  [INFERRED]
  apps/testlab/main.cpp → src/pipeline/include/pipeline/scene.hpp
- `vem_body_load()` --calls--> `body`  [INFERRED]
  src/fea/src/vem.cpp → tests/support/mms.hpp
- `sizing_field` --semantically_similar_to--> `resolve_mesh_size`  [INFERRED] [semantically similar]
  src/adapt/CMakeLists.txt → examples/README.md
- `merge_unique()` --references--> `NodalMesh`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp

## Import Cycles
- None detected.

## Communities (538 total, 185 thin omitted)

### Community 0 - "FeaError"
Cohesion: 0.13
Nodes (57): Fun, kP2Mono, kP2Vec, FeaError, runtime_error, b_from_grads(), array, Dynamic (+49 more)

### Community 1 - "analyze_campaign.py"
Cohesion: 0.12
Nodes (37): accuracy_of(), aggregate_configs(), analyze_one(), CfgAgg, config_label(), factor_breakdown(), _fmt_ms(), _fmt_pct() (+29 more)

### Community 2 - "CurvedMeshMetrics"
Cohesion: 0.06
Nodes (36): CurvedMeshMetrics, composite_score, has_circular, has_tet_aspect, has_volume, m1_max, m1_mean, m2_max (+28 more)

### Community 3 - "Viewport"
Cohesion: 0.04
Nodes (48): array, DisplayMode, uint32_t, vector, Vector3d, VectorXd, Viewport, background_program_ (+40 more)

### Community 4 - "fea library"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "ManufacturedSolution"
Cohesion: 0.15
Nodes (21): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+13 more)

### Community 6 - "Palette"
Cohesion: 0.05
Nodes (41): apply_theme(), ThemeId, ImVec4, make_interwebz_palette(), make_slate_palette(), make_studio_palette(), Palette, accent (+33 more)

### Community 7 - "build_advisor_dataset.py"
Cohesion: 0.06
Nodes (57): family_of(), main(), probe_of(), load_json(), main(), Path, The named metric of a reference, from the working tree or a git revision., reference_metric() (+49 more)

### Community 8 - "render_showcase.py"
Cohesion: 0.05
Nodes (79): _aspect(), build_grid(), choose_cols(), main(), matched_panels(), PanelSpec, Image, ImageDraw (+71 more)

### Community 9 - "expansion_nt"
Cohesion: 0.06
Nodes (51): interval_nt, mat2, mat3, aligned_3d(), aligned_3d_exact(), approximate(), CDT2d::create_intersection(), vec2E (+43 more)

### Community 10 - "PolyMesh"
Cohesion: 0.07
Nodes (58): CellKind, poly_mesh_to_vem(), Cell, faces, kind, Face, neighbour, owner (+50 more)

### Community 11 - "App"
Cohesion: 0.05
Nodes (41): App, custom_font, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, improve_running (+33 more)

### Community 12 - "TetFillOutput"
Cohesion: 0.12
Nodes (15): size_t, OctaFillOutput, h, mesh, n_boundary_pyramids, n_octahedra, array, uint32_t (+7 more)

### Community 13 - "msh.cpp"
Cohesion: 0.19
Nodes (19): GmshType, ElementType, FaceType, path, string, string_view, vector, gmsh_num_nodes() (+11 more)

### Community 14 - "is_nan"
Cohesion: 0.40
Nodes (5): float32, float64, is_nan(), random_float32(), random_float64()

### Community 15 - "run_kirsch_annulus"
Cohesion: 0.08
Nodes (37): RadialMap, check_validity, box_hex_mesh(), box_tet_mesh(), cell_corners(), array, uint32_t, uint64_t (+29 more)

### Community 16 - "TestLabState"
Cohesion: 0.07
Nodes (48): CheckpointState, ImVec4, path, size_t, string, draw_results_panel(), draw_testlab_panel(), fmt_opt_num() (+40 more)

### Community 17 - "Advisor measure-first program (canonical agent plan)"
Cohesion: 0.08
Nodes (24): 0. One-sentence strategy, 10. Related files, 1. Substrate (keep forever until proven wrong), 2. Claims (product honesty), 3.1 Five-number scorecard + residual gate, 3.2 What to score vs dashboard (stress), 3.3 Chordal efficiency (edge residual), 3.4 Over-budget diagnosis (+16 more)

### Community 18 - "AdaptSuggestion"
Cohesion: 0.18
Nodes (15): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+7 more)

### Community 19 - "mixed_fill_surface"
Cohesion: 0.22
Nodes (25): closed_poly_volume(), array, function, size_t, SizeFieldFn, span, uint32_t, vector (+17 more)

### Community 20 - "solve_elastostatics"
Cohesion: 0.10
Nodes (33): Precond, function, SolveOptions, cg_accept_tol, cg_max_iters, cg_progress_chunk, cg_threshold, cg_tol (+25 more)

### Community 21 - "SolveJob::start"
Cohesion: 0.09
Nodes (31): BoundarySupportKind, SizeSource, h, x, BoundarySupport, id, kind, uint32_t (+23 more)

### Community 22 - "snap_boundary_nodes"
Cohesion: 0.06
Nodes (52): BoundaryTargetFn, CollectOffendersFn, NodeOffendsFn, RepairInteriorFn, BoundaryProjectionContext, provenance, target, BoundaryTarget (+44 more)

### Community 23 - "ProgressHeartbeat"
Cohesion: 0.09
Nodes (19): atomic, mutex, size_t, time_point, ProgressHeartbeat, cfg_id_, cg_iter_, cg_resid_ (+11 more)

### Community 24 - "SolveJob"
Cohesion: 0.07
Nodes (28): State, time_point, uint64_t, load, SolveJob, active_max_mem_gb_, cancel_, clear_failure (+20 more)

### Community 25 - "d6_tier3.cpp"
Cohesion: 0.09
Nodes (39): add_node(), array, int64_t, json, map, string, uint32_t, vector (+31 more)

### Community 26 - "Delaunay_psm.cpp"
Cohesion: 0.01
Nodes (264): E, AssertMode, EMSCRIPTEN_KEEPALIVE, InvalidInput, Node, ProgressClient, siginfo_t, abnormal_program_termination() (+256 more)

### Community 27 - "Predicates_psm.cpp"
Cohesion: 0.04
Nodes (97): coord_index_t, Sign, SOSMode, det_3d(), det_3d_exact(), det_3d_filter(), det_4d(), det_4d_filter() (+89 more)

### Community 28 - "TransitionFillOutput"
Cohesion: 0.11
Nodes (20): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+12 more)

### Community 29 - "polymesh CMake Project"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "gen_part_library.py"
Cohesion: 0.24
Nodes (14): _assert_manifold_facets(), _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre. x ∈…, Parse emitted ASCII facet blocks and require edge multiplicity 2. (+6 more)

### Community 31 - "CartesianGrid"
Cohesion: 0.10
Nodes (43): CartesianGrid, cell, nx, ny, nz, origin, Vector3d, runtime_error (+35 more)

### Community 32 - "CadModel"
Cohesion: 0.11
Nodes (20): shared_ptr, CadModel, compute_bbox, has_brep, impl_, load_brep, load_step, name_ (+12 more)

### Community 33 - "dataset.py"
Cohesion: 0.05
Nodes (74): build(), fit(), main(), Module, action_group_slices(), _best_actions(), build_action_dims(), candidate_grid() (+66 more)

### Community 34 - "SimSetup"
Cohesion: 0.08
Nodes (29): function, SimSetup, adapt_leb_waves, adapt_passes, bc_grading, boundary_builder, element_tendency, eta_target (+21 more)

### Community 35 - "thread"
Cohesion: 0.20
Nodes (8): box_model(), box_surface(), path, string, read_all(), scratch_dir(), ITERATOR, thread()

### Community 36 - "metric_field.cpp"
Cohesion: 0.05
Nodes (72): Matrix3d, size_t, vector, Vector3d, Vector3i, Metric3d, axes, clamped (+64 more)

### Community 37 - "CsrMatrix"
Cohesion: 0.18
Nodes (15): CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t, vector (+7 more)

### Community 38 - "GeometrySizing"
Cohesion: 0.06
Nodes (36): FeatureSizing, blend_, dist_, h_max_, h_min_, size_at, GeometrySizing, blend_ (+28 more)

### Community 39 - "Hand-calculated reference truths"
Cohesion: 0.07
Nodes (28): cantilever, corpus-primitives, corpus-primitives-cantilever, corpus-primitives-external (supersedes corpus-primitives-provisional), corpus-primitives-kirsch, corpus-primitives-provisional (historical), cylinder, Engineering estimate: polar compression as a short column (+20 more)

### Community 40 - "accuracy"
Cohesion: 0.12
Nodes (16): additionalProperties, properties, required, type, description, type, accuracy, name (+8 more)

### Community 41 - "index_t"
Cohesion: 0.09
Nodes (16): PackedArrays::clear(), aligned_free(), index_t, std::vector<bool>, std::vector<T, Memory::aligned_allocator<T> >, expansion::delete_expansion_on_heap(), expansion::new_expansion_on_heap(), expansion::show_all_stats() (+8 more)

### Community 42 - "Stats"
Cohesion: 0.11
Nodes (19): Stats, phase_0_t_, phase_I_classify_t_, phase_I_insert_nb_, phase_I_insert_t_, phase_I_nb_cross_, phase_I_nb_inside_, phase_I_nb_outside_ (+11 more)

### Community 43 - "null"
Cohesion: 0.09
Nodes (26): description, minimum, type, description, minimum, type, description, type (+18 more)

### Community 44 - "NodalElement"
Cohesion: 0.10
Nodes (32): CornerEdges, CellQualityStats, mean, min, n_measured, n_unmeasured, size_t, element_num_nodes() (+24 more)

### Community 45 - "CalculiX / PolyMesh cantilever cross-validation"
Cohesion: 0.33
Nodes (5): CalculiX / PolyMesh cantilever cross-validation, Cases to port next, Common install paths (documentation only), Run (CI-safe), Runner contract

### Community 47 - "eval_shape"
Cohesion: 0.13
Nodes (27): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+19 more)

### Community 48 - "Plan: Mesher / Solver Accuracy + Performance Overhaul"
Cohesion: 0.07
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 49 - "gen_cad_parts.py"
Cohesion: 0.16
Nodes (17): _is_valid(), main(), make_cylinder(), make_icecream_cone(), make_plate_hole(), make_sphere(), Path, Tessellate and write an ASCII-friendly STL for visual compare only. (+9 more)

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
Cohesion: 0.13
Nodes (42): RawFaceProvenance, bisector_keep_site(), build_cell(), build_cell_tet_nbr(), array, global_index_t, pair, size_t (+34 more)

### Community 58 - "SolveResourceEstimate"
Cohesion: 0.13
Nodes (15): Index, SolveResourceEstimate, assembly_workspace_bytes, cell_storage_bytes, cg_peak_bytes, cg_workspace_bytes, common_peak_bytes, csr_nnz_upper (+7 more)

### Community 59 - "Delaunay_psm.h"
Cohesion: 0.03
Nodes (54): align(), clear(), copy(), Counted(), GEOGRAM_API, float32, float64, is_specialized (+46 more)

### Community 60 - "CI Grep-Audit Anti-Cheat Job"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "HpMode"
Cohesion: 0.11
Nodes (21): Entity, HpMode, edge_odd, entity, entity_index, index0, index1, index2 (+13 more)

### Community 62 - "loop.md"
Cohesion: 0.40
Nodes (4): 1. PLAN, 2. BUILD, 3. VERIFY, 4. LOOP OR STOP

### Community 63 - "BrepFaceIndex"
Cohesion: 0.08
Nodes (28): Handle, BrepFaceIndex, adaptors, bins, boxes, cell, edge_ids, edge_map (+20 more)

### Community 64 - "run_packing_microbench.py"
Cohesion: 0.23
Nodes (21): boundary_residual_placeholder(), bubble_relax(), _clamp01(), _dedupe(), _dist2(), fill_fraction_proxy(), main(), pack_case() (+13 more)

### Community 65 - "hierarchical.cpp"
Cohesion: 0.09
Nodes (42): Dynamic, Matrix, VectorXd, HpShape, dn, n, b_matrix(), build_hex() (+34 more)

### Community 66 - "run_tier3.py"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "testlab_data.cpp"
Cohesion: 0.19
Nodes (31): checkpoint_state_cstr(), count_result_lines(), Checkpoint, CheckpointState, json, optional, path, string (+23 more)

### Community 68 - "viewport.cpp"
Cohesion: 0.13
Nodes (19): fea_colormap(), fit, array, DisplayMode, optional, uint32_t, vector, Vector3d (+11 more)

### Community 69 - "train.py"
Cohesion: 0.07
Nodes (58): Optimizer, load_pruned(), Read the accumulated pruning ledger; missing file means nothing pruned., activation_record(), append_history(), _auc(), BatchView, build_model() (+50 more)

### Community 71 - "KindCounts"
Cohesion: 0.22
Nodes (10): count_kinds(), size_t, KindCounts, hex, other, pyr, tet, vem (+2 more)

### Community 72 - "Pareto analysis — `varyhedron-short-1`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 73 - "main"
Cohesion: 0.18
Nodes (13): draw_line(), main(), parse_vtu_ascii(), point_in_roi(), project(), Path, quadratic_edge_mids(), quadratic_edge_points() (+5 more)

### Community 76 - "index_t"
Cohesion: 0.02
Nodes (171): condition_variable, local_index_t, Periodic, SFrame, AdaptiveKdTree::create_kd_tree_recursive(), AdaptiveKdTree::new_node(), BooleanExpression::parse_variable(), CDTBase2d::insert() (+163 more)

### Community 77 - "hp_assembly.cpp"
Cohesion: 0.07
Nodes (48): QuadKey, HpElementDef, order, type, vertices, ElementType, uint32_t, uint8_t (+40 more)

### Community 78 - "assemble_body_load"
Cohesion: 0.11
Nodes (33): Vector3d, QuadraturePoint, weight, xi, assemble_body_load(), BodyForce, VectorXd, Dynamic (+25 more)

### Community 79 - "ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "sparse_bits_flip_bit"
Cohesion: 0.83
Nodes (4): SparseBits, ExactCDT2d::classify_triangles_union_cnstr_operand_bits_is_operand_id(), sparse_bits_flip_bit(), sparse_bits_is_zero()

### Community 82 - "plot_benchmarks.py"
Cohesion: 0.22
Nodes (19): d6_records(), gate1_records(), load_json(), main(), parse_mms_elements(), parse_mms_hierarchical(), parse_tier1_table(), plot_dof_time() (+11 more)

### Community 84 - "Material"
Cohesion: 0.12
Nodes (11): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+3 more)

### Community 85 - "widgets.cpp"
Cohesion: 0.24
Nodes (22): draw_column_splitter(), begin_field(), begin_group_box(), begin_group_box_fill(), button(), checkbox(), ImVec4, draw_accent_fill() (+14 more)

### Community 86 - "interior_points"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "Model"
Cohesion: 0.09
Nodes (25): optional, Vector3d, Model, bbox_max, bbox_min, cad, name, region_count (+17 more)

### Community 88 - "crossval.py"
Cohesion: 0.06
Nodes (59): action_matrix(), advisor_scores(), aggregate(), aggregate_tolerance(), build_choosers(), decode_policy(), main(), parse_args() (+51 more)

### Community 89 - "scene.cpp"
Cohesion: 0.08
Nodes (44): selected, max_value, apply_spectral_sizing(), boundary_shell_topology(), BoundaryShellTopology, n_edges, n_nonmanifold, n_open (+36 more)

### Community 90 - "Grid3d"
Cohesion: 0.06
Nodes (57): complex, BudgetResult, budget_met, filter, h_scale, predicted_after, predicted_before, FilterReport (+49 more)

### Community 91 - "gate1_rows"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face. Face…, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "ResultRow"
Cohesion: 0.04
Nodes (47): AnswersInfo, load_area_rel_err, load_face_area, sigma_face_mean, strain_energy, tip_deflection, HealthInfo, free_residual_rel (+39 more)

### Community 94 - "vector"
Cohesion: 0.06
Nodes (19): array, string, vector, array, vector, optional, Backend, map (+11 more)

### Community 95 - "T"
Cohesion: 0.04
Nodes (38): B, DIM2, initializer_list, T, T2, U, vector_type, length2() (+30 more)

### Community 96 - ".resize"
Cohesion: 0.04
Nodes (52): COORD, KeepInitialValues, MESH, MeshElementsFlags, MeshOrder, NearestNeighbors, AdaptiveKdTree::build_tree(), AdaptiveKdTree::split_kd_node() (+44 more)

### Community 97 - "total_volume"
Cohesion: 0.39
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 100 - "GradedSizing"
Cohesion: 0.07
Nodes (31): int32_t, GradedSizing, as_size_field, build_grid, cell_start_, grid_cell_, grid_nx_, grid_ny_ (+23 more)

### Community 109 - "Camera"
Cohesion: 0.16
Nodes (17): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+9 more)

### Community 119 - "resource_budget.cpp"
Cohesion: 0.36
Nodes (15): Index, string, string_view, uint64_t, csr_bytes(), dense_square_cap(), effective_memory_budget(), estimate_solve_resources() (+7 more)

### Community 120 - "index_t"
Cohesion: 0.16
Nodes (11): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, geo_pause(), atomic, index_t (+3 more)

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "BRepAdaptor_Surface"
Cohesion: 0.33
Nodes (5): BRepAdaptor_Surface, compute_geometry_descriptors(), RadiusSample, area, radius

### Community 125 - "SiteGrid"
Cohesion: 0.11
Nodes (18): uint32_t, vector, SiteGrid, build, cell_, cell_start_, inv_cell_, items_ (+10 more)

### Community 126 - "PolyMesh Showcase"
Cohesion: 0.11
Nodes (18): Architecture diagram, `bench_dof_time.png`, `bench_mms.png`, `bench_tier1.png`, Benchmark charts, `gallery_cantilever.png`, `gallery_cylinder.png`, `gallery_icecream_cone.png` (+10 more)

### Community 127 - "evaluate_curved_mesh_quality"
Cohesion: 0.19
Nodes (16): CircularFeature, axis_dir, axis_point, radius, select_band, Vector3d, clamp01(), array (+8 more)

### Community 128 - "GeomError"
Cohesion: 0.16
Nodes (19): bbox_diagonal, GeomError, runtime_error, CadModel::tessellate(), Soup, triangulate_shape(), byte, path (+11 more)

### Community 129 - "backend_cuda.cu"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "gui/main.cpp"
Cohesion: 0.21
Nodes (19): capture_screenshot(), size_t, string, draw_colorbar(), draw_frame(), draw_study_panel(), draw_viewport_content(), drop_callback() (+11 more)

### Community 131 - "cell_validity.hpp"
Cohesion: 0.28
Nodes (20): hex8_jacobian_det(), hex8_min_jacobian(), hex8_shape_quality(), array, Vector3d, max_edge(), prism_min_corner_jacobian(), prism_shape_quality() (+12 more)

### Community 132 - "local_refine_tets"
Cohesion: 0.25
Nodes (20): FreeFaceKey, bisect_tet(), array, EdgeKey, size_t, span, uint32_t, vector (+12 more)

### Community 133 - "report.py"
Cohesion: 0.08
Nodes (59): accuracy_vs_cost(), _arrow(), _best_so_far(), _box(), _caption(), checkpoint_shape(), contract_heads(), corpus_rows() (+51 more)

### Community 134 - "EffectiveMemoryBudget"
Cohesion: 0.22
Nodes (10): MemoryAvailabilitySource, EffectiveMemoryBudget, available, effective_cap_bytes, safety_cap_bytes, user_cap_bytes, uint64_t, MemoryAvailability (+2 more)

### Community 135 - "cad_model.cpp"
Cohesion: 0.13
Nodes (32): BRepExtrema_DistShapeShape, gp_Pnt, empty, shape_handle, box_lower_bound(), CadModel::Impl, shape, CadSupportKind (+24 more)

### Community 136 - "regret.py"
Cohesion: 0.07
Nodes (52): budget_levels(), build_cases(), Case, cost_at_tolerance(), decades_to_factor(), dof_to_target(), feasible_mask(), finest_action_chooser() (+44 more)

### Community 137 - "Sign"
Cohesion: 0.03
Nodes (110): DList, CDT2d_ConstraintWalker, i, j, t, t_prev, v, v_cnstr (+102 more)

### Community 138 - "advisor-measure-first-program.md"
Cohesion: 0.15
Nodes (12): Agent bootstrap — overnight / autonomous work on the DAG, Program DAG — how to pick up work, CMake, Geogram (vendored subset), Layout, Normative docs, Dual hard-block, Geogram subset — what PolyMesh takes (+4 more)

### Community 139 - "CadEdge"
Cohesion: 0.08
Nodes (27): CadEdgeFeature, CadSurfaceKind, CadEdge, dihedral_rad, feature, id, kappa_samples, length (+19 more)

### Community 140 - "KirschRun"
Cohesion: 0.29
Nodes (7): size_t, KirschRun, free_dofs, n_elements, n_nodes, scf, scf_rel_err

### Community 141 - "p_elevate.cpp"
Cohesion: 0.08
Nodes (39): Fn, N, ElementTypeCounts, hex20, hex8, other, tet10, tet4 (+31 more)

### Community 142 - "run_calculix_cantilever.py"
Cohesion: 0.23
Nodes (18): input_id_lines(), load_json(), main(), node_id(), parse_ccx_tip_displacement(), parse_polymesh_tip_displacement(), CompletedProcess, Path (+10 more)

### Community 143 - "run_batch.py"
Cohesion: 0.09
Nodes (49): append_throughput(), build_rects(), campaign_json(), cfg_id_of(), completed_pairs(), count_lines(), expand_grid(), iter_rows() (+41 more)

### Community 144 - "M9 frozen baseline — `varyhedron-baseline-m9`"
Cohesion: 0.17
Nodes (12): Campaign matrix, Case primary accuracy metrics (as wired at freeze), Freeze identity, Known issues frozen-in (not blockers for freeze), M9 frozen baseline — `varyhedron-baseline-m9`, Metric schema version, Outcome summary, Per-run snapshot (+4 more)

### Community 145 - ".run"
Cohesion: 0.13
Nodes (9): DWORD, LPVOID, CERRStream, spinlock, ThreadGroup, MonoThreadingThreadManager::run_concurrent_threads(), run_threads(), ThreadManager::run_threads() (+1 more)

### Community 146 - "Pareto analysis — `settings-frontier-1`"
Cohesion: 0.12
Nodes (16): `cantilever`, Config ranking (weighted mean score), `curved`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 147 - "Config"
Cohesion: 0.12
Nodes (17): Config, adapt_leb_waves, adapt_passes, bc_grading, curvature_turn_deg, element_tendency, eta_target, feature_refine (+9 more)

### Community 148 - "PartCase"
Cohesion: 0.12
Nodes (16): adaptive_setup(), MetricSpec, derivation, name, probe, tol, PartCase, bcs (+8 more)

### Community 149 - "required"
Cohesion: 0.12
Nodes (15): additionalProperties, description, $id, required, $schema, title, type, accuracy (+7 more)

### Community 150 - "png_writer.hpp"
Cohesion: 0.57
Nodes (7): adler32_of(), crc32_of(), size_t, uint32_t, put_chunk(), put_u32be(), write_png_rgba()

### Community 151 - "plot_evaluation.py"
Cohesion: 0.08
Nodes (50): advisor_evaluation(), band_levels(), coincident(), collapse_families(), decades_to_factor(), default_crossval(), draw_bands(), draw_failures() (+42 more)

### Community 152 - "merge_face_component"
Cohesion: 0.16
Nodes (12): canonical_face_key(), coalesce_rvd_interior_faces(), FaceId, VertexId, merge_face_component(), rvd_edge(), RvdEdgeKey, a (+4 more)

### Community 153 - "CadTopology"
Cohesion: 0.08
Nodes (42): CadEdgeClassCounts, n_seam, n_sharp, n_smooth, CadTopology, edges, faces, vertices (+34 more)

### Community 154 - "external_truth.py"
Cohesion: 0.09
Nodes (48): all_case_ids(), analytic_case_ids(), audit_against_git(), base_provenance(), bbox_diagonal(), cad_feature_sizes(), derive_tol(), dump_json() (+40 more)

### Community 155 - "PassTrace"
Cohesion: 0.08
Nodes (25): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, PassTrace, eta_max (+17 more)

### Community 156 - "SurfaceFace"
Cohesion: 0.09
Nodes (46): BcSpec, box, cad_face_ids, fix, Box3, hi, lo, compute_probes() (+38 more)

### Community 157 - "CaseFeatures"
Cohesion: 0.04
Nodes (45): CaseFeatures, bbox_dx, bbox_dy, bbox_dz, curved_frac, diag, fix_area_frac, fix_load_dist_over_diag (+37 more)

### Community 158 - "Test-lab interfaces (normative)"
Cohesion: 0.10
Nodes (20): 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp` (+12 more)

### Community 159 - "ADR-0022: Full experiment warehouse + headless Grok improvement loop"
Cohesion: 0.12
Nodes (14): Campaign warehouse, Directory layout, git-LFS, Short-campaign defaults (Lane V), Wireframe PNGs (`wire.png`), ADR-0022: Full experiment warehouse + headless Grok improvement loop, Alternatives rejected, Consequences (+6 more)

### Community 160 - "FaceConformityStats"
Cohesion: 0.12
Nodes (27): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+19 more)

### Community 267 - "run_gmsh_peer.py"
Cohesion: 0.10
Nodes (40): analytic_cases(), bbox_diagonal(), build_supports_uniform(), cad_bbox_diagonal(), classify_failure(), failed_result_row(), flatten_box(), load_arguments() (+32 more)

### Community 268 - "gen_primitive_corpus.py"
Cohesion: 0.14
Nodes (38): _box(), build_box_hole(), build_channel(), build_ellipsoid_boss(), build_l_bracket(), build_lobed_shaft(), build_perforated_plate(), build_plate_notch() (+30 more)

### Community 269 - "AdvisorNet"
Cohesion: 0.09
Nodes (26): Train one model from scratch. No warm start: a fold must be independent., train_fold(), add_split_args(), Attach the ``--split`` / ``--fold`` / ``--n-folds`` trio to a parser. Shared so…, default_checkpoint(), main(), ndarray, Path (+18 more)

### Community 270 - "Variable-everything meshing + learned mesh advisor"
Cohesion: 0.05
Nodes (35): 1.1 `adapt::MetricField` (new), 1.2 Sizing field end-to-end, 1.3 Conforming variable order, 1.4 Quantitative sizing from error, Corpus, Deployment, Label design (decided), Non-goals (+27 more)

### Community 271 - "NodalMesh"
Cohesion: 0.08
Nodes (34): Vector3d, NodalMesh, compact_unused_nodes, elements, nodes, uint32_t, vector, outer_wall_sag() (+26 more)

### Community 272 - "export_onnx.py"
Cohesion: 0.13
Nodes (33): write_json(), check_fixture_guarantees(), default_checkpoint(), export_graph(), force_head_values(), load_trained(), main(), normalization_drift() (+25 more)

### Community 273 - "dashboard.py"
Cohesion: 0.17
Nodes (31): begin_end_annotations(), chart(), dash_y2(), ensure_plotly(), fmt(), guardrails_block(), js_json(), load_activations() (+23 more)

### Community 274 - "LinearConstraints"
Cohesion: 0.08
Nodes (27): map, pair, size_t, uint32_t, vector, LinearConstraint, masters, slave_dof (+19 more)

### Community 305 - "TetGrid"
Cohesion: 0.18
Nodes (26): BuriedFaceStats, n_buried, n_free_faces, size_t, buried_face_ids(), buried_free_tet_face_owners(), count_buried_free_tet_faces(), array (+18 more)

### Community 306 - "ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "traction.cpp"
Cohesion: 0.09
Nodes (40): traction, Sink, ConsistentLoad, area, conservation_error, loads, resultant, face_num_nodes() (+32 more)

### Community 308 - "vec3"
Cohesion: 0.05
Nodes (58): COORD_T, angle(), CDT2d::create_enclosing_quad(), CDT2d::create_enclosing_triangle(), ConvexCell::barycenter(), ConvexCell::compute_triangle_point(), ConvexCell::squared_inner_radius(), ConvexCell::squared_radius() (+50 more)

### Community 309 - "The adaptive solver core, explained"
Cohesion: 0.18
Nodes (10): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, Decision policy (v1, `adapt::drive_hp`), The adaptive solver core, explained, What is implemented (node `fe-vem-assembly`) (+2 more)

### Community 310 - "Pareto analysis — `smoke`"
Cohesion: 0.14
Nodes (13): Config ranking (weighted mean score), Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run, Pareto analysis — `smoke`, Pareto by geometric class (+5 more)

### Community 311 - "test_hierarchical.cpp"
Cohesion: 0.32
Nodes (6): count_zero_modes(), Dynamic, Matrix, MatrixXd, unit_hex_coords(), unit_tet_coords()

### Community 312 - "json"
Cohesion: 0.10
Nodes (30): json, main(), Path, run_one(), _bootstrap_slope(), descriptor_distances(), family_of(), family_recovery() (+22 more)

### Community 313 - "BRepGeometryFidelity"
Cohesion: 0.12
Nodes (17): BRepGeometryFidelity, available, brep, brep_surface_fallback_vertex_count, brep_surface_sample_face_count, brep_surface_samples_to_mesh_boundary, brep_surface_uv_attempt_count, brep_vertices_to_mesh_boundary_nodes (+9 more)

### Community 314 - "Decision"
Cohesion: 0.14
Nodes (14): 1. Substrate (keep, do not replace), 2. Element technology claims, 3. Packing evolution, 4. CAD edge classification (normative), 5. Measurement order (two-week horizon), 6. License landscape (core vs plugin), 7. Sizing field, 8. p-order (+6 more)

### Community 315 - "Grok improvement loop"
Cohesion: 0.25
Nodes (7): Autonomous vs supervised answers, Grok improvement loop, Handoff contents, Headless invoke (default), Manual interactive (optional), Safety, When it runs

### Community 316 - "GuiSettings"
Cohesion: 0.08
Nodes (29): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+21 more)

### Community 317 - "testlab_data.hpp"
Cohesion: 0.03
Nodes (71): AccuracyInfo, metric, rel_err, truth, value, CampaignResources, max_mem_gb, max_threads (+63 more)

### Community 318 - "Advisor::Impl"
Cohesion: 0.06
Nodes (31): Env, MemoryInfo, Session, SessionOptions, Advisor::Impl, action_dims, adapt_passes, candidates (+23 more)

### Community 319 - "lloyd_cvt"
Cohesion: 0.32
Nodes (15): density_from_size(), bisector_keep_site(), build_rvd_cell(), optional, SizeFieldFn, span, Vector3d, density_mg() (+7 more)

### Community 320 - "MeshEdgeSegment"
Cohesion: 0.67
Nodes (3): MeshEdgeSegment, a, b

### Community 321 - "HpSystem"
Cohesion: 0.08
Nodes (27): HpModel, elements, nodes, Index, SparseMatrix, vector, Vector3d, HpSystem (+19 more)

### Community 322 - "ADR-0024: Advisor measure-first answers (normative Q&A)"
Cohesion: 0.17
Nodes (12): ADR-0024: Advisor measure-first answers (normative Q&A), Compressed path (do not invent another), Q10 — High-dimensional traps, Q1 — 1e20 von Mises with 1e-13 residual, Q2 — Next 3–5 days order, Q3 — Geogram, Q4 — Chordal efficiency e ~ 100 at h_scale=5, Q5 — Cylinder truth (+4 more)

### Community 323 - "main"
Cohesion: 0.53
Nodes (5): main(), parse_mesh_stdout(), Path, Best-effort wireframe via pure-Python exterior edges, then meshio., try_render_png()

### Community 324 - "ClipBox"
Cohesion: 0.08
Nodes (35): CvtSite, fixed, pos, ConstrainedLloydResult, lloyd_stats, project_stats, seed_stats, sites (+27 more)

### Community 325 - "HpDriverPolicy"
Cohesion: 0.11
Nodes (19): HpDriverPolicy, coarsen_geom_factor, coarsen_theta, cost_h, cost_p, cost_shape, dorfler_theta, eta_rel_floor (+11 more)

### Community 326 - "T"
Cohesion: 0.06
Nodes (35): A1, A2, DIM, DIM2, T, T1, T2, vector_type (+27 more)

### Community 327 - "HpDriverPlan"
Cohesion: 0.12
Nodes (16): HpDriverPlan, coarsen_mark, decisions, global_shape, h_mark, h_suggestion, n_coarsen, n_h (+8 more)

### Community 328 - "ElementHpSignal"
Cohesion: 0.15
Nodes (13): ElementHpSignal, eta, h, h_geometry, hex_fit, kappa, p_max, poly_fit (+5 more)

### Community 329 - "Pareto analysis — `varyhedron-smoke`"
Cohesion: 0.13
Nodes (14): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+6 more)

### Community 330 - "Campaign"
Cohesion: 0.11
Nodes (18): Campaign, grid, max_dof, max_elems, max_pack_wall_s, max_run_wall_s, name, on_finish_analyze (+10 more)

### Community 331 - "ClippedVoronoiExportStats"
Cohesion: 0.09
Nodes (24): ClippedVoronoiExport, mesh, site_to_cell, stats, ClippedVoronoiExportStats, domain_clip_used, geogram_ok, n_boundary_faces (+16 more)

### Community 332 - "ElementHpDecision"
Cohesion: 0.18
Nodes (11): HpAction, ElementHpDecision, action, h_next, p_next, reason, shape, utility_h (+3 more)

### Community 333 - "hp_driver.cpp"
Cohesion: 0.25
Nodes (13): best_shape_vote(), clamp01(), ShapeTendency, string, Vector3d, decide_element(), drive_hp(), geometry_severity() (+5 more)

### Community 334 - "max_abs_diff"
Cohesion: 0.50
Nodes (4): vector, VectorXd, max_abs_diff(), random_vector()

### Community 335 - ".size"
Cohesion: 0.04
Nodes (70): ExactPoint, IT, BalancedKdTree::build_tree(), BooleanExpression::operator()(), Cavity(), CDT2d::insert(), compute_BRIO_order(), compute_BRIO_order_periodic() (+62 more)

### Community 336 - "promote_truth.py"
Cohesion: 0.14
Nodes (29): best_rows(), check_promotable(), main(), measured_by_metric(), parse_args(), promote(), provenance(), Any (+21 more)

### Community 337 - "PeriodicVertexArray3d"
Cohesion: 0.14
Nodes (11): AdaptiveKdTree::plane_split(), Hilbert_vcmp_periodic<COORD, false, PeriodicVertexMesh3d>, Hilbert_vcmp_periodic<COORD, true, PeriodicVertexMesh3d>, PeriodicVertexArray3d, base_, nb_real_vertices_, nb_vertices_, stride_ (+3 more)

### Community 338 - "SolveDecision"
Cohesion: 0.29
Nodes (7): SolveMethod, string, uint64_t, SolveDecision, estimated_bytes, method, note

### Community 339 - "testlab/main.cpp"
Cohesion: 0.16
Nodes (35): AdvisorScorer, advisor_, cfg_id_of(), cmd_pause_status(), cmd_validate(), completed_keys(), map, path (+27 more)

### Community 340 - "Geogram / restricted CVT — vendoring study path"
Cohesion: 0.15
Nodes (13): 1. Why Geogram BSD-3 (not clean-room clipped Voronoi), 2.1 Vendor from Geogram (BSD-3), 2.2 We write ourselves, 2.3 Dual hard-block, 2. What to vendor vs what we write, 3. Dependency order (do not invent another), 4. Packing context (how this sits in varyhedron), 5. Vendored `third_party/` layout (+5 more)

### Community 341 - "solve_rung"
Cohesion: 0.12
Nodes (28): Case, consistent_face_loads(), cross_check(), dat_block(), face_geometry(), free_faces(), in_box(), match_by_coordinates() (+20 more)

### Community 342 - "dorfler_mark"
Cohesion: 0.43
Nodes (7): size_t, vector, Vector3d, dorfler_coarsen_mark(), dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth()

### Community 343 - "ADR-0021: Varyhedron — variable polyhedral packing mesher"
Cohesion: 0.29
Nodes (6): ADR-0021: Varyhedron — variable polyhedral packing mesher, Alternatives rejected, Consequences, Context, Decision, Research anchors

### Community 344 - "M-A1 — first trained advisor (2026-08-10)"
Cohesion: 0.07
Nodes (29): 0003 — Training log, Batch 1, Capacity was not the problem, Corpus and ground truth, Corpus widened for power, not coverage, Data, Data and artifact, Does it choose a better mesh than the default? (+21 more)

### Community 345 - "PolyCell"
Cohesion: 0.40
Nodes (5): uint32_t, vector, PolyCell, faces, nodes

### Community 346 - "ProbeAnswers"
Cohesion: 0.06
Nodes (34): LoadAreaStatus, optional, evaluate_probe(), ProbeAnswers, authored_area_checked, authored_area_consistent, authored_area_rel_diff, dominant_load_axis (+26 more)

### Community 347 - "Edge"
Cohesion: 0.20
Nodes (12): AtomicEdge, direction, from, key, to, Edge, a, b (+4 more)

### Community 348 - "make_hp_signals"
Cohesion: 0.48
Nodes (7): at_or_broadcast(), at_or_broadcast_int(), size_t, span, vector, estimate_surplus_from_zz(), make_hp_signals()

### Community 349 - "MixedFillOutput"
Cohesion: 0.07
Nodes (29): size_t, Vector3d, MixedFillOutput, boundary_max_distance, boundary_quads, cells, classification_refinement_levels, classification_volume_error (+21 more)

### Community 350 - "ADR-0020: True BRep volume meshing (product path)"
Cohesion: 0.33
Nodes (5): ADR-0020: True BRep volume meshing (product path), Alternatives rejected, Consequences, Context, Decision

### Community 351 - "index_t"
Cohesion: 0.08
Nodes (19): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CDTBase2d::check_edge_intersections(), CompactSpinLockArray, spinlocks_, Delaunay2d(), geo_pause() (+11 more)

### Community 352 - "ConstrainedLloydParams"
Cohesion: 0.12
Nodes (17): CvtLloydParams, h_floor, max_iters, move_tol_rel, size_at, SizeFieldFn, ConstrainedLloydParams, lloyd (+9 more)

### Community 353 - ".empty"
Cohesion: 0.09
Nodes (26): ConvexCellFlags, Frame, begin_task(), cancel(), ConvexCell::compute_geometry(), ConvexCell::compute_mg(), ConvexCell::ConvexCell(), ConvexCell::facet_area() (+18 more)

### Community 354 - "backend.cpp"
Cohesion: 0.39
Nodes (8): backend_description(), string, init_runtime_performance(), openmp_default_threads(), openmp_enabled(), openmp_max_threads(), performance_description(), set_openmp_threads()

### Community 355 - "RuntimeError"
Cohesion: 0.24
Nodes (24): RuntimeError, _bbox(), _cell_type_summary(), _dependency_version(), _deterministic_subsample(), _distance_statistics(), _exact_point_distances(), _file_snapshot() (+16 more)

### Community 356 - "ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)"
Cohesion: 0.20
Nodes (10): 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3), 2. Dual hard-block (ADR-0024 Q8), 3. `third_party/` plan, 4. Order (do not invent another), ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block), Alternatives rejected, Consequences, Context (+2 more)

### Community 357 - "VolumeMeshOutput"
Cohesion: 0.09
Nodes (24): map, uint32_t, join_worker, note_mesh_stats, set_status, VolumeMeshOutput, boundary_node_region, boundary_quads (+16 more)

### Community 358 - "JobProgress"
Cohesion: 0.10
Nodes (21): GeometryVolumeAssessment, available, cad_volume, mesh_volume, relative_error, GeometryVolumeLimitError, assessment, solved_stage (+13 more)

### Community 359 - "Feedback loop — campaign → defaults"
Cohesion: 0.40
Nodes (5): Feedback loop — campaign → defaults, Final findings (settings-frontier-1, finished), Procedure after campaign finishes, Tooling, When to change product defaults

### Community 360 - "wall_tangential_project"
Cohesion: 0.11
Nodes (22): size_t, WallProjectStats, max_surface_residual, mean_surface_residual, n_iters, n_moved, n_reverted, n_wall_nodes (+14 more)

### Community 361 - "Grok improvement handoff — `varyhedron-short-1`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-short-1`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 362 - "Varyhedron packing — algorithm survey (V5)"
Cohesion: 0.07
Nodes (28): 0. Normative ranking (ADR-0023 / plan — do not ignore), 1. Goals (from ADR-0021), 2. Bubble / sphere packing → Delaunay, 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage), 4. Field-aligned hex-dominant (PGP3D-class), 5. CAD edge protecting balls / PLC constraints, 6. Licensing notes (core vs plugin), 7. Decision: v1 algorithm (+20 more)

### Community 363 - "graded_tet_fill_surface"
Cohesion: 0.14
Nodes (24): size_t, span, vector, Vector3d, flat_idx(), stamp_ball(), stamp_curvature_cells(), stamp_feature_cells() (+16 more)

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
Cohesion: 0.06
Nodes (40): CreatorType, GEO_NODISCARD, int16, int8, Param1, Registry, char_to_string(), create() (+32 more)

### Community 368 - "CvtLloydStats"
Cohesion: 0.12
Nodes (16): CvtLloydResult, positions, stats, CvtLloydStats, converged, domain_diag, geogram_ok, max_move (+8 more)

### Community 369 - "Prior art: ML for mesh generation and adaptive refinement"
Cohesion: 0.08
Nodes (23): 1. MeshingNet and MeshingNet3D — the closest direct analogue, 2. Reinforcement learning for AMR, 3. GNN/neural error estimators and surrogate indicators, 4. Learned anisotropic metric fields, 5. Commercial/industrial state of the art, 6. Code/weights availability at a glance, Bottom line, E2N: Error Estimation Networks for Goal-Oriented Mesh Adaptation (+15 more)

### Community 370 - "structured_mesh.hpp"
Cohesion: 0.03
Nodes (59): ElementCentroidStress, centroid, element_index, quality, stress, volume, Stress, uint32_t (+51 more)

### Community 371 - "run_one"
Cohesion: 0.12
Nodes (21): action_json(), case_features_json(), compute_scorecard_geom(), json, detect_hmin_features(), geo_fidelity_of(), geom_class_of(), HminFeatureReport (+13 more)

### Community 372 - "brep_fidelity.cpp"
Cohesion: 0.40
Nodes (15): append_triangle(), boundary_surface(), boundary_surface_volume(), brep_fidelity_summary(), FreeFace, size_t, uint32_t, vector (+7 more)

### Community 373 - "resolve_campaign"
Cohesion: 0.67
Nodes (3): main(), Path, resolve_campaign()

### Community 375 - "intervalBase"
Cohesion: 0.07
Nodes (14): Sign2, control_add(), control_check(), control_mul(), control_set(), control_sub(), intervalBase, intervalDummy (+6 more)

### Community 376 - "Checkpoint"
Cohesion: 0.22
Nodes (9): Checkpoint, campaign, completed_runs, hooks_failed, started_utc, state, survivors, tier (+1 more)

### Community 377 - "calibration.py"
Cohesion: 0.19
Nodes (21): choose_threshold(), conformal_report(), fit_ood(), fold_report(), main(), ood_scores(), Any, ndarray (+13 more)

### Community 378 - "rebuild_results.py"
Cohesion: 0.18
Nodes (21): main(), parse_args(), part_names(), plan_order(), prefix_divergence(), Any, Namespace, Path (+13 more)

### Community 379 - "Grok improvement handoff — `varyhedron-baseline-m9`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-baseline-m9`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 380 - "probe_util.hpp"
Cohesion: 0.23
Nodes (11): combine_mesher_notes(), dominant_axis(), face_mean_displacement_component(), face_mean_displacement_mag(), global_max_displacement_mag(), size_t, string, uint32_t (+3 more)

### Community 381 - "progress.md"
Cohesion: 0.14
Nodes (9): Active program (do not skip), graphify, Polyhedral-FEA — agent notes, Showcase asset index, ADR-0032: The mesh may not depend on which standard library built it, Consequences, Context, Decision (+1 more)

### Community 382 - "Path"
Cohesion: 0.13
Nodes (22): _charmap(), _covers(), digest(), _digest_file(), finish(), font_path(), footer_source(), git_revision() (+14 more)

### Community 383 - "PROGRESS"
Cohesion: 0.33
Nodes (6): Active (read this first), Background / older phases, Benchmark table, Done, Open issues, PROGRESS

### Community 389 - "Campaign metrics — normative definitions for agents"
Cohesion: 0.22
Nodes (9): 1. Score vs dashboard vs gate, 2. Minimum scorecard (five numbers + residual gate), 3. Case-specific accuracy scores, 4. Chordal efficiency \(e\), 5. Gates and kills (not scores), 6. Displacement probes, 7. Agent checklist before claiming a campaign “win”, Campaign metrics — normative definitions for agents (+1 more)

### Community 390 - "RefinementPlan"
Cohesion: 0.10
Nodes (22): size_t, SizeFieldFn, vector, RefinementPlan, h_fine, h_min, n_bc_seeds, n_geometry_seeds (+14 more)

### Community 391 - "Triangle"
Cohesion: 0.23
Nodes (11): ConvexCell::connect_triangles(), ConvexCell::triangle_is_in_conflict(), ushort, make_triangle(), make_triangle_with_flags(), Triangle, i, j (+3 more)

### Community 392 - "Predicates_psm.h"
Cohesion: 0.09
Nodes (37): aligned_3d(), aligned_3d_exact(), det2x2(), det3x3(), det4x4(), geo_clamp(), geo_cmp(), geo_sgn() (+29 more)

### Community 393 - "TriSurface"
Cohesion: 0.06
Nodes (47): vector, VertexCurvature, kappa, VertexThickness, thickness, array, uint32_t, vector (+39 more)

### Community 394 - "manifest.json"
Cohesion: 0.50
Nodes (3): generated_utc, git_rev, images

### Community 395 - "Protecting balls + local feature size (LFS)"
Cohesion: 0.33
Nodes (6): 1. Role, 2. CDS radius formula (must-change), 3. Reference, 4. Risk cases, 5. Agent checklist, Protecting balls + local feature size (LFS)

### Community 396 - "SolveResult"
Cohesion: 0.08
Nodes (26): BoundaryConditions, dirichlet, loads, array, VectorXd, SolveResult, boundary_quads, displacement (+18 more)

### Community 397 - "pointer_"
Cohesion: 0.08
Nodes (25): function_pointer, aligned_allocator, ALIGNMENT, aligned_free(), aligned_malloc(), A2, const_pointer, const_reference (+17 more)

### Community 398 - "operator=="
Cohesion: 0.12
Nodes (21): result, mat4, M, A1, DIM, FT, initializer_list, matrix_type (+13 more)

### Community 399 - "BRep face-tag BCs / probes (design stub)"
Cohesion: 0.33
Nodes (6): BRep face-tag BCs / probes (design stub), Exit criteria (future work item), Historical icecream instability (superseded fixture; why face tags), Out of scope for this stub, Target model (sketch), Why boxes are temporary

### Community 400 - "vec4"
Cohesion: 0.06
Nodes (41): Attribute, IncidentTetrahedra, ostream, barycenter(), ConvexCell::append_to_mesh(), ConvexCell::clip_by_plane(), ConvexCell::clip_by_plane_fast(), ConvexCell::for_each_Voronoi_vertex() (+33 more)

### Community 401 - "ProjectResult"
Cohesion: 0.12
Nodes (17): BRepSurfaceSamples, face_count, face_ids, fallback_vertex_count, points, uv_attempt_count, CadSupportKind, size_t (+9 more)

### Community 402 - "Variable-everything idea bank"
Cohesion: 0.10
Nodes (20): 10. Additional axes worth varying, 1. A2 + A1 + A4: Mmg3d-backed solution-driven metric adaptation, 1. Size / density, 2. Anisotropy, 2. O1 + O2: productionize the existing hierarchical HpModel, 3. G1 + G2: independently variable curved CAD geometry order, 3. Polynomial order, 4. Element shape / topology (+12 more)

### Community 403 - "declare_arg"
Cohesion: 0.12
Nodes (35): ArgFlags, ArgType, GroupArgs, Arg, desc, flags, arg_group(), name (+27 more)

### Community 404 - "Split"
Cohesion: 0.16
Nodes (18): One side of the part-hash split, fully materialized as numpy arrays., Return a row-filtered copy (used to apply the pruning ledger)., Split, head_residuals(), keep_mask(), load_ledger(), main(), prune_run() (+10 more)

### Community 405 - "boundary_faces.cpp"
Cohesion: 0.10
Nodes (58): EdgeOwners, Loop, NodeNeighbors, atomize_loop_edges(), collect_element_loops(), array, map, set (+50 more)

### Community 406 - "ReferenceCase"
Cohesion: 0.23
Nodes (11): BenchError, runtime_error, string, ReferenceCase, citation, name, values, path (+3 more)

### Community 407 - "BRepInspection"
Cohesion: 0.17
Nodes (12): BRepInspection, available, closed, closed_shell_count, edge_count, face_count, shell_count, solid_count (+4 more)

### Community 408 - "expansion"
Cohesion: 0.19
Nodes (20): compress_expansion(), expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 409 - "Matrix"
Cohesion: 0.18
Nodes (7): FT, initializer_list, matrix_type, Matrix, coeff_, dim, mult()

### Community 410 - "expansion"
Cohesion: 0.17
Nodes (20): expansion, compress_expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 411 - "GradedTetFillOutput"
Cohesion: 0.09
Nodes (24): GradedTetFillOutput, classification_refinement_levels, classification_volume_error, field_h_max, field_h_min, h_coarse, h_fine, mesh (+16 more)

### Community 414 - "Any"
Cohesion: 0.11
Nodes (19): annotate_n(), assert_glyphs(), axes_off(), figure(), panel_title(), Any, ratio_bars(), Force a common y-range across panels, or record why not. (+11 more)

### Community 416 - "IndexType"
Cohesion: 0.21
Nodes (8): IndexType, KeepOrderType, basic_bindex, indices, basic_quadindex, indices, basic_trindex, indices

### Community 417 - "cli/main.cpp"
Cohesion: 0.31
Nodes (20): cmd_check(), cmd_diag(), cmd_mesh(), cmd_solve(), constraint_defect(), count_boundary_nodes(), size_t, span (+12 more)

### Community 418 - "percent"
Cohesion: 0.67
Nodes (3): int64, percent(), PredicateStats::show_stats()

### Community 419 - "compile"
Cohesion: 0.47
Nodes (6): bind_line_attr(), compile(), link(), init, GLenum, GLuint

### Community 420 - "ClipPlane"
Cohesion: 0.09
Nodes (24): ClippedCell, barycenter, empty, n_planes, n_triangles, volume, ClipPlane, a (+16 more)

### Community 421 - "varyhedron_fill_surface"
Cohesion: 0.30
Nodes (17): boundary_nodes(), bubble_relax_volume(), array, span, uint32_t, vector, Vector3d, far_enough() (+9 more)

### Community 422 - "Geometry"
Cohesion: 0.17
Nodes (19): build_geometry(), case_json(), fix_box(), generate(), Geometry, kirsch_reference(), load_box(), main() (+11 more)

### Community 423 - "AdvisorDecision"
Cohesion: 0.10
Nodes (21): AdvisorDecision, adapt_passes, budget_refusal, clamped, eta_target, failure_prob, h_rel, mesher (+13 more)

### Community 424 - "HexFillOutput"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 425 - "properties"
Cohesion: 0.10
Nodes (20): type, type, enum, cad_face, detected_from, kind, recommended_h_m, refused_at_h_m (+12 more)

### Community 426 - "advisor.cpp"
Cohesion: 0.21
Nodes (18): Advisor::apply_action(), Advisor::evaluate(), apply_action, encode, mahalanobis, run, Advisor::recommend(), argmax() (+10 more)

### Community 427 - "GroupBoxFrame"
Cohesion: 0.40
Nodes (5): GroupBoxFrame, fixed_content_h, start, title, width

### Community 428 - "properties"
Cohesion: 0.08
Nodes (26): description, type, description, type, type, description, type, type (+18 more)

### Community 429 - "figstyle.py"
Cohesion: 0.15
Nodes (18): clamp_to_floor(), field_cmap(), field_lut(), font_px(), _gui_lut(), pil_font(), _preregister(), ndarray (+10 more)

### Community 430 - "CommandLineDesc"
Cohesion: 0.25
Nodes (8): Args, GroupNames, Groups, CommandLineDesc, args, argv0, group_names, groups

### Community 431 - "test_quadrature.cpp"
Cohesion: 0.27
Nodes (7): affine_image(), ElementType, Matrix3d, vector, Vector3d, integrate(), isoparametric_volume()

### Community 432 - "coord_index_t"
Cohesion: 0.05
Nodes (34): AdaptiveKdTree::AdaptiveKdTree(), AdaptiveKdTree::get_node(), BalancedKdTree::BalancedKdTree(), BalancedKdTree::get_node(), BalancedKdTree::split_kd_node(), ComparePointCoord, nb_points_, points_ (+26 more)

### Community 433 - "plot_hole_bug.py"
Cohesion: 0.16
Nodes (17): cad_bore_volume(), classify(), draw_panel(), load_mesh(), main(), Mesh, mesh_volume(), parse_args() (+9 more)

### Community 434 - "Box"
Cohesion: 0.22
Nodes (8): bbox_union(), bboxes_overlap(), Box, Box2d, xy_max, xy_min, xyz_max, xyz_min

### Community 435 - "SharpEdge"
Cohesion: 0.09
Nodes (34): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector (+26 more)

### Community 436 - "DomainTet"
Cohesion: 0.29
Nodes (7): DomainTet, centroid, v0, v1, v2, v3, Vector3d

### Community 437 - "main"
Cohesion: 0.50
Nodes (8): git_provenance(), main(), Path, repo_path(), run_diag(), sha256(), summary(), validate_fidelity()

### Community 438 - "plot_truth_independence.py"
Cohesion: 0.19
Nodes (17): build(), finding(), load(), main(), panel_box_hole(), panel_convergence(), panel_identities(), panel_stepped() (+9 more)

### Community 439 - "wall_time_s"
Cohesion: 0.29
Nodes (7): wall_time_s, additionalProperties, required, type, mesh, solve, total

### Community 440 - "GeometryDescriptors"
Cohesion: 0.11
Nodes (17): GeometryDescriptors, area_over_v23, aspect_max, aspect_mid, available, curved_area_frac, cyl_area_frac, face_area_cv (+9 more)

### Community 441 - "settings-frontier-1 — campaign-1 close-out"
Cohesion: 0.33
Nodes (5): Caveat, Product default decision (feedback-loop), settings-frontier-1 — campaign-1 close-out, Survivors (tier-2 keep), Tooling top-score cfg (global ranking)

### Community 442 - "figures.py"
Cohesion: 0.26
Nodes (16): activation_map(), epoch_series(), _finding(), load_json(), main(), parse_args(), _pct(), Any (+8 more)

### Community 443 - "recover_nodal_stress"
Cohesion: 0.23
Nodes (15): A, p, ElementType, Matrix, Stress, vector, Vector3d, VectorXd (+7 more)

### Community 444 - "test_brep_fidelity.cpp"
Cohesion: 0.23
Nodes (15): BoundaryQuad, boundary_nodes(), boundary_quadratic_mids(), array, span, uint32_t, vector, Vector3d (+7 more)

### Community 445 - "M5 VEM gate — campaign results (2026-07-13)"
Cohesion: 0.29
Nodes (7): Campaign, Hard-learned (do not re-open without new evidence), History, M5 VEM gate — campaign results (2026-07-13), Next to flip M5 → done, Results (latest: VEM τ=0.08 + plate-only wall + cylinder shell sites + OCC snap), What landed (code)

### Community 446 - "OmpSettingsGuard"
Cohesion: 0.40
Nodes (3): OmpSettingsGuard, dynamic, threads

### Community 447 - "LocalRefineStats"
Cohesion: 0.22
Nodes (9): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets, n_skipped_slivers (+1 more)

### Community 461 - "load_area.hpp"
Cohesion: 0.10
Nodes (21): assess_load_area(), AuthoredAreaCheck, checked, consistent, rel_diff, check_authored_area(), LoadAreaStatus, optional (+13 more)

### Community 465 - "Decision"
Cohesion: 0.12
Nodes (16): 10. ZZ patch recovery may not extrapolate an under-determined fit, 1. Project quadratic boundary mid-edge nodes onto exact CAD, 2. The validity guard follows stiffness quadrature, 3. Do not ship the void-jut repair, 4. A peak truth requires a peak probe, 5. Render topology, not raw connectivity order, 6. Every order-2 producer projects, 7. Engine changes require a row-schema bump and retraining (+8 more)

### Community 466 - "0004 — Model card: learned mesh advisor"
Cohesion: 0.13
Nodes (15): 0004 — Model card: learned mesh advisor, Decision quality, Deployment cost, Evaluation harness, Intended use, Known failure modes, Out-of-scope use, Per-head accuracy (+7 more)

### Community 467 - "0005 — Data card: advisor training corpus"
Cohesion: 0.13
Nodes (15): 0005 — Data card: advisor training corpus, Action grid, Composition, Consequence for interpretation, Coverage and gaps, Ethics and risk, How the corpus splits, and what it costs, Known defects in the features (+7 more)

### Community 468 - "test_d6_bench_smoke.cpp"
Cohesion: 0.47
Nodes (4): string, run_cmd(), slurp(), temp_out_path()

### Community 469 - "mean_lateral_radial_residual"
Cohesion: 0.40
Nodes (5): array, uint32_t, vector, Vector3d, mean_lateral_radial_residual()

### Community 470 - "WindowsThreadPoolManager"
Cohesion: 0.13
Nodes (13): LONG, PTP_CALLBACK_INSTANCE, PTP_CLEANUP_GROUP, PTP_POOL, PTP_WORK, PVOID, WindowsThreadPoolManager, cbe_ (+5 more)

### Community 473 - "MixedCell"
Cohesion: 0.15
Nodes (14): MixedCellKind, array, uint32_t, uint8_t, vector, MixedCell, kind, n_nodes (+6 more)

### Community 474 - "detect_hole_roi"
Cohesion: 0.15
Nodes (14): bbox_of(), _cell_centroids(), detect_hole_roi(), _empty_core(), _occupancy(), Densest in-plane radial band of free-surface nodes about ``axis``. Returns…, Rasterise the mid-slab normal to ``axis``, resolving ``radius``. Returns (grid,…, Does the void at the candidate radius reach the outside of the part? A bore… (+6 more)

### Community 475 - "Decision"
Cohesion: 0.15
Nodes (13): 1. Learn action-conditioned outcomes, never a "best config" label, 2. A second head predicts the sizing field, 3. Context features are scale-free and cheap, 4. Reference truth, 5. BC sampling varies topology, not magnitude, 6. Corpus and licensing, 7. Deployment: LightGBM C API, no Python at runtime, 8. The advisor proposes; the estimator disposes (+5 more)

### Community 476 - "Decision"
Cohesion: 0.15
Nodes (13): 1. Truth comes from outside this engine, 2. Applied load may not depend on mesh resolution, 3. One rule has one implementation, 4. A check that cannot verify must not report success, 5. An artifact write may not kill a run, 6. Evaluate the rule you ship, on a split that does not leak, 7. Promotion may only overwrite truth this repo generated, 8. Accuracy is re-derived at build time, not frozen into rows (+5 more)

### Community 477 - "Decision"
Cohesion: 0.15
Nodes (13): 1. A quadrature rule is defined by the shape functions it integrates, and a test must say so, 2. A retracted cause is deleted, not reworded, 3. Display and physics are two different boundary contracts, 4. One definition of a cell's volume, 5. Geometry may only be deleted if the boundary survives it, ADR-0030: The ruler was wrong — retracting the fan-transition defect, and drawing the curvature we already compute, Consequences, Context (+5 more)

### Community 478 - "SampleDistribution"
Cohesion: 0.17
Nodes (12): DistanceDistribution, metres, over_bbox_diagonal, over_h, size_t, SampleDistribution, count, max (+4 more)

### Community 480 - "FeatureAwareClassification"
Cohesion: 0.15
Nodes (13): FeatureAwareClassification, child_inside_mask, classified_volume, coarse_inside, grid, inside, n_mixed_cells, refinement_levels (+5 more)

### Community 481 - "test_advisor_inference.cpp"
Cohesion: 0.17
Nodes (10): columns_of(), FeatureColumns, json, path, load(), Scored, action, dof (+2 more)

### Community 482 - "BcSelection"
Cohesion: 0.20
Nodes (12): BcSelection, face_fallback, faces, fallback_band, from_box, nodes, slab_nodes, string (+4 more)

### Community 483 - "ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening"
Cohesion: 0.17
Nodes (11): 1. Spectral sizing (`adapt::spectral`, new module), 2. Coarsening (`HpAction::kCoarsen` + loop executor), 3. Budget-feasible advisor chooser, 4. CG equilibration, 5. Advisor hygiene (measured, no retrain), ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening, Consequences, Context (+3 more)

### Community 484 - "convergence"
Cohesion: 0.20
Nodes (10): Line2D, convergence(), Fit, fit_loglog(), Three points is the minimum honest basis for a slope., Plot a convergence series and state its measured slope. With fewer than three…, Style for a named series. Unregistered names get a stable hashed slot., Legend handles that show colour *and* marker *and* dash together. (+2 more)

### Community 485 - "Dirichlet"
Cohesion: 0.25
Nodes (11): uint64_t, dirichlet_node_set_hash(), hash_mix(), load_vector_hash(), selected_face_set_hash(), selected_node_set_hash(), Dirichlet, dof_values (+3 more)

### Community 486 - "0001 — Advisor architecture"
Cohesion: 0.18
Nodes (11): 0001 — Advisor architecture, CLI, Data path, Deployment, Feature and action schema, Heads, Policy head layout, Two-pass inference (+3 more)

### Community 487 - "Top-5 shortlist with implementation sketches"
Cohesion: 0.18
Nodes (10): 1. WL-01 — MMS compiler and observed-order gates, 2. WL-02 — conforming hierarchical $hp$ production cutover, 3. WL-03 — DWR QoI adaptivity, 4. WL-04 — exact/curved CAD boundary geometry, 5. WL-05 — certified QoI intervals, Do not bother—at least not yet, Outside-the-box leverage across the whole program, Ranked idea bank (+2 more)

### Community 488 - "plot_load_deficit.py"
Cohesion: 0.36
Nodes (10): correction_stats(), load_audit(), load_block(), main(), Path, Predicted vs observed change, with the 1:1 line., Per-metric old/new truth rows plus the headline block we cross-check against., Recomputed from the per-metric rows. Nothing here is typed in. (+2 more)

### Community 489 - "main"
Cohesion: 0.33
Nodes (10): campaign_json(), main(), Any, CompletedProcess, Path, One coarse config on one part: the cheapest campaign that still writes…, The newest built testlab binary, or None when the tree has no build. Windows…, rows() (+2 more)

### Community 490 - "vtu_wire_png.py"
Cohesion: 0.29
Nodes (9): boundary_edges(), cell_faces(), convex_hull_faces(), face_key(), _fixed_faces(), _ordered_planar_hull(), _polyhedron_face_blocks(), Order a planar facet's corner nodes, dropping collinear/interior nodes. (+1 more)

### Community 491 - "AdvisorError"
Cohesion: 0.45
Nodes (11): AdvisorError, runtime_error, Advisor::Advisor(), load_clamps, load_normalization, load_ood, json, path (+3 more)

### Community 492 - "AdvisorRawOutputs"
Cohesion: 0.18
Nodes (11): AdvisorRawOutputs, dof_log10, failure_logit, geo_chamfer_log10, geo_p99_log10, mesh_ms_log10, policy, rel_err_log10 (+3 more)

### Community 493 - "BrepFidelitySummary"
Cohesion: 0.18
Nodes (11): BrepFidelitySummary, available, chamfer_mean, dist_max, dist_p95, dist_p99, n_samples, normal_angle_p95_rad (+3 more)

### Community 494 - "0002 — Objectives and guardrails"
Cohesion: 0.20
Nodes (10): 0002 — Objectives and guardrails, 1. Penalty barrier — during training, 2. Hard clamps — at inference, 3. Feasibility veto — at inference, Guardrails, Loss, Outlier pruning, Staged curriculum (+2 more)

### Community 495 - "LAN access — `hunter-pc` (the 3080 Ti box)"
Cohesion: 0.20
Nodes (10): 1. Destination, 2. Host key fingerprints — verify these on first connect, 3. Enrolling a key — this is what "it's not working" was, 4.1 Resolved 2026-08-14 — gcc and MSVC do not agree on every mesh, 4. Verified state of the box (2026-08-14), 5. Notes for long unattended runs, 6. Launching a batch on this box, LAN access — `hunter-pc` (the 3080 Ti box) (+2 more)

### Community 496 - "2. Training tracks (all four selected, in dependency order)"
Cohesion: 0.20
Nodes (10): 0. What the box needs (bring-up checklist), 1.1 v4 regeneration — COMPLETE 2026-08-14, gcc only, 1. Campaign regeneration (decided: everything, after the tangle fix), 2. Training tracks (all four selected, in dependency order), 2a. Retrain the current advisor (first, cheap, de-risks the pipeline), 2b. Learned error estimator / h-selector (second), 2c. Per-region size field GNN (the flagship, 1–3 day runs), 2d. Learned repair policy (research-grade, LAST) (+2 more)

### Community 497 - "VtuPointData"
Cohesion: 0.22
Nodes (10): string, vector, VectorXd, VtuCellData, name, scalars, VtuPointData, name (+2 more)

### Community 498 - "write_vtu"
Cohesion: 0.36
Nodes (9): ElementType, path, uint32_t, vector, poly_face_locals(), tet4_cell_quality(), usable_faces(), vtk_cell_type() (+1 more)

### Community 499 - "build_loads"
Cohesion: 0.22
Nodes (9): build_loads(), FILE, uint32_t, VectorXd, LoadSpec, dir, force, traction_mode (+1 more)

### Community 500 - "CgAttempt"
Cohesion: 0.22
Nodes (9): CgStop, cg_stop_text(), CgAttempt, iterations, reliable_restarts, stop, true_relative_residual, x (+1 more)

### Community 501 - "0008 — The v4 corpus, the retrain, and the metric that punished being right"
Cohesion: 0.22
Nodes (9): 0008 — The v4 corpus, the retrain, and the metric that punished being right, 1. Why there is a v4 at all, 2. The retrain, 3. Decision quality: the advisor now leads on every reference it should, 4.1 The advisor was right and the label was wrong, 4.2 The macro mean still needs a fold-size guard, 4. The open question from 0006 §4, answered: one fold was eating the mean, 5. The tolerance selector: much closer, still not deliverable (+1 more)

### Community 502 - "ADR-0033: A gate must measure what ships"
Cohesion: 0.22
Nodes (9): 1. The pyramid gate measured a different cell than the diagnostics, 2. `hex_fill` gated on sampled signs, and nothing else, 3. `snap_boundary_nodes` computed a whole-mesh proof and threw it away, ADR-0033: A gate must measure what ships, Consequences, Context, Decision, Open: the ellipsoidal boss, and where the wall gets stuck (+1 more)

### Community 503 - "Public CAD corpora for training a mesh advisor"
Cohesion: 0.22
Nodes (8): Commercial-development track, Concrete ingestion and labeling rules for PolyMesh, Dataset evaluation, Non-commercial research benchmark add-on, Public CAD corpora for training a mesh advisor, Recommended starter corpus for one workstation, The datasets that actually pair geometry, BCs, and structural FEA, What this means for PolyMesh

### Community 504 - "_healed"
Cohesion: 0.25
Nodes (9): _closed_spline_wire(), _ellipsoid(), _extruded_section(), _healed(), _lofted_sections(), Sphere scaled anisotropically — a genuine non-spherical curved surface. Every…, Periodic interpolating B-spline through `points` as one closed wire.…, Prism a closed spline section along +x into a solid. (+1 more)

### Community 505 - "Advisor"
Cohesion: 0.22
Nodes (8): Advisor, apply_action, defaults, evaluate, impl_, recommend, Impl, unique_ptr

### Community 506 - "ADR-0031: A jut has a side"
Cohesion: 0.25
Nodes (7): ADR-0031: A jut has a side, Consequences, Context, Decision, The defect, The test, What this does not fix

### Community 507 - "colorbar"
Cohesion: 0.29
Nodes (8): FuncFormatter, colorbar(), Colourbar in SI units. Refuses to draw without a unit string., 3.84e6, 'Pa' -> '3.84 MPa'. Exponent soup is unreadable on a colourbar., Pick one SI scale for a whole axis/colourbar from its top value., si(), si_prefix(), unit_formatter()

### Community 508 - "GeometryCompleteness"
Cohesion: 0.25
Nodes (8): GeometryCompleteness, available, brep_volume, complete, mesh_volume, relative_volume_error, relative_volume_tolerance, evaluate_geometry_completeness()

### Community 509 - "ElementTendencyPlan"
Cohesion: 0.25
Nodes (8): ElementTendencyPlan, label, mesher, native_poly_transitions, remapped, skin_layers, tendency, VolumeMesher

### Community 510 - "BoxSel"
Cohesion: 0.33
Nodes (7): BoxSel, hi, lo, set, cad_pressure_area(), optional, Vector3d

### Community 511 - "run_artifacts.hpp"
Cohesion: 0.48
Nodes (6): atomic_write(), path, string, is_transient_rename_error(), write_run_json(), error_code

### Community 512 - "enum"
Cohesion: 0.29
Nodes (7): status, description, enum, failed, ok, refused, timeout

### Community 513 - "0006 — The clean-data retrain, and what it cost the advisor's claims"
Cohesion: 0.29
Nodes (7): 0006 — The clean-data retrain, and what it cost the advisor's claims, 1. The old labels were a different mesher, not a stale one, 2. `latest.pt` was never the model worth shipping, 3. The cost heads were missing the scale law, 4.1 The obvious fix for it was tried and is wrong, 4. What the retrain did to the product claim, 5. Provenance

### Community 514 - "check"
Cohesion: 0.33
Nodes (7): _boxes_intersect(), check(), occ_bbox(), Path, Axis-aligned bounds straight from OpenCASCADE (used by --check only)., read_step(), write_json()

### Community 515 - "MshModel"
Cohesion: 0.29
Nodes (7): map, string, vector, MshModel, mesh, physical_faces, physical_names

### Community 516 - "0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number"
Cohesion: 0.33
Nodes (6): 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number, 1. The track asked for a deliverable, not a model, 2. The measurement, 3. A safety margin does not fix it, and the way it fails is the finding, 4. What ships, and what does not, 5. Provenance

### Community 517 - "LogLimits"
Cohesion: 0.33
Nodes (4): loglim(), LogLimits, What ``loglim`` had to do to the data, so the caller can say it., Set honest log limits, flooring the axis under the *bulk* of the data. Two…

### Community 518 - "LSolve"
Cohesion: 0.33
Nodes (6): VectorXd, LSolve, energy, mesh, peak_vm_at_corner, u

### Community 519 - "refusal"
Cohesion: 0.40
Nodes (5): refusal, description, required, type, kind

### Community 520 - "emit_transition_poly"
Cohesion: 0.40
Nodes (5): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, emit_transition_poly()

### Community 521 - "HoleROI"
Cohesion: 0.40
Nodes (4): HoleROI, An ROI plus whether a hole was actually found inside it. ``detected`` is the…, Build the ROI box about ``axis`` and package the verdict with it., _roi_for()

### Community 522 - "schema_version"
Cohesion: 0.50
Nodes (4): schema_version, const, description, type

### Community 523 - "land_contract_cutover.sh"
Cohesion: 0.83
Nodes (3): require(), say(), land_contract_cutover.sh script

### Community 524 - "stage_argv"
Cohesion: 0.67
Nodes (3): main(), Namespace, stage_argv()

### Community 525 - "DomainClipParams"
Cohesion: 0.50
Nodes (4): DomainClipParams, clip_radius, min_area_frac, surface

### Community 526 - "summarize_samples"
Cohesion: 0.67
Nodes (4): span, interpolated_quantile(), summarize_distances(), summarize_samples()

### Community 527 - "test_advisor_dataset_rederive.cpp"
Cohesion: 0.67
Nodes (3): string, python_exe(), run_python()

### Community 528 - "Case"
Cohesion: 0.50
Nodes (4): Case, h, name, path

### Community 529 - "test_truth_guard.cpp"
Cohesion: 0.67
Nodes (3): string, python_exe(), run_python()

### Community 530 - "host"
Cohesion: 0.67
Nodes (3): description, type, host

### Community 531 - "label"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 533 - "same_node_bytes"
Cohesion: 0.67
Nodes (3): vector, Vector3d, same_node_bytes()

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **2553 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+2548 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **185 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `thread()` connect `thread` to `gui/main.cpp`, `index_t`, `index_t`, `.size`, `testlab/main.cpp`, `T`, `ProgressHeartbeat`, `SolveJob`, `scene.cpp`, `Delaunay_psm.cpp`, `Delaunay_psm.h`, `test_brep_fidelity.cpp`, `vector`, `run_artifacts.hpp`?**
  _High betweenness centrality (0.033) - this node is a cross-community bridge._
- **Why does `Viewport` connect `Viewport` to `compile`, `App`, `viewport.cpp`, `Camera`?**
  _High betweenness centrality (0.026) - this node is a cross-community bridge._
- **Why does `NodalMesh` connect `NodalMesh` to `FeaError`, `CurvedMeshMetrics`, `MshModel`, `ManufacturedSolution`, `LSolve`, `PolyMesh`, `SolveResult`, `p_elevate.cpp`, `run_kirsch_annulus`, `solve_elastostatics`, `boundary_faces.cpp`, `.fix_node`, `d6_tier3.cpp`, `PassTrace`, `SurfaceFace`, `cli/main.cpp`, `NodalElement`, `eval_shape`, `traction.cpp`, `recover_nodal_stress`, `test_brep_fidelity.cpp`, `HpMode`, `HpSystem`, `KindCounts`, `assemble_body_load`, `scene.cpp`, `Grid3d`, `total_volume`, `VolumeMeshOutput`, `Dirichlet`, `write_vtu`, `build_loads`, `run_one`, `structured_mesh.hpp`, `resource_budget.cpp`?**
  _High betweenness centrality (0.022) - this node is a cross-community bridge._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _2553 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `FeaError` be split into smaller, more focused modules?**
  _Cohesion score 0.13188142770719904 - nodes in this community are weakly interconnected._
- **Should `analyze_campaign.py` be split into smaller, more focused modules?**
  _Cohesion score 0.11517165005537099 - nodes in this community are weakly interconnected._