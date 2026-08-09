<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Variable-everything idea bank (mesh degrees of freedom)

## Variable-everything idea bank

Ranking is within the reality of one Windows workstation, the current C++20/Eigen stack, and a linear-elasticity product. `Impact` means likely improvement in trustworthy accuracy per DOF or second, not novelty.

### 1. Size / density

**[S1] Close the solve–estimate–remesh loop around scalar h** — `size/density` · impact H · effort M · risk M
- *What:* Convert `ZzRecovery::element_eta` into a continuous target-size field, normalize it to a requested element/DOF complexity, limit gradation in $\log h$, remesh, transfer displacement/BC classification, and repeat to estimator stagnation. This is posterior-driven density, unlike the current one-shot geometry/load seed field.
- *Why it wins:* It spends elements where this actual load case is wrong rather than where curvature merely suggests it might be wrong; equidistributing local error is usually much more DOF-efficient than fixed thresholds and Dörfler-only split/remesh decisions.
- *Prior art:* Loseille & Alauzet, continuous-mesh error/complexity normalization, summarized and used in the PETSc–ParMmg integration: https://arxiv.org/pdf/2201.02806
- *Fit to PolyMesh:* Extend `src/fea/src/zz.cpp`, `src/adapt/src/graded_sizing.cpp`, and `pipeline::build_refinement_plan()` in `src/pipeline/src/scene.cpp`; add conservative mesh-to-mesh displacement/stress transfer and convergence bookkeeping in `adapt::drive_hp`.
- *Killer risk:* ZZ recovery can be confidently wrong at singular corners and across abrupt material/BC changes, so the loop can concentrate the mesh on estimator artifacts.

**[S2] Goal-oriented h for one reported quantity** — `size/density` · impact H · effort L · risk M
- *What:* Add a dual-weighted residual (DWR) option that sizes for compliance, a selected displacement, or an averaged stress rather than global energy error. Run one adjoint solve per quantity of interest and fuse primal/dual element contributions into the sizing field.
- *Why it wins:* A global norm wastes DOFs in regions irrelevant to the answer the user reads; DWR targets error propagation into that answer.
- *Prior art:* Wallwork et al., “Anisotropic Goal-Oriented Mesh Adaptation in Firedrake”: https://doi.org/10.5281/zenodo.3653101
- *Fit to PolyMesh:* Reuse the assembled symmetric stiffness matrix and BC machinery in `src/fea`; add QoI load-vector construction beside loads, and feed indicators into `graded_sizing.cpp`/`hp_driver.cpp`.
- *Killer risk:* Point stress/displacement QoIs are mathematically singular or noisy unless regularized, making impressive-looking convergence meaningless.

**[S3] Hessian-magnitude isotropic sizing** — `size/density` · impact M · effort M · risk M
- *What:* Recover Hessians of each displacement component or selected stress invariants, collapse them to an isotropic scalar using the largest absolute eigenvalue, and derive $h(x)$ for a target interpolation error. This is a stepping stone to a full tensor metric and isolates whether second-derivative information is useful before rebuilding the mesher.
- *Why it wins:* It detects thin curved solution layers and bending gradients that the current curvature/thickness seeds cannot predict.
- *Prior art:* The PETSc–ParMmg paper constructs metrics from recovered Hessians and reports optimal convergence where uniform refinement fails: https://arxiv.org/pdf/2201.02806
- *Fit to PolyMesh:* Generalize the patch SVD in `src/fea/src/zz.cpp` from stress recovery to gradient/Hessian recovery, then emit `SizeSource`s consumed by `seed_plan()`.
- *Killer risk:* Second derivatives recovered from low-order, poor-quality tets are noisy; regularization may erase precisely the localized information sought.

**[S4] Curvature-to-chord-error sizing, not a curvature heuristic** — `size/density` · impact M · effort S · risk L
- *What:* Replace a turn-angle dial with a geometric error budget: choose edge length from principal curvature so sagitta/chordal deviation and normal error remain below tolerances, independently per CAD face. Couple it to geometry order so a quadratic boundary is allowed larger spans than a planar one.
- *Why it wins:* It turns CAD boundary approximation into a measurable error budget and avoids globally refining flat regions or over-refining mildly curved large-radius faces.
- *Prior art:* Boissonnat & Oudot’s surface sampling gives Hausdorff/normal guarantees with samples adapted to medial distance: https://doi.org/10.1016/j.gmod.2005.01.004
- *Fit to PolyMesh:* Build on `geom::CadTopology::chordal_edge_metrics*`, OCC face curvature, `surface_project.cpp`, and `pipeline::build_refinement_plan()`.
- *Killer risk:* Small geometric error does not imply small elasticity error near concentrated loads or re-entrant features.

**[S5] Local-feature-size and protecting-ball field** — `size/density` · impact H · effort L · risk M
- *What:* Approximate distance to the medial axis and install protecting balls around CAD vertices, sharp edges, close opposing faces, and small-radius blends; use this as a hard upper bound on h. Unlike the current inward-ray thickness grid, local feature size sees close nonparallel features and prevents a mesher from stepping over topology.
- *Why it wins:* It raises reliable resolution only where geometry imposes it and prevents missed holes, narrow gaps, and short sharp edges—the failures that no posterior solve can repair after topology was lost.
- *Prior art:* Ruppert/Shewchuk Delaunay refinement uses local feature size for size-optimal grading and protecting features: https://people.eecs.berkeley.edu/~jrs/meshbook.html
- *Fit to PolyMesh:* Add an OCC-aware feature oracle beside `CadTopology` and thickness estimation; emit protected `SizeSource`s and boundary constraints to each `VolumeMesher`.
- *Killer risk:* Robust medial-axis approximation on dirty STEP topology is a substantial geometry project; raw medial axes are notoriously unstable.

**[S6] Offset-layer / through-thickness sizing** — `size/density` · impact H · effort L · risk H
- *What:* Detect wall pairs, construct approximate mid-surfaces and offset layers, prescribe a discrete number of elements through thickness, and grade only tangentially outside the layer. Make the count depend on solution order and expected bending, not a universal “three elements” rule.
- *Why it wins:* Isotropic tets pay cubically to resolve a thin wall; layered prisms/hexes spend DOFs through thickness only where bending strain varies.
- *Prior art:* Park et al., boundary-layer adaptation separates in-plane and thickness adaptation: https://arxiv.org/pdf/1405.0620
- *Fit to PolyMesh:* Replace the scalar output of the wall-thickness ray grid with paired faces, normals, and layer counts; extend `prism_sweep`, hybrid meshing, `wall_project.cpp`, and sizing sources.
- *Killer risk:* Offsets self-intersect at fillets, junctions, and thickness changes; a partially valid layer is worse than robust tetrahedra.

**[S7] Multi-source gradation as an optimization problem** — `size/density` · impact M · effort M · risk L
- *What:* Represent every size request as a log-size upper bound with provenance, solve for the coarsest Lipschitz-continuous field satisfying them, and enforce a global complexity budget. Report which source—geometry, BC, posterior error, feature size, or user box—wins at every point.
- *Why it wins:* It prevents hidden precedence and seed-radius interactions from creating unnecessary fine halos while keeping transition ratios safe.
- *Prior art:* “Smooth Gradation of Anisotropic Meshes Using Log-Euclidean Metrics”: https://doi.org/10.2514/1.J059864
- *Fit to PolyMesh:* Replace ball-by-ball fusion in `graded_sizing.cpp` and `decimate_sources()` with a background-field solve; expose provenance through `RefinementPlan` and testlab JSON.
- *Killer risk:* A mathematically smooth field cannot rescue a mesher whose local operators do not actually honor it.

### 2. Anisotropy

**[A1] First-class SPD Riemannian metric field $M(x)$** — `anisotropy` · impact H · effort L · risk M
- *What:* Make a symmetric positive-definite 3×3 metric—not scalar h—the core sizing contract. Define metric edge length, element quality/volume, eigenvalue clamps, aspect-ratio caps, complexity normalization, log-Euclidean interpolation, and intersection/union; scalar h becomes $M=h^{-2}I$.
- *Why it wins:* One field simultaneously controls size, stretch, and orientation, allowing thin-wall and bending layers to be resolved with orders-of-magnitude fewer cells than isotropic h.
- *Prior art:* Loseille & Alauzet’s continuous metric framework and PETSc’s concrete SPD/intersection/normalization API: https://arxiv.org/pdf/2201.02806
- *Fit to PolyMesh:* Replace/augment `adapt::SizeSource`, `SeedPlan`, and `RefinementPlan`; add Eigen-based SPD utilities and serialize six tensor components plus provenance in campaign results.
- *Killer risk:* This is only data plumbing until at least one production mesher can create unit elements in the metric.

**[A2] Vendor Mmg3d as the first production anisotropic remesher** — `anisotropy` · impact H · effort L · risk M
- *What:* Feed the current conforming tet mesh, vertex metric, CAD boundary references, required vertices/edges, and no-swap/no-move tags to `libmmg3d`; use split, collapse, face swap, and relocation until edges are near unit metric length. LGPL Mmg is serial but appropriate for this six-core workstation and much less risky than implementing 3-D local topology operations.
- *Why it wins:* It converts A1 into a mature solve–metric–remesh loop quickly and directly addresses PolyMesh’s largest missing accuracy-per-DOF axis.
- *Prior art:* Mmg’s actual operators and PETSc integration are documented in Wallwork et al.: https://arxiv.org/pdf/2201.02806 ; project: https://github.com/MmgTools/mmg
- *Fit to PolyMesh:* Add a `VolumeMesher::kMetricTet` adapter around `PolyMesh`↔Mmg arrays, preserve OCC entity IDs, project moved boundary vertices, transfer regions/BC sets, and validate determinant/conformity after return.
- *Killer risk:* Mmg is tetrahedra-only and may not preserve PolyMesh selection semantics or exact CAD classification without careful required-entity tagging and post-projection.

**[A3] Do not “support every anisotropic library”; benchmark three adapters first** — `anisotropy` · impact M · effort M · risk L
- *What:* Prototype the same fixed metric/mesh through Mmg3d, Omega_h, and PRAgMaTIc, measuring validity, metric conformity, determinism, boundary drift, runtime, and Windows build friction. Treat Feflo.a/AMG-Lib (academic/non-commercial), AFLR (licensing/integration), and Gmsh BAMG (2-D anisotropy, not the needed 3-D elasticity path) as reference implementations, not initial dependencies.
- *Why it wins:* A two-week bake-off avoids binding product architecture to an attractive library whose geometry/classification behavior fails on STEP parts.
- *Prior art:* Omega_h is BSD, C++17, simplex/metric and GPU-capable: https://github.com/SCOREC/omega_h ; PRAgMaTIc is BSD and metric-simplex based: https://meshadaptation.github.io/doxygen/md_README.html ; AFLR boundary layers: https://www.simcenter.msstate.edu/software/documentation/system/index.html
- *Fit to PolyMesh:* Put adapters behind one `MetricRemesher` interface and add campaign cases, but ship one winner only; do not create three permanent meshing conventions.
- *Killer risk:* The comparison can become infrastructure theater; kill any candidate immediately if it cannot preserve boundary tags deterministically.

**[A4] Elasticity Hessian metric with invariant-safe fusion** — `anisotropy` · impact H · effort L · risk H
- *What:* Recover Hessians for $u_x,u_y,u_z$ and possibly compliance/stress invariants, convert absolute eigensystems to SPD metrics, normalize each to complexity, then intersect them. Cap anisotropy near noisy eigenvalue crossings and rigid-body-like regions.
- *Why it wins:* It aligns fine spacing with directions of rapid solution curvature while remaining coarse along smooth directions—the canonical accuracy-per-DOF mechanism of anisotropic adaptation.
- *Prior art:* Metric construction from absolute Hessian eigenvalues, normalization, intersection, and repeated solves are described in the PETSc–ParMmg paper: https://arxiv.org/pdf/2201.02806
- *Fit to PolyMesh:* Extend the patch-centered SVD in `zz.cpp`; add metric intersection in `adapt`, then send vertex tensors through the Mmg adapter.
- *Killer risk:* There is no unique “Hessian of vector elasticity”; a poor invariant/fusion rule can miss shear or make orientation flicker between iterations.

**[A5] Log-Euclidean interpolation, smoothing, and metric intersection** — `anisotropy` · impact H · effort M · risk L
- *What:* Interpolate $\log M$ rather than six raw tensor entries, limit its spatial gradient, exponentiate back, and combine constraints by simultaneous reduction/intersection of quadratic forms. This preserves SPD and avoids swollen/rotated ellipsoids caused by naive component interpolation.
- *Why it wins:* Stable tensor transport and controlled transitions are prerequisites for anisotropic elements that do not destroy conditioning or trigger endless split-collapse oscillation.
- *Prior art:* Log-Euclidean mesh gradation: https://doi.org/10.2514/1.J059864 ; ellipse/metric intersection: https://doi.org/10.1007/s00366-017-0533-y
- *Fit to PolyMesh:* New small Eigen utility in `adapt`; use it for geometry, Hessian, wall, and user metrics before meshing and emit eigenvalue/gradation diagnostics.
- *Killer risk:* “Intersection” is not associative in every practical construction, so source order can silently break determinism unless specified and tested.

**[A6] Thin-wall/bending metric with principal normal spacing** — `anisotropy` · impact H · effort M · risk M
- *What:* Set one metric eigenvector to the paired-wall normal with spacing thickness/$n$, and tangent eigenvalues from curvature and posterior error; create stretched tets initially, then prefer prisms where offsets remain valid. Also derive a bending metric from recovered strain curvature through thickness after the first solve.
- *Why it wins:* It attacks the exact regime where isotropic volume meshing has the worst cubic DOF explosion.
- *Prior art:* Anisotropic boundary layers are treated as independent in-plane and thickness meshes in Park et al.: https://arxiv.org/pdf/1402.6753
- *Fit to PolyMesh:* Upgrade thickness estimation and `wall_project.cpp`, fuse the tensor in A5, and add metric-quality checks to Mmg/prism outputs.
- *Killer risk:* Highly stretched displacement elements can be poorly conditioned and inaccurate if not aligned extremely well; aspect-ratio caps and scaling are mandatory.

### 3. Polynomial order

**[O1] Make the existing hierarchical HpModel the production model** — `order` · impact H · effort M · risk M
- *What:* Retire selective nodal tet4→tet10/hex8→hex20 promotion as the adaptive path and route production through `HpModel`, `hp_modes()`, and `assemble_hp()`. Keep its hex $p\le6$/tet $p\le4$ hierarchy and shared-entity minimum rule.
- *Why it wins:* It unlocks real variable p without the shipped nonconforming p=1/p=2 interface, and reuses already implemented high-order modes rather than adding another basis.
- *Prior art:* Gates & Bittens define hierarchical entity bases and the minimum rule for nonuniform p: https://arxiv.org/pdf/2012.15581
- *Fit to PolyMesh:* Integrate `src/fea/include/fea/hp_assembly.hpp`, `src/fea/src/hp_assembly.cpp`, and `hierarchical.cpp` into `scene.cpp`; migrate loads, BCs, recovery, result export, and solver selection before deleting the unsafe promotion path.
- *Killer risk:* Production-dead code may pass unit tests yet lack complete traction/body-load, stress recovery, visualization, and conditioning behavior at mixed p.

**[O2] Entity-wise p with an explicit conformity contract** — `order` · impact H · effort M · risk L
- *What:* Store orders on edges, faces, and cell interiors; each shared trace uses the minimum adjacent cell order while surplus modes remain cell-local. Report every p transition and reject any nodal mixed-order path without constraints.
- *Why it wins:* Smooth cell interiors can gain high-order accuracy without forcing every neighbor to the same p, while conformity is guaranteed by construction.
- *Prior art:* The hp-hierarchical FEEC framework gives the hierarchical minimum rule on subsimplices: https://arxiv.org/pdf/2012.15581
- *Fit to PolyMesh:* Generalize `HpElementDef` and existing edge/face slot/orientation maps in `hp_assembly.cpp`; expose entity order maps in diagnostics and campaign JSON.
- *Killer risk:* Orientation/sign mistakes on shared face modes produce subtle patch-test failures rather than clean crashes.

**[O3] Anisotropic tensor-product order $(p_x,p_y,p_z)$ on hexes** — `order` · impact H · effort L · risk M
- *What:* Assign independent modal degree in each local hexahedral direction, with face traces taking directional minima. Couple directions to sweep axes or metric eigenvectors, raising through-thickness p for smooth bending without paying $p^3$ everywhere.
- *Why it wins:* Tensor-product physics is directional; replacing $p^3$ growth by, for example, $p_t^2p_n$ is a direct DOF reduction.
- *Prior art:* Tensor-product high-order FEM enables anisotropic refinement and sum-factorized evaluation: https://doi.org/10.1177/1094342018816368
- *Fit to PolyMesh:* Extend `hp_modes`, mode counts, quadrature, and `map_hex_face_mode`; require a stable local orientation from sweep/frame fields.
- *Killer risk:* Local axes can permute across hex faces, making directional minimum rules and mode mapping very difficult on an unstructured hex mesh.

**[O4] Spectral-coefficient decay decides h versus p** — `order` · impact H · effort M · risk M
- *What:* Estimate solution regularity from the tail decay of hierarchical modal coefficients: exponential decay earns p, slow/nonmonotone decay earns h. Replace `drive_hp`’s hand-set heuristic weights with measured approximation behavior.
- *Why it wins:* p is spectacular in analytic regions and wasteful near singularities; this discriminator directs each refinement type to the regime where it converges.
- *Prior art:* Gates & Bittens develop a problem-independent spectral error/decay indicator specifically for hp choice: https://arxiv.org/pdf/2012.15581
- *Fit to PolyMesh:* Read coefficients from `HpSystem`, add decay fits to `ElementHpSignal`, and retain Dörfler marking for the amount of refinement.
- *Killer risk:* At low p there are too few tail coefficients to infer smoothness reliably, causing early wrong choices to reinforce themselves.

**[O5] Variable order on VEM cells and faces** — `order` · impact M · effort L · risk H
- *What:* Generalize current VEM k=1/2 to per-cell k, with face polynomial moments at the adjacent minimum and static condensation of internal moments. Use higher k only on shape-regular polyhedra whose local projector/stabilization remains well conditioned.
- *Why it wins:* It lets native-poly regions participate in hp adaptation instead of forcing topology changes or globally raising every polyhedron.
- *Prior art:* High-order VEM on polyhedral meshes: https://arxiv.org/pdf/1703.02882
- *Fit to PolyMesh:* Extend `poly_to_vem`, VEM projector/moment assembly, face ownership/order maps, quadrature, and `drive_hp`; add stabilization spectral diagnostics.
- *Killer risk:* High-order monomial moments on distorted 3-D cells become catastrophically ill-conditioned unless bases are scaled/orthogonalized.

**[O6] Spectral/GLL hexes plus static condensation** — `order` · impact M · effort XL · risk M
- *What:* Add Gauss–Lobatto nodal or hierarchical tensor-product hex bases, sum factorization, and condensation of interior DOFs. Use them only on coherent swept blocks, not arbitrary hybrids.
- *Why it wins:* High arithmetic intensity and condensed traces make $p=4$–8 viable where the solution is smooth, improving accuracy per second as well as DOF.
- *Prior art:* Kolev et al., efficient high-order finite elements and partial assembly: https://doi.org/10.1177/10943420211020803
- *Fit to PolyMesh:* New tensor kernel behind `HpModel`, variable quadrature, condensed assembly/solver maps, and prism/pyramid trace compatibility.
- *Killer risk:* PolyMesh’s current direct/assembled Eigen path will erase most of the performance benefit unless solver architecture changes too.

### 4. Element shape / topology

**[T1] Replace the fake pyramid stiffness with a conforming pyramid element** — `topology` · impact H · effort M · risk M
- *What:* Implement a genuine rational-basis pyramid5 (and later higher-order pyramid) whose traces exactly match Q1 hex faces and P1 tet faces. Until then, do not claim that a two-tet stiffness split is a native pyramid element.
- *Why it wins:* Correct pyramids permit compact hex-to-tet transitions without hidden internal diagonals, orientation dependence, or artificial stiffness.
- *Prior art:* Bergot, Cohen & Duruflé, “Higher-order finite elements for hybrid meshes using new nodal pyramidal elements”: https://doi.org/10.1016/j.jcp.2010.06.020
- *Fit to PolyMesh:* Replace pyramid handling in element stiffness/shape evaluation and `pyramid_rule`; verify patch, bending, orientation, and hex–pyramid–tet interface tests.
- *Killer risk:* Rational pyramid bases require careful quadrature and mapping; a superficially passing linear patch test is not enough.

**[T2] Purpose-built hex–prism–pyramid–tet transition complexes** — `topology` · impact H · effort L · risk H
- *What:* Generate template-based transition layers around swept/structured blocks instead of fan-splitting opportunistically. Choose templates by face valence and enforce compatible geometry/order traces.
- *Why it wins:* It preserves high-value aligned hex/prism interiors while localizing tets to genuinely unstructured junctions.
- *Prior art:* Owen et al., pyramid formation for hex-to-tet transitions: https://doi.org/10.1016/S0045-7825(00)00330-3
- *Fit to PolyMesh:* Rework `kHexPyramid`, `kPrismSweep`, and `kHybrid` topology generation; depends on T1 for honest analysis.
- *Killer risk:* The combinatorial case count at CAD junctions explodes, and one invalid template can compromise the whole mesh.

**[T3] Error-driven topology selection, not a global tendency dial** — `topology` · impact H · effort L · risk H
- *What:* Choose tet, swept prism/hex, or VEM poly region-by-region from geometry sweepability, wall pairing, recovered solution anisotropy, and local mesher confidence. Preserve the continuous tendency only as a prior/cost weight, not as the decision itself.
- *Why it wins:* Different topologies win for different mechanisms: prisms for layers, hexes for directional smoothness, tets for robust complex junctions, VEM for agglomerated irregular regions.
- *Prior art:* The large-scale tet/hex study finds no universal hex advantage and emphasizes basis/mesh context: https://doi.org/10.1145/3508372
- *Fit to PolyMesh:* Replace global remapping in `VolumeMesher` selection with a tagged region plan consumed by hybrid generators and scored by testlab Pareto runs.
- *Killer risk:* Interfaces between locally selected families can cost more DOFs and error than the regional gains.

**[T4] Selective CVT/Voronoi VEM only where agglomeration helps** — `topology` · impact M · effort M · risk L
- *What:* Keep clipped Voronoi cells in benign bulky interiors or use them to absorb ugly tet clusters; forbid them near thin layers, sharp traction boundaries, or cells with bad chunkiness. Vary site density and anisotropic distance only after VEM quality is measurable.
- *Why it wins:* Polyhedra can reduce cell count and absorb topology without transition templates, but only if used selectively rather than as a universal “fewer cells” claim.
- *Prior art:* A recent broad comparison concludes polyhedral/VEM benefit is not universal and often matches linear simplices: https://arxiv.org/html/2412.06164
- *Fit to PolyMesh:* Add projector/stabilization condition metrics to `cvt_export.cpp`, `poly_to_vem`, and campaign quality JSON; gate `kCvtPoly` regionally.
- *Killer risk:* Cell count is not DOF count—many-faced VEM cells may have more trace DOFs and denser local work than the tets replaced.

**[T5] Agglomerated non-convex VEM as a repair operator** — `topology` · impact M · effort L · risk H
- *What:* Merge slivers and over-refined tet clusters into star-shaped polyhedra, permitting mild non-convexity only when kernel/chunkiness and projector conditioning pass thresholds. Treat agglomeration as mesh repair/coarsening, not a primary generator.
- *Why it wins:* It can remove pathological tiny elements without retriangulating an entire CAD region and is naturally compatible with VEM.
- *Prior art:* VEM supports arbitrary-order schemes on non-convex/degenerate polytopes under geometric assumptions: https://arxiv.org/pdf/1405.3741
- *Fit to PolyMesh:* Extend `PolyMesh::check_geometry`, face orientation, `poly_to_vem`, and coarsening logic; compute kernel visibility and stabilization spectra.
- *Killer risk:* “General polyhedron” theory still has shape-regularity assumptions; accepting arbitrary non-convex cells produces unstable local projectors.

**[T6] Immersed cut cells for CAD micro-features and late geometry changes** — `topology` · impact M · effort XL · risk H
- *What:* Embed OCC geometry in a Cartesian/octree background, integrate physical portions of cut cells, enforce boundaries weakly, and agglomerate/stabilize tiny cuts. This trades boundary-conforming meshing difficulty for intersection, quadrature, and conditioning difficulty.
- *Why it wins:* It could make geometry resolution independent of mesh topology and enable very fast remeshing after small CAD edits.
- *Prior art:* CutFEM stability/conditioning and remedies: https://arxiv.org/pdf/2208.08538
- *Fit to PolyMesh:* New CAD-cell intersection engine, cut quadrature, Nitsche/ghost penalties, active-cell graph, and solver preconditioner; it is not a small extension of `kOctahedral`.
- *Killer risk:* Tiny-cut conditioning and robust 3-D BREP intersections can consume more engineering than the existing entire meshing stack.

### 5. Alignment / orientation

**[L1] CAD feature-frame alignment** — `alignment` · impact H · effort M · risk M
- *What:* Extract stable local frames from extrusion/revolution axes, ruled-face directions, sharp-edge tangents, wall normals, and curvature directions; align swept blocks and anisotropic metric eigenvectors to them. Use confidence and blend frames in log/rotation space rather than snapping everywhere.
- *Why it wins:* Alignment makes long elements accurate along smooth geometry/solution directions and greatly improves through-thickness efficiency before any solve is available.
- *Prior art:* CubeCover uses a boundary-aligned volumetric frame field to guide parameterization and hex extraction: https://doi.org/10.1111/j.1467-8659.2011.02014.x
- *Fit to PolyMesh:* Extend `CadTopology`/OCC face analysis and `kPrismSweep`; emit frame confidence into metric construction and mesh diagnostics.
- *Killer risk:* Frame directions become singular or ambiguous at junctions, spheres, and blends; forced alignment there creates worse elements.

**[L2] Principal-stress-aligned remeshing after the first solve** — `alignment` · impact M · effort L · risk H
- *What:* Compute a smoothed orthonormal frame from principal stress/strain directions and intersect it with Hessian-derived sizes; remesh iteratively. Disable orientation where eigenvalues are repeated, stresses are tiny, or directions rotate faster than permitted gradation.
- *Why it wins:* In bending and load paths, interpolation error and stiffness response are strongly directional, so edges aligned with principal behavior can resolve it with fewer crosswise cells.
- *Prior art:* Principal-stress initialized quad remeshing is discussed by Schiftner & Balzer and later stress/curvature alignment work: https://www.geometrie.tuwien.ac.at/geom/ig/publications/principalstress/principalstress.pdf
- *Fit to PolyMesh:* Add eigenframe recovery to `zz.cpp`, sign/permutation-consistent frame smoothing, then feed Mmg or sweep orientation.
- *Killer risk:* Principal directions are not defined at equal eigenvalues and can swap discontinuously; naive smoothing creates nonphysical twists.

**[L3] Frame-field-guided hex meshing (CubeCover → HexEx)** — `alignment` · impact M · effort XL · risk H
- *What:* Solve a boundary-aligned 3-D frame field, place admissible singularities, compute an integer-grid parameterization, sanitize it, and extract hexes. HexEx solves extraction robustness, not the hard frame/topology/parameterization stages.
- *Why it wins:* When successful, it creates globally coherent, aligned hex blocks suitable for high anisotropic p and matrix-free tensor kernels.
- *Prior art:* CubeCover: https://doi.org/10.1111/j.1467-8659.2011.02014.x ; HexEx: https://www.graphics.rwth-aachen.de/publication/03260/
- *Fit to PolyMesh:* Entirely new volumetric field/parameterization stack seeded from the current tet mesh; retain hybrid fallback around singularities and failed extraction regions.
- *Killer risk:* Robust automatic all-hex topology remains research-grade; this is a multi-year diversion for one developer.

**[L4] Polycube / MIQ-derived block decomposition** — `alignment` · impact M · effort XL · risk H
- *What:* Segment CAD surfaces, optimize a polycube map or integer-grid surface parameterization, then fill blocks with hexes and local hybrids. Use it only for parts that pass a blockability score and permit user hints.
- *Why it wins:* Block structure gives predictable element orientation, tensor-product order, and inexpensive refinement on extrusion-like mechanical parts.
- *Prior art:* MIQ guarantees quad layouts from an integer-grid map: https://doi.org/10.1145/1399504.1360673 ; HexDom polycube hex-dominant meshing: https://arxiv.org/abs/2103.04183
- *Fit to PolyMesh:* New surface cross-field/segmentation and block graph feeding existing hex/prism/hybrid generators.
- *Killer risk:* Automatic segmentation and singularity placement, not element filling, determine success and routinely need expert intervention.

### 6. Curvature of the geometry map

**[G1] CAD-projected P2/Q2 boundary nodes now** — `geometry map` · impact H · effort M · risk M
- *What:* For tet10/hex20 and hierarchical $p\ge2$, create mid-edge and face geometry nodes by evaluating the owning OCC curve/surface, not by averaging corners; reconcile edge-on-face ownership at seams. Keep solution and geometry order separate.
- *Why it wins:* It removes the current planar-facet geometry floor, especially on holes/fillets where normals, areas, tractions, and stress concentration are boundary-sensitive.
- *Prior art:* Hughes et al. show fixed polynomial geometry can cap convergence as solution p rises: https://doi.org/10.1016/j.cma.2004.10.008
- *Fit to PolyMesh:* Extend boundary ownership from `CadTopology`, `surface_project.cpp`, element mapping/Jacobians, traction integration, and visualization export.
- *Killer risk:* Blind projection can invert curved elements or place shared nodes inconsistently at trims and seams.

**[G2] Variable geometry order $q$ independent of solution order p** — `geometry map` · impact H · effort L · risk M
- *What:* Choose $q$ from CAD curvature and geometric error while choosing p from solution regularity; permit superparametric $q>p$ near Neumann curved boundaries and $q=1$ in flat interiors. Validate Jacobian positivity at more points than the analysis quadrature.
- *Why it wins:* It buys accurate normals and boundary integrals without adding displacement DOFs, preventing geometry error from wasting high p.
- *Prior art:* Superparametric geometry can restore accuracy on curved domains, especially Neumann boundaries: https://arxiv.org/pdf/2304.13766
- *Fit to PolyMesh:* Add geometry-node/order storage to `NodalMesh`/`HpModel`, variable mapping evaluation, and a CAD deviation estimator in pipeline results.
- *Killer risk:* Curved high-order validity is harder than straight-element validity; positive corner Jacobians prove little.

**[G3] NURBS-enhanced boundary FEM (NEFEM flavor)** — `geometry map` · impact M · effort XL · risk H
- *What:* Keep ordinary volume shape functions but evaluate boundary geometry and boundary integrals directly on OCC/NURBS parameterizations, avoiding a polynomial approximation on boundary faces. This is a narrower bridge than full IGA.
- *Why it wins:* It can eliminate CAD boundary error while preserving the existing volume mesh, algebra, and DOF topology.
- *Prior art:* NURBS-enhanced FEM/IGA rationale and exact geometry: Hughes et al.: https://doi.org/10.1016/j.cma.2004.10.008
- *Fit to PolyMesh:* OCC-parametric boundary-face maps, inverse maps, rational Jacobians/normals, curved-face quadrature, and load/BC integration.
- *Killer risk:* Trimmed STEP faces are not simple tensor-product NURBS patches; robust inverse mapping and watertight patch coupling dominate the work.

**[G4] Full isogeometric analysis on selected CAD patches** — `geometry map` · impact M · effort XL · risk H
- *What:* Analyze spline volumes/patches with NURBS/B-spline basis, exact geometry, and high continuity; couple them to ordinary FEM regions with mortar/Nitsche. Restrict to CAD models with analysis-suitable parameterizations.
- *Why it wins:* Exact geometry and high continuity can be exceptionally DOF-efficient for smooth bending-dominated structures.
- *Prior art:* Hughes, Cottrell & Bazilevs, foundational IGA paper: https://doi.org/10.1016/j.cma.2004.10.008
- *Fit to PolyMesh:* New spline-volume construction, extraction operators, patch coupling, quadrature, solver/preconditioner, and OCC topology mapping.
- *Killer risk:* STEP BREP provides trimmed surface patches, not analysis-suitable volumetric splines; parameterization is the unsolved product problem.

### 7. Quadrature

**[Q1] Per-element quadrature order from $(p,q)$ and integrand degree** — `quadrature` · impact H · effort S · risk L
- *What:* Select rules from solution order p, geometry order q, element family, material variation, and load variation instead of `default_rule()`. Cache rules and allow a verification mode that compares with the next higher rule.
- *Why it wins:* It prevents underintegration from silently invalidating high-order work while avoiding excessive points on affine low-order elements.
- *Prior art:* High-order hierarchical implementations exploit order-aware simplex quadrature: https://arxiv.org/pdf/2012.15581
- *Fit to PolyMesh:* Generalize `src/fea/src/quadrature.cpp`, `quadrature.hpp`, stiffness/body/traction assembly, and VEM projector integration.
- *Killer risk:* Exact polynomial-degree reasoning fails for rational pyramid/NURBS maps, so a hardcoded formula can still underintegrate.

**[Q2] Embedded adaptive quadrature as a diagnostic fallback** — `quadrature` · impact M · effort M · risk L
- *What:* Compare nested/adjacent rules and recursively subdivide only elements whose stiffness/load integrals disagree beyond tolerance; record quadrature error separately from discretization error. Use this for curved, distorted, or spatially varying integrands, not every straight element.
- *Why it wins:* It catches integration error floors without permanently multiplying assembly cost.
- *Prior art:* Adaptive integration of cut/finite cells: https://doi.org/10.1007/978-3-030-37518-8_2
- *Fit to PolyMesh:* Add an integration-error channel in element assembly and campaign `quality.json`; cache accepted rules by element/map signature.
- *Killer risk:* Matrix-entry convergence is expensive and does not directly guarantee the final QoI; unrestricted recursion can dominate runtime.

**[Q3] Moment-fitting quadrature for cut/polyhedral cells** — `quadrature` · impact M · effort L · risk H
- *What:* Fit weights to exact/robustly computed geometric moments, enforce nonnegative weights where possible, and fall back to tetrahedral subdivision when conditioning is poor. Vary moment degree with VEM/order needs.
- *Why it wins:* It uses far fewer points than octree subdivision while exactly integrating the polynomial moments that projectors require.
- *Prior art:* Non-negative moment fitting for cut cells: https://doi.org/10.1007/s00466-022-02203-9
- *Fit to PolyMesh:* Add robust polyhedral moments to `poly_to_vem`/cut-cell work and replace indiscriminate tet subdivision for integration.
- *Killer risk:* Moment systems on skinny/non-convex cells are ill-conditioned and positive weights may not exist at the requested degree.

**[Q4] Selective reduced integration with measured hourglass energy** — `quadrature` · impact L · effort M · risk H
- *What:* Permit reduced integration only for specifically validated hex formulations and add consistent hourglass stabilization plus an energy-ratio diagnostic. Do not apply it to the current broad hybrid/VEM stack.
- *Why it wins:* It may reduce assembly cost and some locking in nearly incompressible/bending cases, but it is not a general accuracy feature.
- *Prior art:* Flanagan & Belytschko, “A uniform strain hexahedron and quadrilateral with orthogonal hourglass control”: https://doi.org/10.1002/nme.1620170504
- *Fit to PolyMesh:* New hex kernel and result diagnostics; requires material/locking test coverage beyond the present isotropic benchmark set.
- *Killer risk:* Spurious zero-energy modes can yield plausible displacements and wrong stresses; this is easy to misuse.

### 8. Continuity / conformity

**[C1] General hanging-node constraint graph** — `continuity` · impact H · effort L · risk M
- *What:* Allow 1-irregular local h refinement and express slave vertex/edge/face modes as interpolation of coarse-master trace modes; eliminate constraints during assembly or apply $C^TAC$. Combine with entity-wise p constraints in one acyclic graph.
- *Why it wins:* It avoids propagation of refinement solely to maintain conformity and makes local hp refinement genuinely local.
- *Prior art:* Bangerth & Kim describe hanging-node constraints and why they preserve conforming subspaces: https://doi.org/10.1137/16M1071432
- *Fit to PolyMesh:* New constraint map in `HpModel`/`HpSystem`, modify assembly/BC/recovery, and add octree/Rivara interfaces that retain hanging entities rather than closing the mesh globally.
- *Killer risk:* Cycles and inconsistent edge/face orientations in combined h+p constraints produce rank deficiency.

**[C2] Mortar coupling between independently meshed regions** — `continuity` · impact M · effort XL · risk H
- *What:* Weakly tie nonmatching traces with dual Lagrange multipliers, especially at structured thin-wall blocks, imported feature substructures, or CAD partitions. Permit different element family, h, and p on each side.
- *Why it wins:* Each region can use its optimal mesh without transition-element combinatorics or refinement propagation.
- *Prior art:* Wohlmuth, mortar finite elements for interface problems: https://doi.org/10.1007/s00607-003-0062-y
- *Fit to PolyMesh:* Surface intersection/overlap mesh, multiplier spaces, saddle-point or condensed coupling, interface integration, and new preconditioning.
- *Killer risk:* Robust 3-D overlap integration and inf-sup-stable multiplier choices are harder than generating many conforming transitions.

**[C3] Nitsche coupling for nonmatching FEM/IGA/cut regions** — `continuity` · impact M · effort L · risk H
- *What:* Enforce displacement continuity and traction balance weakly with consistent flux and penalty terms; vary penalty using local h, p, and material stiffness. Prefer it over mortar when avoiding multiplier DOFs matters.
- *Why it wins:* It enables clean regional discretization changes and immersed boundaries without explicit transition cells.
- *Prior art:* Hansbo & Hansbo-style Nitsche interface CutFEM and multipatch coupling: https://arxiv.org/pdf/1703.07077
- *Fit to PolyMesh:* New interface facet assembly, trace evaluation for every element family, penalty scaling, and coercivity diagnostics.
- *Killer risk:* A wrong penalty is either unstable or badly conditioned, and heterogeneous p/topology makes “right” scaling nontrivial.

**[C4] Local DG only across pathological interfaces** — `continuity` · impact L · effort XL · risk H
- *What:* Permit discontinuous displacement traces and interior-penalty coupling in a narrow interface band where conformity is too expensive. Do not convert the whole elasticity solver to DG.
- *Why it wins:* It can isolate remeshed, nonmatching, or non-convex regions while leaving the efficient conforming bulk unchanged.
- *Prior art:* hp-DG for nearly incompressible elasticity: https://doi.org/10.1016/j.cma.2005.10.012
- *Fit to PolyMesh:* Facet DOF duplication, jumps/averages, penalty terms, error estimator, and a stronger preconditioner.
- *Killer risk:* DOF and matrix-connectivity growth overwhelms any mesh flexibility for this workstation-scale linear solver.

### 9. Solve/time-aware variation

**[V1] Condition-aware metric and element-quality objective** — `solve-aware` · impact H · effort M · risk M
- *What:* Optimize both interpolation-in-metric quality and a conditioning proxy: Jacobian singular values, minimum metric altitude, local stiffness spectrum, and anisotropy alignment. Relax requested anisotropy when its predicted solver cost exceeds its error benefit.
- *Why it wins:* The theoretically lowest-DOF mesh is useless if CG iterations explode; this optimizes accuracy per second rather than geometry alone.
- *Prior art:* Huang, “A study on conditioning ... arbitrary anisotropic meshes”: https://arxiv.org/pdf/1302.6868
- *Fit to PolyMesh:* Extend mesh quality JSON, metric clamps, Mmg callbacks/postchecks, and campaign Pareto objectives with iteration count and condition estimates.
- *Killer risk:* Cheap local proxies may correlate poorly with the globally preconditioned system.

**[V2] Static condensation as a variable per element family/order** — `solve-aware` · impact H · effort L · risk M
- *What:* Condense cell-interior hp/VEM modes locally while retaining shared trace modes; choose condensation when dense local factorization plus a smaller global system wins. Cache factorizations for recovery.
- *Why it wins:* Variable p otherwise bloats the global Eigen system; condensation makes high interior order affordable and reduces CG/direct memory.
- *Prior art:* High-order finite element partial assembly/condensation context: https://doi.org/10.1177/10943420211020803
- *Fit to PolyMesh:* Split `HpSystem` local/global maps, modify `assemble_hp`, solver/recovery, and cost model in `drive_hp`.
- *Killer risk:* Condensed trace systems can still be ill-conditioned, and local dense work is expensive on irregular high-face-count VEM cells.

**[V3] Matrix-free eligibility as a mesh label** — `solve-aware` · impact M · effort XL · risk M
- *What:* Mark coherent affine/curved tensor-product regions for sum-factorized matrix-free action while assembling irregular tet/pyramid/VEM regions. Let topology/order selection account for whether an element lands on the fast path.
- *Why it wins:* It turns aligned high-order hexes from a memory liability into a compute-dense kernel suited to the RTX 2070/CPU cache hierarchy.
- *Prior art:* Matrix-free higher-order FEM reduces memory traffic by reevaluating integrals: https://arxiv.org/abs/2408.12479
- *Fit to PolyMesh:* Operator interface replacing “matrix always exists,” mixed assembled/matrix-free CG, p-multigrid or low-order preconditioner, CUDA tensor kernels.
- *Killer risk:* A small or fragmented eligible region will not amortize complexity and mixed preconditioning.

**[V4] AMG-aggregate-aware coarsening and graph ordering** — `solve-aware` · impact M · effort L · risk M
- *What:* Add an AMG preconditioner first, then preserve strong mechanical connections and low-diameter aggregates during mesh coarsening; separately use RCM/AMD ordering for direct solves. Treat partitioning as cache locality on one workstation, not MPI theater.
- *Why it wins:* Current incomplete-Cholesky CG is the ceiling for high-resolution anisotropic/hp meshes; a mesh that coarsens geometrically gives AMG a much better hierarchy.
- *Prior art:* Vaněk, Mandel & Brezina, aggregation-based AMG: https://doi.org/10.1137/S1064827594276611
- *Fit to PolyMesh:* Add AMG dependency/solver path, expose mesh adjacency and near-nullspace rigid modes, and feed measured iterations into campaign ranking.
- *Killer risk:* Designing the mesh around one preconditioner can reduce approximation quality, and elasticity AMG needs correct rigid-body near-nullspace handling.

### 10. Additional axes worth varying

**[X1] Exact/condensed recurring CAD feature operators** — `feature treatment` · impact H · effort XL · risk H
- *What:* Recognize parameterized holes, fillets, bosses, ribs, bolt patterns, and thin ligaments; replace a local volume mesh by a precomputed boundary-to-boundary Schur complement or analytic enrichment, with certified parameter/interpolation bounds. This operationalizes the owner’s “pre-solved feature” idea without pretending a finite library is universally zero-error.
- *Why it wins:* Repeated features consume enormous DOFs to reproduce nearly the same local response; condensation pays that solve once and exposes only boundary modes globally.
- *Prior art:* Component-mode synthesis is adjacent but not a CAD-feature, zero-discretization-error library: Craig & Bampton, https://doi.org/10.2514/3.3019 . `NONE FOUND — original proposal` for certified parameterized CAD-feature operators inserted automatically into arbitrary conforming volume FEA.
- *Fit to PolyMesh:* OCC feature recognition in `geom`, a feature cache keyed by nondimensional parameters/material, mortar/Nitsche or exact trace coupling, offline campaign generation, and online error certification.
- *Killer risk:* Boundary conditions and neighboring geometry destroy feature locality; certification over shape/material/load parameter space is the real research problem.

**[X2] Partition-of-unity singular enrichments** — `basis/enrichment` · impact H · effort L · risk H
- *What:* Add crack-tip, re-entrant-corner, point-load, and hole asymptotic functions locally while leaving the mesh coarse; activate enrichments by CAD/BC feature recognition. Orthogonalize and condense enrichment DOFs locally.
- *Why it wins:* It attacks known non-polynomial singular behavior directly, where both h and p convergence are slow.
- *Prior art:* Babuška & Melenk, partition of unity FEM: https://doi.org/10.1002/(SICI)1097-0207(19970228)40:4%3C727::AID-NME86%3E3.0.CO;2-N
- *Fit to PolyMesh:* Extend basis evaluation/assembly in `src/fea`, tag features in `CadTopology`, and teach ZZ/DWR not to interpret represented singular modes as unresolved error.
- *Killer risk:* Wrong or overlapping enrichments make the system nearly linearly dependent and worsen conditioning.

**[X3] Mixed-dimensional shell/beam/solid adaptivity** — `model dimension` · impact H · effort XL · risk H
- *What:* Replace sufficiently thin, regular regions by shell midsurfaces and slender members by beam centerlines, retaining 3-D solids near joints/load introductions and coupling models variationally. Vary model dimension as another adaptivity decision with an estimated modeling error.
- *Why it wins:* Removing an unnecessary thickness dimension beats any 3-D mesh optimization in DOF efficiency.
- *Prior art:* Mixed-dimensional beam/shell/solid coupling via Nitsche/mortar is established; one representative Nitsche model-coupling paper: https://doi.org/10.1016/j.cma.2022.114562
- *Fit to PolyMesh:* Robust OCC midsurface/centerline extraction, shell/beam elements, coupling interfaces, model-error estimator, loads/BC mapping, and result reconciliation.
- *Killer risk:* Automatic midsurface extraction and valid 3-D joint coupling are full product lines, not mesher tweaks.

**[X4] QoI portfolio / Pareto mesh instead of one “optimal” mesh** — `objective variation` · impact M · effort M · risk L
- *What:* Build metrics for several QoIs and loads, then intersect them under a shared complexity budget or generate a Pareto set of meshes. Let users choose compliance-safe, peak-stress-safe, or balanced meshes with explicit estimated errors.
- *Why it wins:* A mesh optimized for one load/QoI can be dangerously poor for another; portfolio optimization avoids uniform worst-case refinement.
- *Prior art:* Metric intersection and goal-oriented adaptation are established separately: https://arxiv.org/pdf/2201.02806 and https://doi.org/10.5281/zenodo.3653101
- *Fit to PolyMesh:* Reuse campaign runner/load cases, DWR metrics, A5 intersection, and `scripts/analyze_campaign.py` for error-cost fronts.
- *Killer risk:* Intersecting many objectives trends toward the globally finest mesh unless budgets and priorities are explicit.

**[X5] Learned knob policy with a hard physics/verifier cage** — `adaptation policy` · impact M · effort M · risk M
- *What:* Use testlab-generated labels to predict initial mesher, topology regions, metric complexity, layer count, p cap, and likely failure; every prediction still passes deterministic geometry checks and posterior error loops. Learn cost/error priors, never stresses or benchmark answers.
- *Why it wins:* It can skip many expensive failed candidate meshes and warm-start the genuinely physics-based adaptive loop.
- *Prior art:* `NONE FOUND — original proposal` for a single-workstation policy jointly choosing PolyMesh’s h/p/topology/geometry/quadrature/solver knobs under deterministic verification.
- *Fit to PolyMesh:* Extend `apps/testlab`, results JSONL, and `analyze_campaign.py`; small CPU inference model, with product code consuming only bounded knob suggestions.
- *Killer risk:* Training distribution shift across CAD families makes confident bad choices; the verifier must be able to override everything.

**[X6] Mesh-change economics and hysteresis** — `adaptation dynamics` · impact M · effort M · risk L
- *What:* Penalize split-collapse thrashing, p up/down oscillation, topology churn, and solution-transfer error in the adapt objective; require predicted error benefit to exceed remesh/transfer cost with hysteresis. Track element lineage and metric/order changes across iterations.
- *Why it wins:* Accuracy per second includes adaptation and repeated factorization, not only final DOFs; stable changes converge faster and preserve deterministic behavior.
- *Prior art:* `NONE FOUND — original proposal` as a unified economic controller for h/p/topology/geometry-order changes and transfer error in this setting.
- *Fit to PolyMesh:* Add lineage IDs, transfer-error estimates, and action costs to `drive_hp`; use campaign timing to calibrate but not hardcode decisions.
- *Killer risk:* Conservative hysteresis can freeze an initially bad mesh and hide estimator improvements.

## Top five for PolyMesh

### 1. A2 + A1 + A4: Mmg3d-backed solution-driven metric adaptation
Implement one `MetricField` contract first (SPD enforcement, eigenvalue/aspect clamps, log interpolation, complexity normalization), then one Mmg3d adapter. Start from an ordinary conforming tet mesh; run solve → recover displacement-component Hessians/ZZ signal → construct/intersect metric → Mmg remesh → reproject/tag OCC boundary → transfer state → repeat two or three times. Gate shipment on metric-edge histograms, CAD Hausdorff/normal error, tag preservation, positive Jacobians, repeatability, and error-versus-DOF campaigns. Do not first build a native anisotropic hybrid mesher or support three libraries.

### 2. O1 + O2: productionize the existing hierarchical HpModel
Inventory every production operation that currently assumes `NodalMesh`: constraints and loads, body/traction quadrature, solve, stress recovery, visualization, reference comparison, and adaptive signaling. Migrate them as a clean cutover to `HpModel`, preserve the shared-entity minimum rule, add entity-order diagnostics, then remove selective unconstrained tet10/hex20 promotion. Use coefficient-decay regularity only after the path passes mixed-p patch tests and end-to-end benchmark solves; correctness precedes clever p selection.

### 3. G1 + G2: independently variable curved CAD geometry order
Attach OCC curve/face ownership and parametric coordinates to boundary entities, then generate projected q=2 edge/face geometry nodes with seam reconciliation. Make element maps use q independently from solution p, validate Jacobians densely, integrate tractions on the curved map, and report chordal/normal error. This is much smaller than IGA and removes the geometry floor that otherwise makes high p look ineffective on holes and fillets.

### 4. S5 + S6 + A6: feature-size-aware thin-wall metric and selective prism layers
Upgrade wall rays into paired-face regions and derive a local normal/tangent frame, through-thickness count, medial/protecting-ball cap, and offset validity confidence. Feed a tensor field to Mmg where layers fail, and generate prisms only on offset regions that remain collision-free; transition through genuine pyramids/tets. Benchmark on plates, ribs, fillets, and wall junctions with bending energy/stress error, not cell count.

### 5. S2 + O4: goal-oriented hp decisions
Add one regularized QoI (compliance is the easiest), solve the symmetric adjoint using the existing stiffness factorization/preconditioner, and compute dual-weighted element signals. Within marked cells, use hierarchical coefficient decay to choose p for smooth error and h for singular error; use metric anisotropy where directional Hessian evidence is strong. This turns `drive_hp` from hand-weighted heuristics into a measurable “which error matters, and is it smooth?” policy.

## Single highest-leverage missing capability

**A solution-driven anisotropic Riemannian metric field connected to a mature 3-D metric remesher (Mmg3d) is the highest-leverage missing capability for accuracy per DOF.** It adds three freedoms at once—local size, aspect ratio, and orientation—where PolyMesh currently has only scalar isotropic h. The strongest directly observed literature evidence is Wallwork et al.’s PETSc–ParMmg experiment: Hessian metrics recovered in a repeated solve/adapt loop produced highly multiscale, anisotropic tetrahedral meshes; for a sharp spatially varying solution, the adapted sequence retained the expected $L^2$ convergence rate while uniform refinement did not, with element volumes spanning eight orders of magnitude: https://arxiv.org/pdf/2201.02806. That is not proof of the same gain on PolyMesh elasticity, so the claim must be validated on thin-wall bending, holes/fillets, and localized loads in testlab. The immediate correctness prerequisite is separate: disable or replace the current unconstrained nodal mixed-p path, because a nonconforming p interface can invalidate any apparent accuracy gain.

## Do not bother (now)

- **Implementing a native 3-D anisotropic remesher from scratch:** Mmg/Omega_h/Pragmatic already embody years of split-collapse-swap-relocate robustness; write an adapter and spend effort on CAD tags, metric construction, and validation.
- **Full automatic all-hex CubeCover/HexEx/polycube meshing:** XL research risk, difficult singularity/topology decisions, and modern tet/hex evidence does not support a universal hex accuracy advantage. Revisit only after metric tets and swept blocks plateau.
- **Full IGA from arbitrary STEP:** trimmed surface BREP is not an analysis-suitable spline volume. Projected q=2/q=3 geometry or boundary-only NURBS treatment captures much of the value first.
- **Global DG:** it multiplies trace DOFs and solver difficulty to solve conformity problems that hierarchical minimum rules, hanging constraints, and occasional Nitsche interfaces solve more cheaply.
- **Reduced integration as a speed feature:** underintegration/hourglass failure is too easy to hide in linear elasticity; only revisit for a validated locking problem with energy diagnostics.
- **Octahedra as an accuracy strategy:** no identified elasticity mechanism makes them systematically superior; keep only if campaign evidence beats tet/hex/poly alternatives at equal DOF and time.
- **Unrestricted non-convex polyhedra:** VEM is not magic—shape regularity and projector conditioning still matter. Use controlled agglomeration as repair, never “accept anything.”
- **AMG-aware mesh partitioning before adding AMG:** on one workstation this is premature. First add a competent elasticity AMG path and measure it; ordering and cache locality are enough meanwhile.
- **Supporting Mmg, Omega_h, Pragmatic, Feflo.a, AFLR, and Gmsh permanently:** benchmark adapters, ship one. Feflo licensing/access, AFLR integration, and Gmsh’s BAMG 2-D focus make them poor first product dependencies.
- **A learned mesher replacing error estimation:** learned policy is useful only as a warm start under deterministic geometry and posterior-error checks; otherwise it conflicts with PolyMesh’s evidence-first constraint.
