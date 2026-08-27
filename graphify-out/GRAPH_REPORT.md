# Graph Report - Polyhedral-FEA  (2026-08-26)

## Corpus Check
- 3117 files · ~24,955,189 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 10464 nodes · 21455 edges · 593 communities (414 shown, 179 thin omitted)
- Extraction: 95% EXTRACTED · 5% INFERRED · 0% AMBIGUOUS · INFERRED: 990 edges (avg confidence: 0.78)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `0b403a33`
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
- build_advisor_dataset.py
- render_showcase.py
- CinemaState
- PolyMesh
- App
- TetFillOutput
- MshModel
- index_t
- NodalMesh
- TestLabState
- Advisor measure-first program (canonical agent plan)
- AdaptSuggestion
- CantileverSetup
- solve_elastostatics
- .resize
- MirrorFrame
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
- Model
- scene.hpp
- metric_field.cpp
- CsrMatrix
- GeometrySizing
- Hand-calculated reference truths
- accuracy
- spectral_sizing.cpp
- Stats
- null
- NodalElement
- CalculiX / PolyMesh cantilever cross-validation
- POLYMESH_WITH_CUDA
- eval_shape
- Plan: Mesher / Solver Accuracy + Performance Overhaul
- gen_cad_parts.py
- gen_primitive_corpus.py
- Goodier Spherical Cavity Case
- ADR-0001 Geometry kernel
- Pareto analysis — `varyhedron-baseline-m9`
- D6 Tier-3 L-domain instrument
- fea::Element unified trait
- ValidityError
- cvt_export.cpp
- SolveResourceEstimate
- Delaunay_psm.h
- CI Grep-Audit Anti-Cheat Job
- TetRecipe
- expansion_nt
- BrepFaceIndex
- run_packing_microbench.py
- hierarchical.cpp
- run_tier3.py
- testlab_data.cpp
- Camera
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
- CampaignSpec
- ADR-0004 Mesh data structure
- plot_benchmarks.py
- lame-cylinder case
- Material
- surface_render.cpp
- interior_points
- PolicyObjective
- crossval.py
- scene.cpp
- Grid3d
- json
- ResultRow
- Tier 3 Performance Benchmarks
- vector
- T
- MESH
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
- vector
- Layer Dependency Direction Rule
- estimate_solve_resources
- index_t
- ROADMAP — Get PolyMesh off the ground
- pin_feature_nodes
- bench_harness library
- User-Paintable Region Override (GUI)
- cinema.cpp
- PolyMesh Showcase
- evaluate_curved_mesh_quality
- GeomError
- backend_cuda.cu
- colormap.hpp
- cell_validity.hpp
- local_refine_tets
- report.py
- CinemaCue
- cad_model.cpp
- regret.py
- Sign
- advisor-measure-first-program.md
- coord_index_t
- FilterReport
- FeaError
- run_calculix_cantilever.py
- run_batch.py
- M9 frozen baseline — `varyhedron-baseline-m9`
- .size
- Pareto analysis — `settings-frontier-1`
- Config
- make_compare_grid.py
- required
- png_writer.hpp
- plot_evaluation.py
- merge_face_component
- CadTopology
- external_truth.py
- PassTrace
- testlab/main.cpp
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
- report
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
- Geometry
- AdvisorNet
- Variable-everything meshing + learned mesh advisor
- test_p_conformity.cpp
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
- tet_fill.cpp
- ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)
- SurfaceFace
- vec3
- The adaptive solver core, explained
- Pareto analysis — `smoke`
- unit_hex_coords
- geometry_features.py
- BRepGeometryFidelity
- Decision
- Grok improvement loop
- GuiSettings
- CampaignSummary
- Advisor::Impl
- ClipBox
- MeshEdgeSegment
- HpSystem
- ADR-0024: Advisor measure-first answers (normative Q&A)
- viewport.cpp
- CvtSite
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
- lowpass_signal
- promote_truth.py
- PeriodicVertexArray3d
- SolveCostMeasured
- solve_hp
- Geogram / restricted CVT — vendoring study path
- graded_tet_fill_surface
- dorfler_mark
- render_cinema.py
- M-A1 — first trained advisor (2026-08-10)
- mathlib_probe.cpp
- ProbeAnswers
- CDT2d_ConstraintWalker
- make_hp_signals
- MixedFillOutput
- ADR-0020: True BRep volume meshing (product path)
- index_t
- ConstrainedLloydParams
- .empty
- backend.cpp
- RuntimeError
- ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)
- CinemaSizingStory
- RefinementPlan
- draw_cinema_cells
- wall_tangential_project
- Grok improvement handoff — `varyhedron-short-1`
- Varyhedron packing — algorithm survey (V5)
- CinemaHistogram
- VaryhedronFillOutput
- Grok improvement handoff — `varyhedron-smoke`
- write_grok_handoff.py
- string
- CvtLloydStats
- Prior art: ML for mesh generation and adaptive refinement
- ElementCentroidStress
- CinemaHud
- brep_fidelity.cpp
- resolve_campaign
- check_no_product_stl.sh
- intervalBase
- Checkpoint
- calibration.py
- rebuild_results.py
- Grok improvement handoff — `varyhedron-baseline-m9`
- face_mean_displacement_component
- progress.md
- Path
- PROGRESS
- invoke_grok_improve.sh
- LiveProgress
- Campaign metrics — normative definitions for agents
- ResolvedMeshSize
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
- CinemaCaption
- boundary_faces.cpp
- ReferenceCase
- ScorecardInfo
- expansion
- Matrix
- expansion
- GradedTetFillOutput
- build.sh
- post-m10-smoke/README.md
- Any
- render_stress
- IndexType
- cli/main.cpp
- HealthInfo
- CinemaType
- SiteGrid
- varyhedron_fill_surface
- NetworkEdges
- AdvisorDecision
- HexFillOutput
- properties
- advisor.cpp
- ExteriorConformStats
- properties
- figstyle.py
- CommandLineDesc
- test_quadrature.cpp
- 0010 — The v6 corpus: the geometry objective stopped discriminating
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
- ADR-0035: Boundary nodes belong on the BRep, not near it
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
- CellQualityStats
- advisor_fields
- CLI and options reference
- Decision
- 0004 — Model card: learned mesh advisor
- 0005 — Data card: advisor training corpus
- test_d6_bench_smoke.cpp
- mean_lateral_radial_residual
- verify_fields.py
- VtuPointData
- detect_hole_roi
- Decision
- Decision
- Decision
- SampleDistribution
- self_improve.sh
- BcSelection
- test_advisor_inference.cpp
- case
- ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening
- Act 5 — `solve`: the answer, in the order it is computed
- cad_geometry_features.cpp
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
- FaceLoops
- Data
- Logger
- default_crossval
- 0008 — The v4 corpus, the retrain, and the metric that punished being right
- ADR-0033: A gate must measure what ships
- Public CAD corpora for training a mesh advisor
- ADR-0032: The mesh may not depend on which standard library built it
- Advisor
- ADR-0031: A jut has a side
- mp4
- GeometryCompleteness
- lines
- lowpass_grid_energy
- run_artifacts.hpp
- enum
- 0006 — The clean-data retrain, and what it cost the advisor's claims
- as_bytes
- Geogram subset — what PolyMesh takes
- test_solve_cost.cpp
- LogLimits
- ADR-0040: A boundary condition names the exact closure of a CAD face
- refusal
- recover_nodal_stress
- HoleROI
- schema_version
- land_contract_cutover.sh
- stage_argv
- DomainClipParams
- MeshPreview
- ActivationFrame
- Case
- spectral_fields
- host
- label
- SurfaceTessellation
- ADR-0044: GLM cannot be the math library, and is not a useful second one
- archive/README.md
- check_cross_stdlib_mesh.sh
- enroll_lan_keys.sh
- cinema/manifest.json
- ADR-0036: A symmetric part gets a symmetric tiling
- ADR-0043: A film someone can read
- analyze_solve_cost
- SnapStats
- PElevateResult
- capture
- HostCalibration
- CinemaCellKey
- SolveCostEstimate
- BRepInspection
- ProximityFeatures
- gif
- Gmsh: swapping the mesh source
- Case
- ADR-0038: A fixture is applied to the boundary, not to a volume of nodes
- ADR-0042: The advisor explains itself on screen
- RawFace
- element_jacobians_positive
- SpectrumResult
- Portable-cost advisor retrain
- poster
- ADR-0037: A box selection is a region, and a smooth field is sampled on element sizes
- cost_labels.py
- Figure text: the words a reader actually sees
- GuiReport
- test_mixed_fill.cpp
- AdvisorScale
- command
- VertexArray
- HandoffInfo
- 0.1.0 — 2026-08-26
- SurfaceRender
- homogeneous_boundary
- test_kirsch_plate.cpp
- 0011 — v7 retrain: authoritative curved CAD geometry
- gradient_canvas
- Dirichlet
- CinemaChapter
- bake_result
- render_architecture
- draw_colorbar
- HexFace
- Vector3d
- Row
- run_probe.sh
- take
- ffmpeg_encoders
- clip_rule_text
- param_key
- read_all
- Third-party notices
- version
- nodal_sum
- make_cylinder

## God Nodes (most connected - your core abstractions)
1. `NodalMesh` - 180 edges
2. `PeriodicDelaunay3dThread` - 125 edges
3. `Viewport` - 111 edges
4. `Delaunay3dThread` - 109 edges
5. `CinemaState` - 103 edges
6. `App` - 88 edges
7. `TriSurface` - 87 edges
8. `Logger` - 69 edges
9. `Model` - 65 edges
10. `CaseFeatures` - 64 edges

## Surprising Connections (you probably didn't know these)
- `set_window_icon()` --calls--> `coverage`  [INFERRED]
  apps/gui/main.cpp → src/pipeline/include/pipeline/surface_render.hpp
- `extract_case_features()` --calls--> `selected`  [INFERRED]
  src/pipeline/src/scene.cpp → apps/gui/testlab_panel.hpp
- `assemble()` --calls--> `traction`  [INFERRED]
  src/fea/src/traction.cpp → apps/testlab/main.cpp
- `select_exact_cad_load_faces()` --calls--> `target`  [INFERRED]
  apps/testlab/main.cpp → src/mesh/include/mesh/surface_project.hpp
- `run_one()` --calls--> `request_cancel`  [INFERRED]
  apps/testlab/main.cpp → src/pipeline/include/pipeline/scene.hpp

## Import Cycles
- None detected.

## Communities (593 total, 179 thin omitted)

### Community 0 - "vem.cpp"
Cohesion: 0.14
Nodes (52): Fun, kP2Mono, kP2Vec, b_from_grads(), array, Dynamic, function, Matrix (+44 more)

### Community 1 - "analyze_campaign.py"
Cohesion: 0.12
Nodes (37): accuracy_of(), aggregate_configs(), analyze_one(), CfgAgg, config_label(), factor_breakdown(), _fmt_ms(), _fmt_pct() (+29 more)

### Community 2 - "CurvedMeshMetrics"
Cohesion: 0.06
Nodes (37): CurvedMeshMetrics, composite_score, has_circular, has_tet_aspect, has_volume, m1_max, m1_mean, m2_max (+29 more)

### Community 3 - "Viewport"
Cohesion: 0.03
Nodes (77): array, CinemaView, DisplayMode, FieldSweep, size_t, uint32_t, vector, Vector3d (+69 more)

### Community 4 - "fea library"
Cohesion: 0.13
Nodes (20): resolve_mesh_size, adapt library, adapt error estimation (error.cpp), adapt loop (loop.cpp), sizing_field, stiffness assembly, CUDA backend (optional), fea library (+12 more)

### Community 5 - "ManufacturedSolution"
Cohesion: 0.15
Nodes (22): Matrix, uint64_t, Vector3d, VectorXd, energy_norm_error(), array, map, ManufacturedSolution (+14 more)

### Community 6 - "Palette"
Cohesion: 0.05
Nodes (73): apply_theme(), ThemeId, ImVec4, make_interwebz_palette(), make_slate_palette(), make_studio_palette(), Palette, accent (+65 more)

### Community 7 - "build_advisor_dataset.py"
Cohesion: 0.06
Nodes (57): family_of(), main(), probe_of(), load_json(), main(), Path, The named metric of a reference, from the working tree or a git revision., reference_metric() (+49 more)

### Community 8 - "render_showcase.py"
Cohesion: 0.14
Nodes (29): matched_panels(), PanelSpec, Everything about a panel that must match its siblings. ``label`` is free; every…, Raise unless every panel shares camera, zoom window and colour limits. A side-…, build_mesh_tiles(), cli_path(), fmt_h(), git_rev() (+21 more)

### Community 9 - "CinemaState"
Cohesion: 0.03
Nodes (60): cinema_act_name(), cinema_act_window(), cinema_solver_token(), CinemaState, active, advisor_dir, advisor_note, advisor_ran (+52 more)

### Community 10 - "PolyMesh"
Cohesion: 0.05
Nodes (73): CellKind, poly_mesh_to_vem(), Cell, faces, kind, Face, neighbour, owner (+65 more)

### Community 11 - "App"
Cohesion: 0.03
Nodes (121): CinemaRender, deform_scale, mode, result_max, sweep, DisplayMode, FieldSweep, App (+113 more)

### Community 12 - "TetFillOutput"
Cohesion: 0.07
Nodes (29): FeaturePinReport, chains, edge_pinned, max_edge_residual, rejected, vertex_pinned, worst_node, worst_node_distance (+21 more)

### Community 13 - "MshModel"
Cohesion: 0.13
Nodes (26): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+18 more)

### Community 14 - "index_t"
Cohesion: 0.09
Nodes (15): PackedArrays::clear(), index_t, std::vector<bool>, std::vector<T, Memory::aligned_allocator<T> >, expansion::delete_expansion_on_heap(), expansion::new_expansion_on_heap(), expansion::show_all_stats(), GEOGRAM_API SOS_sort() (+7 more)

### Community 15 - "NodalMesh"
Cohesion: 0.05
Nodes (57): count_orphan_nodes(), surface_face_area(), Vector3d, NodalMesh, check_validity, compact_unused_nodes, elements, nodes (+49 more)

### Community 16 - "TestLabState"
Cohesion: 0.07
Nodes (49): CheckpointState, ImVec4, path, size_t, string, draw_results_panel(), draw_testlab_panel(), fmt_opt_num() (+41 more)

### Community 17 - "Advisor measure-first program (canonical agent plan)"
Cohesion: 0.08
Nodes (24): 0. One-sentence strategy, 10. Related files, 1. Substrate (keep forever until proven wrong), 2. Claims (product honesty), 3.1 Five-number scorecard + residual gate, 3.2 What to score vs dashboard (stress), 3.3 Chordal efficiency (edge residual), 3.4 Over-budget diagnosis (+16 more)

### Community 18 - "AdaptSuggestion"
Cohesion: 0.18
Nodes (15): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+7 more)

### Community 19 - "CantileverSetup"
Cohesion: 0.22
Nodes (10): CantileverSetup, bc, length, mesh, nfree, Dirichlet, Index, VectorXd (+2 more)

### Community 20 - "solve_elastostatics"
Cohesion: 0.07
Nodes (43): CgStop, Precond, function, SolveOptions, cg_accept_tol, cg_max_iters, cg_progress_chunk, cg_threshold (+35 more)

### Community 21 - ".resize"
Cohesion: 0.04
Nodes (53): IT, BalancedKdTree::build_tree(), Cavity(), CDT2d::clear(), CDT2d::insert(), CDTBase2d::clear(), CDTBase2d::create_enclosing_quad(), CDTBase2d::create_enclosing_triangle() (+45 more)

### Community 22 - "MirrorFrame"
Cohesion: 0.07
Nodes (50): CollectOffendersFn, RelaxNeighborhoodFn, RepairInteriorFn, array, Vector3d, mirror_fold(), mirror_unfold(), MirrorFrame (+42 more)

### Community 23 - "ProgressHeartbeat"
Cohesion: 0.09
Nodes (19): atomic, mutex, size_t, time_point, ProgressHeartbeat, cfg_id_, cg_iter_, cg_resid_ (+11 more)

### Community 24 - "SolveJob"
Cohesion: 0.03
Nodes (69): ElementTendencyPlan, label, mesher, native_poly_transitions, remapped, skin_layers, tendency, function (+61 more)

### Community 25 - "d6_tier3.cpp"
Cohesion: 0.08
Nodes (44): add_node(), array, int64_t, json, map, string, uint32_t, vector (+36 more)

### Community 26 - "Delaunay_psm.cpp"
Cohesion: 0.01
Nodes (246): E, AssertMode, EMSCRIPTEN_KEEPALIVE, InvalidInput, Node, siginfo_t, abnormal_program_termination(), absolute_path() (+238 more)

### Community 27 - "Predicates_psm.cpp"
Cohesion: 0.04
Nodes (108): coord_index_t, int64, Sign, SOSMode, det_3d(), det_3d_exact(), det_3d_filter(), det_4d() (+100 more)

### Community 28 - "TransitionFillOutput"
Cohesion: 0.12
Nodes (19): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+11 more)

### Community 29 - "polymesh CMake Project"
Cohesion: 0.20
Nodes (14): polymesh-d6-tier3 Bench Binary, polymesh CLI Executable, polymesh-gui Executable, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, POLYMESH_WITH_GUI, src/adapt Library (+6 more)

### Community 30 - "gen_part_library.py"
Cohesion: 0.24
Nodes (14): _assert_manifold_facets(), _cross(), _facet(), main(), _norm(), Path, Centered plate with through-hole along z. Origin at plate mid-plane centre. x ∈…, Parse emitted ASCII facet blocks and require edge multiplicity 2. (+6 more)

### Community 31 - "CartesianGrid"
Cohesion: 0.15
Nodes (30): CartesianGrid, cell, nx, ny, nz, origin, size_t, span (+22 more)

### Community 32 - "CadModel"
Cohesion: 0.11
Nodes (20): CadModel, compute_bbox, has_brep, impl_, load_brep, load_step, name_, tessellate (+12 more)

### Community 33 - "dataset.py"
Cohesion: 0.05
Nodes (76): build(), fit(), main(), Module, action_group_slices(), add_split_args(), _best_actions(), build_action_dims() (+68 more)

### Community 34 - "Model"
Cohesion: 0.06
Nodes (60): update_overlays, MeshStageSink, optional, Model, bbox_max, bbox_min, cad, mirror (+52 more)

### Community 35 - "scene.hpp"
Cohesion: 0.10
Nodes (20): mutex, BoundaryConditions, dirichlet, loads, atomic, Dirichlet, VectorXd, box_model() (+12 more)

### Community 36 - "metric_field.cpp"
Cohesion: 0.05
Nodes (72): Matrix3d, size_t, vector, Vector3d, Vector3i, Metric3d, axes, clamped (+64 more)

### Community 37 - "CsrMatrix"
Cohesion: 0.19
Nodes (14): CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t, vector (+6 more)

### Community 38 - "GeometrySizing"
Cohesion: 0.08
Nodes (23): FeatureSizing, blend_, h_max_, h_min_, size_at, GeometrySizing, blend_, curv_frac_ (+15 more)

### Community 39 - "Hand-calculated reference truths"
Cohesion: 0.05
Nodes (35): 1. Deformation, 2. Stress, 3. The 0.7% the cantilever misses, and where it comes from, 4. Convergence of the stress recovery, 5. Two measurements that were wrong before they were right, 6. What this does not verify, Field verification — stress *and* deformation, pointwise, cantilever (+27 more)

### Community 40 - "accuracy"
Cohesion: 0.12
Nodes (16): additionalProperties, properties, required, type, description, type, accuracy, name (+8 more)

### Community 41 - "spectral_sizing.cpp"
Cohesion: 0.16
Nodes (16): array, complex, function, size_t, vector, Vector3d, enforce_element_budget(), floor_pow2() (+8 more)

### Community 42 - "Stats"
Cohesion: 0.11
Nodes (19): Stats, phase_0_t_, phase_I_classify_t_, phase_I_insert_nb_, phase_I_insert_t_, phase_I_nb_cross_, phase_I_nb_inside_, phase_I_nb_outside_ (+11 more)

### Community 43 - "null"
Cohesion: 0.09
Nodes (26): description, minimum, type, description, minimum, type, description, type (+18 more)

### Community 44 - "NodalElement"
Cohesion: 0.11
Nodes (31): CornerEdges, element_type_name(), ElementType, uint32_t, vector, NodalElement, faces, nodes (+23 more)

### Community 45 - "CalculiX / PolyMesh cantilever cross-validation"
Cohesion: 0.33
Nodes (5): CalculiX / PolyMesh cantilever cross-validation, Cases to port next, Common install paths (documentation only), Run (CI-safe), Runner contract

### Community 47 - "eval_shape"
Cohesion: 0.13
Nodes (28): A, Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType (+20 more)

### Community 48 - "Plan: Mesher / Solver Accuracy + Performance Overhaul"
Cohesion: 0.07
Nodes (30): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+22 more)

### Community 49 - "gen_cad_parts.py"
Cohesion: 0.08
Nodes (43): _bbox(), check_step(), _classify(), _count(), _cylinder(), _display(), _face_area(), _faces() (+35 more)

### Community 50 - "gen_primitive_corpus.py"
Cohesion: 0.07
Nodes (55): axis_aligned_planar_faces(), _beam_region_response(), _boxes_intersect(), case_json(), case_specs(), check(), check_coverage(), _coverage_module() (+47 more)

### Community 53 - "Pareto analysis — `varyhedron-baseline-m9`"
Cohesion: 0.12
Nodes (16): Config ranking (weighted mean score), `curved`, `cylinder`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 56 - "ValidityError"
Cohesion: 0.07
Nodes (40): Vector3d, runtime_error, ValidityError, array, uint32_t, vector, Vector3d, PrismFillOutput (+32 more)

### Community 57 - "cvt_export.cpp"
Cohesion: 0.15
Nodes (38): ClipPlane, a, b, c, d, bisector_keep_site(), build_cell(), build_cell_tet_nbr() (+30 more)

### Community 58 - "SolveResourceEstimate"
Cohesion: 0.08
Nodes (26): MemoryAvailabilitySource, EffectiveMemoryBudget, available, effective_cap_bytes, safety_cap_bytes, user_cap_bytes, Index, uint64_t (+18 more)

### Community 59 - "Delaunay_psm.h"
Cohesion: 0.03
Nodes (48): align(), clear(), copy(), Counted(), GEOGRAM_API, float32, float64, is_specialized (+40 more)

### Community 60 - "CI Grep-Audit Anti-Cheat Job"
Cohesion: 0.67
Nodes (3): Anti-Cheat Boundary (No Hardcoded Refs in src/apps), CI Workflow (build-test + format + grep-audit), CI Grep-Audit Anti-Cheat Job

### Community 61 - "TetRecipe"
Cohesion: 0.08
Nodes (25): Entity, HpMode, edge_odd, entity, entity_index, index0, index1, index2 (+17 more)

### Community 62 - "expansion_nt"
Cohesion: 0.06
Nodes (49): interval_nt, mat2, mat3, aligned_3d(), aligned_3d_exact(), approximate(), ConvexCell::triangle_is_in_conflict(), vec2E (+41 more)

### Community 63 - "BrepFaceIndex"
Cohesion: 0.07
Nodes (32): BRepExtrema_DistShapeShape, Handle, BrepFaceIndex, adaptors, bins, boxes, cell, edge_ids (+24 more)

### Community 64 - "run_packing_microbench.py"
Cohesion: 0.23
Nodes (21): boundary_residual_placeholder(), bubble_relax(), _clamp01(), _dedupe(), _dist2(), fill_fraction_proxy(), main(), pack_case() (+13 more)

### Community 65 - "hierarchical.cpp"
Cohesion: 0.16
Nodes (27): Dynamic, Matrix, VectorXd, HpShape, dn, n, b_matrix(), build_hex() (+19 more)

### Community 66 - "run_tier3.py"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 67 - "testlab_data.cpp"
Cohesion: 0.19
Nodes (31): checkpoint_state_cstr(), count_result_lines(), Checkpoint, CheckpointState, json, optional, path, string (+23 more)

### Community 68 - "Camera"
Cohesion: 0.15
Nodes (19): Camera, distance_, dolly, eye, fov_y_, orbit, pan, pitch_ (+11 more)

### Community 69 - "train.py"
Cohesion: 0.07
Nodes (57): Optimizer, One side of the part-hash split, fully materialized as numpy arrays., Return a row-filtered copy (used to apply the pruning ledger)., Split, head_residuals(), keep_mask(), load_ledger(), load_pruned() (+49 more)

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
Nodes (144): condition_variable, local_index_t, Periodic, SFrame, CDTBase2d::insert(), CDTBase2d::insert_constraint(), CellStatusArray, capacity_ (+136 more)

### Community 77 - "hp_assembly.cpp"
Cohesion: 0.09
Nodes (38): QuadKey, assemble_hp(), array, EdgeKey, ElementType, pair, size_t, uint32_t (+30 more)

### Community 78 - "assemble_body_load"
Cohesion: 0.13
Nodes (27): Vector3d, QuadraturePoint, weight, xi, assemble_body_load(), BodyForce, VectorXd, Dynamic (+19 more)

### Community 79 - "ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "CampaignSpec"
Cohesion: 0.07
Nodes (28): AccuracyInfo, metric, rel_err, truth, value, CampaignResources, max_mem_gb, max_threads (+20 more)

### Community 82 - "plot_benchmarks.py"
Cohesion: 0.17
Nodes (24): d6_records(), gate1_records(), load_json(), main(), parse_mms_elements(), parse_mms_hierarchical(), parse_tier1_table(), plot_advisor_budget() (+16 more)

### Community 84 - "Material"
Cohesion: 0.05
Nodes (35): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+27 more)

### Community 85 - "surface_render.cpp"
Cohesion: 0.07
Nodes (51): line, uint8_t, vector, Image, height, rgb, width, NormalDeviation (+43 more)

### Community 86 - "interior_points"
Cohesion: 0.40
Nodes (4): ElementType, vector, Vector3d, interior_points()

### Community 87 - "PolicyObjective"
Cohesion: 0.10
Nodes (29): BranchAdvisorNet, main(), Any, device, Module, Tensor, sign_test_paired(), train_model() (+21 more)

### Community 88 - "crossval.py"
Cohesion: 0.06
Nodes (65): action_matrix(), advisor_scores(), aggregate(), aggregate_tolerance(), build_choosers(), decode_policy(), main(), parse_args() (+57 more)

### Community 89 - "scene.cpp"
Cohesion: 0.06
Nodes (55): join_worker, set_status, axis_distance(), Bore, axis, point, radius, cad_face_curvature_sources() (+47 more)

### Community 90 - "Grid3d"
Cohesion: 0.13
Nodes (19): Grid3d, at, dims, index, origin, sample, spacing, values (+11 more)

### Community 91 - "json"
Cohesion: 0.11
Nodes (22): json, face_nodes_hex20(), gate1_rows(), hex20_node_count(), main(), Structured hex20 node count for nx×ny×nz cells (8 corners + 12 edge mids)., Nodes on one structured face with n_perp==0 index, na×nb cells on face. Face…, Labeled gate1-p1 points for scoreboard (Lamé, Kirsch, cantilever). (+14 more)

### Community 92 - "ResultRow"
Cohesion: 0.07
Nodes (28): AnswersInfo, load_area_rel_err, load_face_area, sigma_face_mean, strain_energy, tip_deflection, QualityInfo, M1max (+20 more)

### Community 94 - "vector"
Cohesion: 0.04
Nodes (38): array, string, vector, pid(), array, vector, optional, combine_mesher_notes() (+30 more)

### Community 95 - "T"
Cohesion: 0.04
Nodes (41): B, DIM2, FT, initializer_list, T, T2, U, vector_type (+33 more)

### Community 96 - "MESH"
Cohesion: 0.04
Nodes (54): COORD, KeepInitialValues, MESH, MeshElementsFlags, MeshOrder, ProgressClient, Base_ccmp, Base_fcmp (+46 more)

### Community 97 - "total_volume"
Cohesion: 0.39
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 100 - "GradedSizing"
Cohesion: 0.06
Nodes (46): Demand, int32_t, GradedSizing, as_size_field, build_grid, cell_start_, grid_cell_, grid_nx_ (+38 more)

### Community 109 - "vector"
Cohesion: 0.07
Nodes (52): adaptive_setup(), BcSpec, box, cad_face_ids, fix, Box3, hi, lo (+44 more)

### Community 119 - "estimate_solve_resources"
Cohesion: 0.33
Nodes (16): Index, optional, string, string_view, uint64_t, csr_bytes(), dense_square_cap(), effective_memory_budget() (+8 more)

### Community 120 - "index_t"
Cohesion: 0.16
Nodes (11): acquire_spinlock(), BasicSpinLockArray, spinlocks_, CompactSpinLockArray, spinlocks_, geo_pause(), atomic, index_t (+3 more)

### Community 121 - "ROADMAP — Get PolyMesh off the ground"
Cohesion: 0.15
Nodes (13): Agent loop protocol (how to finish this), Current status snapshot, Parallel tracks, Recommended order (critical path to “usable product”), ROADMAP — Get PolyMesh off the ground, Track A — GUI (P6.5 pulled forward), Track B — Mesh quality (P2 remaining), Track C — Hybrid / features (P3 + P4) (+5 more)

### Community 122 - "pin_feature_nodes"
Cohesion: 0.09
Nodes (32): CellHash, uint32_t, vector, MirrorNodeOrbit, axes_, buckets_, canonical, cell_of (+24 more)

### Community 125 - "cinema.cpp"
Cohesion: 0.08
Nodes (45): advisor_display_scale(), beat_seconds(), build_cinema_skeleton(), capture_curve_spectrum(), capture_curve_story(), cinema_cue(), cinema_decision_lead(), cinema_opening_fade() (+37 more)

### Community 126 - "PolyMesh Showcase"
Cohesion: 0.09
Nodes (23): Architecture diagram, `bench_advisor_budget.png`, `bench_dof_time.png`, `bench_mms.png`, `bench_tier1.png`, Benchmark charts, Boundary conformity, `gallery_cantilever.png` (+15 more)

### Community 127 - "evaluate_curved_mesh_quality"
Cohesion: 0.19
Nodes (16): CircularFeature, axis_dir, axis_point, radius, select_band, Vector3d, clamp01(), array (+8 more)

### Community 128 - "GeomError"
Cohesion: 0.19
Nodes (17): bbox_diagonal, GeomError, runtime_error, CadModel::tessellate(), byte, path, size_t, Soup (+9 more)

### Community 129 - "backend_cuda.cu"
Cohesion: 0.27
Nodes (9): __global__, csr_spmv_kernel(), size_t, string, T, cuda_free(), device_available(), device_name() (+1 more)

### Community 130 - "colormap.hpp"
Cohesion: 0.67
Nodes (3): fea_colormap(), array, signed_colormap()

### Community 131 - "cell_validity.hpp"
Cohesion: 0.28
Nodes (20): hex8_jacobian_det(), hex8_min_jacobian(), hex8_shape_quality(), array, Vector3d, max_edge(), prism_min_corner_jacobian(), prism_shape_quality() (+12 more)

### Community 132 - "local_refine_tets"
Cohesion: 0.21
Nodes (24): FreeFaceKey, bisect_tet(), array, EdgeKey, size_t, span, uint32_t, vector (+16 more)

### Community 133 - "report.py"
Cohesion: 0.08
Nodes (59): accuracy_vs_cost(), _arrow(), _best_so_far(), _box(), _caption(), checkpoint_shape(), contract_heads(), corpus_rows() (+51 more)

### Community 134 - "CinemaCue"
Cohesion: 0.05
Nodes (39): cinema_render(), cinema_view(), CinemaCue, act, act_span, act_t, action_bridge_alpha, activation_wave (+31 more)

### Community 135 - "cad_model.cpp"
Cohesion: 0.16
Nodes (26): BRepAdaptor_Surface, gp_Pnt, empty, shape_handle, CadModel::Impl, shape, CadSupportKind, gp_Vec (+18 more)

### Community 136 - "regret.py"
Cohesion: 0.07
Nodes (52): budget_levels(), build_cases(), Case, cost_at_tolerance(), decades_to_factor(), dof_to_target(), feasible_mask(), finest_action_chooser() (+44 more)

### Community 137 - "Sign"
Cohesion: 0.03
Nodes (103): DList, CDT2d::incircle(), CDT2d::orient2d(), CDTBase2d::constrain_edges(), CDTBase2d::Delaunayize_new_edges(), CDTBase2d::Delaunayize_vertex_neighbors(), CDTBase2d::find_intersected_edges(), CDTBase2d::insert_vertex_in_edge() (+95 more)

### Community 138 - "advisor-measure-first-program.md"
Cohesion: 0.11
Nodes (17): Agent bootstrap — overnight / autonomous work on the DAG, Program DAG — how to pick up work, ADR-0021: Varyhedron — variable polyhedral packing mesher, Alternatives rejected, Consequences, Context, Decision, Research anchors (+9 more)

### Community 139 - "coord_index_t"
Cohesion: 0.06
Nodes (34): NearestNeighbors, AdaptiveKdTree::AdaptiveKdTree(), AdaptiveKdTree::build_tree(), AdaptiveKdTree::get_node(), AdaptiveKdTree::split_kd_node(), BalancedKdTree::BalancedKdTree(), BalancedKdTree::best_splitting_coord(), BalancedKdTree::get_node() (+26 more)

### Community 140 - "FilterReport"
Cohesion: 0.14
Nodes (13): BudgetResult, budget_met, filter, h_scale, predicted_after, predicted_before, FilterReport, energy_fraction (+5 more)

### Community 141 - "FeaError"
Cohesion: 0.14
Nodes (30): N, FeaError, runtime_error, uint32_t, vector, PolyCell, faces, nodes (+22 more)

### Community 142 - "run_calculix_cantilever.py"
Cohesion: 0.23
Nodes (18): input_id_lines(), load_json(), main(), node_id(), parse_ccx_tip_displacement(), parse_polymesh_tip_displacement(), CompletedProcess, Path (+10 more)

### Community 143 - "run_batch.py"
Cohesion: 0.09
Nodes (49): append_throughput(), build_rects(), campaign_json(), cfg_id_of(), completed_pairs(), count_lines(), expand_grid(), iter_rows() (+41 more)

### Community 144 - "M9 frozen baseline — `varyhedron-baseline-m9`"
Cohesion: 0.17
Nodes (12): Campaign matrix, Case primary accuracy metrics (as wired at freeze), Freeze identity, Known issues frozen-in (not blockers for freeze), M9 frozen baseline — `varyhedron-baseline-m9`, Metric schema version, Outcome summary, Per-run snapshot (+4 more)

### Community 145 - ".size"
Cohesion: 0.04
Nodes (40): DWORD, ExactPoint, LONG, LPVOID, PTP_CALLBACK_INSTANCE, PTP_CLEANUP_GROUP, PTP_POOL, PTP_WORK (+32 more)

### Community 146 - "Pareto analysis — `settings-frontier-1`"
Cohesion: 0.12
Nodes (16): `cantilever`, Config ranking (weighted mean score), `curved`, Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run (+8 more)

### Community 147 - "Config"
Cohesion: 0.11
Nodes (19): Config, adapt_leb_waves, adapt_passes, bc_grading, cost_only, curvature_turn_deg, element_tendency, eta_target (+11 more)

### Community 148 - "make_compare_grid.py"
Cohesion: 0.10
Nodes (30): Run, _aspect(), build_grid(), _Caption, _caption_parts(), choose_cols(), draw_line(), keyline() (+22 more)

### Community 149 - "required"
Cohesion: 0.12
Nodes (15): additionalProperties, description, $id, required, $schema, title, type, accuracy (+7 more)

### Community 150 - "png_writer.hpp"
Cohesion: 0.50
Nodes (8): adler32_of(), crc32_of(), size_t, uint32_t, vector, put_chunk(), put_u32be(), write_png_rgba()

### Community 151 - "plot_evaluation.py"
Cohesion: 0.09
Nodes (46): advisor_evaluation(), band_levels(), budget_phrase(), coincident(), collapse_families(), decades_to_factor(), draw_bands(), draw_failures() (+38 more)

### Community 152 - "merge_face_component"
Cohesion: 0.16
Nodes (12): canonical_face_key(), coalesce_rvd_interior_faces(), FaceId, VertexId, merge_face_component(), rvd_edge(), RvdEdgeKey, a (+4 more)

### Community 153 - "CadTopology"
Cohesion: 0.05
Nodes (72): CadEdgeFeature, CadSurfaceKind, CadEdge, dihedral_rad, feature, id, kappa_samples, length (+64 more)

### Community 154 - "external_truth.py"
Cohesion: 0.05
Nodes (83): all_case_ids(), analytic_case_ids(), audit_against_git(), base_provenance(), bbox_diagonal(), cad_feature_sizes(), Case, consistent_face_loads() (+75 more)

### Community 155 - "PassTrace"
Cohesion: 0.07
Nodes (30): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, PassTrace, cg_iters (+22 more)

### Community 156 - "testlab/main.cpp"
Cohesion: 0.06
Nodes (77): accumulate_solve_cost(), action_json(), AdvisorScorer, advisor_, case_features_json(), cfg_id_of(), cmd_pause_status(), cmd_validate() (+69 more)

### Community 157 - "CaseFeatures"
Cohesion: 0.03
Nodes (58): CaseFeatures, bbox_dx, bbox_dy, bbox_dz, case_load_multiaxiality, curved_frac, diag, fix_area_frac (+50 more)

### Community 158 - "Test-lab interfaces (normative)"
Cohesion: 0.10
Nodes (20): 1. Campaign spec — `bench/campaigns/<name>/campaign.json`, 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`, 3. Results — `bench/campaigns/<name>/results.jsonl`, 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`, 4. Part case — `tests/fixtures/parts/<part>.case.json`, 5. Reference truth — `bench/reference/<part>.json`, 6. Live solve progress — `<run_dir>/progress.json`, 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp` (+12 more)

### Community 159 - "ADR-0022: Full experiment warehouse + headless Grok improvement loop"
Cohesion: 0.12
Nodes (14): Campaign warehouse, Directory layout, git-LFS, Short-campaign defaults (Lane V), Wireframe PNGs (`wire.png`), ADR-0022: Full experiment warehouse + headless Grok improvement loop, Alternatives rejected, Consequences (+6 more)

### Community 160 - "FaceConformityStats"
Cohesion: 0.12
Nodes (27): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+19 more)

### Community 172 - "report"
Cohesion: 0.06
Nodes (35): report, acts, acts_note, added_cells, advisor_activations, advisor_unavailable, candidates, decision (+27 more)

### Community 267 - "run_gmsh_peer.py"
Cohesion: 0.10
Nodes (40): analytic_cases(), bbox_diagonal(), build_supports_uniform(), cad_bbox_diagonal(), classify_failure(), failed_result_row(), flatten_box(), load_arguments() (+32 more)

### Community 268 - "Geometry"
Cohesion: 0.11
Nodes (47): _box(), build_bossed_plate(), build_box_hole(), build_channel(), build_ellipsoid_boss(), build_geometry(), build_gusset_bracket(), build_l_bracket() (+39 more)

### Community 269 - "AdvisorNet"
Cohesion: 0.08
Nodes (29): Linear, AdvisorNet, main(), _matrix(), Any, no_grad, Tensor, Concatenate continuous columns with the two category embeddings. (+21 more)

### Community 270 - "Variable-everything meshing + learned mesh advisor"
Cohesion: 0.05
Nodes (35): 1.1 `adapt::MetricField` (new), 1.2 Sizing field end-to-end, 1.3 Conforming variable order, 1.4 Quantitative sizing from error, Corpus, Deployment, Label design (decided), Non-goals (+27 more)

### Community 271 - "test_p_conformity.cpp"
Cohesion: 0.18
Nodes (16): affine_boundary(), affine_displacement(), affine_max_error(), CantileverSolution, energy, u, Dirichlet, size_t (+8 more)

### Community 272 - "export_onnx.py"
Cohesion: 0.07
Nodes (52): Turn a raw, possibly incomplete feature dict into a model input vector. Mirrors…, standardize_row(), write_json(), activation_layout_path(), build_fixture(), check_fixture_guarantees(), default_checkpoint(), export_graph() (+44 more)

### Community 273 - "dashboard.py"
Cohesion: 0.17
Nodes (31): begin_end_annotations(), chart(), dash_y2(), ensure_plotly(), fmt(), guardrails_block(), js_json(), load_activations() (+23 more)

### Community 274 - "LinearConstraints"
Cohesion: 0.09
Nodes (27): map, pair, size_t, uint32_t, vector, LinearConstraint, masters, slave_dof (+19 more)

### Community 305 - "tet_fill.cpp"
Cohesion: 0.17
Nodes (27): BuriedFaceStats, n_buried, n_free_faces, size_t, buried_face_ids(), buried_free_tet_face_owners(), count_buried_free_tet_faces(), array (+19 more)

### Community 306 - "ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)"
Cohesion: 0.20
Nodes (9): 1. One stiffness matrix, two formulations, 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal, 3. Order caps by shape, 4. The (h, p, shape) driver, ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis), Alternatives rejected, Context, Decision (+1 more)

### Community 307 - "SurfaceFace"
Cohesion: 0.07
Nodes (66): Sink, ConsistentLoad, area, conservation_error, resultant, face_num_nodes(), FaceType, VectorXd (+58 more)

### Community 308 - "vec3"
Cohesion: 0.05
Nodes (58): COORD_T, angle(), barycenter(), CDT2d::create_enclosing_quad(), CDT2d::create_enclosing_triangle(), ConvexCell::barycenter(), ConvexCell::compute_triangle_point(), ConvexCell::squared_inner_radius() (+50 more)

### Community 309 - "The adaptive solver core, explained"
Cohesion: 0.18
Nodes (10): 1. Why three knobs instead of one, 2. The hierarchical basis: how p becomes cheap and conforming, 3. Shape: FE fast paths + VEM for everything else, 4. The driver: choosing (h, p, shape) together, 5. How to follow the code, Decision policy (v1, `adapt::drive_hp`), The adaptive solver core, explained, What is implemented (node `fe-vem-assembly`) (+2 more)

### Community 310 - "Pareto analysis — `smoke`"
Cohesion: 0.14
Nodes (13): Config ranking (weighted mean score), Default-knob recommendations, Factor-level winners (mean config score), Full factor breakdown, Global Pareto frontier (mean accuracy vs mean total time), How to re-run, Pareto analysis — `smoke`, Pareto by geometric class (+5 more)

### Community 311 - "unit_hex_coords"
Cohesion: 0.67
Nodes (4): Dynamic, Matrix, unit_hex_coords(), unit_tet_coords()

### Community 312 - "geometry_features.py"
Cohesion: 0.07
Nodes (53): _bootstrap_slope(), centroid_pairwise(), coverage_report(), descriptor_distances(), family_of(), family_recovery(), learning_curve_fit(), load_descriptors() (+45 more)

### Community 313 - "BRepGeometryFidelity"
Cohesion: 0.11
Nodes (18): BRepGeometryFidelity, available, brep, brep_surface_fallback_vertex_count, brep_surface_sample_face_count, brep_surface_samples_to_mesh_boundary, brep_surface_uv_attempt_count, brep_vertices_to_mesh_boundary_nodes (+10 more)

### Community 314 - "Decision"
Cohesion: 0.14
Nodes (14): 1. Substrate (keep, do not replace), 2. Element technology claims, 3. Packing evolution, 4. CAD edge classification (normative), 5. Measurement order (two-week horizon), 6. License landscape (core vs plugin), 7. Sizing field, 8. p-order (+6 more)

### Community 315 - "Grok improvement loop"
Cohesion: 0.25
Nodes (7): Autonomous vs supervised answers, Grok improvement loop, Handoff contents, Headless invoke (default), Manual interactive (optional), Safety, When it runs

### Community 316 - "GuiSettings"
Cohesion: 0.09
Nodes (28): path, GuiSettings, campaigns_root, campaigns_root_path, max_mem_gb, max_threads, refresh_interval_s, resolved_testlab_binary (+20 more)

### Community 317 - "CampaignSummary"
Cohesion: 0.11
Nodes (18): CampaignSummary, dir, has_campaign_json, has_checkpoint, has_results, name, result_count, state (+10 more)

### Community 318 - "Advisor::Impl"
Cohesion: 0.05
Nodes (38): Env, MemoryInfo, Session, SessionOptions, Advisor::Impl, action_dims, activation_note, activations_available (+30 more)

### Community 319 - "ClipBox"
Cohesion: 0.18
Nodes (23): density_from_size(), ClipBox, max, min, ring, bisector_keep_site(), build_rvd_cell(), build_rvd_cell_grid() (+15 more)

### Community 320 - "MeshEdgeSegment"
Cohesion: 0.67
Nodes (3): MeshEdgeSegment, a, b

### Community 321 - "HpSystem"
Cohesion: 0.08
Nodes (31): HpElementDef, order, type, vertices, HpModel, elements, nodes, ElementType (+23 more)

### Community 322 - "ADR-0024: Advisor measure-first answers (normative Q&A)"
Cohesion: 0.17
Nodes (12): ADR-0024: Advisor measure-first answers (normative Q&A), Compressed path (do not invent another), Q10 — High-dimensional traps, Q1 — 1e20 von Mises with 1e-13 residual, Q2 — Next 3–5 days order, Q3 — Geogram, Q4 — Chordal efficiency e ~ 100 at h_scale=5, Q5 — Cylinder truth (+4 more)

### Community 323 - "viewport.cpp"
Cohesion: 0.11
Nodes (31): bind_cinema_attr(), bind_cinema_line_attr(), bind_line_attr(), bind_sizing_attr(), cinema_cell_key(), count, CinemaCellKeyHash, compile() (+23 more)

### Community 324 - "CvtSite"
Cohesion: 0.09
Nodes (31): CvtSite, fixed, pos, ConstrainedLloydResult, lloyd_stats, project_stats, seed_stats, sites (+23 more)

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
Nodes (19): Campaign, grid, host, max_dof, max_elems, max_pack_wall_s, max_run_wall_s, name (+11 more)

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

### Community 335 - "lowpass_signal"
Cohesion: 0.38
Nodes (13): clamp_fraction(), complex, size_t, span, vector, fft_inplace(), is_pow2(), lerp_signal() (+5 more)

### Community 336 - "promote_truth.py"
Cohesion: 0.14
Nodes (29): best_rows(), check_promotable(), main(), measured_by_metric(), parse_args(), promote(), provenance(), Any (+21 more)

### Community 337 - "PeriodicVertexArray3d"
Cohesion: 0.14
Nodes (11): AdaptiveKdTree::plane_split(), Hilbert_vcmp_periodic<COORD, false, PeriodicVertexMesh3d>, Hilbert_vcmp_periodic<COORD, true, PeriodicVertexMesh3d>, PeriodicVertexArray3d, base_, nb_real_vertices_, nb_vertices_, stride_ (+3 more)

### Community 338 - "SolveCostMeasured"
Cohesion: 0.11
Nodes (20): SolveMethod, string, uint64_t, VectorXd, LinearSolveResult, cost, reactions, reactions_complete (+12 more)

### Community 339 - "solve_hp"
Cohesion: 0.40
Nodes (5): loads, Index, map, solve_hp(), loads

### Community 340 - "Geogram / restricted CVT — vendoring study path"
Cohesion: 0.15
Nodes (13): 1. Why Geogram BSD-3 (not clean-room clipped Voronoi), 2.1 Vendor from Geogram (BSD-3), 2.2 We write ourselves, 2.3 Dual hard-block, 2. What to vendor vs what we write, 3. Dependency order (do not invent another), 4. Packing context (how this sits in varyhedron), 5. Vendored `third_party/` layout (+5 more)

### Community 341 - "graded_tet_fill_surface"
Cohesion: 0.07
Nodes (39): CanonicalCellMap, axis, canonical, flipped, FeatureAwareClassification, child_inside_mask, classified_volume, coarse_inside (+31 more)

### Community 342 - "dorfler_mark"
Cohesion: 0.43
Nodes (7): size_t, vector, Vector3d, dorfler_coarsen_mark(), dorfler_mark(), FeatureGradedSizing::size_at(), mark_smooth()

### Community 343 - "render_cinema.py"
Cohesion: 0.16
Nodes (31): _as_int(), auto_spec(), Case, encode_gif(), encode_mp4(), ffmpeg(), ffmpeg_version(), frame_path() (+23 more)

### Community 344 - "M-A1 — first trained advisor (2026-08-10)"
Cohesion: 0.07
Nodes (29): 0003 — Training log, Batch 1, Capacity was not the problem, Corpus and ground truth, Corpus widened for power, not coverage, Data, Data and artifact, Does it choose a better mesh than the default? (+21 more)

### Community 345 - "mathlib_probe.cpp"
Cohesion: 0.13
Nodes (30): b_matrix(), best_of(), build_data(), Matrix, Matrix3d, size_t, Vector3d, k1_eigen() (+22 more)

### Community 346 - "ProbeAnswers"
Cohesion: 0.07
Nodes (28): LoadAreaStatus, ProbeAnswers, authored_area_checked, authored_area_consistent, authored_area_rel_diff, dominant_load_axis, free_residual_rel, load_area_ok (+20 more)

### Community 347 - "CDT2d_ConstraintWalker"
Cohesion: 0.18
Nodes (10): CDT2d_ConstraintWalker, i, j, t, t_prev, v, v_cnstr, v_prev (+2 more)

### Community 348 - "make_hp_signals"
Cohesion: 0.48
Nodes (7): at_or_broadcast(), at_or_broadcast_int(), size_t, span, vector, estimate_surplus_from_zz(), make_hp_signals()

### Community 349 - "MixedFillOutput"
Cohesion: 0.05
Nodes (73): EdgeSplitFn, FineNbrFn, FineNodeFn, InbFn, MixedCellKind, array, size_t, uint32_t (+65 more)

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
Cohesion: 0.08
Nodes (30): ConvexCellFlags, Frame, begin_task(), cancel(), ConvexCell::compute_geometry(), ConvexCell::compute_mg(), ConvexCell::ConvexCell(), ConvexCell::facet_area() (+22 more)

### Community 354 - "backend.cpp"
Cohesion: 0.33
Nodes (9): active_backend(), backend_description(), string, init_runtime_performance(), openmp_default_threads(), openmp_enabled(), openmp_max_threads(), performance_description() (+1 more)

### Community 355 - "RuntimeError"
Cohesion: 0.14
Nodes (35): RuntimeError, _bbox(), _cell_type_summary(), _dependency_version(), _deterministic_subsample(), _distance_statistics(), _exact_point_distances(), _file_snapshot() (+27 more)

### Community 356 - "ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)"
Cohesion: 0.20
Nodes (10): 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3), 2. Dual hard-block (ADR-0024 Q8), 3. `third_party/` plan, 4. Order (do not invent another), ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block), Alternatives rejected, Consequences, Context (+2 more)

### Community 357 - "CinemaSizingStory"
Cohesion: 0.07
Nodes (30): CinemaSizingStory, bc_seeds, brep_curvature, curvature_filtered, curvature_raw, curve_energy_fraction, curve_mode_kept, curve_modes_kept (+22 more)

### Community 358 - "RefinementPlan"
Cohesion: 0.08
Nodes (26): prepare_cinema_features(), draw_run_group(), SizeFieldFn, vector, Vector3d, RefinementPlan, geometry_curvature_from_brep, h_fine (+18 more)

### Community 359 - "draw_cinema_cells"
Cohesion: 0.15
Nodes (27): array, ImDrawList, ImFont, ImU32, ImVec2, ImVec4, draw_cinema_cells(), draw_cinema_equations() (+19 more)

### Community 360 - "wall_tangential_project"
Cohesion: 0.11
Nodes (22): size_t, WallProjectStats, max_surface_residual, mean_surface_residual, n_iters, n_moved, n_reverted, n_wall_nodes (+14 more)

### Community 361 - "Grok improvement handoff — `varyhedron-short-1`"
Cohesion: 0.22
Nodes (8): Autonomous defaults, Campaign snapshot, Grok improvement handoff — `varyhedron-short-1`, Invoke (already done if you are reading this from invoke_grok_improve.sh), Sync first, Trends (mesh/solve/quality vs tier), Warehouse / visuals, Your mission this session

### Community 362 - "Varyhedron packing — algorithm survey (V5)"
Cohesion: 0.07
Nodes (28): 0. Normative ranking (ADR-0023 / plan — do not ignore), 1. Goals (from ADR-0021), 2. Bubble / sphere packing → Delaunay, 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage), 4. Field-aligned hex-dominant (PGP3D-class), 5. CAD edge protecting balls / PLC constraints, 6. Licensing notes (core vs plugin), 7. Decision: v1 algorithm (+20 more)

### Community 363 - "CinemaHistogram"
Cohesion: 0.08
Nodes (26): CinemaHistogram, bins, max, mean, min, p99, quantiles, samples (+18 more)

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

### Community 370 - "ElementCentroidStress"
Cohesion: 0.25
Nodes (8): ElementCentroidStress, centroid, element_index, quality, stress, volume, uint32_t, Vector3d

### Community 371 - "CinemaHud"
Cohesion: 0.08
Nodes (26): CinemaHud, adapt_passes, added_elements, cinema_elements, cinema_skipped_elements, deform_scale, dof, elements (+18 more)

### Community 372 - "brep_fidelity.cpp"
Cohesion: 0.40
Nodes (15): append_triangle(), boundary_surface(), boundary_surface_volume(), brep_fidelity_summary(), FreeFace, size_t, uint32_t, vector (+7 more)

### Community 373 - "resolve_campaign"
Cohesion: 0.67
Nodes (3): main(), Path, resolve_campaign()

### Community 375 - "intervalBase"
Cohesion: 0.06
Nodes (16): Sign2, control_add(), control_check(), control_mul(), control_set(), control_sub(), vec2HE, incircle_2d_SOS() (+8 more)

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

### Community 380 - "face_mean_displacement_component"
Cohesion: 0.38
Nodes (7): face_mean_displacement_component(), face_mean_displacement_mag(), global_max_displacement_mag(), size_t, uint32_t, vector, VectorXd

### Community 381 - "progress.md"
Cohesion: 0.09
Nodes (11): Active program (do not skip), graphify, Polyhedral-FEA — agent notes, 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number, 1. The track asked for a deliverable, not a model, 2. The measurement, 3. A safety margin does not fix it, and the way it fails is the finding, 4. What ships, and what does not (+3 more)

### Community 382 - "Path"
Cohesion: 0.09
Nodes (29): assert_glyphs(), _charmap(), _covers(), digest(), _digest_file(), figure(), finish(), font_path() (+21 more)

### Community 383 - "PROGRESS"
Cohesion: 0.33
Nodes (6): Active (read this first), Background / older phases, Benchmark table, Done, Open issues, PROGRESS

### Community 388 - "LiveProgress"
Cohesion: 0.18
Nodes (11): LiveProgress, cfg_id, cg_iter, cg_resid, elapsed_ms, n_elems, n_nodes, part (+3 more)

### Community 389 - "Campaign metrics — normative definitions for agents"
Cohesion: 0.22
Nodes (9): 1. Score vs dashboard vs gate, 2. Minimum scorecard (five numbers + residual gate), 3. Case-specific accuracy scores, 4. Chordal efficiency \(e\), 5. Gates and kills (not scores), 6. Displacement probes, 7. Agent checklist before claiming a campaign “win”, Campaign metrics — normative definitions for agents (+1 more)

### Community 390 - "ResolvedMeshSize"
Cohesion: 0.07
Nodes (31): CurvedGeometryResult, constraints, mesh, n_h_refined, n_partial, n_projected, n_promoted, n_pyramids_split (+23 more)

### Community 391 - "Triangle"
Cohesion: 0.25
Nodes (10): ConvexCell::connect_triangles(), ushort, make_triangle(), make_triangle_with_flags(), Triangle, i, j, k (+2 more)

### Community 392 - "Predicates_psm.h"
Cohesion: 0.07
Nodes (41): aligned_3d(), aligned_3d_exact(), aligned_malloc(), det2x2(), det3x3(), det4x4(), geo_argused(), geo_clamp() (+33 more)

### Community 393 - "TriSurface"
Cohesion: 0.05
Nodes (51): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector (+43 more)

### Community 394 - "manifest.json"
Cohesion: 0.50
Nodes (3): generated_utc, git_rev, images

### Community 395 - "Protecting balls + local feature size (LFS)"
Cohesion: 0.33
Nodes (6): 1. Role, 2. CDS radius formula (must-change), 3. Reference, 4. Risk cases, 5. Agent checklist, Protecting balls + local feature size (LFS)

### Community 396 - "SolveResult"
Cohesion: 0.04
Nodes (58): set_cinema_motion_bounds, GeometryVolumeAssessment, available, cad_volume, mesh_volume, relative_error, GeometryVolumeLimitError, assessment (+50 more)

### Community 397 - "pointer_"
Cohesion: 0.11
Nodes (17): function_pointer, const_pointer, const_reference, FPTR, reference, function_pointer_to_generic_pointer(), generic_pointer_to_function_pointer(), pointer_ (+9 more)

### Community 398 - "operator=="
Cohesion: 0.08
Nodes (24): mat4, M, aligned_allocator, ALIGNMENT, aligned_free(), aligned_malloc(), A1, A2 (+16 more)

### Community 399 - "BRep face-tag BCs / probes (design stub)"
Cohesion: 0.33
Nodes (6): BRep face-tag BCs / probes (design stub), Exit criteria (future work item), Historical icecream instability (superseded fixture; why face tags), Out of scope for this stub, Target model (sketch), Why boxes are temporary

### Community 400 - "vec4"
Cohesion: 0.06
Nodes (40): Attribute, IncidentTetrahedra, ostream, ConvexCell::append_to_mesh(), ConvexCell::clip_by_plane(), ConvexCell::clip_by_plane_fast(), ConvexCell::for_each_Voronoi_vertex(), ConvexCell::grow_v() (+32 more)

### Community 401 - "ProjectResult"
Cohesion: 0.12
Nodes (17): BRepSurfaceSamples, face_count, face_ids, fallback_vertex_count, points, uv_attempt_count, CadSupportKind, size_t (+9 more)

### Community 402 - "Variable-everything idea bank"
Cohesion: 0.10
Nodes (20): 10. Additional axes worth varying, 1. A2 + A1 + A4: Mmg3d-backed solution-driven metric adaptation, 1. Size / density, 2. Anisotropy, 2. O1 + O2: productionize the existing hierarchical HpModel, 3. G1 + G2: independently variable curved CAD geometry order, 3. Polynomial order, 4. Element shape / topology (+12 more)

### Community 403 - "declare_arg"
Cohesion: 0.12
Nodes (35): ArgFlags, ArgType, GroupArgs, Arg, desc, flags, arg_group(), name (+27 more)

### Community 404 - "CinemaCaption"
Cohesion: 0.16
Nodes (25): build_caption(), cinema_caption(), CinemaCaption, footer, headline, headline_color, note, note_color (+17 more)

### Community 405 - "boundary_faces.cpp"
Cohesion: 0.08
Nodes (70): EdgeOwners, Loop, NodeNeighbors, AtomicEdge, direction, from, key, to (+62 more)

### Community 406 - "ReferenceCase"
Cohesion: 0.09
Nodes (26): RadialMap, BenchError, runtime_error, string, ReferenceCase, citation, name, values (+18 more)

### Community 407 - "ScorecardInfo"
Cohesion: 0.18
Nodes (11): ScorecardInfo, accuracy_rel_err, chordal_efficiency_max, edge_hausdorff_over_h, has_health_ok, health_ok, min_element_quality, n_dof (+3 more)

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
Cohesion: 0.10
Nodes (22): Line2D, annotate_n(), axes_off(), convergence(), Fit, fit_loglog(), Any, Force a common y-range across panels, or record why not. (+14 more)

### Community 415 - "render_stress"
Cohesion: 0.14
Nodes (24): _camera_state(), color_range(), compose(), fit_camera(), nice_factor(), over(), _plotter(), ndarray (+16 more)

### Community 416 - "IndexType"
Cohesion: 0.21
Nodes (8): IndexType, KeepOrderType, basic_bindex, indices, basic_quadindex, indices, basic_trindex, indices

### Community 417 - "cli/main.cpp"
Cohesion: 0.15
Nodes (39): benchmark_bytes_per_second(), benchmark_flops_per_second(), benchmark_reference_mesh_ms(), BoxSel, hi, lo, set, cad_pressure_area() (+31 more)

### Community 418 - "HealthInfo"
Cohesion: 0.25
Nodes (8): HealthInfo, free_residual_rel, has_load_area_ok, load_area_ok, n_orphans, ok, present, reaction_sum_err

### Community 419 - "CinemaType"
Cohesion: 0.10
Nodes (22): cinema_strip_height(), cinema_type(), CinemaType, caption, chapter, font, footer, headline (+14 more)

### Community 420 - "SiteGrid"
Cohesion: 0.07
Nodes (31): ClippedCell, barycenter, empty, n_planes, n_triangles, volume, size_t, uint32_t (+23 more)

### Community 421 - "varyhedron_fill_surface"
Cohesion: 0.30
Nodes (17): boundary_nodes(), bubble_relax_volume(), array, span, uint32_t, vector, Vector3d, far_enough() (+9 more)

### Community 422 - "NetworkEdges"
Cohesion: 0.11
Nodes (21): ActivationTaps, fc1, fc2, heads, input, size_t, string, vector (+13 more)

### Community 423 - "AdvisorDecision"
Cohesion: 0.08
Nodes (25): AdvisorDecision, adapt_passes, budget_refusal, clamped, eta_target, failure_prob, h_rel, mesher (+17 more)

### Community 424 - "HexFillOutput"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 425 - "properties"
Cohesion: 0.10
Nodes (20): type, type, enum, cad_face, detected_from, kind, recommended_h_m, refused_at_h_m (+12 more)

### Community 426 - "advisor.cpp"
Cohesion: 0.20
Nodes (20): decide, Advisor::apply_action(), Advisor::decide(), Advisor::evaluate(), Advisor::explain(), apply_action, encode, forward (+12 more)

### Community 427 - "ExteriorConformStats"
Cohesion: 0.06
Nodes (34): BoundaryTargetFn, BoundaryFit, cad, projection, topo, BoundaryProjectionContext, provenance, target (+26 more)

### Community 428 - "properties"
Cohesion: 0.07
Nodes (27): description, type, description, type, type, description, type, type (+19 more)

### Community 429 - "figstyle.py"
Cohesion: 0.09
Nodes (32): FuncFormatter, clamp_to_floor(), colorbar(), field_cmap(), field_lut(), font_px(), _gui_lut(), metric_label() (+24 more)

### Community 430 - "CommandLineDesc"
Cohesion: 0.25
Nodes (8): Args, GroupNames, Groups, CommandLineDesc, args, argv0, group_names, groups

### Community 431 - "test_quadrature.cpp"
Cohesion: 0.27
Nodes (7): affine_image(), ElementType, Matrix3d, vector, Vector3d, integrate(), isoparametric_volume()

### Community 432 - "0010 — The v6 corpus: the geometry objective stopped discriminating"
Cohesion: 0.12
Nodes (15): 0009 — The v5 corpus: a better mesher, better predictions, and no decision win, 1. Why there is a v5, 2. The retrain, 3. Decision quality: the honest result is "no change", 4. The tolerance selector: still not deliverable, 5. Deployed behaviour, checked end to end, 6. Provenance, 0010 — The v6 corpus: the geometry objective stopped discriminating (+7 more)

### Community 433 - "plot_hole_bug.py"
Cohesion: 0.16
Nodes (17): cad_bore_volume(), classify(), draw_panel(), load_mesh(), main(), Mesh, mesh_volume(), parse_args() (+9 more)

### Community 434 - "Box"
Cohesion: 0.22
Nodes (8): bbox_union(), bboxes_overlap(), Box, Box2d, xy_max, xy_min, xyz_max, xyz_min

### Community 435 - "SharpEdge"
Cohesion: 0.08
Nodes (39): dist_, blend_to_max(), clamp_size(), DistanceFn, unique_ptr, vector, Vector3d, FeatureSizing::FeatureSizing() (+31 more)

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
Cohesion: 0.28
Nodes (14): epoch_series(), _finding(), load_json(), main(), parse_args(), _pct(), Any, Namespace (+6 more)

### Community 443 - "ADR-0035: Boundary nodes belong on the BRep, not near it"
Cohesion: 0.20
Nodes (10): 1. The report, 2. What was actually wrong, 3. The fix, 4. Measured, h = 8 mm, `polymesh diag`, 5.1 Facet kinks: the defect a user actually sees, 5.2 Measured, h = 8 mm, second wave, 5. Second wave: the exterior that ships, 6. What is still open, and why it is not hidden (+2 more)

### Community 444 - "test_brep_fidelity.cpp"
Cohesion: 0.13
Nodes (23): BoundaryQuad, boundary_nodes(), boundary_quadratic_mids(), Case, edge_p99_over_h, mesher, name, node_max_over_h (+15 more)

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
Cohesion: 0.13
Nodes (17): assess_load_area(), AuthoredAreaCheck, checked, consistent, rel_diff, check_authored_area(), LoadAreaStatus, optional (+9 more)

### Community 462 - "CellQualityStats"
Cohesion: 0.29
Nodes (6): CellQualityStats, mean, min, n_measured, n_unmeasured, size_t

### Community 463 - "advisor_fields"
Cohesion: 0.10
Nodes (21): adapt_passes, applied, candidates, eta_target, frames, gate_threshold, h_rel, ood_distance (+13 more)

### Community 464 - "CLI and options reference"
Cohesion: 0.09
Nodes (21): Build options, CLI and options reference, Commands, GUI, Host calibration, Loads and boundary conditions, Meshers, Product limits (+13 more)

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

### Community 470 - "verify_fields.py"
Cohesion: 0.27
Nodes (15): cantilever_moment(), cantilever_stations(), Check, check_axisymmetric(), check_cantilever(), check_cylinder(), check_plate(), check_sanity() (+7 more)

### Community 473 - "VtuPointData"
Cohesion: 0.15
Nodes (19): string, vector, VectorXd, VtuCellData, name, scalars, VtuPointData, name (+11 more)

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
Cohesion: 0.14
Nodes (16): DistanceDistribution, metres, over_bbox_diagonal, over_h, size_t, SampleDistribution, count, max (+8 more)

### Community 480 - "BcSelection"
Cohesion: 0.12
Nodes (19): BcSelection, face_fallback, faces, fallback_band, from_box, nodes, region, slab_nodes (+11 more)

### Community 481 - "test_advisor_inference.cpp"
Cohesion: 0.15
Nodes (10): columns_of(), FeatureColumns, json, path, load(), Scored, action, dof (+2 more)

### Community 482 - "case"
Cohesion: 0.11
Nodes (19): case, adapt_passes_configured, case_json, eta_target_configured, feature_grading_configured, fix_faces, h_mm_configured, h_note (+11 more)

### Community 483 - "ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening"
Cohesion: 0.18
Nodes (11): 1. Spectral sizing (`adapt::spectral`, new module), 2. Coarsening (`HpAction::kCoarsen` + loop executor), 3. Budget-feasible advisor chooser, 4. CG equilibration, 5. Advisor hygiene (measured, no retrain), ADR-0034: Spectral sizing, budget-feasible advisor, and coarsening, Consequences, Context (+3 more)

### Community 484 - "Act 5 — `solve`: the answer, in the order it is computed"
Cohesion: 0.11
Nodes (19): Act 1 — `skeleton`: exact CAD to measured curvature, Act 2 — `deliberate`: choosing a mesh, Act 3 — `build`: the mesher executing the decision, Act 4 — `mesh_hold`: the finished mesh, Act 5 — `solve`: the answer, in the order it is computed, Load ramp, Material, Mechanics overlay windows (+11 more)

### Community 485 - "cad_geometry_features.cpp"
Cohesion: 0.40
Nodes (4): compute_geometry_descriptors(), RadiusSample, area, radius

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
Cohesion: 0.28
Nodes (17): AdvisorObjective, AdvisorError, runtime_error, Advisor::Advisor(), load_activation_layout, load_clamps, load_normalization, load_ood (+9 more)

### Community 492 - "AdvisorRawOutputs"
Cohesion: 0.15
Nodes (13): AdvisorRawOutputs, dof_log10, failure_logit, geo_chamfer_log10, geo_p99_log10, mesh_ms_log10, mesh_work_log10, policy (+5 more)

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

### Community 497 - "FaceLoops"
Cohesion: 0.16
Nodes (15): fit, fit_oriented, initializer_list, uint32_t, vector, Vector3d, FaceLoops, nodes (+7 more)

### Community 498 - "Data"
Cohesion: 0.11
Nodes (18): array, vector, Vector3f, Data, e_dn, e_hex, e_pts, e_sym (+10 more)

### Community 499 - "Logger"
Cohesion: 0.07
Nodes (46): android_get_number_of_cores(), CDT2d::save(), int64, enable_cancel(), enable_multithreading(), GeogramLibSingleton, get_display_arg(), initialize() (+38 more)

### Community 500 - "default_crossval"
Cohesion: 0.33
Nodes (6): default_crossval(), main(), parse_args(), Namespace, Path, The canonical cross-validation record, or the newest stand-in. The producer…

### Community 501 - "0008 — The v4 corpus, the retrain, and the metric that punished being right"
Cohesion: 0.22
Nodes (9): 0008 — The v4 corpus, the retrain, and the metric that punished being right, 1. Why there is a v4 at all, 2. The retrain, 3. Decision quality: the advisor now leads on every reference it should, 4.1 The advisor was right and the label was wrong, 4.2 The macro mean still needs a fold-size guard, 4. The open question from 0006 §4, answered: one fold was eating the mean, 5. The tolerance selector: much closer, still not deliverable (+1 more)

### Community 502 - "ADR-0033: A gate must measure what ships"
Cohesion: 0.22
Nodes (9): 1. The pyramid gate measured a different cell than the diagnostics, 2. `hex_fill` gated on sampled signs, and nothing else, 3. `snap_boundary_nodes` computed a whole-mesh proof and threw it away, ADR-0033: A gate must measure what ships, Consequences, Context, Decision, Open: the ellipsoidal boss, and where the wall gets stuck (+1 more)

### Community 503 - "Public CAD corpora for training a mesh advisor"
Cohesion: 0.22
Nodes (8): Commercial-development track, Concrete ingestion and labeling rules for PolyMesh, Dataset evaluation, Non-commercial research benchmark add-on, Public CAD corpora for training a mesh advisor, Recommended starter corpus for one workstation, The datasets that actually pair geometry, BCs, and structural FEA, What this means for PolyMesh

### Community 504 - "ADR-0032: The mesh may not depend on which standard library built it"
Cohesion: 0.40
Nodes (5): ADR-0032: The mesh may not depend on which standard library built it, Amendment 2026-08-21: an unstable sort is the same defect, Consequences, Context, Decision

### Community 505 - "Advisor"
Cohesion: 0.17
Nodes (11): Advisor, apply_action, defaults, evaluate, explain, has_activations, impl_, recommend (+3 more)

### Community 506 - "ADR-0031: A jut has a side"
Cohesion: 0.25
Nodes (7): ADR-0031: A jut has a side, Consequences, Context, Decision, The defect, The test, What this does not fix

### Community 507 - "mp4"
Cohesion: 0.11
Nodes (18): bytes, codec, encoder, faststart, file, fps, pix_fmt, rate_control (+10 more)

### Community 508 - "GeometryCompleteness"
Cohesion: 0.25
Nodes (8): GeometryCompleteness, available, brep_volume, complete, mesh_volume, relative_volume_error, relative_volume_tolerance, evaluate_geometry_completeness()

### Community 509 - "lines"
Cohesion: 0.11
Nodes (18): lines, cinema: act build frames 1116..1728 t 18.6000..28.8000 s, cinema: act deliberate frames 648..1115 t 10.8000..18.6000 s, cinema: act mesh_hold frames 1729..2340 t 28.8000..39.0000 s, cinema: act skeleton frames 0..647 t 0.0000..10.8000 s, cinema: act solve frames 2341..3599 t 39.0000..60.0000 s, cinema: advisor bench/advisor candidates 108 gate_threshold 0.05 frames 109 decision hybrid_zoo h_rel 0.1 order 1 adapt_passes 0 eta_target 0 vetoed 1 ood_distance 67.6031 applied 0, cinema: advisor panel panel_action_columns 13 panel_case_columns 68 panel_winner_candidate 29 panel_score_min 1.27218 panel_score_max 5.83291 panel_input_p98 9.86192 panel_fc1_p98 18.362 panel_fc2_p98 33.101 panel_heads_p98 127.315 panel_contribution_p98 2.56529 (+10 more)

### Community 510 - "lowpass_grid_energy"
Cohesion: 0.50
Nodes (4): max_value, min_value, clamp_fraction(), lowpass_grid_energy()

### Community 511 - "run_artifacts.hpp"
Cohesion: 0.48
Nodes (6): atomic_write(), path, string, is_transient_rename_error(), write_run_json(), error_code

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

### Community 516 - "test_solve_cost.cpp"
Cohesion: 0.18
Nodes (17): assemble_reduced_without_cost(), brute_force_ldlt_nnz(), Dirichlet, Index, SparseMatrix, uint64_t, vector, VectorXd (+9 more)

### Community 517 - "LogLimits"
Cohesion: 0.33
Nodes (4): loglim(), LogLimits, What ``loglim`` had to do to the data, so the caller can say it., Set honest log limits, flooring the axis under the *bulk* of the data. Two…

### Community 518 - "ADR-0040: A boundary condition names the exact closure of a CAD face"
Cohesion: 0.11
Nodes (17): 1. The report, 2. Cross-checking the GUI against the CLI, 3. Root cause A: nearest-triangle region roulette (GUI / SolveJob), 4. Root cause B: the CLI's default end slab is not a face, 5. The fix, 6. Measured, 7. What this does not fix, 8. Consequences (+9 more)

### Community 519 - "refusal"
Cohesion: 0.40
Nodes (5): refusal, description, required, type, kind

### Community 520 - "recover_nodal_stress"
Cohesion: 0.21
Nodes (16): p, ElementType, Matrix, size_t, Stress, vector, Vector3d, VectorXd (+8 more)

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

### Community 526 - "MeshPreview"
Cohesion: 0.07
Nodes (22): CinemaLine, color, text, ImVec4, size_t, uint32_t, MeshPreview, n_elems (+14 more)

### Community 527 - "ActivationFrame"
Cohesion: 0.12
Nodes (17): ActivationFrame, action, candidate, fc1, fc2, gate_pass, heads, input (+9 more)

### Community 528 - "Case"
Cohesion: 0.50
Nodes (4): Case, h, name, path

### Community 529 - "spectral_fields"
Cohesion: 0.12
Nodes (16): spectral_fields, applied, bc_seeds, brep_curvature, edge_seeds, energy_kept, field_samples, geometry_seeds (+8 more)

### Community 530 - "host"
Cohesion: 0.67
Nodes (3): description, type, host

### Community 531 - "label"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 532 - "SurfaceTessellation"
Cohesion: 0.14
Nodes (16): array, uint32_t, uint8_t, vector, Vector3d, LoadRegion, hi, lo (+8 more)

### Community 533 - "ADR-0044: GLM cannot be the math library, and is not a useful second one"
Cohesion: 0.13
Nodes (13): Anti-cheat boundary, mathlib probe — GLM vs Eigen, Reading the numbers, What it measures, 1. Scope: GLM cannot express most of what this codebase does, 2. Performance: where GLM can compete, it is a wash, 3. The one large margin is an Eigen finding, not a GLM one, 4. Numerics: GLM's only real offer is disqualified (+5 more)

### Community 537 - "cinema/manifest.json"
Cohesion: 0.13
Nodes (14): generated_utc, git_rev, model, dir, onnx, onnx_sha256, outputs, schema (+6 more)

### Community 538 - "ADR-0036: A symmetric part gets a symmetric tiling"
Cohesion: 0.13
Nodes (15): 1. The report, 2. What was actually wrong, 3. The fix, 4. Measured, 5. Rejected alternative, measured rather than argued, 6. What is still open, 7. Consequences, 8. Follow-up: the order-dependent passes and the tessellation ceiling (+7 more)

### Community 539 - "ADR-0043: A film someone can read"
Cohesion: 0.13
Nodes (15): 10. The hero is now a load path, not a prop, 11. Cause and effect may be aligned, never relabelled, 12. The solver pane teaches with the result, 13. One frame must contain the whole motion, 1. What was wrong with the film, 2. The four rows, 3. One subject per chapter, 4. Holding on results (+7 more)

### Community 540 - "analyze_solve_cost"
Cohesion: 0.29
Nodes (11): analyze_solve_cost(), Dirichlet, SparseMatrix, vector, DisjointSet, parent_, elimination_forest(), free_dof_pattern() (+3 more)

### Community 541 - "SnapStats"
Cohesion: 0.13
Nodes (15): ConformityStats, count, max_distance, mean_distance, size_t, SmoothStats, max_residual, n_moved (+7 more)

### Community 543 - "PElevateResult"
Cohesion: 0.14
Nodes (14): ElementTypeCounts, hex20, hex8, other, tet10, tet4, size_t, PElevateResult (+6 more)

### Community 544 - "capture"
Cohesion: 0.15
Nodes (13): capture, auto_spec, duration_s, env, ffmpeg, fps, frame_count, frame_size (+5 more)

### Community 545 - "HostCalibration"
Cohesion: 0.17
Nodes (11): HostCalibration, bytes_per_s, flops_per_s, generated_utc, host, ref_mesh_ms, string, string (+3 more)

### Community 546 - "CinemaCellKey"
Cohesion: 0.20
Nodes (11): CinemaCellKey, corners, family, array, ElementType, int64_t, uint8_t, element_type_color() (+3 more)

### Community 547 - "SolveCostEstimate"
Cohesion: 0.17
Nodes (11): Dirichlet, Index, uint64_t, SolveCostEstimate, cg_bytes_per_iter, cg_flops_per_iter, factor_flops, factor_nnz (+3 more)

### Community 548 - "BRepInspection"
Cohesion: 0.17
Nodes (12): BRepInspection, available, closed, closed_shell_count, edge_count, face_count, shell_count, solid_count (+4 more)

### Community 549 - "ProximityFeatures"
Cohesion: 0.17
Nodes (12): ProximityFeatures, dihedral_p10, dihedral_p50, dihedral_p90, feat_pair_dist_mean_rel, feat_pair_dist_min_rel, feat_pair_dist_p10_rel, feature_centroids (+4 more)

### Community 550 - "gif"
Cohesion: 0.18
Nodes (11): attempts, bytes, duration_s, file, fps, max_bytes, sha256, start_s (+3 more)

### Community 551 - "Gmsh: swapping the mesh source"
Cohesion: 0.18
Nodes (10): A defect the comparison exposed, CalculiX: swapping the solver, Charging for degrees of freedom, Coverage, External comparisons: Gmsh and CalculiX, Gmsh run-to-run noise, Gmsh: swapping the mesh source, Order-2 pairing (+2 more)

### Community 552 - "Case"
Cohesion: 0.18
Nodes (11): Case, feature_refine, h, name, path, plane, array, uint32_t (+3 more)

### Community 553 - "ADR-0038: A fixture is applied to the boundary, not to a volume of nodes"
Cohesion: 0.20
Nodes (10): 1. The report, 2. What the purple region actually was, 3. The library never had it, 4. The rule, and where it lives, 5. What it did to the answer, 6. The picture, 7. What this does *not* fix, 8. Consequences (+2 more)

### Community 554 - "ADR-0042: The advisor explains itself on screen"
Cohesion: 0.20
Nodes (10): 1. What the heatmap could not show, 2. The activations come from the graph, not from a second implementation, 3. The mesh evolves at the granularity the mesher has, 4. What is cosmetic, exactly, 5. The case is chosen by the gate, not by what looks good, 6. The two halves share the clock, because the causal link is the point, 7. The fields animate in the order the answer is computed, 8. Packaging and provenance (+2 more)

### Community 555 - "RawFace"
Cohesion: 0.22
Nodes (10): RawFaceProvenance, vector, RawCell, faces, volume, RawFace, loop, neighbour_site (+2 more)

### Community 556 - "element_jacobians_positive"
Cohesion: 0.31
Nodes (9): coords_of(), Dynamic, ElementType, Matrix, span, uint32_t, element_jacobians_positive(), rule_positive() (+1 more)

### Community 557 - "SpectrumResult"
Cohesion: 0.22
Nodes (9): SpectrumResult, eigen, glm, label, SpectrumScore, over_tol, reported_failures, total (+1 more)

### Community 558 - "Portable-cost advisor retrain"
Cohesion: 0.22
Nodes (9): Campaign coverage and dataset, Deployed selection behavior, Feature and corpus expansion, Honest limitations, Host calibration and reporting, Portable-cost advisor retrain, Precision and architecture experiments, Solver instrumentation (+1 more)

### Community 559 - "poster"
Cohesion: 0.22
Nodes (9): poster, bytes, file, frame_source, height, sha256, source_bytes, source_frame (+1 more)

### Community 560 - "ADR-0037: A box selection is a region, and a smooth field is sampled on element sizes"
Cohesion: 0.22
Nodes (9): 1. The report, 2. Measuring the jag, 3.1 The load stopped on a staircase, 3.2 The wall smoother was not equalising anything, 3. Two causes, measured separately, 4. Four-fixture matrix, 5. What is left, and what it is, 6. Consequences (+1 more)

### Community 561 - "cost_labels.py"
Cohesion: 0.61
Nodes (8): finite_float(), host_calibration(), main(), mesh_work(), portable_cost_label(), self_test(), solve_bytes(), solve_flops()

### Community 562 - "Figure text: the words a reader actually sees"
Cohesion: 0.22
Nodes (8): Figure text: the words a reader actually sees, Glossary — use the right column, Hard never — reject on sight, Ownership, Rulings landed during the sweep, The unit trap — this is the important one, Verification is not optional, Voice

### Community 563 - "GuiReport"
Cohesion: 0.22
Nodes (6): gif_window(), GuiReport, _pairs(), parse_report(), What the GUI said about the take. Absent fields stay None, never 0., Which slice of the take the inline GIF shows, and where that came from. The…

### Community 564 - "test_mixed_fill.cpp"
Cohesion: 0.28
Nodes (8): bore_wall(), cell_faces(), array, pair, size_t, uint32_t, vector, surface_face_area()

### Community 565 - "AdvisorScale"
Cohesion: 0.25
Nodes (8): AdvisorScale, action_columns, contribution, layer, ready, score_max, score_min, winner_frame

### Community 566 - "command"
Cohesion: 0.25
Nodes (8): command, -a, --auto, /home/hunter/Desktop/Polyhedral-FEA/build/apps/gui/polymesh-gui, load tests/fixtures/parts/wishbone.step; h 5.5; material 200 0.3; mesher tet; solver direct; order 1; feature off; spectral on; fix 9; fix 10; loadface 5 -2500 0 -4000; cinema on; cinema advisor bench/advisor; adapt 0 0; wire off; solve; record /home/hunter/Desktop/Polyhedral-FEA/build/cinema/frames 3600; quit, -s, -screen 0 1920x1080x24, xvfb-run

### Community 567 - "VertexArray"
Cohesion: 0.25
Nodes (6): VertexArray, base_, nb_vertices_, stride_, VertexMesh, vertices

### Community 568 - "HandoffInfo"
Cohesion: 0.29
Nodes (7): HandoffInfo, campaign, checkpoint_state, finished_utc, git_head, mode, open_program_nodes

### Community 569 - "0.1.0 — 2026-08-26"
Cohesion: 0.29
Nodes (6): 0.1.0 — 2026-08-26, Changelog, Distribution, Known limits, Product, Studio

### Community 570 - "SurfaceRender"
Cohesion: 0.29
Nodes (7): size_t, RenderCoverage, pixels_covered, silhouette_area_px, SurfaceRender, coverage, image

### Community 571 - "homogeneous_boundary"
Cohesion: 0.29
Nodes (7): Index, map, uint32_t, vector, Vector3d, homogeneous_boundary(), modes_on_boundary()

### Community 572 - "test_kirsch_plate.cpp"
Cohesion: 0.33
Nodes (6): array, int64_t, Matrix3d, Vector3d, kirsch_stress(), param_key()

### Community 573 - "0011 — v7 retrain: authoritative curved CAD geometry"
Cohesion: 0.33
Nodes (6): 0011 — v7 retrain: authoritative curved CAD geometry, Calibration, tolerance and OOD, Decision quality against v6 (macro-mean regret, family-held-out folds), Provenance, What was regenerated, Why a whole generation

### Community 574 - "gradient_canvas"
Cohesion: 0.40
Nodes (6): gradient_canvas(), hex_to_rgb(), mix(), Parse a figstyle theme colour into 8-bit channels for PIL/PyVista., Blend two theme colours; used for the viewport's vertical gradient., Vertical viewport gradient, panel at the top easing to page at the foot.

### Community 575 - "Dirichlet"
Cohesion: 0.33
Nodes (5): Dirichlet, dof_values, Index, map, uint32_t

### Community 576 - "CinemaChapter"
Cohesion: 0.40
Nodes (5): cinema_chapters(), CinemaChapter, act, label, CinemaAct

### Community 577 - "bake_result"
Cohesion: 0.50
Nodes (5): DisplayMode, bake_result, ensure_framebuffer, frame_content, render

### Community 578 - "render_architecture"
Cohesion: 0.40
Nodes (5): _arch_center(), _arch_line_h(), Height of ``n`` lines of ``pt`` type, in axes fractions of the panel. Checked…, Pipeline diagram: two rows, tight feedback lane, subordinate taps., render_architecture()

### Community 579 - "draw_colorbar"
Cohesion: 0.40
Nodes (5): draw_colorbar(), _font(), ImageDraw, A glyph-verified font at figstyle's point size for this canvas width. ``out_w``…, Vertical viridis colour bar with SI-prefixed ticks (``0`` .. ``3.84 MPa``).…

### Community 580 - "HexFace"
Cohesion: 0.40
Nodes (5): HexFace, fixed_axis, fixed_val, vary0, vary1

### Community 581 - "Vector3d"
Cohesion: 0.50
Nodes (5): box_lower_bound(), Vector3d, FaceBox, hi, lo

### Community 582 - "Row"
Cohesion: 0.50
Nodes (4): Row, eigen_ns, glm_ns, name

### Community 583 - "run_probe.sh"
Cohesion: 0.83
Nodes (3): build_and_run(), run_all(), run_probe.sh script

### Community 584 - "take"
Cohesion: 0.50
Nodes (4): take, duration, fps, frames

### Community 585 - "ffmpeg_encoders"
Cohesion: 0.50
Nodes (4): ffmpeg_encoders(), pick_encoder(), Encoder names this ffmpeg build actually has, from ``-encoders``., The h264 encoder to use, and its rate-control arguments. An explicit…

### Community 586 - "clip_rule_text"
Cohesion: 0.50
Nodes (4): clip_rule_text(), ordinal(), The rule sentence. Identical in every stress footer and caption., 92' -> '92nd', '99.5' -> '99.5th'. Keeps captions readable prose.

### Community 587 - "param_key"
Cohesion: 0.50
Nodes (4): array, int64_t, Vector3d, param_key()

### Community 588 - "read_all"
Cohesion: 0.50
Nodes (4): path, uint8_t, vector, read_all()

### Community 589 - "Third-party notices"
Cohesion: 0.50
Nodes (3): Bundled or linked into distributed binaries, System dependencies, Third-party notices

### Community 590 - "version"
Cohesion: 0.67
Nodes (3): version, description, type

### Community 591 - "nodal_sum"
Cohesion: 0.67
Nodes (3): Vector3d, VectorXd, nodal_sum()

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **3401 isolated node(s):** `energy`, `free_dofs`, `nnodes`, `nelems`, `mesh_s` (+3396 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **179 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `thread()` connect `scene.hpp` to `Delaunay_psm.cpp`, `index_t`, `App`, `index_t`, `.size`, `ProgressHeartbeat`, `SolveJob`, `scene.cpp`, `test_brep_fidelity.cpp`, `Delaunay_psm.h`, `testlab/main.cpp`, `T`, `run_artifacts.hpp`?**
  _High betweenness centrality (0.022) - this node is a cross-community bridge._
- **Why does `HpMode` connect `TetRecipe` to `hierarchical.cpp`, `vector`?**
  _High betweenness centrality (0.021) - this node is a cross-community bridge._
- **Why does `BRepGeometryFidelity` connect `BRepGeometryFidelity` to `brep_fidelity.cpp`, `BRepInspection`, `BrepFidelitySummary`, `SampleDistribution`?**
  _High betweenness centrality (0.018) - this node is a cross-community bridge._
- **What connects `energy`, `free_dofs`, `nnodes` to the rest of the system?**
  _3401 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `vem.cpp` be split into smaller, more focused modules?**
  _Cohesion score 0.1444121915820029 - nodes in this community are weakly interconnected._
- **Should `analyze_campaign.py` be split into smaller, more focused modules?**
  _Cohesion score 0.11517165005537099 - nodes in this community are weakly interconnected._