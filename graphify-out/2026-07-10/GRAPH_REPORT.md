# Graph Report - Polyhedral-FEA  (2026-07-10)

## Corpus Check
- 191 files · ~111,503 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1734 nodes · 2948 edges · 130 communities (112 shown, 18 thin omitted)
- Extraction: 92% EXTRACTED · 8% INFERRED · 0% AMBIGUOUS · INFERRED: 248 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `68dd7ba4`
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
- unit_box
- bench_harness library
- User-Paintable Region Override (GUI)
- vector
- optional
- string
- VectorXd
- VolumeMesher

## God Nodes (most connected - your core abstractions)
1. `Viewport` - 58 edges
2. `NodalMesh` - 55 edges
3. `TriSurface` - 44 edges
4. `App` - 42 edges
5. `Palette` - 38 edges
6. `FeaError` - 36 edges
7. `Material` - 33 edges
8. `SolveJob` - 24 edges
9. `SolveResult` - 23 edges
10. `vem_energy_error_sq()` - 21 edges

## Surprising Connections (you probably didn't know these)
- `vem_body_load()` --calls--> `body`  [INFERRED]
  src/fea/src/vem.cpp → tests/support/mms.hpp
- `Hybrid Element Zoo` --semantically_similar_to--> `Element Types: tet/hex/prism/pyramid/VEM polyhedra`  [INFERRED] [semantically similar]
  docs/spec.md → README.md
- `Holdout Geometry Anti-Cheat` --semantically_similar_to--> `Anti-Cheat Boundary (No Hardcoded Refs in src/apps)`  [INFERRED] [semantically similar]
  docs/benchmarks.md → CONTRIBUTING.md
- `sizing_field` --semantically_similar_to--> `resolve_mesh_size`  [INFERRED] [semantically similar]
  src/adapt/CMakeLists.txt → examples/README.md
- `merge_unique()` --references--> `NodalMesh`  [INFERRED]
  apps/bench/d6_tier3.cpp → src/fea/include/fea/nodal_mesh.hpp

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Core library dependency stack (geom→mesh→fea→pipeline)** — cmakelists_src_geom, cmakelists_src_mesh, cmakelists_src_fea, cmakelists_src_pipeline, cmakelists_src_adapt [EXTRACTED 1.00]
- **Verification tier ladder (Tier 0–3)** — docs_benchmarks_tier0, docs_benchmarks_tier1, docs_benchmarks_tier2_mms, docs_benchmarks_tier3_perf [EXTRACTED 1.00]
- **Anti-cheat and honesty system** — contributing_anti_cheat, docs_benchmarks_holdout_geometries, github_workflows_ci_grep_audit, claude_commands_audit, audits_readme_holdout_protocol [INFERRED 0.85]
- **Tier-1 analytical verification cases** — docs_decisions_0009_tier1_verification_setups_kirsch_plate, docs_decisions_0009_tier1_verification_setups_goodier_cavity, docs_decisions_0009_tier1_verification_setups_l_domain, docs_decisions_0009_tier1_verification_setups_gmsh_import [EXTRACTED 1.00]
- **Adaptive mesh density strategies** — docs_decisions_0012_hybrid_graded_tet_graded_tet_fill, docs_decisions_0014_adapt_seed_remesh_dorfler_seed_remesh, docs_decisions_0016_local_h_refine_rivara_leb, docs_bench_d6_tier3_graded_tet10 [INFERRED 0.85]
- **Unified element formulation zoo** — docs_decisions_0003_element_formulations_fea_element_trait, docs_decisions_0003_element_formulations_isoparametric_fem, docs_decisions_0003_element_formulations_vem, docs_decisions_0011_vem_k1_vem_k1_formulation, docs_decisions_0017_vem_k2_vem_k2_serendipity [EXTRACTED 1.00]
- **Public fixtures exercised via mesh/solve example scripts on product CLI** — examples_readme_public_geometry_fixtures, examples_readme_run_mesh_public, examples_readme_run_solve_public, examples_readme_polymesh_cli, examples_readme_mesher_options [EXTRACTED 1.00]

## Communities (130 total, 18 thin omitted)

### Community 0 - "VEM & Nodal Mesh"
Cohesion: 0.07
Nodes (82): BodyForce, Fun, function, kP2Mono, kP2Vec, FeaError, runtime_error, Vector3d (+74 more)

### Community 1 - "Grid Classification"
Cohesion: 0.06
Nodes (73): EdgeKey, set, CartesianGrid, cell, nx, ny, nz, origin (+65 more)

### Community 2 - "Element Assembly Core"
Cohesion: 0.10
Nodes (12): Element, num_nodes, order, stiffness, Material, d_matrix, poissons_ratio, youngs_modulus (+4 more)

### Community 3 - "GUI Viewport GL"
Cohesion: 0.05
Nodes (41): DisplayMode, uint32_t, Vector3d, VectorXd, Viewport, background_program_, background_vao_, baked_max_ (+33 more)

### Community 4 - "Example Geometries"
Cohesion: 0.08
Nodes (32): Cantilever-style boundary conditions, Cartesian grid product fills (ADR-0015), cylinder_prism.stl, l_domain.stl, Mesher options (tet|hex|graded|hexpyr|hexvem), plate.stl, polymesh product CLI, Public geometry fixtures (+24 more)

### Community 5 - "Hybrid Graded Fill"
Cohesion: 0.07
Nodes (42): CartesianGrid, MixedFillOutput, GradedTetFillOutput, h_coarse, h_fine, mesh, n_coarse_cells, n_feature_cells (+34 more)

### Community 6 - "GUI Theme Palette"
Cohesion: 0.05
Nodes (40): apply_theme(), ImVec4, make_interwebz_palette(), make_slate_palette(), Palette, accent, accent_dim, accent_mid (+32 more)

### Community 7 - "Bench Emit Scripts"
Cohesion: 0.31
Nodes (9): fmt_num(), load_results(), main(), Any, Path, Markdown-friendly ASCII sparkline; skips None., Minimal inline SVG polyline for accuracy trend (lower often better)., sparkline() (+1 more)

### Community 8 - "SpMV CSR Backend"
Cohesion: 0.09
Nodes (27): Backend, CsrMatrix, col_idx, cols, row_ptr, rows, values, size_t (+19 more)

### Community 9 - "CLI Mesh Solve"
Cohesion: 0.11
Nodes (27): cmd_check(), cmd_mesh(), cmd_solve(), span, string, string_view, VolumeMesher, load_surface() (+19 more)

### Community 10 - "Poly Mesh Topology"
Cohesion: 0.09
Nodes (26): CellId, CellKind, FaceId, Cell, faces, kind, Face, neighbour (+18 more)

### Community 11 - "GUI App State"
Cohesion: 0.05
Nodes (65): App, deform_auto, deform_scale, deform_true_scale, dof_count, hovered_region, job, lmb_drag_px (+57 more)

### Community 12 - "Gmsh MSH Import"
Cohesion: 0.13
Nodes (26): GmshType, map, string, vector, MshModel, mesh, physical_faces, physical_names (+18 more)

### Community 13 - "Surface Traction"
Cohesion: 0.09
Nodes (31): face_num_nodes(), FaceType, uint32_t, vector, SurfaceFace, nodes, type, assemble_traction_load() (+23 more)

### Community 14 - "Pipeline Scene Jobs"
Cohesion: 0.12
Nodes (27): Model, optional, ResolvedMeshSize, SimSetup, SolveResult, size_t, span, TriSurface (+19 more)

### Community 15 - "Structured Mesh Tests"
Cohesion: 0.17
Nodes (23): Vector3d, NodalMesh, check_validity, elements, nodes, box_hex_mesh(), box_tet_mesh(), cell_corners() (+15 more)

### Community 16 - "Surface Projection"
Cohesion: 0.14
Nodes (15): ClosestPoint, distance, point, triangle, ConformityStats, count, max_distance, mean_distance (+7 more)

### Community 17 - "Shape Functions"
Cohesion: 0.23
Nodes (17): Dynamic, Matrix, VectorXd, ShapeEval, dn, n, ElementType, vector (+9 more)

### Community 18 - "Adapt Loop Seeds"
Cohesion: 0.12
Nodes (21): AdaptSuggestion, h_next, marked_fraction, n_marked, refine_seeds, seed_band, size_t, vector (+13 more)

### Community 19 - "Mixed Cell Fill"
Cohesion: 0.10
Nodes (22): MixedCellKind, array, size_t, uint32_t, uint8_t, vector, Vector3d, MixedCell (+14 more)

### Community 20 - "FEA Solve Methods"
Cohesion: 0.12
Nodes (22): solve_l_mesh(), Dirichlet, dof_values, Index, map, SolveMethod, SolveOptions, cg_max_iters (+14 more)

### Community 21 - "Scene Solve Result"
Cohesion: 0.14
Nodes (14): VectorXd, SolveResult, boundary_quads, displacement, element_eta, global_eta, max_displacement, max_nodal_eta (+6 more)

### Community 22 - "GUI Widgets"
Cohesion: 0.16
Nodes (21): ClosestPoint, CollectOffendersFn, ConformityStats, SnapStats, closest_on_surface(), closest_on_surface_brute(), closest_on_triangle(), size_t (+13 more)

### Community 23 - "MMS Convergence Tests"
Cohesion: 0.17
Nodes (10): VectorXd, energy_norm_error(), ElementType, Expectation, name, order, type, solve_mms_error() (+2 more)

### Community 24 - "Solve Job Pipeline"
Cohesion: 0.12
Nodes (19): load, SolveJob, clear_failure, error_, join_worker, mesh_only_, result_, set_status (+11 more)

### Community 25 - "D6 Tier3 Bench"
Cohesion: 0.06
Nodes (53): add_node(), array, int64_t, map, string, uint32_t, vector, fill_block_tets() (+45 more)

### Community 26 - "Viewport Camera"
Cohesion: 0.17
Nodes (18): Camera, distance_, dolly, eye, fit, fov_y_, orbit, pan (+10 more)

### Community 27 - "Sizing Field Blend"
Cohesion: 0.14
Nodes (18): dist_, blend_to_max(), clamp_size(), DistanceFn, vector, Vector3d, FeatureSizing::FeatureSizing(), FeatureSizing::size_at() (+10 more)

### Community 28 - "Transition Fill"
Cohesion: 0.11
Nodes (20): array, size_t, uint32_t, uint8_t, vector, Vector3d, TransitionCell, kind (+12 more)

### Community 29 - "CMake Project Build"
Cohesion: 0.22
Nodes (14): polymesh CLI Executable, SimplicialLDLT Direct Sparse Solver, POLYMESH_ENABLE_LTO (OFF Default, Eigen-Safe), POLYMESH_NATIVE_ARCH (OFF Default, Eigen-Safe), polymesh CMake Project, src/adapt Library, src/fea Library, src/geom Library (+6 more)

### Community 30 - "Scene Model Bounds"
Cohesion: 0.22
Nodes (9): set_model, update_overlays, Model, bbox_max, bbox_min, name, region_count, surface (+1 more)

### Community 31 - "Geom Indicators"
Cohesion: 0.19
Nodes (15): vector, VertexCurvature, kappa, VertexThickness, thickness, size_t, uint32_t, Vector3d (+7 more)

### Community 32 - "STL Geometry IO"
Cohesion: 0.22
Nodes (15): GeomError, runtime_error, byte, path, size_t, Soup, span, T (+7 more)

### Community 33 - "TriSurface Geometry"
Cohesion: 0.16
Nodes (14): array, uint32_t, vector, Vector3d, TriSurface, triangles, validate, vertices (+6 more)

### Community 34 - "Sim Setup Loads"
Cohesion: 0.14
Nodes (14): VolumeMesher, SimSetup, adapt_leb_waves, adapt_passes, eta_target, fixtures, loads, mesh_size (+6 more)

### Community 35 - "Kirsch Graded Tests"
Cohesion: 0.16
Nodes (15): RadialMap, array, int64_t, Matrix3d, size_t, Vector3d, kirsch_stress(), KirschRun (+7 more)

### Community 36 - "P-Elevate Elements"
Cohesion: 0.05
Nodes (39): __global__, BenchError, map, runtime_error, string, ReferenceCase, citation, name (+31 more)

### Community 37 - "Tet Quality Metrics"
Cohesion: 0.13
Nodes (25): FaceConformityStats, is_conforming, n_boundary_faces, n_hanging_faces, n_interior_faces, n_nonconforming, n_tet_faces, n_unique_faces (+17 more)

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
Cohesion: 0.15
Nodes (14): L-domain re-entrant corner case, kirsch-plate case, ADR-0009 Tier-1 verification setups, C5 equal-DOF logarithmic grading, Gmsh ASCII 2.2 import, Goodier cavity, Kirsch plate (SCF=3), L-domain Williams singularity (+6 more)

### Community 42 - "Boundary Faces"
Cohesion: 0.27
Nodes (13): FaceKey, add_face(), array, map, uint32_t, vector, emit_element_faces(), extract_boundary_faces() (+5 more)

### Community 43 - "Schema Mesh Solve"
Cohesion: 0.15
Nodes (13): description, minimum, type, mesh, solve, total, description, minimum (+5 more)

### Community 45 - "Competitive Peer Solvers"
Cohesion: 0.40
Nodes (6): CalculiX Peer Runner Contract, CalculiX First Peer Solver Priority, Code_Aster Third Peer Solver, Elmer Second Peer Solver, Competitive Benchmark Harness, ADR-0005 Benchmark Baseline (Uniform Tet10 + CalculiX)

### Community 46 - "Roadmap Phases CUDA"
Cohesion: 0.29
Nodes (7): POLYMESH_WITH_CUDA, Phase P6 Performance Engineering, Phase P6.5 GUI, Phase P7 OSS Release Readiness, ADR-0006 GUI In Scope (P6.5), ADR-0008 Optional CUDA Backend, CUDA SpMV Backend (Optional)

### Community 47 - "Geometry Sizing"
Cohesion: 0.17
Nodes (12): GeometrySizing, blend_, curv_frac_, edges_, h_max_, h_min_, kappa_, size_at (+4 more)

### Community 48 - "Reference Case Bench"
Cohesion: 0.04
Nodes (43): Anti-cheat, Assembly change for H2, Constraints (do not break), Context, Critical files, Epic exit (E1), File ownership (to avoid merge thrash), First concrete commits after approval (+35 more)

### Community 49 - "FEA Colormap Display"
Cohesion: 0.23
Nodes (9): element_num_nodes(), ElementType, uint32_t, vector, NodalElement, faces, nodes, type (+1 more)

### Community 50 - "Schema Type Props"
Cohesion: 0.18
Nodes (11): description, type, description, type, properties, case_id, host, timestamp (+3 more)

### Community 51 - "Analytical Tier1 Cases"
Cohesion: 0.33
Nodes (7): Goodier Spherical Cavity Case, Kirsch Plate Circular-Hole Case, L-Shaped Domain Singularity Case, Lamé Thick-Walled Cylinder Case, Tier 1 Analytical Solutions, Timoshenko Cantilever Case, Phase P3 Geometric Feature Hybrid Meshing

### Community 52 - "Geometry Kernel ADR"
Cohesion: 0.22
Nodes (11): ADR-0001 Geometry kernel, OpenCASCADE B-rep/STEP, POLYMESH_WITH_OCC CMake option, STL path (always compiled), ADR-0002 License BSD-3-Clause, BSD-3-Clause license, C++20 only (CMake + Ninja), CLAUDE.md / agents notes (moved) (+3 more)

### Community 53 - "Cantilever Iterative"
Cohesion: 0.22
Nodes (10): CantileverSetup, bc, length, loads, mesh, nfree, Index, VectorXd (+2 more)

### Community 54 - "D6 Scoreboard Bench"
Cohesion: 0.31
Nodes (10): D6 Tier-3 L-domain instrument, Graded tet10 path, Tier-3 targets (≥5× DOF, ≥3× wall time), Uniform tet10 baseline path, Benchmark scoreboard, CalculiX peer solver, ADR-0005 Benchmark baseline, CalculiX audit cross-check (+2 more)

### Community 55 - "Element Formulations ADR"
Cohesion: 0.27
Nodes (10): fea::Element unified trait, Isoparametric FEM p=1..4, Virtual Element Method k=1,2, Wachspress/mean-value polyhedral FEM, ADR-0011 VEM k=1, ElementType::kPolyVem, VEM k=1 formulation, ADR-0017 VEM k=2 (+2 more)

### Community 56 - "CUDA SpMV Kernel"
Cohesion: 0.18
Nodes (11): array, uint32_t, vector, Vector3d, PrismFillOutput, boundary_max_distance, boundary_quads, h (+3 more)

### Community 57 - "Feature Graded Error"
Cohesion: 0.22
Nodes (8): FeatureGradedSizing, alpha_, edges_, h_max_, h_min_, size_at, surface_, vector

### Community 58 - "Hex Fill Output"
Cohesion: 0.20
Nodes (10): HexFillOutput, boundary_max_distance, boundary_quads, h, hexes, nodes, array, uint32_t (+2 more)

### Community 59 - "Local Refine Tests"
Cohesion: 0.40
Nodes (9): array, size_t, uint32_t, vector, Vector3d, extract_tet4(), nearest_tet(), tets_to_nodal() (+1 more)

### Community 60 - "Audit Bench Policy"
Cohesion: 0.17
Nodes (12): polymesh-d6-tier3 Bench Binary, Holdout Geometry Audit Protocol, Edge-Case Mesh Fixtures Suite, Shared-Edge Ray-Parity Grid Fill Fix, Public Geometry Fixtures Suite, unit_box.stl Public Fixture, External Contributor PR Policy, src/bench Library (Reference Loader) (+4 more)

### Community 61 - "Mesh Spec Layers"
Cohesion: 0.42
Nodes (9): ensure_built(), find_binary(), _fmt(), main(), Any, Path, Emit competitive-schema rows: per-path headline + summary metrics as notes., split_for_scoreboard() (+1 more)

### Community 62 - "Gate1 Baseline Freeze"
Cohesion: 0.33
Nodes (6): /audit Adversarial Audit Command, /loop Autonomous Plan-Build-Verify Loop, ZZ Estimator Honesty (Effectivity [0.5, 2]), Phase P0 Decisions & Scaffolding, Phase P1 Reference Solver Baseline, Phase P2 Mesh Core + Tet + Validity

### Community 63 - "Hybrid Mesher ADR"
Cohesion: 0.22
Nodes (9): ADR-0012 Hybrid graded tet + mixed zoo, graded_tet_fill, Kuhn 6-tet hex split, mixed_fill / VolumeMesher::kHybrid, ADR-0013 Hex core + pyramid skin, expand_hex_core_to_pyramids, VolumeMesher::kHexPyramid, Surface snap + Jacobian unsnap (+1 more)

### Community 64 - "HP Mesh Co-Design"
Cohesion: 0.38
Nodes (7): GATE 1 P1 Baseline Convergence Report, P1 Solver Baseline Frozen, GATE 1 Baseline Freeze, hp-Adaptive Mesh Co-Design, Linear Elastostatics (3D), North Star: Heterogeneous hp Mesh + FEA Co-Optimization, PolyMesh

### Community 65 - "ZZ Stress Recovery"
Cohesion: 0.32
Nodes (8): Dynamic, Matrix, Stress, Vector3d, VectorXd, element_centroid(), recover_zz(), stress_at()

### Community 66 - "Geom Features Extract"
Cohesion: 0.36
Nodes (8): size_t, span, vector, Vector3d, detect_sharp_edges(), distance_to_features(), point_segment_distance(), tri_normal()

### Community 67 - "L-Domain Solve Tests"
Cohesion: 0.18
Nodes (10): uint32_t, VectorXd, LSolve, energy, mesh, peak_vm_at_corner, u, make_l_hex_mesh() (+2 more)

### Community 68 - "Solve Energy Output"
Cohesion: 0.22
Nodes (11): array, DisplayMode, uint32_t, vector, fea_colormap(), bake_result, ensure_framebuffer, render (+3 more)

### Community 69 - "Root Schema JSON"
Cohesion: 0.25
Nodes (7): additionalProperties, description, $id, required, $schema, title, type

### Community 70 - "Polyhedral Ideas P4P5"
Cohesion: 0.29
Nodes (7): fea-madness Idea Harvest Source, Seed-Based Voronoi/Laguerre Polyhedral Meshing, Phase P4 Polyhedral VEM Elements, ADR-0003 Unified Element Interface, Hybrid Element Zoo, Virtual Element Method (VEM), Element Types: tet/hex/prism/pyramid/VEM polyhedra

### Community 71 - "Lame Cylinder Tests"
Cohesion: 0.22
Nodes (7): cantilever_setup(), path, write_box_stl(), box_end_regions(), path, write_box_stl(), thread

### Community 72 - "Backend Bench Smoke"
Cohesion: 0.33
Nodes (5): Benchmark table, Current phase, Done, Open issues, PROGRESS

### Community 73 - "Resolved Mesh Size"
Cohesion: 0.52
Nodes (6): b_matrix(), Dynamic, Matrix, MatrixXd, element_coords(), element_stiffness()

### Community 74 - "CUDA Backend ADR"
Cohesion: 0.29
Nodes (7): ADR-0003 Element formulations, ADR-0007 Language C++20, ADR-0008 CUDA backend, Batched element stiffness (CUDA target), CUDA optional backend, fea/backend.hpp dispatch layer, SpMV in CG iterative solves

### Community 75 - "Adapt Remesh ADR"
Cohesion: 0.33
Nodes (7): ADR-0014 Dörfler seed remesh, Dörfler seed remesh, ZZ error recovery, ADR-0016 Local h-refine LEB, Hanging-node MPCs (deferred), mesh::local_refine_tets API, Rivara longest-edge bisection

### Community 76 - "ZZ Recovery API"
Cohesion: 0.13
Nodes (10): Stress, vector, ZzRecovery, element_eta, global_eta, nodal_stress, array, int64_t (+2 more)

### Community 77 - "STEP Geometry Load"
Cohesion: 0.43
Nodes (6): bounding_diagonal(), path, Soup, load_step(), triangulate_shape(), TopoDS_Shape

### Community 78 - "Kirsch Plate Tests"
Cohesion: 0.29
Nodes (7): size_t, LocalRefineStats, n_bisections, n_input_tets, n_marked, n_new_nodes, n_output_tets

### Community 79 - "Theme Apply Palettes"
Cohesion: 0.33
Nodes (5): ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn), Alternatives rejected, Consequences, Context, Decision

### Community 80 - "GL Shader Bind"
Cohesion: 0.47
Nodes (6): bind_line_attr(), compile(), link(), init, GLenum, GLuint

### Community 81 - "Mesh Structure ADR"
Cohesion: 0.40
Nodes (6): ADR-0004 Mesh data structure, Face-based owner/neighbour mesh, Half-face/half-edge alternative, ADR-0010 Edge vs face mesh store, Derived edge adjacency index, Edge-primary topology (rejected)

### Community 82 - "GUI Phase ADR"
Cohesion: 0.40
Nodes (6): ADR-0006 GUI phase P6.5, Desktop GUI (GLFW + Dear ImGui + OpenGL), Draft voxel mesher v0, Interwebz v2 GUI theme, GUI theme & layout rules, Theme tokens (theme.hpp/cpp)

### Community 83 - "Mesher Solver Co-Design"
Cohesion: 0.40
Nodes (5): lame-cylinder case, PolyMesh solver, timoshenko-cantilever case, hp-adaptivity (order + size), Mesher-solver co-design

### Community 84 - "Agent Loop Process"
Cohesion: 0.40
Nodes (5): Agent loop harness rules, One iteration = one ROADMAP ID, ROADMAP/progress/phases source of truth, Stuck protocol (3 failures), Hunter-124 commit attribution policy

### Community 85 - "Quadrature Tests"
Cohesion: 0.22
Nodes (9): array, map, uint32_t, vector, VolumeMeshOutput, boundary_node_region, boundary_quads, mesh (+1 more)

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
Cohesion: 0.40
Nodes (5): Stress, vector, VectorXd, recover_nodal_stress(), von_mises()

### Community 90 - "Schema DOFs Field"
Cohesion: 0.50
Nodes (4): description, minimum, type, dofs

### Community 91 - "Schema Version Field"
Cohesion: 0.50
Nodes (4): schema_version, const, description, type

### Community 92 - "Schema Wall Time"
Cohesion: 0.50
Nodes (4): wall_time_s, additionalProperties, required, type

### Community 93 - "Adaptivity Pipeline"
Cohesion: 0.28
Nodes (9): Tier 3 Performance Benchmarks, Goal-Oriented (Adjoint) Adaptivity, Phase P5 Adaptive Loop Product, Dörfler Marking, Geometry-Driven A Priori Sizing, Architecture Pipeline geom→mesh→fea→adapt, Solution-Driven A Posteriori Adaptivity, Tier-3 Win Targets (≥5× DOF, ≥3× Wall Time) (+1 more)

### Community 94 - "Schema Label Field"
Cohesion: 0.67
Nodes (3): description, type, label

### Community 95 - "Schema Notes Field"
Cohesion: 0.67
Nodes (3): description, type, notes

### Community 96 - "Schema Solver Field"
Cohesion: 0.67
Nodes (3): solver, description, type

### Community 97 - "Schema Version Meta"
Cohesion: 0.67
Nodes (3): version, description, type

### Community 100 - "Region Pick Optional"
Cohesion: 0.25
Nodes (8): size_t, string, ResolvedMeshSize, auto_chosen, h, min_feature_length, n_sharp_edges, note

### Community 118 - "polymesh-gui Executable"
Cohesion: 0.50
Nodes (4): polymesh-gui Executable, POLYMESH_WITH_GUI, Layer Dependency Direction Rule, GUI Presentation-Only Rule

### Community 119 - "Verification"
Cohesion: 0.33
Nodes (5): atomic, mutex, Vector3d, RegionLoad, force

## Ambiguous Edges - Review These
- `adapt loop (loop.cpp)` → `FEA solve`  [AMBIGUOUS]
  src/adapt/CMakeLists.txt · relation: conceptually_related_to

## Knowledge Gaps
- **544 isolated node(s):** `Current phase`, `Done`, `Benchmark table`, `Open issues`, `Parallel tracks` (+539 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **18 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **What is the exact relationship between `adapt loop (loop.cpp)` and `FEA solve`?**
  _Edge tagged AMBIGUOUS (relation: conceptually_related_to) - confidence is low._
- **Why does `NodalMesh` connect `Structured Mesh Tests` to `VEM & Nodal Mesh`, `GroupBox Frame UI`, `ZZ Stress Recovery`, `L-Domain Solve Tests`, `P-Elevate Elements`, `Resolved Mesh Size`, `Boundary Faces`, `CLI Mesh Solve`, `Gmsh MSH Import`, `Surface Traction`, `FEA Colormap Display`, `FEA Solve Methods`, `Scene Solve Result`, `Quadrature Tests`, `MMS Convergence Tests`, `Cantilever Iterative`, `D6 Tier3 Bench`, `Local Refine Tests`?**
  _High betweenness centrality (0.078) - this node is a cross-community bridge._
- **Why does `Viewport` connect `GUI Viewport GL` to `Solve Energy Output`, `Poly Mesh Topology`, `GUI App State`, `Mesh Face Headers`, `GL Shader Bind`, `Context`, `Viewport Camera`, `Scene Model Bounds`?**
  _High betweenness centrality (0.062) - this node is a cross-community bridge._
- **Why does `TriSurface` connect `TriSurface Geometry` to `STL Geometry IO`, `Grid Classification`, `Geom Features Extract`, `P-Elevate Elements`, `CLI Mesh Solve`, `Mesh Face Headers`, `STEP Geometry Load`, `Geometry Sizing`, `Tet Fill Tests`, `Feature Graded Error`, `Sizing Field Blend`, `Scene Model Bounds`, `Geom Indicators`?**
  _High betweenness centrality (0.044) - this node is a cross-community bridge._
- **What connects `Current phase`, `Done`, `Benchmark table` to the rest of the system?**
  _562 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `VEM & Nodal Mesh` be split into smaller, more focused modules?**
  _Cohesion score 0.07053291536050156 - nodes in this community are weakly interconnected._
- **Should `Grid Classification` be split into smaller, more focused modules?**
  _Cohesion score 0.05526675786593707 - nodes in this community are weakly interconnected._