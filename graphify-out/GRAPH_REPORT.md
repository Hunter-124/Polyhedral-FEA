# Graph Report - Polyhedral-FEA  (2026-09-25)

## Corpus Check
- 1396 files · ~4,196,728 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 10725 nodes · 22133 edges · 563 communities (419 shown, 144 thin omitted)
- Extraction: 94% EXTRACTED · 6% INFERRED · 0% AMBIGUOUS · INFERRED: 1271 edges (avg confidence: 0.79)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `04041cfa`
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
- T
- PolyMesh
- CinemaState
- GradedFillState
- MshModel
- index_t
- ReferenceCase
- TestLabState
- Advisor measure-first program
- AdaptSuggestion
- CantileverSetup
- solve_elastostatics
- CurvedGeometryResult
- fill_progress_poll
- Projection
- SolveJob
- d6_tier3.cpp
- string
- Predicates_psm.cpp
- TransitionFillOutput
- polymesh CMake Project
- gen_part_library.py
- CartesianGrid
- CadModel
- dataset.py
- Model
- Config
- metric_field.cpp
- CsrMatrix
- TriSurface
- Hand-calculated reference truths
- accuracy
- spectral_sizing.cpp
- Stats
- null
- NodalElement
- CalculiX / PolyMesh cantilever cross-validation
- POLYMESH_WITH_CUDA
- eval_shape
- run_one
- gen_cad_parts.py
- timestamp
- Goodier Spherical Cavity Case
- ADR-0001 Geometry kernel
- Pareto analysis — `varyhedron-baseline-m9`
- D6 Tier-3 L-domain instrument
- fea::Element unified trait
- PrismFillOutput
- export_clipped_voronoi
- SolveResourceEstimate
- Delaunay_psm.h
- CI Grep-Audit Anti-Cheat Job
- HpMode
- mathlib_probe.cpp
- BrepFaceIndex
- run_packing_microbench.py
- hierarchical.cpp
- run_tier3.py
- testlab_data.cpp
- Camera
- train.py
- volume_mesh_impl
- viewport.cpp
- Pareto analysis — `varyhedron-short-1`
- main
- ADR-0003 Element formulations
- ADR-0014 Dörfler seed remesh
- index_t
- assemble_hp
- assemble_body_load
- ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)
- CampaignSpec
- ADR-0004 Mesh data structure
- plot_benchmarks.py
- lame-cylinder case
- Material
- widgets.cpp
- interior_points
- PolicyObjective
- load_dataset
- render_cinema.py
- Grid3d
- emit_polymesh_gate1.py
- ResultRow
- Tier 3 Performance Benchmarks
- vector
- vecng
- vector
- test_local_refine.cpp
- run_mesh_public.sh
- run_solve_public.sh
- GradedSizing
- run_polymesh_smoke.sh
- P1 MMS Convergence Orders (tet4/hex8/tet10/hex20)
- POLYMESH_WITH_OCC
- POLYMESH_WITH_OPENMP
- Patch Test Is Sacred
- case_features.cpp
- cinema_draw.cpp
- Scorecard
- Layer Dependency Direction Rule
- resource_budget.cpp
- index_t
- CinemaCue
- feature_pin.cpp
- bench_harness library
- Variable-everything meshing + learned mesh advisor
- SiteGrid
- PolyMesh Showcase
- evaluate_curved_mesh_quality
- GeomError
- backend_cuda.cu
- string
- cell_validity.hpp
- local_refine_tets
- paths.py
- EffectiveMemoryBudget
- gp_Pnt
- regret.py
- Delaunay_psm.cpp
- docs/archive/README.md
- CadEdge
- FilterReport
- Edge
- run_calculix_cantilever.py
- run_batch.py
- M9 frozen baseline — `varyhedron-baseline-m9`
- cinema_timeline.cpp
- Pareto analysis — `settings-frontier-1`
- SolveResult
- GuiSettings
- required
- png_writer.hpp
- report
- campaign_config.cpp
- CadTopology
- external_truth.py
- PassTrace
- make_compare_grid.py
- CaseFeatures
- Test-lab interfaces (normative)
- ADR-0022: Full experiment warehouse + headless Grok improvement loop
- quality.cpp
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
- case_specs
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
- geometry_features.py
- ExteriorConformStats
- BoundaryProjectionContext
- Plan: Mesher / Solver Accuracy + Performance Overhaul
- CinemaSizingStory
- stress.cpp
- PointLookup
- RawFace
- ProbeAnswers
- Varyhedron packing — algorithm survey (V5)
- CinemaHud
- PartCase
- cinema_strip.cpp
- ProgressHeartbeat
- Prior art: ML for mesh generation and adaptive refinement
- run_gmsh_peer.py
- gen_primitive_corpus.py
- AdvisorNet
- operator==
- NodalMesh
- export_fixtures.py
- dashboard_charts.py
- LinearConstraints
- cvt_rvd_faces.cpp
- CinemaType
- graded_sizing.cpp
- report_external.py
- render_stress
- poly_mesh_geometry.cpp
- CinemaHistogram
- Variable-everything idea bank
- surface_render.cpp
- Campaign
- mixed_fill_snap.cpp
- verify_fields.py
- png_encode.cpp
- cinema/manifest.json
- case
- ClosestPoint
- SolvePhaseLog
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
- pull_buried_free_faces
- ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)
- SurfaceFace
- ComparePointCoord
- 6. What the linear solve costs
- Pareto analysis — `smoke`
- unit_hex_coords
- corpus_evidence.py
- BRepGeometryFidelity
- Decision
- CinemaRender
- ProcessRunner
- CampaignSummary
- Advisor::Impl
- lloyd_cvt
- MeshEdgeSegment
- HpElementDef
- ADR-0024: Advisor measure-first answers (normative Q&A)
- mp4
- ConstrainedSiteSeedResult
- HpDriverPolicy
- T
- HpDriverPlan
- ElementHpSignal
- Pareto analysis — `varyhedron-smoke`
- Act 5 — `solve`: the answer, in the order it is computed
- ClippedVoronoiExportStats
- ElementHpDecision
- hp_driver.cpp
- test_spmv.cpp
- lowpass_signal
- mixed_fill_emit.cpp
- PeriodicVertexArray3d
- SolveCostMeasured
- upload_boundary_edges
- spectral_fields
- FeatureAwareClassification
- drive_hp
- ADR-0021: Varyhedron — variable polyhedral packing mesher
- M-A1 — first trained advisor (2026-08-10)
- HpSystem
- SurfaceTessellation
- CDT2d_ConstraintWalker
- make_hp_signals
- MixedFillOutput
- ADR-0020: True BRep volume meshing (product path)
- index_t
- ConstrainedLloydParams
- vec4
- backend.cpp
- RuntimeError
- ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)
- analyze_solve_cost
- RefinementPlan
- load_from_args
- wall_tangential_project
- ADR-0044: GLM cannot be the math library, and is not a useful second one
- ADR-0036: A symmetric part gets a symmetric tiling
- build_graded_lattice
- varyhedron_fill_surface
- ADR-0043: A film someone can read
- hp_topology.cpp
- SolvePhaseTimings
- CvtLloydStats
- lines
- structured_mesh.hpp
- Commands
- brep_fidelity.cpp
- resolve_campaign
- check_no_product_stl.sh
- MixedCell
- report_network.py
- calibration.py
- rebuild_results.py
- pressure_face
- probe_util.hpp
- progress-history.md
- figstyle.py
- Progress
- Geogram / restricted CVT — vendoring study path
- capture
- LiveProgress
- Advisor::decide
- SizeSource
- Triangle
- Predicates_psm.h
- indicators.cpp
- manifest.json
- BcSelection
- VolumeMeshOutput
- pointer_
- Matrix
- CircularFeature
- function
- BRepInspection
- Stages
- declare_arg
- Split
- resolve_boundary_loops
- SolveCostEstimate
- ScorecardInfo
- expansion
- Matrix
- expansion
- GradedTetFillOutput
- build.sh
- post-m10-smoke/README.md
- Any
- ClipBox
- IndexType
- Traction
- HealthInfo
- Top-5 shortlist with implementation sketches
- ClippedCell
- gif
- 0009 — The v5 corpus: a better mesher, better predictions, and no decision win
- AdvisorDecision
- HexFillOutput
- properties
- element_jacobians_positive
- Case
- properties
- vec3Hg
- CommandLineDesc
- PHASES — Guided DAG Plan
- 0010 — The v6 corpus: the geometry objective stopped discriminating
- plot_hole_bug.py
- 2. Training tracks (all four selected, in dependency order)
- advisor_fields
- voronoi_neighbours
- main
- plot_truth_independence.py
- wall_time_s
- GeometryDescriptors
- settings-frontier-1 — campaign-1 close-out
- figures.py
- ADR-0035: Boundary nodes belong on the BRep, not near it
- ADR-0038: A fixture is applied to the boundary, not to a volume of nodes
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
- scene.hpp
- ADR-0042: The advisor explains itself on screen
- TetRecipe
- .operator()
- Decision
- 0004 — Model card: learned mesh advisor
- 0005 — Data card: advisor training corpus
- python_test.hpp
- mean_lateral_radial_residual
- WindowsThreadPoolManager
- commands_calibrate.cpp
- Limits
- detect_hole_roi
- Decision
- Decision
- Decision
- SampleDistribution
- CinemaCellKey
- SpectrumResult
- CgAttempt
- Portable-cost advisor retrain
- ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening
- Campaign metrics — normative definitions for agents
- Public CAD corpora for training a mesh advisor
- 0001 — Advisor architecture
- ADR-0037: A box selection is a region, and a smooth field is sampled on element sizes
- ADR-0040: A boundary condition names the exact closure of a CAD face
- Architecture decision records
- vtu_wire_png.py
- cost_labels.py
- rationalg
- BrepFidelitySummary
- 0002 — Objectives and guardrails
- vecng<4, T>
- render_surface
- command
- ADR-0039: A stranded boundary node is rescued, not abandoned
- ADR-0041: A deformed render carries its undeformed outline
- plot_evaluation.py
- 0008 — The v4 corpus, the retrain, and the metric that punished being right
- ADR-0033: A gate must measure what ships
- ParityResult
- ADR-0032: The mesh may not depend on which standard library built it
- json
- ADR-0031: A jut has a side
- HostCalibration
- GeometryCompleteness
- FaceConformityStats
- FanSpan
- campaign_progress.cpp
- enum
- 0006 — The clean-data retrain, and what it cost the advisor's claims
- as_bytes
- Geogram subset — what PolyMesh takes
- 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number
- FaceGeometry
- surface_render.hpp
- refusal
- advisor_fixture.cpp
- HoleROI
- schema_version
- Case
- bore_wall
- VertexArray
- Progress history
- SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver
- Case
- Field verification — stress *and* deformation, pointwise
- host
- label
- Face
- TetQuality
- archive/README.md
- check_cross_stdlib_mesh.sh
- AnswersInfo
- 0011 — v7 retrain: authoritative curved CAD geometry
- BRep face-tag BCs / probes (design stub)
- Protecting balls + local feature size (LFS)
- Status
- PointAlignment<6>
- gradient_canvas
- Dirichlet
- Image
- RenderView
- model.cpp
- stages_run
- draw_colorbar
- HexFace
- strain_displacement.hpp
- LexicoCompare
- TypedThreadGroup
- Row
- run_probe.sh
- take
- clip_rule_text
- Polyhedral-FEA — agent notes
- Learned mesh advisor — document index
- same_node_bytes
- PointAlignment<4>
- PointAlignment<8>

## God Nodes (most connected - your core abstractions)
1. `NodalMesh` - 183 edges
2. `PeriodicDelaunay3dThread` - 125 edges
3. `Viewport` - 111 edges
4. `Delaunay3dThread` - 109 edges
5. `CinemaState` - 103 edges
6. `TriSurface` - 91 edges
7. `App` - 70 edges
8. `Logger` - 69 edges
9. `CaseFeatures` - 64 edges
10. `Model` - 63 edges

## Surprising Connections (you probably didn't know these)
- `set_window_icon()` --calls--> `coverage`  [INFERRED]
  apps/gui/chrome.cpp → src/pipeline/include/pipeline/surface_render.hpp
- `assemble()` --calls--> `traction`  [INFERRED]
  src/fea/src/traction.cpp → apps/testlab/testlab_internal.hpp
- `vem_body_load()` --calls--> `body`  [INFERRED]
  src/fea/src/vem.cpp → tests/support/mms.hpp
- `sizing_field` --semantically_similar_to--> `resolve_mesh_size`  [INFERRED] [semantically similar]
  src/adapt/CMakeLists.txt → examples/README.md
- `merge_unique()` --references--> `NodalMesh`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp

## Import Cycles
- None detected.

## Communities (563 total, 144 thin omitted)

### Community 0 - "FeaError"
Cohesion: 0.12
Nodes (60): Fun, kP2Mono, kP2Vec, FeaError, runtime_error, uint32_t, vector, PolyCell (+52 more)

### Community 1 - "analyze_campaign.py"
Cohesion: 0.12
Nodes (37): accuracy_of(), aggregate_configs(), analyze_one(), CfgAgg, config_label(), factor_breakdown(), _fmt_ms(), _fmt_pct() (+29 more)

### Community 2 - "CurvedMeshMetrics"
Cohesion: 0.11
Nodes (19): CurvedMeshMetrics, composite_score, has_circular, has_tet_aspect, has_volume, m1_max, m1_mean, m2_max (+11 more)

### Community 3 - "Viewport"
Cohesion: 0.03
Nodes (77): array, CinemaView, DisplayMode, FieldSweep, size_t, uint32_t, vector, Vector3d (+69 more)

### Community 4 - "fea library"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "ManufacturedSolution"
Cohesion: 0.15
Nodes (21): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+13 more)

### Community 6 - "Palette"
Cohesion: 0.05
Nodes (43): apply_theme(), ThemeId, ImU32, ImVec4, make_interwebz_palette(), make_slate_palette(), make_studio_palette(), Palette (+35 more)

### Community 7 - "build_advisor_dataset.py"
Cohesion: 0.07
Nodes (51): family_of(), main(), probe_of(), load_json(), main(), Path, The named metric of a reference, from the working tree or a git revision., reference_metric() (+43 more)

### Community 8 - "render_showcase.py"
Cohesion: 0.12
Nodes (32): matched_panels(), PanelSpec, Everything about a panel that must match its siblings.      ``label`` is free;, Raise unless every panel shares camera, zoom window and colour limits.      A, _arch_center(), _arch_line_h(), build_mesh_tiles(), cli_path() (+24 more)

### Community 9 - "T"
Cohesion: 0.11
Nodes (11): T, vector_type, vecng<2, T>, dim, x, y, vecng<3, T>, dim (+3 more)

### Community 10 - "PolyMesh"
Cohesion: 0.15
Nodes (15): poly_mesh_to_vem(), Vector3d, PolyMesh, cells, check_geometry, check_validity, faces, triangulate_boundary_incident_faces (+7 more)

### Community 11 - "CinemaState"
Cohesion: 0.03
Nodes (61): build_cinema_skeleton(), capture_curve_spectrum(), capture_curve_story(), CinemaState, active, adopt_final_result, advance, advisor_dir (+53 more)

### Community 12 - "GradedFillState"
Cohesion: 0.07
Nodes (27): BoundaryFit, cad, projection, topo, FeaturePinReport, chains, edge_pinned, max_edge_residual (+19 more)

### Community 13 - "MshModel"
Cohesion: 0.14
Nodes (24): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+16 more)

### Community 14 - "index_t"
Cohesion: 0.09
Nodes (16): PackedArrays::clear(), aligned_free(), index_t, std::vector<bool>, std::vector<T, Memory::aligned_allocator<T> >, expansion::delete_expansion_on_heap(), expansion::new_expansion_on_heap(), expansion::show_all_stats() (+8 more)

### Community 15 - "ReferenceCase"
Cohesion: 0.05
Nodes (60): RadialMap, BenchError, runtime_error, string, ReferenceCase, citation, name, values (+52 more)

### Community 16 - "TestLabState"
Cohesion: 0.07
Nodes (48): CheckpointState, ImVec4, path, size_t, string, draw_results_panel(), draw_testlab_panel(), fmt_opt_num() (+40 more)

### Community 17 - "Advisor measure-first program"
Cohesion: 0.08
Nodes (24): 0. One-sentence strategy, 10. Related files, 1. Substrate (keep forever until proven wrong), 2. Claims (product honesty), 3.1 Five-number scorecard + residual gate, 3.2 What to score vs dashboard (stress), 3.3 Chordal efficiency (edge residual), 3.4 Over-budget diagnosis (+16 more)

### Community 18 - "AdaptSuggestion"
Cohesion: 0.18
Nodes (15): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+7 more)

### Community 19 - "CantileverSetup"
Cohesion: 0.10
Nodes (21): Cantilever, mesh, u, VectorXd, CantileverSetup, bc, length, loads (+13 more)

### Community 20 - "solve_elastostatics"
Cohesion: 0.10
Nodes (40): Precond, function, SolveOptions, cg_accept_tol, cg_max_iters, cg_progress_chunk, cg_threshold, cg_tol (+32 more)

### Community 21 - "CurvedGeometryResult"
Cohesion: 0.06
Nodes (31): CurvedGeometryResult, constraints, mesh, n_h_refined, n_partial, n_projected, n_promoted, n_pyramids_split (+23 more)

### Community 22 - "fill_progress_poll"
Cohesion: 0.05
Nodes (86): CollectOffendersFn, RelaxNeighborhoodFn, RepairInteriorFn, fill_progress_poll(), array, CellHash, uint32_t, vector (+78 more)

### Community 23 - "Projection"
Cohesion: 0.08
Nodes (60): EdgeOwners, NodeNeighbors, cross_2d(), vector, Vector2d, Vector3d, Projection, area (+52 more)

### Community 24 - "SolveJob"
Cohesion: 0.05
Nodes (56): atomic, mutex, State, time_point, uint64_t, load, SolveJob, active_max_mem_gb_ (+48 more)

### Community 25 - "d6_tier3.cpp"
Cohesion: 0.09
Nodes (40): add_node(), array, int64_t, json, map, string, uint32_t, vector (+32 more)

### Community 26 - "string"
Cohesion: 0.02
Nodes (204): E, ArgType, AssertMode, ExactPoint, absolute_path(), aligned_allocator, ALIGNMENT, aligned_free() (+196 more)

### Community 27 - "Predicates_psm.cpp"
Cohesion: 0.04
Nodes (102): coord_index_t, int64, Sign, SOSMode, det_3d(), det_3d_exact(), det_3d_filter(), det_4d() (+94 more)

### Community 28 - "TransitionFillOutput"
Cohesion: 0.11
Nodes (20): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+12 more)

### Community 29 - "polymesh CMake Project"
Cohesion: 0.22
Nodes (13): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+5 more)

### Community 30 - "gen_part_library.py"
Cohesion: 0.24
Nodes (14): _assert_manifold_facets(), _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre., Parse emitted ASCII facet blocks and require edge multiplicity 2. (+6 more)

### Community 31 - "CartesianGrid"
Cohesion: 0.10
Nodes (42): CartesianGrid, cell, nx, ny, nz, origin, size_t, Vector3d (+34 more)

### Community 32 - "CadModel"
Cohesion: 0.08
Nodes (28): CadModel, bbox_diagonal, has_brep, impl_, load_brep, load_step, name_, shape_handle (+20 more)

### Community 33 - "dataset.py"
Cohesion: 0.05
Nodes (59): action_group_slices(), build_action_dims(), candidate_grid(), clamp_table(), continuous_box_halfwidths(), Any, ndarray, The C4 ``clamps.json`` payload. (+51 more)

### Community 34 - "Model"
Cohesion: 0.04
Nodes (60): optional, pair, string, Vector3d, Vector3f, load_cinema_advisor(), region_box(), region_centroid() (+52 more)

### Community 35 - "Config"
Cohesion: 0.04
Nodes (53): evaluate_probe(), Checkpoint, campaign, completed_runs, hooks_failed, started_utc, state, survivors (+45 more)

### Community 36 - "metric_field.cpp"
Cohesion: 0.05
Nodes (73): Matrix3d, size_t, vector, Vector3d, Vector3i, Metric3d, axes, clamped (+65 more)

### Community 37 - "CsrMatrix"
Cohesion: 0.18
Nodes (15): CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t, vector (+7 more)

### Community 38 - "TriSurface"
Cohesion: 0.03
Nodes (93): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector (+85 more)

### Community 39 - "Hand-calculated reference truths"
Cohesion: 0.07
Nodes (28): cantilever, corpus-primitives, corpus-primitives-cantilever, corpus-primitives-external (supersedes corpus-primitives-provisional), corpus-primitives-kirsch, corpus-primitives-provisional (historical), cylinder, Engineering estimate: polar compression as a short column (+20 more)

### Community 40 - "accuracy"
Cohesion: 0.12
Nodes (16): additionalProperties, properties, required, type, description, type, accuracy, name (+8 more)

### Community 41 - "spectral_sizing.cpp"
Cohesion: 0.14
Nodes (19): max_value, clamp_fraction(), array, complex, function, size_t, vector, Vector3d (+11 more)

### Community 42 - "Stats"
Cohesion: 0.11
Nodes (19): Stats, phase_0_t_, phase_I_classify_t_, phase_I_insert_nb_, phase_I_insert_t_, phase_I_nb_cross_, phase_I_nb_inside_, phase_I_nb_outside_ (+11 more)

### Community 43 - "null"
Cohesion: 0.09
Nodes (26): description, minimum, type, description, minimum, type, description, type (+18 more)

### Community 44 - "NodalElement"
Cohesion: 0.07
Nodes (45): CornerEdges, CellQualityStats, mean, min, n_measured, n_unmeasured, size_t, element_num_nodes() (+37 more)

### Community 45 - "CalculiX / PolyMesh cantilever cross-validation"
Cohesion: 0.33
Nodes (5): CalculiX / PolyMesh cantilever cross-validation, Cases to port next, Common install paths (documentation only), Run (CI-safe), Runner contract

### Community 47 - "eval_shape"
Cohesion: 0.15
Nodes (25): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+17 more)

### Community 48 - "run_one"
Cohesion: 0.09
Nodes (49): compute_probes(), count_orphan_nodes(), Dirichlet, span, uint32_t, uint64_t, vector, Vector3d (+41 more)

### Community 49 - "gen_cad_parts.py"
Cohesion: 0.08
Nodes (40): _bbox(), check_step(), _classify(), _count(), _cylinder(), _display(), _face_area(), _faces() (+32 more)

### Community 50 - "timestamp"
Cohesion: 0.50
Nodes (4): timestamp, description, format, type

### Community 53 - "Pareto analysis — `varyhedron-baseline-m9`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 56 - "PrismFillOutput"
Cohesion: 0.18
Nodes (11): array, uint32_t, vector, Vector3d, PrismFillOutput, boundary_max_distance, boundary_quads, h (+3 more)

### Community 57 - "export_clipped_voronoi"
Cohesion: 0.13
Nodes (24): ClippedVoronoiExport, mesh, site_to_cell, stats, DomainClipParams, clip_radius, min_area_frac, surface (+16 more)

### Community 58 - "SolveResourceEstimate"
Cohesion: 0.12
Nodes (16): Index, SolveResourceEstimate, assembly_workspace_bytes, cell_storage_bytes, cg_peak_bytes, cg_workspace_bytes, common_peak_bytes, csr_nnz_upper (+8 more)

### Community 59 - "Delaunay_psm.h"
Cohesion: 0.01
Nodes (209): Attribute, COORD_T, CreatorType, GEO_NODISCARD, int16, int8, mat2, mat3 (+201 more)

### Community 60 - "CI Grep-Audit Anti-Cheat Job"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "HpMode"
Cohesion: 0.14
Nodes (14): Entity, HpMode, edge_odd, entity, entity_index, index0, index1, index2 (+6 more)

### Community 62 - "mathlib_probe.cpp"
Cohesion: 0.07
Nodes (48): b_matrix(), best_of(), build_data(), array, Matrix, Matrix3d, size_t, vector (+40 more)

### Community 63 - "BrepFaceIndex"
Cohesion: 0.07
Nodes (29): BRepExtrema_DistShapeShape, Handle, BrepFaceIndex, adaptors, bins, boxes, cell, edge_ids (+21 more)

### Community 64 - "run_packing_microbench.py"
Cohesion: 0.23
Nodes (21): boundary_residual_placeholder(), bubble_relax(), _clamp01(), _dedupe(), _dist2(), fill_fraction_proxy(), main(), pack_case() (+13 more)

### Community 65 - "hierarchical.cpp"
Cohesion: 0.16
Nodes (26): Dynamic, Matrix, VectorXd, HpShape, dn, n, build_hex(), build_tet() (+18 more)

### Community 66 - "run_tier3.py"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "testlab_data.cpp"
Cohesion: 0.20
Nodes (29): checkpoint_state_cstr(), count_result_lines(), Checkpoint, CheckpointState, json, optional, path, string (+21 more)

### Community 68 - "Camera"
Cohesion: 0.15
Nodes (19): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+11 more)

### Community 69 - "train.py"
Cohesion: 0.14
Nodes (34): write_json(), activation_record(), append_history(), build_model(), dump_json(), _first_trigger(), head_weights_for(), jsonable() (+26 more)

### Community 70 - "volume_mesh_impl"
Cohesion: 0.07
Nodes (44): prepare_cinema_features(), function, SizeFieldFn, span, Vector3d, function, vector, MixedLattice (+36 more)

### Community 71 - "viewport.cpp"
Cohesion: 0.09
Nodes (40): bind_cinema_attr(), bind_cinema_line_attr(), bind_line_attr(), bind_sizing_attr(), fit, fit_oriented, cinema_cell_key(), count (+32 more)

### Community 72 - "Pareto analysis — `varyhedron-short-1`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 73 - "main"
Cohesion: 0.18
Nodes (13): draw_line(), main(), parse_vtu_ascii(), point_in_roi(), project(), Path, quadratic_edge_mids(), quadratic_edge_points() (+5 more)

### Community 76 - "index_t"
Cohesion: 0.02
Nodes (151): condition_variable, local_index_t, Periodic, SFrame, CDT2d::incircle(), CDT2d::insert(), CDT2d::orient2d(), CDTBase2d::insert() (+143 more)

### Community 77 - "assemble_hp"
Cohesion: 0.16
Nodes (20): assemble_hp(), ElementType, max_order_for_type(), edge_slot(), FaceOrient, sign0, sign1, swap (+12 more)

### Community 78 - "assemble_body_load"
Cohesion: 0.06
Nodes (46): Vector3d, QuadraturePoint, weight, xi, loads, assemble_body_load(), BodyForce, VectorXd (+38 more)

### Community 79 - "ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "CampaignSpec"
Cohesion: 0.11
Nodes (19): CampaignResources, max_mem_gb, max_threads, CampaignScoreWeights, accuracy, mesh_ms, solve_ms, CampaignSpec (+11 more)

### Community 82 - "plot_benchmarks.py"
Cohesion: 0.17
Nodes (24): d6_records(), gate1_records(), load_json(), main(), parse_mms_elements(), parse_mms_hierarchical(), parse_tier1_table(), plot_advisor_budget() (+16 more)

### Community 84 - "Material"
Cohesion: 0.07
Nodes (19): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+11 more)

### Community 85 - "widgets.cpp"
Cohesion: 0.19
Nodes (26): begin_field(), begin_group_box(), begin_group_box_fill(), button(), checkbox(), ImDrawList, ImU32, ImVec2 (+18 more)

### Community 86 - "interior_points"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "PolicyObjective"
Cohesion: 0.10
Nodes (32): Optimizer, BranchAdvisorNet, main(), Any, device, Module, Tensor, sign_test_paired() (+24 more)

### Community 88 - "load_dataset"
Cohesion: 0.05
Nodes (75): action_matrix(), advisor_scores(), aggregate(), aggregate_tolerance(), build_choosers(), decode_policy(), main(), parse_args() (+67 more)

### Community 89 - "render_cinema.py"
Cohesion: 0.10
Nodes (39): _as_int(), auto_spec(), Case, encode_gif(), encode_mp4(), ffmpeg(), ffmpeg_encoders(), ffmpeg_version() (+31 more)

### Community 90 - "Grid3d"
Cohesion: 0.13
Nodes (19): Grid3d, at, dims, index, origin, sample, spacing, values (+11 more)

### Community 91 - "emit_polymesh_gate1.py"
Cohesion: 0.36
Nodes (7): face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face.      F, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever).

### Community 92 - "ResultRow"
Cohesion: 0.07
Nodes (27): AccuracyInfo, metric, rel_err, truth, value, QualityInfo, M1max, M2max (+19 more)

### Community 94 - "vector"
Cohesion: 0.06
Nodes (13): vector, array, array, vector, map, span, unordered_map, set (+5 more)

### Community 95 - "vecng"
Cohesion: 0.12
Nodes (12): A, B, DIM, DIM2, initializer_list, U, length(), length2() (+4 more)

### Community 96 - "vector"
Cohesion: 0.02
Nodes (121): COORD, IT, KeepInitialValues, MESH, MeshElementsFlags, MeshOrder, NearestNeighbors, ProgressClient (+113 more)

### Community 97 - "test_local_refine.cpp"
Cohesion: 0.39
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 100 - "GradedSizing"
Cohesion: 0.09
Nodes (20): GradedSizing, as_size_field, build_grid, cell_start_, grid_cell_, grid_nx_, grid_ny_, grid_nz_ (+12 more)

### Community 107 - "case_features.cpp"
Cohesion: 0.08
Nodes (41): selected, empty, axis_distance(), Bore, axis, point, radius, circle_through() (+33 more)

### Community 108 - "cinema_draw.cpp"
Cohesion: 0.13
Nodes (37): array, ImDrawList, ImFont, ImU32, ImVec2, ImVec4, size_t, string (+29 more)

### Community 109 - "Scorecard"
Cohesion: 0.18
Nodes (11): size_t, string, dump_score(), Scorecard, h, m, mesher, n_elems (+3 more)

### Community 119 - "resource_budget.cpp"
Cohesion: 0.31
Nodes (17): Index, optional, string, string_view, uint64_t, csr_bytes(), dense_square_cap(), effective_memory_budget() (+9 more)

### Community 120 - "index_t"
Cohesion: 0.16
Nodes (11): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, geo_pause(), atomic, index_t (+3 more)

### Community 121 - "CinemaCue"
Cohesion: 0.05
Nodes (37): CinemaChapter, act, label, CinemaCue, act, act_span, act_t, action_bridge_alpha (+29 more)

### Community 122 - "feature_pin.cpp"
Cohesion: 0.33
Nodes (10): chord_stations(), BoundarySupportKind, NodeOffendsFn, uint32_t, vector, Vector3d, polyline_parameter(), polyline_point() (+2 more)

### Community 124 - "Variable-everything meshing + learned mesh advisor"
Cohesion: 0.05
Nodes (35): 1.1 `adapt::MetricField` (new), 1.2 Sizing field end-to-end, 1.3 Conforming variable order, 1.4 Quantitative sizing from error, Corpus, Deployment, Label design (decided), Non-goals (+27 more)

### Community 125 - "SiteGrid"
Cohesion: 0.11
Nodes (18): uint32_t, vector, SiteGrid, build, cell_, cell_start_, inv_cell_, items_ (+10 more)

### Community 126 - "PolyMesh Showcase"
Cohesion: 0.09
Nodes (23): Architecture diagram, `bench_advisor_budget.png`, `bench_dof_time.png`, `bench_mms.png`, `bench_tier1.png`, Benchmark charts, Boundary conformity, `gallery_cantilever.png` (+15 more)

### Community 127 - "evaluate_curved_mesh_quality"
Cohesion: 0.31
Nodes (10): clamp01(), array, FreeFace, uint32_t, vector, Vector3d, cyl_coords(), evaluate_curved_mesh_quality() (+2 more)

### Community 128 - "GeomError"
Cohesion: 0.22
Nodes (15): GeomError, runtime_error, byte, path, size_t, Soup, span, T (+7 more)

### Community 129 - "backend_cuda.cu"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "string"
Cohesion: 0.03
Nodes (102): App, cinema, cinema_font, cinema_stamp, custom_font, deform_auto, deform_scale, deform_true_scale (+94 more)

### Community 131 - "cell_validity.hpp"
Cohesion: 0.26
Nodes (21): hex8_jacobian_det(), hex8_min_jacobian(), hex8_shape_quality(), array, Vector3d, max_edge(), max_edge_squared(), prism_min_corner_jacobian() (+13 more)

### Community 132 - "local_refine_tets"
Cohesion: 0.13
Nodes (33): size_t, OctaFillOutput, h, mesh, n_boundary_pyramids, n_octahedra, bisect_tet(), array (+25 more)

### Community 133 - "paths.py"
Cohesion: 0.15
Nodes (31): accuracy_vs_cost(), _best_so_far(), fidelity_vs_h(), mesh_progress(), _pareto(), ndarray, Path, Indices of the lower-left Pareto front: minimize both axes. (+23 more)

### Community 134 - "EffectiveMemoryBudget"
Cohesion: 0.22
Nodes (10): MemoryAvailabilitySource, EffectiveMemoryBudget, available, effective_cap_bytes, safety_cap_bytes, user_cap_bytes, uint64_t, MemoryAvailability (+2 more)

### Community 135 - "gp_Pnt"
Cohesion: 0.12
Nodes (36): BRepAdaptor_Surface, gp_Pnt, make_cylinder(), Solid cylinder, axis along +z, base circle in z=0 plane, center at origin., CadSupportKind, ProjectResult, distance, face_id (+28 more)

### Community 136 - "regret.py"
Cohesion: 0.07
Nodes (50): budget_levels(), build_cases(), Case, cost_at_tolerance(), dof_to_target(), feasible_mask(), finest_action_chooser(), fit_constant_action() (+42 more)

### Community 137 - "Delaunay_psm.cpp"
Cohesion: 0.01
Nodes (229): EMSCRIPTEN_KEEPALIVE, interval_nt, InvalidInput, siginfo_t, SparseBits, abnormal_program_termination(), AdaptiveKdTree::AdaptiveKdTree(), AdaptiveKdTree::create_kd_tree_recursive() (+221 more)

### Community 138 - "docs/archive/README.md"
Cohesion: 0.12
Nodes (14): Program DAG — how to pick up work, Assessment, Idea harvest: fea-madness (Grok-generated spec, 2026-07-10), Rejected / deferred, Worth incorporating, Archive, Campaign analysis — `results.jsonl` → `PARETO.md`, Changing product defaults (+6 more)

### Community 139 - "CadEdge"
Cohesion: 0.09
Nodes (26): CadEdgeFeature, CadSurfaceKind, CadEdge, dihedral_rad, feature, id, kappa_samples, length (+18 more)

### Community 140 - "FilterReport"
Cohesion: 0.15
Nodes (13): BudgetResult, budget_met, filter, h_scale, predicted_after, predicted_before, FilterReport, energy_fraction (+5 more)

### Community 141 - "Edge"
Cohesion: 0.07
Nodes (44): N, ElementTypeCounts, hex20, hex8, other, tet10, tet4, size_t (+36 more)

### Community 142 - "run_calculix_cantilever.py"
Cohesion: 0.23
Nodes (18): input_id_lines(), load_json(), main(), node_id(), parse_ccx_tip_displacement(), parse_polymesh_tip_displacement(), CompletedProcess, Path (+10 more)

### Community 143 - "run_batch.py"
Cohesion: 0.05
Nodes (79): best_rows(), check_promotable(), main(), measured_by_metric(), parse_args(), promote(), provenance(), Any (+71 more)

### Community 144 - "M9 frozen baseline — `varyhedron-baseline-m9`"
Cohesion: 0.17
Nodes (12): Campaign matrix, Case primary accuracy metrics (as wired at freeze), Freeze identity, Known issues frozen-in (not blockers for freeze), M9 frozen baseline — `varyhedron-baseline-m9`, Metric schema version, Outcome summary, Per-run snapshot (+4 more)

### Community 145 - "cinema_timeline.cpp"
Cohesion: 0.09
Nodes (32): invalidate_uploads, beat_seconds(), cinema_cue(), cinema_decision_lead(), cinema_opening_fade(), cinema_panel_fade(), cinema_solver_token(), cinema_view() (+24 more)

### Community 146 - "Pareto analysis — `settings-frontier-1`"
Cohesion: 0.12
Nodes (16): `cantilever`, Config ranking (weighted mean score), `curved`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 147 - "SolveResult"
Cohesion: 0.06
Nodes (36): CinemaState::push_solve_stage(), set_cinema_motion_bounds, BoundaryConditions, dirichlet, loads, array, Dirichlet, VectorXd (+28 more)

### Community 148 - "GuiSettings"
Cohesion: 0.14
Nodes (15): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+7 more)

### Community 149 - "required"
Cohesion: 0.12
Nodes (15): additionalProperties, description, $id, required, $schema, title, type, accuracy (+7 more)

### Community 150 - "png_writer.hpp"
Cohesion: 0.50
Nodes (8): adler32_of(), crc32_of(), size_t, uint32_t, vector, put_chunk(), put_u32be(), write_png_rgba()

### Community 151 - "report"
Cohesion: 0.06
Nodes (35): report, acts, acts_note, added_cells, advisor_activations, advisor_unavailable, candidates, decision (+27 more)

### Community 152 - "campaign_config.cpp"
Cohesion: 0.13
Nodes (32): cfg_id_of(), Checkpoint, json, path, string, vector, VolumeMesher, expand_grid() (+24 more)

### Community 153 - "CadTopology"
Cohesion: 0.07
Nodes (51): CadEdgeClassCounts, n_seam, n_sharp, n_smooth, CadTopology, edges, faces, vertices (+43 more)

### Community 154 - "external_truth.py"
Cohesion: 0.05
Nodes (83): all_case_ids(), analytic_case_ids(), audit_against_git(), base_provenance(), bbox_diagonal(), cad_feature_sizes(), Case, consistent_face_loads() (+75 more)

### Community 155 - "PassTrace"
Cohesion: 0.07
Nodes (28): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, PassTrace, cg_iters (+20 more)

### Community 156 - "make_compare_grid.py"
Cohesion: 0.10
Nodes (28): Run, _aspect(), build_grid(), _Caption, _caption_parts(), choose_cols(), draw_line(), keyline() (+20 more)

### Community 157 - "CaseFeatures"
Cohesion: 0.04
Nodes (56): CaseFeatures, bbox_dx, bbox_dy, bbox_dz, case_load_multiaxiality, curved_frac, diag, fix_area_frac (+48 more)

### Community 158 - "Test-lab interfaces (normative)"
Cohesion: 0.12
Nodes (17): 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp` (+9 more)

### Community 159 - "ADR-0022: Full experiment warehouse + headless Grok improvement loop"
Cohesion: 0.12
Nodes (14): Campaign warehouse, Directory layout, Short-campaign defaults (Lane V), What git tracks, Wireframe PNGs (`wire.png`), ADR-0022: Full experiment warehouse + headless Grok improvement loop, Alternatives rejected, Consequences (+6 more)

### Community 160 - "quality.cpp"
Cohesion: 0.40
Nodes (10): array, size_t, uint32_t, vector, Vector3d, polygon_corner_quality(), summarize_tet4_quality(), tet4_aspect_quality() (+2 more)

### Community 172 - "case_specs"
Cohesion: 0.09
Nodes (33): _beam_region_response(), _boxes_intersect(), case_specs(), check(), check_coverage(), _coverage_module(), generate(), internal_truth_eligible() (+25 more)

### Community 252 - "geometry_features.py"
Cohesion: 0.12
Nodes (32): _axis_distance(), _count(), _crease_dihedral(), _crease_opening_angle(), _distance_to_face(), _edge_circle_radius(), _edge_stations(), extract() (+24 more)

### Community 253 - "ExteriorConformStats"
Cohesion: 0.06
Nodes (32): BoundaryShellTopology, n_edges, n_nonmanifold, n_open, ExteriorConformStats, connected_edge_census, edge_pass_ms, n_candidates (+24 more)

### Community 254 - "BoundaryProjectionContext"
Cohesion: 0.08
Nodes (30): BoundaryTargetFn, BoundaryProjectionContext, provenance, topology, BoundarySupport, id, kind, array (+22 more)

### Community 255 - "Plan: Mesher / Solver Accuracy + Performance Overhaul"
Cohesion: 0.06
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 256 - "CinemaSizingStory"
Cohesion: 0.07
Nodes (30): CinemaSizingStory, bc_seeds, brep_curvature, curvature_filtered, curvature_raw, curve_energy_fraction, curve_mode_kept, curve_modes_kept (+22 more)

### Community 257 - "stress.cpp"
Cohesion: 0.11
Nodes (28): ElementCentroidStress, centroid, element_index, quality, stress, volume, uint32_t, Vector3d (+20 more)

### Community 258 - "PointLookup"
Cohesion: 0.12
Nodes (25): CellKind, Cell, faces, kind, FaceId, vector, CellHash, pair (+17 more)

### Community 259 - "RawFace"
Cohesion: 0.08
Nodes (27): RawFaceProvenance, ScaffoldFaceProvenance, array, size_t, vector, Vector3d, QuantHash, QuantKey (+19 more)

### Community 260 - "ProbeAnswers"
Cohesion: 0.07
Nodes (28): LoadAreaStatus, ProbeAnswers, authored_area_checked, authored_area_consistent, authored_area_rel_diff, dominant_load_axis, free_residual_rel, load_area_ok (+20 more)

### Community 261 - "Varyhedron packing — algorithm survey (V5)"
Cohesion: 0.07
Nodes (28): 0. Normative ranking (ADR-0023 / plan — do not ignore), 1. Goals (from ADR-0021), 2. Bubble / sphere packing → Delaunay, 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage), 4. Field-aligned hex-dominant (PGP3D-class), 5. CAD edge protecting balls / PLC constraints, 6. Licensing notes (core vs plugin), 7. Decision: v1 algorithm (+20 more)

### Community 262 - "CinemaHud"
Cohesion: 0.07
Nodes (27): CinemaHud, adapt_passes, added_elements, cinema_elements, cinema_skipped_elements, deform_scale, dof, elements (+19 more)

### Community 263 - "PartCase"
Cohesion: 0.09
Nodes (26): with_exact_cad_selections(), adaptive_setup(), BcSpec, box, cad_face_ids, fix, array, uint32_t (+18 more)

### Community 264 - "cinema_strip.cpp"
Cohesion: 0.16
Nodes (24): CinemaCaption, footer, headline, headline_color, note, note_color, numbers, fmt() (+16 more)

### Community 265 - "ProgressHeartbeat"
Cohesion: 0.08
Nodes (23): mutex, path, size_t, time_point, ProgressHeartbeat, cfg_id_, cg_iter_, cg_resid_ (+15 more)

### Community 266 - "Prior art: ML for mesh generation and adaptive refinement"
Cohesion: 0.08
Nodes (23): 1. MeshingNet and MeshingNet3D — the closest direct analogue, 2. Reinforcement learning for AMR, 3. GNN/neural error estimators and surrogate indicators, 4. Learned anisotropic metric fields, 5. Commercial/industrial state of the art, 6. Code/weights availability at a glance, Bottom line, E2N: Error Estimation Networks for Goal-Oriented Mesh Adaptation (+15 more)

### Community 267 - "run_gmsh_peer.py"
Cohesion: 0.10
Nodes (40): analytic_cases(), bbox_diagonal(), build_supports_uniform(), cad_bbox_diagonal(), classify_failure(), failed_result_row(), flatten_box(), load_arguments() (+32 more)

### Community 268 - "gen_primitive_corpus.py"
Cohesion: 0.08
Nodes (69): make_plate_hole(), Centered plate with through-hole along z — matches legacy plate_hole dims., _require_solid(), _box(), build_bossed_plate(), build_box_hole(), build_channel(), build_ellipsoid_boss() (+61 more)

### Community 269 - "AdvisorNet"
Cohesion: 0.07
Nodes (32): Linear, ExportWrapper, Tensor, Adapts ``AdvisorNet`` to the flat tuple signature ONNX needs.      ``taps`` se, AdvisorNet, main(), _matrix(), Any (+24 more)

### Community 270 - "operator=="
Cohesion: 0.10
Nodes (15): M, A1, DIM, T1, T2, T2, mult(), operator==() (+7 more)

### Community 271 - "NodalMesh"
Cohesion: 0.03
Nodes (85): Vector3d, NodalMesh, compact_unused_nodes, elements, nodes, Dirichlet, SolveHealth, free_residual_rel (+77 more)

### Community 272 - "export_fixtures.py"
Cohesion: 0.08
Nodes (50): Vectorized ``standardize_row`` for a whole ``(n, D)`` raw input matrix.      `, standardize_matrix(), build_fixture(), check_fixture_guarantees(), force_head_values(), Any, Namespace, ndarray (+42 more)

### Community 273 - "dashboard_charts.py"
Cohesion: 0.15
Nodes (33): begin_end_annotations(), chart(), dash_y2(), fmt(), js_json(), no_data(), panel_baseline(), panel_benchmark() (+25 more)

### Community 274 - "LinearConstraints"
Cohesion: 0.09
Nodes (27): map, pair, size_t, uint32_t, vector, LinearConstraint, masters, slave_dof (+19 more)

### Community 275 - "cvt_rvd_faces.cpp"
Cohesion: 0.16
Nodes (20): canonical_face_key(), claim_rvd_faces(), coalesce_rvd_interior_faces(), FaceId, size_t, span, vector, Vector3d (+12 more)

### Community 276 - "CinemaType"
Cohesion: 0.09
Nodes (23): CinemaLayout, content_h, content_w, panel_w, settled_view_aspect, strip_h, type, CinemaType (+15 more)

### Community 277 - "graded_sizing.cpp"
Cohesion: 0.18
Nodes (22): Demand, cell_coord(), cell_offset(), int32_t, size_t, SizeFieldFn, span, vector (+14 more)

### Community 278 - "report_external.py"
Cohesion: 0.16
Nodes (22): load_json(), Any, CSV cell -> float, with blanks and junk mapped to NaN., to_float(), _cost_tally(), external_comparison(), _external_rows(), _external_tolerance() (+14 more)

### Community 279 - "render_stress"
Cohesion: 0.13
Nodes (23): _camera_state(), color_range(), compose(), fit_camera(), nice_factor(), over(), _plotter(), ndarray (+15 more)

### Community 280 - "poly_mesh_geometry.cpp"
Cohesion: 0.26
Nodes (22): coplanar_polygons_overlap(), array, CellId, vector, Vector3d, VertexId, face_geometry(), orient_2d() (+14 more)

### Community 281 - "CinemaHistogram"
Cohesion: 0.09
Nodes (22): CinemaHistogram, bins, max, mean, min, p99, samples, tallest_bin (+14 more)

### Community 282 - "Variable-everything idea bank"
Cohesion: 0.10
Nodes (20): 10. Additional axes worth varying, 1. A2 + A1 + A4: Mmg3d-backed solution-driven metric adaptation, 1. Size / density, 2. Anisotropy, 2. O1 + O2: productionize the existing hierarchical HpModel, 3. G1 + G2: independently variable curved CAD geometry order, 3. Polynomial order, 4. Element shape / topology (+12 more)

### Community 283 - "surface_render.cpp"
Cohesion: 0.17
Nodes (20): NormalDeviation, max, mean, p99, samples, angle_between_deg(), Basis, eye_dir (+12 more)

### Community 284 - "Campaign"
Cohesion: 0.10
Nodes (20): run_wall_limit_s(), scalar_score(), Campaign, grid, host, max_dof, max_elems, max_pack_wall_s (+12 more)

### Community 285 - "mixed_fill_snap.cpp"
Cohesion: 0.25
Nodes (19): Fan, array, function, map, size_t, span, uint32_t, vector (+11 more)

### Community 286 - "verify_fields.py"
Cohesion: 0.27
Nodes (15): cantilever_moment(), cantilever_stations(), Check, check_axisymmetric(), check_cantilever(), check_cylinder(), check_plate(), check_sanity() (+7 more)

### Community 287 - "png_encode.cpp"
Cohesion: 0.25
Nodes (15): adler32(), BitWriter, acc_, bit_count_, size_t, string, uint32_t, uint8_t (+7 more)

### Community 288 - "cinema/manifest.json"
Cohesion: 0.11
Nodes (18): generated_utc, git_rev, model, dir, onnx, onnx_sha256, outputs, poster (+10 more)

### Community 289 - "case"
Cohesion: 0.11
Nodes (19): case, adapt_passes_configured, case_json, eta_target_configured, feature_grading_configured, fix_faces, h_mm_configured, h_note (+11 more)

### Community 290 - "ClosestPoint"
Cohesion: 0.11
Nodes (19): ClosestPoint, distance, point, triangle, ConformityStats, count, max_distance, mean_distance (+11 more)

### Community 291 - "SolvePhaseLog"
Cohesion: 0.12
Nodes (16): time_point, SolvePhaseLog, assemble_s, backsolve_s, bc_s, export_s, factor_s, import_s (+8 more)

### Community 305 - "pull_buried_free_faces"
Cohesion: 0.16
Nodes (27): BuriedFaceStats, n_buried, n_free_faces, size_t, carve_overlapped_sheets(), buried_face_ids(), buried_free_tet_face_owners(), count_buried_free_tet_faces() (+19 more)

### Community 306 - "ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "SurfaceFace"
Cohesion: 0.06
Nodes (82): Sink, ConsistentLoad, area, conservation_error, resultant, face_num_nodes(), FaceType, VectorXd (+74 more)

### Community 308 - "ComparePointCoord"
Cohesion: 0.25
Nodes (6): BalancedKdTree::split_kd_node(), ComparePointCoord, nb_points_, points_, splitting_coord_, stride_

### Community 309 - "6. What the linear solve costs"
Cohesion: 0.12
Nodes (17): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, 6.1 The direct ladder, 6.1a How many threads the factorization gets, 6.2 Where the rest of the time went (+9 more)

### Community 310 - "Pareto analysis — `smoke`"
Cohesion: 0.14
Nodes (13): Config ranking (weighted mean score), Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run, Pareto analysis — `smoke`, Pareto by geometric class (+5 more)

### Community 311 - "unit_hex_coords"
Cohesion: 0.67
Nodes (4): Dynamic, Matrix, unit_hex_coords(), unit_tet_coords()

### Community 312 - "corpus_evidence.py"
Cohesion: 0.19
Nodes (22): _bootstrap_slope(), centroid_pairwise(), coverage_report(), descriptor_distances(), family_of(), family_recovery(), learning_curve_fit(), load_descriptors() (+14 more)

### Community 313 - "BRepGeometryFidelity"
Cohesion: 0.11
Nodes (18): BRepGeometryFidelity, available, brep, brep_surface_fallback_vertex_count, brep_surface_sample_face_count, brep_surface_samples_to_mesh_boundary, brep_surface_uv_attempt_count, brep_vertices_to_mesh_boundary_nodes (+10 more)

### Community 314 - "Decision"
Cohesion: 0.14
Nodes (14): 1. Substrate (keep, do not replace), 2. Element technology claims, 3. Packing evolution, 4. CAD edge classification (normative), 5. Measurement order (two-week horizon), 6. License landscape (core vs plugin), 7. Sizing field, 8. p-order (+6 more)

### Community 315 - "CinemaRender"
Cohesion: 0.16
Nodes (18): CinemaRender, deform_scale, mode, result_max, sweep, color, ImVec2, optional (+10 more)

### Community 316 - "ProcessRunner"
Cohesion: 0.20
Nodes (13): path, string, vector, find_testlab_binary(), State, string, is_executable_file(), ProcessRunner (+5 more)

### Community 317 - "CampaignSummary"
Cohesion: 0.10
Nodes (22): CampaignSummary, dir, has_campaign_json, has_checkpoint, has_results, name, result_count, state (+14 more)

### Community 318 - "Advisor::Impl"
Cohesion: 0.04
Nodes (48): Env, MemoryInfo, Session, SessionOptions, Advisor::Impl, action_dims, activation_note, activations_available (+40 more)

### Community 319 - "lloyd_cvt"
Cohesion: 0.36
Nodes (13): density_from_size(), build_rvd_cell(), optional, SizeFieldFn, span, Vector3d, density_mg(), lloyd_cvt() (+5 more)

### Community 320 - "MeshEdgeSegment"
Cohesion: 0.67
Nodes (3): MeshEdgeSegment, a, b

### Community 321 - "HpElementDef"
Cohesion: 0.13
Nodes (17): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+9 more)

### Community 322 - "ADR-0024: Advisor measure-first answers (normative Q&A)"
Cohesion: 0.17
Nodes (12): ADR-0024: Advisor measure-first answers (normative Q&A), Compressed path (do not invent another), Q10 — High-dimensional traps, Q1 — 1e20 von Mises with 1e-13 residual, Q2 — Next 3–5 days order, Q3 — Geogram, Q4 — Chordal efficiency e ~ 100 at h_scale=5, Q5 — Cylinder truth (+4 more)

### Community 323 - "mp4"
Cohesion: 0.11
Nodes (18): bytes, codec, encoder, faststart, file, fps, pix_fmt, rate_control (+10 more)

### Community 324 - "ConstrainedSiteSeedResult"
Cohesion: 0.10
Nodes (21): ConstrainedLloydResult, lloyd_stats, project_stats, seed_stats, sites, ConstrainedSiteSeedResult, n_interior_free, n_seams_skipped (+13 more)

### Community 325 - "HpDriverPolicy"
Cohesion: 0.11
Nodes (19): HpDriverPolicy, coarsen_geom_factor, coarsen_theta, cost_h, cost_p, cost_shape, dorfler_theta, eta_rel_floor (+11 more)

### Community 326 - "T"
Cohesion: 0.06
Nodes (36): A1, A2, DIM, DIM2, initializer_list, T, T1, T2 (+28 more)

### Community 327 - "HpDriverPlan"
Cohesion: 0.12
Nodes (16): HpDriverPlan, coarsen_mark, decisions, global_shape, h_mark, h_suggestion, n_coarsen, n_h (+8 more)

### Community 328 - "ElementHpSignal"
Cohesion: 0.14
Nodes (14): ElementHpSignal, eta, h, h_geometry, hex_fit, kappa, p, p_max (+6 more)

### Community 329 - "Pareto analysis — `varyhedron-smoke`"
Cohesion: 0.13
Nodes (14): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+6 more)

### Community 330 - "Act 5 — `solve`: the answer, in the order it is computed"
Cohesion: 0.11
Nodes (18): Act 1 — `skeleton`: the part, Act 2 — `deliberate`: choosing a mesh, Act 3 — `build`: the mesher executing the decision, Act 4 — `mesh_hold`: the finished mesh, Act 5 — `solve`: the answer, in the order it is computed, Load ramp, Material, Refine (+10 more)

### Community 331 - "ClippedVoronoiExportStats"
Cohesion: 0.11
Nodes (18): ClippedVoronoiExportStats, domain_clip_used, geogram_ok, n_boundary_faces, n_cells, n_coalesced_face_fragments, n_coalesced_faces, n_domain_plane_clips (+10 more)

### Community 332 - "ElementHpDecision"
Cohesion: 0.18
Nodes (11): HpAction, ElementHpDecision, action, h_next, p_next, reason, shape, utility_h (+3 more)

### Community 333 - "hp_driver.cpp"
Cohesion: 0.30
Nodes (11): best_shape_vote(), clamp01(), ShapeTendency, string, decide_element(), geometry_severity(), is_thin_wall(), shape_awkwardness() (+3 more)

### Community 334 - "test_spmv.cpp"
Cohesion: 0.32
Nodes (7): dist, SparseMatrix, vector, VectorXd, make_spd_test_matrix(), max_abs_diff(), random_vector()

### Community 335 - "lowpass_signal"
Cohesion: 0.38
Nodes (13): clamp_fraction(), complex, size_t, span, vector, fft_inplace(), is_pow2(), lerp_signal() (+5 more)

### Community 336 - "mixed_fill_emit.cpp"
Cohesion: 0.27
Nodes (17): FineNodeFn, closed_poly_volume(), array, function, uint32_t, vector, Vector3d, emit_cell_pyramids() (+9 more)

### Community 337 - "PeriodicVertexArray3d"
Cohesion: 0.14
Nodes (11): AdaptiveKdTree::plane_split(), Hilbert_vcmp_periodic<COORD, false, PeriodicVertexMesh3d>, Hilbert_vcmp_periodic<COORD, true, PeriodicVertexMesh3d>, PeriodicVertexArray3d, base_, nb_real_vertices_, nb_vertices_, stride_ (+3 more)

### Community 338 - "SolveCostMeasured"
Cohesion: 0.15
Nodes (14): SolveMethod, string, uint64_t, SolveCostMeasured, bytes, cg_iterations, cg_restarts, factor_nnz (+6 more)

### Community 339 - "upload_boundary_edges"
Cohesion: 0.17
Nodes (15): array, DisplayMode, ElementType, initializer_list, uint32_t, element_type_color(), sweep_sample_color(), type_color() (+7 more)

### Community 340 - "spectral_fields"
Cohesion: 0.12
Nodes (16): spectral_fields, applied, bc_seeds, brep_curvature, edge_seeds, energy_kept, field_samples, geometry_seeds (+8 more)

### Community 341 - "FeatureAwareClassification"
Cohesion: 0.12
Nodes (19): CanonicalCellMap, axis, canonical, flipped, FeatureAwareClassification, child_inside_mask, classified_volume, coarse_inside (+11 more)

### Community 342 - "drive_hp"
Cohesion: 0.33
Nodes (9): size_t, vector, Vector3d, dorfler_coarsen_mark(), dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth(), Vector3d (+1 more)

### Community 343 - "ADR-0021: Varyhedron — variable polyhedral packing mesher"
Cohesion: 0.33
Nodes (6): ADR-0021: Varyhedron — variable polyhedral packing mesher, Alternatives rejected, Consequences, Context, Decision, Research anchors

### Community 344 - "M-A1 — first trained advisor (2026-08-10)"
Cohesion: 0.07
Nodes (29): 0003 — Training log, Batch 1, Capacity was not the problem, Corpus and ground truth, Corpus widened for power, not coverage, Data, Data and artifact, Does it choose a better mesh than the default? (+21 more)

### Community 345 - "HpSystem"
Cohesion: 0.12
Nodes (16): Index, SparseMatrix, HpSystem, k, local_sign, local_to_global, mode_nodes, n_modes (+8 more)

### Community 346 - "SurfaceTessellation"
Cohesion: 0.14
Nodes (16): array, uint32_t, uint8_t, vector, Vector3d, LoadRegion, hi, lo (+8 more)

### Community 347 - "CDT2d_ConstraintWalker"
Cohesion: 0.09
Nodes (23): DList, CDT2d_ConstraintWalker, i, j, t, t_prev, v, v_cnstr (+15 more)

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
Cohesion: 0.12
Nodes (13): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, Delaunay2d(), geo_pause(), atomic (+5 more)

### Community 352 - "ConstrainedLloydParams"
Cohesion: 0.12
Nodes (17): CvtLloydParams, h_floor, max_iters, move_tol_rel, size_at, SizeFieldFn, ConstrainedLloydParams, lloyd (+9 more)

### Community 353 - "vec4"
Cohesion: 0.06
Nodes (43): ConvexCellFlags, Frame, IncidentTetrahedra, begin_task(), cancel(), ConvexCell::clip_by_plane(), ConvexCell::clip_by_plane_fast(), ConvexCell::ConvexCell() (+35 more)

### Community 354 - "backend.cpp"
Cohesion: 0.39
Nodes (8): backend_description(), string, init_runtime_performance(), openmp_default_threads(), openmp_enabled(), openmp_max_threads(), performance_description(), set_openmp_threads()

### Community 355 - "RuntimeError"
Cohesion: 0.24
Nodes (24): RuntimeError, _bbox(), _cell_type_summary(), _dependency_version(), _deterministic_subsample(), _distance_statistics(), _exact_point_distances(), _file_snapshot() (+16 more)

### Community 356 - "ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)"
Cohesion: 0.20
Nodes (10): 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3), 2. Dual hard-block (ADR-0024 Q8), 3. `third_party/` plan, 4. Order (do not invent another), ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block), Alternatives rejected, Consequences, Context (+2 more)

### Community 357 - "analyze_solve_cost"
Cohesion: 0.28
Nodes (12): already_normalized(), analyze_solve_cost(), Dirichlet, SparseMatrix, vector, DisjointSet, parent_, elimination_forest() (+4 more)

### Community 358 - "RefinementPlan"
Cohesion: 0.09
Nodes (24): SizeFieldFn, vector, Vector3d, RefinementPlan, geometry_curvature_from_brep, h_fine, h_min, n_bc_seeds (+16 more)

### Community 359 - "load_from_args"
Cohesion: 0.14
Nodes (17): load_from_args(), load_json(), main(), model_config(), Any, Architecture descriptor persisted inside every checkpoint., Build the ``AdvisorNet`` construction descriptor.      ``hidden`` is deliberat, ``load_dataset`` driven by a parser built with :func:`add_split_args`. (+9 more)

### Community 360 - "wall_tangential_project"
Cohesion: 0.09
Nodes (24): size_t, WallProjectStats, max_surface_residual, mean_surface_residual, n_iters, n_moved, n_reverted, n_wall_nodes (+16 more)

### Community 361 - "ADR-0044: GLM cannot be the math library, and is not a useful second one"
Cohesion: 0.13
Nodes (13): Anti-cheat boundary, mathlib probe — GLM vs Eigen, Reading the numbers, What it measures, 1. Scope: GLM cannot express most of what this codebase does, 2. Performance: where GLM can compete, it is a wash, 3. The one large margin is an Eigen finding, not a GLM one, 4. Numerics: GLM's only real offer is disqualified (+5 more)

### Community 362 - "ADR-0036: A symmetric part gets a symmetric tiling"
Cohesion: 0.13
Nodes (15): 1. The report, 2. What was actually wrong, 3. The fix, 4. Measured, 5. Rejected alternative, measured rather than argued, 6. What is still open, 7. Consequences, 8. Follow-up: the order-dependent passes and the tessellation ceiling (+7 more)

### Community 363 - "build_graded_lattice"
Cohesion: 0.09
Nodes (36): FillProgressTets, fill_progress_phase(), FillProgressElementsScope, removed_, scope_, tets_, FillProgressScope, last_ (+28 more)

### Community 364 - "varyhedron_fill_surface"
Cohesion: 0.06
Nodes (52): array, uint32_t, vector, Vector3d, TetFillOutput, boundary_quads, h, nodes (+44 more)

### Community 365 - "ADR-0043: A film someone can read"
Cohesion: 0.13
Nodes (15): 10. The hero is now a load path, not a prop, 11. Cause and effect may be aligned, never relabelled, 12. The solver pane teaches with the result, 13. One frame must contain the whole motion, 1. What was wrong with the film, 2. The four rows, 3. One subject per chapter, 4. Holding on results (+7 more)

### Community 366 - "hp_topology.cpp"
Cohesion: 0.23
Nodes (14): array, EdgeKey, pair, QuadKey, TriKey, uint32_t, edge_key(), elem_edge() (+6 more)

### Community 367 - "SolvePhaseTimings"
Cohesion: 0.14
Nodes (13): VectorXd, LinearSolveResult, cost, phases, u, SolvePhaseTimings, analyze_ms, assemble_ms (+5 more)

### Community 368 - "CvtLloydStats"
Cohesion: 0.12
Nodes (16): CvtLloydResult, positions, stats, CvtLloydStats, converged, domain_diag, geogram_ok, max_move (+8 more)

### Community 369 - "lines"
Cohesion: 0.14
Nodes (14): lines, cinema: act build frames 1008..1655 t 16.8000..27.6000 s, cinema: act deliberate frames 469..1007 t 7.8000..16.8000 s, cinema: act mesh_hold frames 1656..1979 t 27.6000..33.0000 s, cinema: act skeleton frames 0..468 t 0.0000..7.8000 s, cinema: act solve frames 1980..3599 t 33.0000..60.0000 s, cinema: advisor bench/advisor candidates 38 gate_threshold 0.05 frames 39 decision hybrid_zoo h_rel 0.1 order 1 adapt_passes 0 eta_target 0 vetoed 1 ood_distance nan applied 0, cinema: maths glyphs merged from /usr/share/fonts/google-noto/NotoSansMath-Regular.ttf (+6 more)

### Community 370 - "structured_mesh.hpp"
Cohesion: 0.06
Nodes (29): Stress, box_faces(), box_nodes(), ElementType, uint32_t, vector, Vector3d, one_cell() (+21 more)

### Community 371 - "Commands"
Cohesion: 0.14
Nodes (14): `backend`, `calibrate --out host.json`, `check <part> [--scale f]`, Commands, Default BC selection, `diag <part> [flags]`, Inputs, `mesh <part> [flags]` (+6 more)

### Community 372 - "brep_fidelity.cpp"
Cohesion: 0.40
Nodes (15): append_triangle(), boundary_surface(), boundary_surface_volume(), brep_fidelity_summary(), FreeFace, size_t, uint32_t, vector (+7 more)

### Community 373 - "resolve_campaign"
Cohesion: 0.67
Nodes (3): main(), Path, resolve_campaign()

### Community 375 - "MixedCell"
Cohesion: 0.15
Nodes (14): MixedCellKind, array, uint32_t, uint8_t, vector, MixedCell, kind, n_nodes (+6 more)

### Community 376 - "report_network.py"
Cohesion: 0.24
Nodes (13): Lighten a palette colour towards the page, for large filled areas., tint(), _arrow(), _box(), checkpoint_shape(), contract_heads(), input_groups(), network_layout() (+5 more)

### Community 377 - "calibration.py"
Cohesion: 0.17
Nodes (23): choose_threshold(), conformal_report(), fit_ood(), fold_report(), main(), ood_scores(), Any, ndarray (+15 more)

### Community 378 - "rebuild_results.py"
Cohesion: 0.18
Nodes (21): main(), parse_args(), part_names(), plan_order(), prefix_divergence(), Any, Namespace, Path (+13 more)

### Community 379 - "pressure_face"
Cohesion: 0.19
Nodes (13): axis_aligned_planar_faces(), _in_box(), is_primary_load_face(), planar_face_area_in_box(), PlanarFace, pressure_face(), Exact area of one planar CAD face clipped by an axis-aligned region., Split the free end face into two disjoint load patches for c3.      A mid-span (+5 more)

### Community 380 - "probe_util.hpp"
Cohesion: 0.23
Nodes (11): combine_mesher_notes(), dominant_axis(), face_mean_displacement_component(), face_mean_displacement_mag(), global_max_displacement_mag(), size_t, string, uint32_t (+3 more)

### Community 382 - "figstyle.py"
Cohesion: 0.05
Nodes (67): FuncFormatter, assert_glyphs(), _charmap(), clamp_to_floor(), colorbar(), _covers(), dark_stage(), digest() (+59 more)

### Community 383 - "Progress"
Cohesion: 0.67
Nodes (3): 2026-08 (second half), 2026-09, Progress

### Community 384 - "Geogram / restricted CVT — vendoring study path"
Cohesion: 0.15
Nodes (13): 1. Why Geogram BSD-3 (not clean-room clipped Voronoi), 2.1 Vendor from Geogram (BSD-3), 2.2 We write ourselves, 2.3 Dual hard-block, 2. What to vendor vs what we write, 3. Dependency order (do not invent another), 4. Packing context (how this sits in varyhedron), 5. Vendored `third_party/` layout (+5 more)

### Community 387 - "capture"
Cohesion: 0.15
Nodes (13): capture, auto_spec, duration_s, env, ffmpeg, fps, frame_count, frame_size (+5 more)

### Community 388 - "LiveProgress"
Cohesion: 0.11
Nodes (18): size_t, uint32_t, LiveProgress, cfg_id, cg_iter, cg_resid, elapsed_ms, n_elems (+10 more)

### Community 389 - "Advisor::decide"
Cohesion: 0.21
Nodes (11): Advisor::decide(), argmax(), FeatureColumns, size_t, vector, from_log10(), sigmoid(), string (+3 more)

### Community 390 - "SizeSource"
Cohesion: 0.12
Nodes (24): SizeSource, h, x, min_value, SpectralSizingReport, applied, budget_met, energy_kept (+16 more)

### Community 391 - "Triangle"
Cohesion: 0.25
Nodes (10): ConvexCell::connect_triangles(), ushort, make_triangle(), make_triangle_with_flags(), Triangle, i, j, k (+2 more)

### Community 392 - "Predicates_psm.h"
Cohesion: 0.09
Nodes (37): aligned_3d(), aligned_3d_exact(), det2x2(), det3x3(), det4x4(), geo_clamp(), geo_cmp(), geo_sgn() (+29 more)

### Community 393 - "indicators.cpp"
Cohesion: 0.13
Nodes (22): vector, VertexCurvature, kappa, VertexThickness, thickness, build_tri_grid(), array, size_t (+14 more)

### Community 394 - "manifest.json"
Cohesion: 0.50
Nodes (3): generated_utc, git_rev, images

### Community 395 - "BcSelection"
Cohesion: 0.15
Nodes (13): BcSelection, face_fallback, faces, fallback_band, from_box, nodes, region, slab_nodes (+5 more)

### Community 396 - "VolumeMeshOutput"
Cohesion: 0.06
Nodes (37): GeometryVolumeAssessment, available, cad_volume, mesh_volume, relative_error, GeometryVolumeLimitError, assessment, solved_stage (+29 more)

### Community 397 - "pointer_"
Cohesion: 0.09
Nodes (17): function_pointer, const_pointer, const_reference, reference, pointer_, aligned_allocator, ALIGNMENT, aligned_malloc() (+9 more)

### Community 398 - "Matrix"
Cohesion: 0.20
Nodes (9): FT, initializer_list, matrix_type, FT, Matrix, coeff_, dim, transform_point() (+1 more)

### Community 399 - "CircularFeature"
Cohesion: 0.15
Nodes (13): CircularFeature, axis_dir, axis_point, radius, select_band, Vector3d, array, uint32_t (+5 more)

### Community 400 - "function"
Cohesion: 0.12
Nodes (17): ConvexCell::for_each_Voronoi_vertex(), function, Thread, parallel_for_slice(), ParallelForSliceThread, from_, func_, to_ (+9 more)

### Community 401 - "BRepInspection"
Cohesion: 0.08
Nodes (24): BRepInspection, available, closed, closed_shell_count, edge_count, face_count, shell_count, solid_count (+16 more)

### Community 402 - "Stages"
Cohesion: 0.17
Nodes (12): 1. Campaign, 2. Dataset, 3. Train, 4-5. Cross-validation and calibration, 6. Export, 7. Verify, Advisor training, Host cost calibration (once per labelling machine) (+4 more)

### Community 403 - "declare_arg"
Cohesion: 0.16
Nodes (29): ArgFlags, GroupArgs, arg_group(), declare_arg(), declare_arg_group(), declare_arg_percent(), Group, args (+21 more)

### Community 404 - "Split"
Cohesion: 0.13
Nodes (21): One side of the group hold-out split, fully materialized as numpy arrays., Return a row-filtered copy (used to apply the pruning ledger)., Split, device, head_residuals(), keep_mask(), load_ledger(), load_pruned() (+13 more)

### Community 405 - "resolve_boundary_loops"
Cohesion: 0.23
Nodes (15): array, Loop, map, uint32_t, vector, extract_boundary_faces(), extract_boundary_polys(), quadratic_edge_mids() (+7 more)

### Community 406 - "SolveCostEstimate"
Cohesion: 0.17
Nodes (11): Dirichlet, Index, uint64_t, SolveCostEstimate, cg_bytes_per_iter, cg_flops_per_iter, factor_flops, factor_nnz (+3 more)

### Community 407 - "ScorecardInfo"
Cohesion: 0.18
Nodes (11): ScorecardInfo, accuracy_rel_err, chordal_efficiency_max, edge_hausdorff_over_h, has_health_ok, health_ok, min_element_quality, n_dof (+3 more)

### Community 408 - "expansion"
Cohesion: 0.19
Nodes (20): compress_expansion(), expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 409 - "Matrix"
Cohesion: 0.27
Nodes (6): FT, matrix_type, Matrix, coeff_, dim, mult()

### Community 410 - "expansion"
Cohesion: 0.17
Nodes (20): expansion, compress_expansion(), expansion::compare(), expansion::is_same_as(), expansion::optimize(), fast_expansion_diff_zeroelim(), fast_expansion_sum_zeroelim(), fast_two_sum() (+12 more)

### Community 411 - "GradedTetFillOutput"
Cohesion: 0.06
Nodes (33): FillProgress, cells_done, cells_total, elements_so_far, sub_phase, GradedTetFillOutput, classification_refinement_levels, classification_volume_error (+25 more)

### Community 414 - "Any"
Cohesion: 0.08
Nodes (24): Line2D, annotate_n(), axes_off(), convergence(), Fit, fit_loglog(), loglim(), LogLimits (+16 more)

### Community 415 - "ClipBox"
Cohesion: 0.32
Nodes (11): ClipBox, max, min, seed_lattice_sites(), constrained_lloyd_cvt(), vector, Vector3d, dist_to_domain_boundary() (+3 more)

### Community 416 - "IndexType"
Cohesion: 0.21
Nodes (8): IndexType, KeepOrderType, basic_bindex, indices, basic_quadindex, indices, basic_trindex, indices

### Community 417 - "Traction"
Cohesion: 0.05
Nodes (54): BoxSel, hi, lo, set, size_t, span, string, vector (+46 more)

### Community 418 - "HealthInfo"
Cohesion: 0.25
Nodes (8): HealthInfo, free_residual_rel, has_load_area_ok, load_area_ok, n_orphans, ok, present, reaction_sum_err

### Community 419 - "Top-5 shortlist with implementation sketches"
Cohesion: 0.18
Nodes (10): 1. WL-01 — MMS compiler and observed-order gates, 2. WL-02 — conforming hierarchical $hp$ production cutover, 3. WL-03 — DWR QoI adaptivity, 4. WL-04 — exact/curved CAD boundary geometry, 5. WL-05 — certified QoI intervals, Do not bother—at least not yet, Outside-the-box leverage across the whole program, Ranked idea bank (+2 more)

### Community 420 - "ClippedCell"
Cohesion: 0.13
Nodes (17): ClippedCell, barycenter, empty, n_planes, n_triangles, volume, size_t, Vector3d (+9 more)

### Community 421 - "gif"
Cohesion: 0.18
Nodes (11): attempts, bytes, duration_s, file, fps, max_bytes, sha256, start_s (+3 more)

### Community 422 - "0009 — The v5 corpus: a better mesher, better predictions, and no decision win"
Cohesion: 0.29
Nodes (7): 0009 — The v5 corpus: a better mesher, better predictions, and no decision win, 1. Why there is a v5, 2. The retrain, 3. Decision quality: the honest result is "no change", 4. The tolerance selector: still not deliverable, 5. Deployed behaviour, checked end to end, 6. Provenance

### Community 423 - "AdvisorDecision"
Cohesion: 0.03
Nodes (92): ActivationFrame, action, candidate, fc1, fc2, gate_pass, heads, input (+84 more)

### Community 424 - "HexFillOutput"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 425 - "properties"
Cohesion: 0.10
Nodes (20): type, type, enum, cad_face, detected_from, kind, recommended_h_m, refused_at_h_m (+12 more)

### Community 426 - "element_jacobians_positive"
Cohesion: 0.29
Nodes (10): coords_of(), Dynamic, ElementType, Matrix, span, uint32_t, element_jacobians_positive(), rule_positive() (+2 more)

### Community 427 - "Case"
Cohesion: 0.18
Nodes (11): Case, feature_refine, h, name, path, plane, array, uint32_t (+3 more)

### Community 428 - "properties"
Cohesion: 0.08
Nodes (26): description, type, description, type, type, description, type, type (+18 more)

### Community 429 - "vec3Hg"
Cohesion: 0.20
Nodes (7): optimize_number_representation(), vec3Hg, w, x, y, z, vec3HgLexicoCompare

### Community 430 - "CommandLineDesc"
Cohesion: 0.25
Nodes (8): Args, GroupNames, Groups, CommandLineDesc, args, argv0, group_names, groups

### Community 431 - "PHASES — Guided DAG Plan"
Cohesion: 0.20
Nodes (10): P0 — Decisions & scaffolding, P1 — Reference solver on standard elements (the trustworthy baseline), P2 — Mesh core + tet meshing + validity, P3 — Geometric feature analysis → a priori hybrid meshing, P4 — Polyhedral elements (VEM) [parallel with P3 after P2], P5 — Adaptive loop (the product), P6.5 — GUI (ADR-0006), P6 — Performance engineering (+2 more)

### Community 432 - "0010 — The v6 corpus: the geometry objective stopped discriminating"
Cohesion: 0.25
Nodes (8): 0010 — The v6 corpus: the geometry objective stopped discriminating, 1. Why there is a v6, 2. The retrain, 3. Prediction improved; the decision is still a tie, 4. The interesting result: conformity removed a decision problem, 5. The deployed rule is unchanged, and one deployed bug was fixed, 6. The tolerance selector is still not deliverable, 7. Provenance

### Community 433 - "plot_hole_bug.py"
Cohesion: 0.16
Nodes (17): cad_bore_volume(), classify(), draw_panel(), load_mesh(), main(), Mesh, mesh_volume(), parse_args() (+9 more)

### Community 434 - "2. Training tracks (all four selected, in dependency order)"
Cohesion: 0.20
Nodes (10): 0. What the box needs (bring-up checklist), 1.1 v4 regeneration — COMPLETE 2026-08-14, gcc only, 1. Campaign regeneration (decided: everything, after the tangle fix), 2. Training tracks (all four selected, in dependency order), 2a. Retrain the current advisor (first, cheap, de-risks the pipeline), 2b. Learned error estimator / h-selector (second), 2c. Per-region size field GNN (the flagship, 1–3 day runs), 2d. Learned repair policy (research-grade, LAST) (+2 more)

### Community 435 - "advisor_fields"
Cohesion: 0.20
Nodes (10): adapt_passes, applied, candidates, eta_target, frames, gate_threshold, h_rel, order (+2 more)

### Community 436 - "voronoi_neighbours"
Cohesion: 0.11
Nodes (32): DomainTet, centroid, v0, v1, v2, v3, Vector3d, ClipPlane (+24 more)

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
Cohesion: 0.09
Nodes (21): GeometryDescriptors, area_over_v23, aspect_max, aspect_mid, available, curved_area_frac, cyl_area_frac, face_area_cv (+13 more)

### Community 441 - "settings-frontier-1 — campaign-1 close-out"
Cohesion: 0.33
Nodes (5): Caveat, Product default decision (feedback-loop), settings-frontier-1 — campaign-1 close-out, Survivors (tier-2 keep), Tooling top-score cfg (global ranking)

### Community 442 - "figures.py"
Cohesion: 0.28
Nodes (14): epoch_series(), _finding(), load_json(), main(), parse_args(), _pct(), Any, Namespace (+6 more)

### Community 443 - "ADR-0035: Boundary nodes belong on the BRep, not near it"
Cohesion: 0.20
Nodes (10): 1. The report, 2. What was actually wrong, 3. The fix, 4. Measured, h = 8 mm, `polymesh diag`, 5.1 Facet kinks: the defect a user actually sees, 5.2 Measured, h = 8 mm, second wave, 5. Second wave: the exterior that ships, 6. What is still open, and why it is not hidden (+2 more)

### Community 444 - "ADR-0038: A fixture is applied to the boundary, not to a volume of nodes"
Cohesion: 0.20
Nodes (10): 1. The report, 2. What the purple region actually was, 3. The library never had it, 4. The rule, and where it lives, 5. What it did to the answer, 6. The picture, 7. What this does *not* fix, 8. Consequences (+2 more)

### Community 445 - "M5 VEM gate — campaign results (2026-07-13)"
Cohesion: 0.29
Nodes (7): Campaign, Hard-learned (do not re-open without new evidence), History, M5 VEM gate — campaign results (2026-07-13), Next to flip M5 → done, Results (latest: VEM τ=0.08 + plate-only wall + cylinder shell sites + OCC snap), What landed (code)

### Community 446 - "OmpSettingsGuard"
Cohesion: 0.40
Nodes (3): OmpSettingsGuard, dynamic, threads

### Community 447 - "LocalRefineStats"
Cohesion: 0.20
Nodes (10): size_t, LocalRefineStats, n_bisections, n_chord_mids, n_input_tets, n_marked, n_new_nodes, n_output_tets (+2 more)

### Community 461 - "scene.hpp"
Cohesion: 0.04
Nodes (58): optional, assess_load_area(), AuthoredAreaCheck, checked, consistent, rel_diff, check_authored_area(), LoadAreaStatus (+50 more)

### Community 462 - "ADR-0042: The advisor explains itself on screen"
Cohesion: 0.20
Nodes (10): 1. What the heatmap could not show, 2. The activations come from the graph, not from a second implementation, 3. The mesh evolves at the granularity the mesher has, 4. What is cosmetic, exactly, 5. The case is chosen by the gate, not by what looks good, 6. The two halves share the clock, because the causal link is the point, 7. The fields animate in the order the answer is computed, 8. Packaging and provenance (+2 more)

### Community 463 - "TetRecipe"
Cohesion: 0.20
Nodes (10): TetRecipe, a, b, c, kind, mode, n1, n2 (+2 more)

### Community 464 - ".operator()"
Cohesion: 0.20
Nodes (7): EdgeKeyHash, EdgeKey, QuadKey, size_t, TriKey, QuadKeyHash, TriKeyHash

### Community 465 - "Decision"
Cohesion: 0.12
Nodes (16): 10. ZZ patch recovery may not extrapolate an under-determined fit, 1. Project quadratic boundary mid-edge nodes onto exact CAD, 2. The validity guard follows stiffness quadrature, 3. Do not ship the void-jut repair, 4. A peak truth requires a peak probe, 5. Render topology, not raw connectivity order, 6. Every order-2 producer projects, 7. Engine changes require a row-schema bump and retraining (+8 more)

### Community 466 - "0004 — Model card: learned mesh advisor"
Cohesion: 0.12
Nodes (17): 0004 — Model card: learned mesh advisor, Current generation, Decision quality, Deployment cost, Evaluation harness, Intended use, Known failure modes, Out-of-scope use (+9 more)

### Community 467 - "0005 — Data card: advisor training corpus"
Cohesion: 0.13
Nodes (15): 0005 — Data card: advisor training corpus, Action grid, Composition, Consequence for interpretation, Coverage and gaps, Ethics and risk, How the corpus splits, and what it costs, Known defects in the features (+7 more)

### Community 468 - "python_test.hpp"
Cohesion: 0.15
Nodes (11): string, python_exe(), run_python_script(), string, run_python(), string, run_cmd(), slurp() (+3 more)

### Community 469 - "mean_lateral_radial_residual"
Cohesion: 0.40
Nodes (5): array, uint32_t, vector, Vector3d, mean_lateral_radial_residual()

### Community 470 - "WindowsThreadPoolManager"
Cohesion: 0.07
Nodes (24): DWORD, LONG, LPVOID, ostream, PTP_CALLBACK_INSTANCE, PTP_CLEANUP_GROUP, PTP_POOL, PTP_WORK (+16 more)

### Community 472 - "commands_calibrate.cpp"
Cohesion: 0.39
Nodes (7): benchmark_bytes_per_second(), benchmark_flops_per_second(), benchmark_reference_mesh_ms(), cmd_calibrate(), span, vector, median_sample()

### Community 473 - "Limits"
Cohesion: 0.29
Nodes (7): is_specialized, numeric_limits<T>, Limits, LimitsHelper, LimitsHelper<T, true>, numbits, size

### Community 474 - "detect_hole_roi"
Cohesion: 0.15
Nodes (14): bbox_of(), _cell_centroids(), detect_hole_roi(), _empty_core(), _occupancy(), Densest in-plane radial band of free-surface nodes about ``axis``.      Returns, Rasterise the mid-slab normal to ``axis``, resolving ``radius``.      Returns (g, Does the void at the candidate radius reach the outside of the part?      A bore (+6 more)

### Community 475 - "Decision"
Cohesion: 0.15
Nodes (13): 1. Learn action-conditioned outcomes, never a "best config" label, 2. A second head predicts the sizing field, 3. Context features are scale-free and cheap, 4. Reference truth, 5. BC sampling varies topology, not magnitude, 6. Corpus and licensing, 7. Deployment: LightGBM C API, no Python at runtime, 8. The advisor proposes; the estimator disposes (+5 more)

### Community 476 - "Decision"
Cohesion: 0.15
Nodes (13): 1. Truth comes from outside this engine, 2. Applied load may not depend on mesh resolution, 3. One rule has one implementation, 4. A check that cannot verify must not report success, 5. An artifact write may not kill a run, 6. Evaluate the rule you ship, on a split that does not leak, 7. Promotion may only overwrite truth this repo generated, 8. Accuracy is re-derived at build time, not frozen into rows (+5 more)

### Community 477 - "Decision"
Cohesion: 0.14
Nodes (13): 1. A quadrature rule is defined by the shape functions it integrates, and a test must say so, 2. A retracted cause is deleted, not reworded, 3. Display and physics are two different boundary contracts, 4. One definition of a cell's volume, 5. Geometry may only be deleted if the boundary survives it, ADR-0030: The ruler was wrong — retracting the fan-transition defect, and drawing the curvature we already compute, Consequences, Context (+5 more)

### Community 478 - "SampleDistribution"
Cohesion: 0.14
Nodes (16): DistanceDistribution, metres, over_bbox_diagonal, over_h, size_t, SampleDistribution, count, max (+8 more)

### Community 479 - "CinemaCellKey"
Cohesion: 0.22
Nodes (7): CinemaCellKey, corners, family, CinemaCellKeyHash, int64_t, uint8_t, CinemaCellFamily

### Community 480 - "SpectrumResult"
Cohesion: 0.22
Nodes (9): SpectrumResult, eigen, glm, label, SpectrumScore, over_tol, reported_failures, total (+1 more)

### Community 481 - "CgAttempt"
Cohesion: 0.22
Nodes (9): CgStop, cg_stop_text(), CgAttempt, iterations, reliable_restarts, stop, true_relative_residual, x (+1 more)

### Community 482 - "Portable-cost advisor retrain"
Cohesion: 0.22
Nodes (9): Campaign coverage and dataset, Deployed selection behavior, Feature and corpus expansion, Honest limitations, Host calibration and reporting, Portable-cost advisor retrain, Precision and architecture experiments, Solver instrumentation (+1 more)

### Community 483 - "ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening"
Cohesion: 0.18
Nodes (11): 1. Spectral sizing (`adapt::spectral`, new module), 2. Coarsening (`HpAction::kCoarsen` + loop executor), 3. Budget-feasible advisor chooser, 4. CG equilibration, 5. Advisor hygiene (measured, no retrain), ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening, Consequences, Context (+3 more)

### Community 484 - "Campaign metrics — normative definitions for agents"
Cohesion: 0.22
Nodes (9): 1. Score vs dashboard vs gate, 2. Minimum scorecard (five numbers + residual gate), 3. Case-specific accuracy scores, 4. Chordal efficiency \(e\), 5. Gates and kills (not scores), 6. Displacement probes, 7. Agent checklist before claiming a campaign “win”, Campaign metrics — normative definitions for agents (+1 more)

### Community 485 - "Public CAD corpora for training a mesh advisor"
Cohesion: 0.22
Nodes (8): Commercial-development track, Concrete ingestion and labeling rules for PolyMesh, Dataset evaluation, Non-commercial research benchmark add-on, Public CAD corpora for training a mesh advisor, Recommended starter corpus for one workstation, The datasets that actually pair geometry, BCs, and structural FEA, What this means for PolyMesh

### Community 486 - "0001 — Advisor architecture"
Cohesion: 0.17
Nodes (12): 0001 — Advisor architecture, CLI, Data path, Deployment, Feature and action schema, Feature families, Heads, Inference (+4 more)

### Community 487 - "ADR-0037: A box selection is a region, and a smooth field is sampled on element sizes"
Cohesion: 0.22
Nodes (9): 1. The report, 2. Measuring the jag, 3.1 The load stopped on a staircase, 3.2 The wall smoother was not equalising anything, 3. Two causes, measured separately, 4. Four-fixture matrix, 5. What is left, and what it is, 6. Consequences (+1 more)

### Community 488 - "ADR-0040: A boundary condition names the exact closure of a CAD face"
Cohesion: 0.22
Nodes (9): 1. The report, 2. Cross-checking the GUI against the CLI, 3. Root cause A: nearest-triangle region roulette (GUI / SolveJob), 4. Root cause B: the CLI's default end slab is not a face, 5. The fix, 6. Measured, 7. What this does not fix, 8. Consequences (+1 more)

### Community 489 - "Architecture decision records"
Cohesion: 0.22
Nodes (9): Adaptivity / sizing, Advisor, Architecture decision records, Geometry / CAD, GUI / presentation, Mesher, Notes, Process / project (+1 more)

### Community 490 - "vtu_wire_png.py"
Cohesion: 0.29
Nodes (9): boundary_edges(), cell_faces(), convex_hull_faces(), face_key(), _fixed_faces(), _ordered_planar_hull(), _polyhedron_face_blocks(), Order a planar facet's corner nodes, dropping collinear/interior nodes. (+1 more)

### Community 491 - "cost_labels.py"
Cohesion: 0.61
Nodes (8): finite_float(), host_calibration(), main(), mesh_work(), portable_cost_label(), self_test(), solve_bytes(), solve_flops()

### Community 492 - "rationalg"
Cohesion: 0.33
Nodes (3): rationalg, denom_, num_

### Community 493 - "BrepFidelitySummary"
Cohesion: 0.18
Nodes (11): BrepFidelitySummary, available, chamfer_mean, dist_max, dist_p95, dist_p99, n_samples, normal_angle_p95_rad (+3 more)

### Community 494 - "0002 — Objectives and guardrails"
Cohesion: 0.20
Nodes (10): 0002 — Objectives and guardrails, 1. Penalty barrier — during training, 2. Hard clamps — at inference, 3. Gate, OOD refusal and veto — at inference, Guardrails, Loss, Outlier pruning, Staged curriculum (+2 more)

### Community 495 - "vecng<4, T>"
Cohesion: 0.22
Nodes (6): vecng<4, T>, dim, w, x, y, z

### Community 496 - "render_surface"
Cohesion: 0.29
Nodes (8): line, array, ElementType, uint8_t, element_type_color(), fill_background(), render_surface(), to_byte()

### Community 497 - "command"
Cohesion: 0.25
Nodes (8): command, -a, --auto, build/apps/gui/polymesh-gui, load tests/fixtures/parts/wishbone.step; h 5.5; material 200 0.3; mesher tet; solver direct; order 1; feature off; spectral on; fix 5; fix 6; loadface 0 -25000 0 -40000; cinema on; cinema advisor bench/advisor; adapt 0 0; wire off; solve; record build/cinema/frames 3600; quit, -s, -screen 0 1920x1080x24, xvfb-run

### Community 498 - "ADR-0039: A stranded boundary node is rescued, not abandoned"
Cohesion: 0.25
Nodes (8): 1. The report, 2. Where the tail lives, 3. The fix: two-stage snap, 4. Two classification defects the dump exposed, 5. Measured, graded, `polymesh diag --no-solve`, 6. What this does not fix, 7. Consequences, ADR-0039: A stranded boundary node is rescued, not abandoned

### Community 499 - "ADR-0041: A deformed render carries its undeformed outline"
Cohesion: 0.25
Nodes (8): 1. The report, 2. The warp was applied. The camera cancelled it, 3. The other five figures showed nothing, for a different reason, 4. The rule, 5. The cone's fixture, and a correction to ADR-0038, 6. What this does not change, 7. Consequences, ADR-0041: A deformed render carries its undeformed outline

### Community 500 - "plot_evaluation.py"
Cohesion: 0.08
Nodes (54): advisor_evaluation(), budget_phrase(), band_levels(), coincident(), collapse_families(), failure_rates(), gate_threshold(), head_of() (+46 more)

### Community 501 - "0008 — The v4 corpus, the retrain, and the metric that punished being right"
Cohesion: 0.22
Nodes (9): 0008 — The v4 corpus, the retrain, and the metric that punished being right, 1. Why there is a v4 at all, 2. The retrain, 3. Decision quality: the advisor now leads on every reference it should, 4.1 The advisor was right and the label was wrong, 4.2 The macro mean still needs a fold-size guard, 4. The open question from 0006 §4, answered: one fold was eating the mean, 5. The tolerance selector: much closer, still not deliverable (+1 more)

### Community 502 - "ADR-0033: A gate must measure what ships"
Cohesion: 0.22
Nodes (9): 1. The pyramid gate measured a different cell than the diagnostics, 2. `hex_fill` gated on sampled signs, and nothing else, 3. `snap_boundary_nodes` computed a whole-mesh proof and threw it away, ADR-0033: A gate must measure what ships, Consequences, Context, Decision, Open: the ellipsoidal boss, and where the wall gets stuck (+1 more)

### Community 503 - "ParityResult"
Cohesion: 0.25
Nodes (4): ParityResult, The parity summary lines, taps reported as their own claim., Worst deviation from the float64 reference, per output group.      The C6 outp, Element-wise worst of two runs of the same graph.

### Community 504 - "ADR-0032: The mesh may not depend on which standard library built it"
Cohesion: 0.40
Nodes (5): ADR-0032: The mesh may not depend on which standard library built it, Amendment 2026-08-21: an unstable sort is the same defect, Consequences, Context, Decision

### Community 505 - "json"
Cohesion: 0.05
Nodes (47): json, main(), Path, run_one(), git_rev(), main(), Advisor, apply_action (+39 more)

### Community 506 - "ADR-0031: A jut has a side"
Cohesion: 0.25
Nodes (7): ADR-0031: A jut has a side, Consequences, Context, Decision, The defect, The test, What this does not fix

### Community 507 - "HostCalibration"
Cohesion: 0.25
Nodes (7): HostCalibration, bytes_per_s, flops_per_s, generated_utc, host, ref_mesh_ms, string

### Community 508 - "GeometryCompleteness"
Cohesion: 0.25
Nodes (8): GeometryCompleteness, available, brep_volume, complete, mesh_volume, relative_volume_error, relative_volume_tolerance, evaluate_geometry_completeness()

### Community 509 - "FaceConformityStats"
Cohesion: 0.25
Nodes (8): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces

### Community 510 - "FanSpan"
Cohesion: 0.25
Nodes (8): FanSpan, apex, corners, end, first, array, size_t, uint32_t

### Community 511 - "campaign_progress.cpp"
Cohesion: 0.10
Nodes (25): mutex, path, size_t, string, time_point, vector, ProgressHeartbeat::loop(), ProgressHeartbeat::ProgressHeartbeat() (+17 more)

### Community 512 - "enum"
Cohesion: 0.29
Nodes (7): status, description, enum, failed, ok, refused, timeout

### Community 513 - "0006 — The clean-data retrain, and what it cost the advisor's claims"
Cohesion: 0.29
Nodes (7): 0006 — The clean-data retrain, and what it cost the advisor's claims, 1. The old labels were a different mesher, not a stale one, 2. `latest.pt` was never the model worth shipping, 3. The cost heads were missing the scale law, 4.1 The obvious fix for it was tried and is wrong, 4. What the retrain did to the product claim, 5. Provenance

### Community 514 - "as_bytes"
Cohesion: 0.50
Nodes (4): as_bytes(), byte, string_view, vector

### Community 515 - "Geogram subset — what PolyMesh takes"
Cohesion: 0.33
Nodes (6): Dual hard-block, Geogram subset — what PolyMesh takes, How PolyMesh consumes it, Included, Stripped / not vendored, Upgrade path

### Community 516 - "0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number"
Cohesion: 0.33
Nodes (6): 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number, 1. The track asked for a deliverable, not a model, 2. The measurement, 3. A safety margin does not fix it, and the way it fails is the finding, 4. What ships, and what does not, 5. Provenance

### Community 517 - "FaceGeometry"
Cohesion: 0.25
Nodes (8): FaceGeometry, area, centroid, diameter, drop_axis, max, min, Vector3d

### Community 518 - "surface_render.hpp"
Cohesion: 0.29
Nodes (7): size_t, RenderCoverage, pixels_covered, silhouette_area_px, SurfaceRender, coverage, image

### Community 519 - "refusal"
Cohesion: 0.40
Nodes (5): refusal, description, required, type, kind

### Community 520 - "advisor_fixture.cpp"
Cohesion: 0.29
Nodes (5): columns_of(), FeatureColumns, json, path, load()

### Community 521 - "HoleROI"
Cohesion: 0.40
Nodes (4): HoleROI, An ROI plus whether a hole was actually found inside it.      ``detected`` is th, Build the ROI box about ``axis`` and package the verdict with it., _roi_for()

### Community 522 - "schema_version"
Cohesion: 0.50
Nodes (4): schema_version, const, description, type

### Community 523 - "Case"
Cohesion: 0.25
Nodes (8): Case, edge_p99_over_h, mesher, name, node_max_over_h, node_p99_over_h, path, VolumeMesher

### Community 524 - "bore_wall"
Cohesion: 0.25
Nodes (8): bore_wall(), cell_faces(), array, pair, size_t, uint32_t, vector, surface_face_area()

### Community 525 - "VertexArray"
Cohesion: 0.25
Nodes (6): VertexArray, base_, nb_vertices_, stride_, VertexMesh, vertices

### Community 526 - "Progress history"
Cohesion: 0.29
Nodes (7): 2026-09-11 — CAD integration dependency release, Active (read this first), Background / older phases, Benchmark table, Done, Open issues, Progress history

### Community 527 - "SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver"
Cohesion: 0.29
Nodes (7): Architecture (pinned), Decisions — ratified at GATE 0 (2026-07-09; full rationale in docs/decisions/), Goals, Key technical positions (pinned unless a phase proves otherwise), Non-goals (v1), Problem statement, SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver

### Community 528 - "Case"
Cohesion: 0.50
Nodes (4): Case, h, name, path

### Community 529 - "Field verification — stress *and* deformation, pointwise"
Cohesion: 0.29
Nodes (7): 1. Deformation, 2. Stress, 3. The 0.7% the cantilever misses, and where it comes from, 4. Convergence of the stress recovery, 5. Two measurements that were wrong before they were right, 6. What this does not verify, Field verification — stress *and* deformation, pointwise

### Community 530 - "host"
Cohesion: 0.67
Nodes (3): description, type, host

### Community 531 - "label"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 532 - "Face"
Cohesion: 0.29
Nodes (7): Face, neighbour, owner, vertices, CellId, optional, VertexId

### Community 533 - "TetQuality"
Cohesion: 0.29
Nodes (7): size_t, TetQuality, max_volume, mean_aspect, min_aspect, min_volume, n_sliver

### Community 536 - "AnswersInfo"
Cohesion: 0.33
Nodes (6): AnswersInfo, load_area_rel_err, load_face_area, sigma_face_mean, strain_energy, tip_deflection

### Community 537 - "0011 — v7 retrain: authoritative curved CAD geometry"
Cohesion: 0.33
Nodes (6): 0011 — v7 retrain: authoritative curved CAD geometry, Calibration, tolerance and OOD, Decision quality against v6 (macro-mean regret, family-held-out folds), Provenance, What was regenerated, Why a whole generation

### Community 538 - "BRep face-tag BCs / probes (design stub)"
Cohesion: 0.33
Nodes (6): BRep face-tag BCs / probes (design stub), Exit criteria (future work item), Historical icecream instability (superseded fixture; why face tags), Out of scope for this stub, Target model (sketch), Why boxes are temporary

### Community 539 - "Protecting balls + local feature size (LFS)"
Cohesion: 0.33
Nodes (6): 1. Role, 2. CDS radius formula (must-change), 3. Reference, 4. Risk cases, 5. Agent checklist, Protecting balls + local feature size (LFS)

### Community 540 - "Status"
Cohesion: 0.33
Nodes (6): Current state, Evidence index, Methodology in force, Next, Open defects, Status

### Community 542 - "gradient_canvas"
Cohesion: 0.40
Nodes (6): gradient_canvas(), hex_to_rgb(), mix(), Parse a figstyle theme colour into 8-bit channels for PIL/PyVista., Blend two theme colours; used for the viewport's vertical gradient., Vertical viewport gradient, panel at the top easing to page at the foot.

### Community 543 - "Dirichlet"
Cohesion: 0.33
Nodes (5): Dirichlet, dof_values, Index, map, uint32_t

### Community 544 - "Image"
Cohesion: 0.33
Nodes (6): uint8_t, vector, Image, height, rgb, width

### Community 545 - "RenderView"
Cohesion: 0.33
Nodes (6): RenderView, azimuth_deg, elevation_deg, height, width, wireframe

### Community 546 - "model.cpp"
Cohesion: 0.40
Nodes (5): size_t, string, Vector3d, Model::load(), triangle_normal()

### Community 547 - "stages_run"
Cohesion: 0.40
Nodes (5): stages_run, frames, gif, mp4, poster

### Community 548 - "draw_colorbar"
Cohesion: 0.40
Nodes (5): draw_colorbar(), _font(), ImageDraw, A glyph-verified font at figstyle's point size for this canvas width.      ``o, Vertical viridis colour bar with SI-prefixed ticks (``0`` .. ``3.84 MPa``).

### Community 549 - "HexFace"
Cohesion: 0.40
Nodes (5): HexFace, fixed_axis, fixed_val, vary0, vary1

### Community 550 - "strain_displacement.hpp"
Cohesion: 0.40
Nodes (4): Dynamic, Matrix, MatrixXd, strain_displacement()

### Community 551 - "LexicoCompare"
Cohesion: 0.40
Nodes (3): GEOGRAM_API SOS_sort(), LexicoCompare, dim_

### Community 552 - "TypedThreadGroup"
Cohesion: 0.40
Nodes (3): THREAD, ThreadGroup, TypedThreadGroup

### Community 553 - "Row"
Cohesion: 0.50
Nodes (4): Row, eigen_ns, glm_ns, name

### Community 554 - "run_probe.sh"
Cohesion: 0.83
Nodes (3): build_and_run(), run_all(), run_probe.sh script

### Community 555 - "take"
Cohesion: 0.50
Nodes (4): take, duration, fps, frames

### Community 556 - "clip_rule_text"
Cohesion: 0.50
Nodes (4): clip_rule_text(), ordinal(), The rule sentence. Identical in every stress footer and caption., 92' -> '92nd', '99.5' -> '99.5th'. Keeps captions readable prose.

### Community 557 - "Polyhedral-FEA — agent notes"
Cohesion: 0.67
Nodes (3): graphify, Methodology in force, Polyhedral-FEA — agent notes

### Community 558 - "Learned mesh advisor — document index"
Cohesion: 0.67
Nodes (3): Current generation, Learned mesh advisor — document index, Series

### Community 559 - "same_node_bytes"
Cohesion: 0.67
Nodes (3): vector, Vector3d, same_node_bytes()

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **3396 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+3391 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **144 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `NodalMesh` connect `NodalMesh` to `FeaError`, `stress.cpp`, `ManufacturedSolution`, `PolyMesh`, `VolumeMeshOutput`, `MshModel`, `Edge`, `ReferenceCase`, `CircularFeature`, `cinema_timeline.cpp`, `bore_wall`, `SolveResult`, `solve_elastostatics`, `resolve_boundary_loops`, `CurvedGeometryResult`, `Projection`, `fill_progress_poll`, `d6_tier3.cpp`, `CantileverSetup`, `PassTrace`, `Model`, `element_jacobians_positive`, `NodalElement`, `eval_shape`, `run_one`, `SurfaceFace`, `HpElementDef`, `volume_mesh_impl`, `viewport.cpp`, `scene.hpp`, `assemble_body_load`, `Grid3d`, `test_local_refine.cpp`, `analyze_solve_cost`, `render_surface`, `structured_mesh.hpp`, `resource_budget.cpp`, `ExteriorConformStats`, `BoundaryProjectionContext`?**
  _High betweenness centrality (0.028) - this node is a cross-community bridge._
- **Why does `TriSurface` connect `TriSurface` to `GeomError`, `PointLookup`, `local_refine_tets`, `indicators.cpp`, `GradedFillState`, `graded_sizing.cpp`, `fill_progress_poll`, `surface_render.cpp`, `mixed_fill_snap.cpp`, `CartesianGrid`, `CadModel`, `Model`, `model.cpp`, `export_clipped_voronoi`, `volume_mesh_impl`, `scene.hpp`, `vector`, `build_graded_lattice`, `varyhedron_fill_surface`, `brep_fidelity.cpp`, `evaluate_curved_mesh_quality`?**
  _High betweenness centrality (0.026) - this node is a cross-community bridge._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _3396 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `FeaError` be split into smaller, more focused modules?**
  _Cohesion score 0.11912568306010929 - nodes in this community are weakly interconnected._
- **Should `analyze_campaign.py` be split into smaller, more focused modules?**
  _Cohesion score 0.11517165005537099 - nodes in this community are weakly interconnected._
- **Should `CurvedMeshMetrics` be split into smaller, more focused modules?**
  _Cohesion score 0.10526315789473684 - nodes in this community are weakly interconnected._