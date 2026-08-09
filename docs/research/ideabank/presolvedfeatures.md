<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Pre-solved / analytically exact treatment of recurring CAD features

# Recurring-feature exactness: ruthless assessment

The owner's intuition is sound, but **“pre-solved” has four very different meanings** that must not be conflated:

1. **Zero added error:** exact symmetry reduction and exact Schur-complement condensation can preserve the original problem/FE system exactly.
2. **Zero geometric error:** IGA can preserve an untrimmed NURBS geometry exactly, but that says nothing about PDE discretization error.
3. **Exact representation of one mode:** GFEM/XFEM can exactly span a known singular/analytic mode, but the complete finite-body solution is almost never just that mode.
4. **Certified nonzero error:** analysis-aware defeaturing and certified reduced bases can deliberately approximate while bounding the added error. For general industrial STEP parts, this is the most useful interpretation.

The idea bank below is ranked in descending practical value for PolyMesh. Impact is potential if it works; risk reflects the chance that the required guarantee or speedup does not materialize.

## Ranked idea bank

**[DF-01] Certified adaptive Neumann defeaturing** — `certified-defeaturing` · impact H · effort L · risk M
- *What:* Detect small traction-free holes, fillets, bosses, grooves, and protrusions; remove them; solve the simplified part; then estimate the **geometry-induced energy error** and restore only features whose contribution exceeds tolerance. Start with feature boundaries carrying homogeneous Neumann data and no intersecting load/constraint selection, where the theory is strongest.
- *Why it wins:* It removes exactly the small CAD details that force tiny elements and bad aspect ratios, while replacing “engineer judgment” with a reliable bound. Antolin–Chanon report a cheap estimator plus an iterative restore strategy for multiple features and linear elasticity, which maps almost perfectly to PolyMesh's solve–estimate–adapt loop.
- *Prior art:* Antolin & Chanon, *Analysis-aware defeaturing of complex geometries with Neumann features* ([arXiv](https://arxiv.org/abs/2212.03141)); Buffa–Chanon–Vázquez, *Analysis-aware defeaturing* ([DOI](https://doi.org/10.1142/S0218202522500099)); equilibrated-flux strengthening ([DOI](https://doi.org/10.1137/23M1627195)).
- *Fit to PolyMesh:* Extend `geom::CadTopology` in `src/geom/include/geom/cad_topology.hpp` / `src/geom/src/cad_topology.cpp`; suppress selected faces through OCC in `src/geom/src/cad_model.cpp`; orchestrate restore iterations in `src/pipeline/src/scene.cpp`; add a certified estimator beside `src/adapt/src/error.cpp` rather than treating `src/fea/src/zz.cpp` as a certificate. `apps/testlab/main.cpp` should report numerical and defeaturing error separately.
- *Killer risk:* A genuinely equilibrated elasticity estimator and valid extension into the removed feature are much harder than the current non-guaranteed ZZ recovery.

**[SYM-01] Exact mirror and cyclic sector solves** — `symmetry` · impact H · effort M · risk L
- *What:* Detect reflectional or order-$n$ rotational symmetry in the B-rep, then verify that material, body loads, tractions, and displacement constraints are equivariant under the same transformation. Solve one half/sector with exact mirror conditions or rotated periodic ties and expand the result.
- *Why it wins:* Under group invariance this introduces **zero continuum-model and zero discrete added error**, while cutting mesh, assembly, and solve size by roughly the symmetry order. Unlike a surrogate, it needs no training and remains exact for every mesh order.
- *Prior art:* B-rep candidate generation and exact/partial symmetry verification are described in *A Framework for Detection of Exact Global and Partial Symmetry in 3D CAD Models* ([DOI](https://doi.org/10.3390/sym15051058)); multi-scale congruence-labelled adjacency graphs are described by Li et al. ([paper](https://cad-journal.net/files/vol_16/CAD_16(1)_2019_50-66.pdf)).
- *Fit to PolyMesh:* Add transformation-aware face matching to `cad_topology.cpp`; clip/sector the `CadModel` in `cad_model.cpp`; add rotated multi-point constraints in `src/fea/src/solve.cpp`; transform selection boxes and loads in `src/fea/src/traction.cpp`; expose verification failures through `pipeline::Model` in `src/pipeline/include/pipeline/scene.hpp`.
- *Killer risk:* Geometry symmetry without **BC/load symmetry** is useless, and silently assuming the latter would produce a convincingly wrong answer.

**[SC-01] Exact repeated-feature static macroelements** — `static-condensation` · impact H · effort L · risk M
- *What:* For an exactly repeated fixed-geometry feature plus a standard “collar” interface, premesh it finely once and eliminate every interior DOF by a Schur complement. Reuse the resulting boundary stiffness, condensed load vector, and recovery operator at every congruent instance.
- *Why it wins:* For PolyMesh's linear statics, full interior condensation is algebraically exact relative to the fine FE discretization; only interface DOFs enter the global solve. A flange with 24 identical bolt-seat sectors can therefore pay the fine local analysis cost once rather than 24 times.
- *Prior art:* Static-condensation reduced-basis element methods explicitly use libraries of interoperable component archetypes and ports ([Eftang & Patera, 2014](https://link.springer.com/article/10.1186/2213-7467-1-3)); commercial superelements are the classical engineering form.
- *Fit to PolyMesh:* Add a macroelement branch to `src/fea/src/assembly.cpp`; use `CadTopology` transforms to establish congruent instances; define interface node ordering in `src/mesh/src/poly_mesh.cpp`; persist matrices keyed by geometry/material/interface signatures; recover internal stresses through `src/fea/src/stress.cpp`.
- *Killer risk:* Reuse stops being exact as soon as the collar geometry, material, interface discretization, or interior loading differs.

**[FR-01] Verified hybrid B-rep feature recognizer** — `feature-recognition` · impact H · effort M · risk M
- *What:* Build an attributed face-adjacency graph using exact OCC topology and analytic-surface parameters, run deterministic rules for cylinders/cones/planes and convexity, then geometrically verify every proposed hole/fillet/chamfer/rib/boss. A learned model may propose ambiguous face groups, but it must never authorize a physics substitution without deterministic reconstruction and tolerance checks.
- *Why it wins:* Every other feature treatment needs reliable feature boundaries, parameters, transforms, and provenance. Rule-first recognition is deterministic and debuggable; ML is useful only for resolving interacting features whose clean graph signature has been destroyed.
- *Prior art:* Hierarchical CADNet's B-rep hierarchy and MFCAD++ results ([DOI](https://doi.org/10.1016/j.cad.2022.103226)); BRepNet's coedge message passing ([arXiv](https://arxiv.org/abs/2104.00706)); OCCT canonical recognition API ([official docs](https://dev.opencascade.org/doc/refman/html/class_shape_analysis___canonical_recognition.html)).
- *Fit to PolyMesh:* Enrich `CadFace`/`CadEdge` in `cad_topology.hpp` with canonical geometry, signed dihedral, loops, and adjacency; create verified `FeatureInstance` records during `extract_topology()`; retain source-face maps across `CadModel` defeaturing; test determinism and tolerance perturbations in geometry tests.
- *Killer risk:* Interacting imported features and tolerance-damaged STEP topology can make semantic decomposition non-unique.

**[SB-01] Singular SBFEM feature cells** — `sbfem` · impact H · effort L · risk M
- *What:* Place one star-convex scaled-boundary polyhedron around a crack tip, sharp re-entrant notch, or material corner and solve its radial coordinate semi-analytically while discretizing only its boundary. Couple its boundary DOFs conformingly to surrounding ordinary FE/VEM cells.
- *Why it wins:* SBFEM eigenvalues reproduce singular radial powers without a radial mesh, so accuracy near a singularity no longer requires many collapsing tetrahedral layers. This is much more general than hard-coding a Williams exponent and is naturally compatible with PolyMesh's polyhedra.
- *Prior art:* Song et al., *Stress analysis of 3D complex geometries using scaled boundary polyhedral finite elements* ([DOI](https://doi.org/10.1007/s00466-016-1312-0)); Liu et al., automatic polyhedral SBFEM meshing ([DOI](https://doi.org/10.1016/j.cma.2016.09.038)).
- *Fit to PolyMesh:* Add an SBFEM element formulation beside `src/fea/src/vem.cpp` / `src/fea/include/fea/vem.hpp`; select eligible star-convex cells in `pipeline::volume_mesh()` in `scene.cpp`; reuse polyhedral face topology from `mesh::PolyMesh`; add crack/notch eigenvalue and patch tests beside `tests/test_l_domain.cpp` and `tests/test_kirsch_plate.cpp`.
- *Killer risk:* Robustly selecting a scaling center and solving the nonsymmetric/complex local eigenproblem for arbitrary ugly cells.

**[RB-01] Certified parametric port-reduced feature library** — `parametric-rom` · impact H · effort XL · risk H
- *What:* Define archetypes such as “through-hole plus collar,” “rib junction,” and “bearing seat” with nondimensional geometry/material parameters; train interior and port bases offline; instantiate and connect them online. Carry a residual/coercivity-based a posteriori bound relative to the high-fidelity FE model.
- *Why it wins:* Unlike interpolation of stiffness entries, SCRBE reduces both feature interiors and interfaces while retaining a computable error indicator. It is the clearest prior-art answer to “does a parameterized library of condensed features exist?”—**yes**, though not as an automatic arbitrary-STEP recognizer.
- *Prior art:* Eftang & Patera, *A port-reduced static condensation reduced basis element method...* ([open article](https://link.springer.com/article/10.1186/2213-7467-1-3)); Huynh–Knezevic–Patera SCRBE ([DOI](https://doi.org/10.1016/j.cma.2013.02.013)).
- *Fit to PolyMesh:* Use the working solver and `apps/testlab` to generate snapshots; store archetype metadata and affine operators; insert reduced ports through `assembly.cpp`; route out-of-training parameters back to ordinary meshing; analyze accuracy/runtime Pareto fronts with `scripts/analyze_campaign.py`.
- *Killer risk:* Geometry parameterization and port compatibility, not POD itself, dominate the engineering effort.

**[DF-02] Certified goal-oriented defeaturing** — `certified-defeaturing` · impact H · effort L · risk M
- *What:* Bound the error in a requested linear quantity—compliance, displacement at a probe, averaged stress, reaction—instead of forcing every removed microfeature to satisfy a global energy tolerance. Use a dual solve to weight each feature's residual by relevance to that output.
- *Why it wins:* A tiny remote hole can create a local energy perturbation yet have negligible effect on the design quantity; goal certification permits much more aggressive simplification. It aligns directly with testlab's reference metrics and Pareto selection.
- *Prior art:* Weder & Buffa, *A Certified Goal-Oriented A Posteriori Defeaturing Error Estimator for Elliptic PDEs*—including linear elasticity, multiple features, and DWR ([arXiv](https://arxiv.org/abs/2512.20124)).
- *Fit to PolyMesh:* Add adjoint load construction to `src/fea/src/solve.cpp`; define QoIs in `src/bench/include/bench/reference_case.hpp`; accumulate per-feature dual-weighted terms in `src/adapt/src/error.cpp`; record the certificate in `result.json`/`results.jsonl` via `apps/testlab/main.cpp`.
- *Killer risk:* Peak point stress is not a stable linear functional, so the most tempting user QoI may not admit the promised bound without regularization.

**[FR-02] Exact congruent-pattern extraction** — `feature-recognition` · impact H · effort L · risk M
- *What:* After individual feature recognition, canonicalize each feature's face subgraph and metric parameters, then cluster instances related by rigid transforms into linear, circular, mirror, or lattice patterns. Verify congruence by transformed analytic surfaces/curves and topology, not tessellation hashes.
- *Why it wins:* Pattern instances are the natural unit for one-time condensation, shared enrichment, and batched local solves. This captures repetition even when the original STEP file has lost its CAD feature history.
- *Prior art:* Congruence-labelled adjacency graphs plus frequent subgraph mining for CAD symmetry/patterns ([Li et al.](https://cad-journal.net/files/vol_16/CAD_16(1)_2019_50-66.pdf)); AAG-based machining-feature recognition ([AAGNet](https://doi.org/10.1016/j.rcim.2023.102661)).
- *Fit to PolyMesh:* Build canonical hashes and transform verification atop `CadTopology`; store group actions in `pipeline::Model`; make meshing seed placement and macroelement assembly deterministic under the group.
- *Killer risk:* Nearby intersecting fillets can make nominally repeated design features geometrically non-congruent after Boolean evaluation.

**[GF-01] Online global–local GFEM enrichment** — `enrichment` · impact H · effort XL · risk H
- *What:* Solve a fine local boundary-value problem around a detected feature using boundary data interpolated from a coarse global solve, then multiply that local solution by a partition of unity to enrich the next global solve. Iterate if the global/local boundary data changes materially.
- *Why it wins:* It generates problem-specific feature modes when no trustworthy analytic solution exists and can reuse the same coarse global mesh. It avoids claiming that a catalogued infinite-domain field is exact for a finite interacting part.
- *Prior art:* Duarte & Kim, *Analysis and applications of a GFEM with global-local enrichment functions* ([DOI](https://doi.org/10.1016/j.cma.2007.07.006)); foundational PUFEM theory by Melenk & Babuška ([DOI](https://doi.org/10.1016/S0045-7825(96)01087-0)).
- *Fit to PolyMesh:* Revive the hierarchical/modal `HpModel` in `src/fea/src/hierarchical.cpp` and `src/fea/src/hp_assembly.cpp`; add nonpolynomial basis evaluation and quadrature in `src/fea/src/quadrature.cpp`; drive patch solves from `src/adapt/src/hp_driver.cpp`.
- *Killer risk:* Blending corrections, quadrature, and near-linear dependence can erase the theoretical gain and destabilize Eigen's iterative solve.

**[GF-02] Mesh-based handbook modes for repeated void clusters** — `enrichment` · impact H · effort XL · risk H
- *What:* Precompute local solutions on canonical patches containing one hole or a small interacting cluster, then use them as GFEM “handbook” functions on coarse meshes. Include several independent far-field traction/displacement modes so a feature response is not tied to one benchmark load.
- *Why it wins:* A small basis can encode the expensive boundary layers around many voids, with robustness to close feature spacing demonstrated in the handbook literature. It is closer to the owner's original library concept than a single Kirsch formula.
- *Prior art:* Strouboulis, Zhang & Babuška, *GFEM using mesh-based handbooks: domains with many voids* ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0045782503003475), [DOI](https://doi.org/10.1016/S0045-7825(03)00347-5)).
- *Fit to PolyMesh:* Generate handbook snapshots through `apps/testlab`; define canonical patch coordinates in `CadTopology`; add enriched basis blocks to `hp_assembly.cpp`; include stable orthogonalization and cache versioning by material/geometry.
- *Killer risk:* The mode library grows combinatorially once neighboring-feature positions and 3D thickness effects vary.

**[GF-03] Kirsch circular-hole enrichment** — `analytical-enrichment` · impact M · effort L · risk M
- *What:* Recognize a circular cylindrical through-hole and enrich a surrounding patch with independent Kirsch displacement/stress modes corresponding to remote in-plane load components, plus ordinary polynomial modes for the regular remainder. Restrict the “analytic” label to homogeneous isotropic plane-stress/plane-strain configurations with sufficient thickness/planarity validation.
- *Why it wins:* The dominant stress concentration can be captured on a coarse mesh, and the exact Kirsch component is represented without resolving the circumference by many radial layers. It is an excellent validation pilot because PolyMesh already has Kirsch tests.
- *Prior art:* PUFEM permits problem-specific local approximation spaces ([Melenk & Babuška](https://doi.org/10.1016/S0045-7825(96)01087-0)); mesh-handbook GFEM explicitly targets many voids ([Strouboulis et al.](https://doi.org/10.1016/S0045-7825(03)00347-5)).
- *Fit to PolyMesh:* Detect canonical cylinders in `cad_topology.cpp`; implement the modes in a new enrichment evaluator used by `hp_assembly.cpp`; validate against `tests/test_kirsch_plate.cpp` and `tests/test_kirsch_c5_graded.cpp` before any product exposure.
- *Killer risk:* A finite 3D plate, nearby boundary, nonuniform traction, or neighboring hole invalidates the infinite-plate Kirsch field, so the whole feature is not generally “exact.”

**[GF-04] Williams crack/re-entrant-corner enrichment** — `analytical-enrichment` · impact H · effort L · risk M
- *What:* Enrich nodes/cells around a verified crack front or sharp re-entrant notch with the first Williams radial-angular asymptotic modes, retaining polynomial functions for the regular field. Extract stress-intensity/notch coefficients directly from enriched DOFs or interaction integrals.
- *Why it wins:* It restores high convergence without radial over-refinement and accurately represents the singular exponent. For an actual crack/notch, this is far more efficient than promoting every nearby element.
- *Prior art:* Belytschko & Black's foundational crack enrichment ([DOI](https://doi.org/10.1002/(SICI)1097-0207(19990620)45:5%3C601::AID-NME598%3E3.0.CO;2-S)); enriched partition-of-unity crack-tip fields ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S004579490300453X)); corrected blending treatment ([DOI](https://doi.org/10.1002/nme.2259)).
- *Fit to PolyMesh:* Extend `CadTopology` to classify true sharp notches versus tessellation/sharp-edge artifacts; add mode and derivative evaluation to `hp_assembly.cpp`; add singular-aware quadrature and scaling; compare against `tests/test_l_domain.cpp`.
- *Killer risk:* Williams terms are local asymptotics, not the complete finite-body solution, and 3D crack fronts require changing local frames and richer modes.

**[GF-05] Regularized Boussinesq–Cerruti load enrichment** — `analytical-enrichment` · impact M · effort L · risk H
- *What:* For a small but finite pressure/shear patch on an approximately planar half-space, enrich with integrated Boussinesq–Cerruti influence fields rather than a literal point-load singularity. Fit normal/tangential polynomial traction modes over the selected patch.
- *Why it wins:* It can eliminate extreme local refinement under bearing/contact-like load patches while preserving the correct spatial decay. Integrating over a finite patch gives a physically and variationally better basis than a nodal point force.
- *Prior art:* Love's triangular-area Boussinesq–Cerruti solution set derives constant, linear, and bilinear normal/tangential distributions from the half-space point solution ([DOI](https://doi.org/10.1023/A:1014013425423)).
- *Fit to PolyMesh:* Replace eligible selection-box point loads with patch tractions in `src/fea/src/traction.cpp`; expose enrichment modes to `hp_assembly.cpp`; reject curved/thin/non-half-space neighborhoods using thickness and curvature indicators.
- *Killer risk:* A mathematical point load has non-finite elastic energy at the application point, so claiming exact FEM convergence for the unregularized field is ill-posed.

**[SB-02] SBFEM as a selectable VEM complement** — `sbfem` · impact M · effort XL · risk M
- *What:* For star-convex general polyhedra, assemble an SBFEM stiffness from the discretized faces and radial eigen-solution; keep VEM for cells that are non-star-convex or fail eigen diagnostics. Compare both formulations cell-by-cell through common boundary DOFs.
- *Why it wins:* SBFEM has explicit interior/radial fields and can naturally expose singular modes, while VEM avoids explicit interior shape functions and is cheap. A mixed policy can choose the stronger formulation only where its extra dense local algebra pays.
- *Prior art:* Arbitrary scaled-boundary polyhedral elements and automated local refinement ([Song et al.](https://doi.org/10.1007/s00466-016-1312-0)); arbitrary faceted star-convex polyhedra are treated by Natarajan et al., EABE 80 (2017), 218–229 ([publisher record](https://www.sciencedirect.com/science/article/abs/pii/S0955799716302261)).
- *Fit to PolyMesh:* Add a formulation enum to the polyhedral cells emitted by `varyhedron_fill.cpp`/`mixed_fill.cpp`; share face quadrature and DOF numbering with `vem.cpp`; benchmark consistency, patch tests, and solve conditioning in `tests/test_fe_vem_assembly.cpp`.
- *Killer risk:* The per-cell eigen-decomposition and dense stiffness may be slower than VEM on the target i7 despite needing fewer cells.

**[SB-03] SBFEM-generated notch/fillet enrichment modes** — `sbfem-enrichment` · impact M · effort L · risk M
- *What:* Use a small offline or online SBFEM patch to compute singular exponents and boundary modes for a recognized notch, fillet termination, or multi-material junction, then inject only those modes into surrounding hp elements. This avoids deriving a new closed form for every corner geometry.
- *Why it wins:* It combines automatic singularity discovery with the cheaper global algebra of GFEM and is more general than a fixed Williams catalog. The expensive eigenanalysis is local and cacheable.
- *Prior art:* Gravenkamp et al., *A high-order finite element technique with automatic treatment of stress singularities by semi-analytical enrichment* ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0045782519303688)).
- *Fit to PolyMesh:* Share a local SBFEM kernel with `SB-01`; feed orthonormalized modes to `hp_assembly.cpp`; trigger it from sharp-edge/corner classifications in `cad_topology.cpp`; version the cache by angle, Poisson ratio, and material interface.
- *Killer risk:* The generated mode can be accurate locally yet nearly dependent on the polynomial basis, causing catastrophic conditioning.

**[FC-01] High-order finite-cell CAD solver** — `finite-cell` · impact H · effort XL · risk H
- *What:* Embed the exact STEP solid in a Cartesian/octree background grid, classify quadrature points with OCC, integrate physical portions of cut cells adaptively, and impose unfitted Dirichlet data weakly. Use high-order hierarchical bases and local $hp$ refinement rather than boundary-conforming volume meshing.
- *Why it wins:* It bypasses the hardest body-meshing step and permits smooth high-order approximation on regular cells, especially for dirty or topologically complex CAD. It is a genuine alternative pipeline, not merely another mesher.
- *Prior art:* Parvizian–Düster–Rank, foundational Finite Cell Method ([DOI](https://doi.org/10.1007/s00466-007-0173-y)); CAD/dirty-geometry integration ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0045782519302208)).
- *Fit to PolyMesh:* Reuse `CadModel` point classification and wall-thickness indicators; revive the modal `HpModel`; add cut-cell quadrature in `quadrature.cpp`; add Nitsche/penalty constraints in `assembly.cpp`; keep the current conforming mesh path as fallback.
- *Killer risk:* Adaptive cut integration and small-cut conditioning can consume more time and memory than the meshing it replaces on a six-core laptop.

**[FC-02] Local finite-cell islands around unmeshable features** — `finite-cell` · impact M · effort XL · risk H
- *What:* Keep the current conforming tet/poly mesh globally but carve a simple box around an awkward fillet cluster or lattice detail and solve that island with an immersed high-order grid. Couple island and host traces with mortar/Nitsche constraints.
- *Why it wins:* It confines expensive cut-cell quadrature and stabilization to the few regions that defeat the product mesher. It also offers a migration path that does not require replacing the complete solver.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Add region handoff in `scene.cpp`, nonmatching interface assembly in `assembly.cpp`, and cut quadrature in `quadrature.cpp`; use `CadTopology` feature boxes to define islands; validate energy transfer across the coupling surface.
- *Killer risk:* The nonmatching coupling layer can introduce more error and complexity than remeshing the feature conventionally.

**[CF-01] Stabilized CutFEM with ghost penalties or aggregation** — `cutfem` · impact M · effort XL · risk H
- *What:* Use an unfitted background FE space, Nitsche boundary conditions, and ghost-penalty or aggregated-cell stabilization so arbitrarily tiny physical cut fractions do not destroy coercivity or conditioning. Treat it as the mathematically disciplined variant of `FC-01`.
- *Why it wins:* Stabilization can make conditioning scale similarly to fitted FEM rather than with the smallest sliver volume. It is necessary if PolyMesh expects CG/IC to survive arbitrary CAD/grid alignment.
- *Prior art:* Burman et al., *CutFEM: discretizing geometry and PDEs* (citation and framework in [arXiv](https://arxiv.org/abs/2101.10052)); de Prenter et al., conditioning/remedies review ([arXiv](https://arxiv.org/abs/2208.08538)); Burman's ghost penalty ([DOI](https://doi.org/10.1016/j.crma.2010.10.006)).
- *Fit to PolyMesh:* Add facet-neighbor stabilization operators to `assembly.cpp`; build active/background cell graphs in `mesh`; implement robust weak BC selection in `boundary_faces.cpp`; benchmark direct and CG paths in `solve.cpp`.
- *Killer risk:* Correct high-order 3D CAD boundary quadrature remains the dominant error/cost even after algebraic stabilization.

**[IG-01] Swept-volume IGA for verified prismatic features** — `isogeometric` · impact M · effort XL · risk M
- *What:* Detect an untrimmed or cleanly decomposable extruded/revolved/swept solid, construct a bijective tensor-product volumetric spline map, and solve it with NURBS basis functions. Use this only for components whose volume parameterization passes positivity/distortion checks.
- *Why it wins:* CAD geometry remains exact under $h/p/k$ refinement, and smooth high-order bases can achieve excellent accuracy per DOF on shafts, rings, and sweepable ribs. Restricting IGA to verified sweepable subdomains avoids pretending arbitrary STEP B-reps are analysis-ready volumes.
- *Prior art:* Hughes–Cottrell–Bazilevs, foundational IGA ([DOI](https://doi.org/10.1016/j.cma.2004.10.008)); volumetric spline parameterization requirements ([arXiv](https://arxiv.org/abs/1902.00650)).
- *Fit to PolyMesh:* Add sweep recognition to `CadTopology`; add NURBS volume/basis data alongside `HpModel`; couple spline patch faces to ordinary FE ports; reuse `kPrismSweep` geometry clues from `scene.cpp`.
- *Killer risk:* A globally bijective low-distortion volumetric parameterization is absent from STEP and difficult to construct robustly.

**[IG-02] Exact-NURBS boundary with immersed volume analysis** — `immersogeometric` · impact M · effort XL · risk H
- *What:* Keep STEP's NURBS/trimmed boundary as the exact geometric oracle but use an immersed high-order volume basis, integrating cut cells against the CAD boundary. This seeks zero boundary approximation error without requiring a trivariate NURBS solid.
- *Why it wins:* It attacks IGA's volume-parameterization blocker while retaining CAD-accurate curved boundaries. It can be especially valuable where current OCC tessellation geometry error dominates stress accuracy.
- *Prior art:* Trimmed-NURBS IGA exposes the loss of tensor-product structure and special quadrature requirement ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0045782512001843)); immersed IGA adaptive integration ([arXiv](https://arxiv.org/abs/1911.11519)).
- *Fit to PolyMesh:* Preserve exact face/trim evaluators in `CadModel`; bypass tessellation in cut-cell integration; add trimmed-surface quadrature and weak BCs; compare against `src/mesh/src/brep_fidelity.cpp` metrics.
- *Killer risk:* “Exact geometry” is lost numerically if trimming curves and cut integrals are only approximated loosely.

**[FR-03] ML candidate recognition, never ML authorization** — `feature-recognition` · impact M · effort M · risk M
- *What:* Train UV-Net/BRepNet/Hierarchical-CADNet-style segmentation on MFCAD++, Fusion 360 Gallery, and PolyMesh's procedurally generated/solver-labelled parts to propose feature face sets. Require OCC canonical fitting, topology checks, and a downstream numerical certificate before taking any reduced action.
- *Why it wins:* ML can recover semantic groups in interacting features that defeat brittle hand rules, while deterministic verification protects the solver from distribution shift. The workstation's RTX 2070 is adequate for offline experimentation and small inference models.
- *Prior art:* UV-Net's UV grids plus face adjacency graph ([arXiv](https://arxiv.org/abs/2006.10211)); BRepNet and its 35k operation-labelled B-reps ([arXiv](https://arxiv.org/abs/2104.00706)); Hierarchical CADNet/MFCAD++ ([DOI](https://doi.org/10.1016/j.cad.2022.103226)).
- *Fit to PolyMesh:* Export B-rep tensors from `cad_topology.cpp`; train outside product code; return face IDs and confidence; run exact verification before populating `FeatureInstance`; record false-positive/false-negative rates in testlab rather than only classifier accuracy.
- *Killer risk:* Machining-feature labels do not imply a valid mechanics substitution, so a high segmentation score can be almost irrelevant to FEA correctness.

**[FR-04] Verified sweepability and thin-feature decomposition** — `feature-recognition` · impact M · effort L · risk M
- *What:* Detect face pairs linked by a common extrusion/revolution/sweep transform, construct candidate source/target profiles, and verify the swept volume against the B-rep. Mark eligible regions for prism sweep, IGA, shell/beam reduction, or reusable cross-section operators.
- *Why it wins:* Sweep structure exposes a one- or two-dimensional parameter direction that ordinary tetrahedral meshing ignores. It can turn ribs, shafts, ducts, and long thin members into structured high-order or condensed regions.
- *Prior art:* Automated multi-axis swept-volume parameterization is explicitly motivated as the bridge to IGA ([IEEE record](https://www.computer.org/csdl/proceedings-article/cw/2025/545800a240/2fiSIm5IBy0)); classical B-rep feature recognition uses face adjacency and analytic geometry.
- *Fit to PolyMesh:* Extend `CadTopology`; feed verified regions to existing `kPrismSweep`; add structured interface metadata for `HpModel` and macroelements; use current wall-thickness estimates only as a hint, not proof.
- *Killer risk:* Blends and Boolean cuts often destroy a single globally valid sweep map even when the part looks swept.

**[SC-02] Load-aware exact condensation and stress recovery** — `static-condensation` · impact M · effort M · risk L
- *What:* Store not only $S$ but $T=-K_{ii}^{-1}K_{ib}$, the interior-load response $q=K_{ii}^{-1}f_i$, and stress-recovery operators for every condensed feature. Back-substitute $u_i=T u_b+q$ whenever detailed feature stress is requested.
- *Why it wins:* This preserves exact discrete displacements and stresses even with known loads inside the condensed region; omitting the condensed load term is a common but serious shortcut. Recovery can be lazy, so visualization cost is paid only when requested.
- *Prior art:* Static condensation and port/component recovery are part of the SCRBE/component framework ([Eftang & Patera](https://link.springer.com/article/10.1186/2213-7467-1-3)).
- *Fit to PolyMesh:* Extend element assembly interfaces in `src/fea/include/fea/element.hpp`; inject $S$ and condensed $f$ in `assembly.cpp`; recover fields in `stress.cpp` and `vtu.cpp`; persist the factorization or compressed recovery map.
- *Killer risk:* Arbitrary user loads or contact introduced after library creation invalidate a fixed $q$ unless their independent load modes were retained.

**[CMS-01] Craig–Bampton only when dynamics arrives** — `component-mode-synthesis` · impact L · effort L · risk M
- *What:* If PolyMesh gains modal/transient dynamics, augment boundary constraint modes with a truncated fixed-interface normal-mode basis for recurring components. Do **not** implement Craig–Bampton merely for current linear statics, where exact static condensation is simpler and stronger.
- *Why it wins:* It preserves important component inertia/flexibility that Guyan/static reduction misses and enables repeated dynamic substructures. In the present product it has no accuracy-per-second mechanism because there is no mass/dynamic solve to accelerate.
- *Prior art:* Craig & Bampton, *Coupling of Substructures for Dynamic Analyses* ([DOI](https://doi.org/10.2514/3.4741), [HAL](https://hal.science/hal-01537654)).
- *Fit to PolyMesh:* Future work would add mass assembly and modal coordinates alongside `assembly.cpp`/`solve.cpp`; no current production change is justified.
- *Killer risk:* Building it now would be architecture astronautics for a solver that only performs static elastostatics.

**[RB-02] POD/reduced-basis local correction fields** — `parametric-rom` · impact M · effort L · risk M
- *What:* Generate high-fidelity local solutions over nondimensional feature parameters and independent boundary/load modes, take an energy-weighted POD/reduced basis, and solve only the reduced coefficients online. Reject extrapolation using a residual/coercivity estimator.
- *Why it wins:* It captures 3D finite-thickness and interaction effects unavailable to textbook formulas while exploiting the owner's working solver as a label generator. Energy weighting focuses modes on mechanically important differences rather than raw nodal variance.
- *Prior art:* Reduced-basis approximation with rigorous a posteriori estimation for parametrized elasticity ([arXiv](https://arxiv.org/abs/1801.06553)); component/port form ([Eftang & Patera](https://link.springer.com/article/10.1186/2213-7467-1-3)).
- *Fit to PolyMesh:* Use `apps/testlab` for sampling and snapshot generation; store affine/material operators; assemble reduced feature corrections in `assembly.cpp`; send failed bounds back to the ordinary hp adaptation path.
- *Killer risk:* Non-affine CAD deformation makes fast online operator assembly difficult without another approximation layer.

**[OR-01] Physics-keyed CAD feature macroelement registry** — `original-combination` · impact H · effort XL · risk H
- *What:* Create one registry whose key contains verified B-rep feature type, nondimensional geometry, material invariants, interface topology/order, load-mode schema, and certificate version; values may be exact Schur operators, certified reduced operators, or enrichment modes. At import, a feature is fitted to a registry entry only after geometric and interface equivalence checks.
- *Why it wins:* It turns the owner's idea into a disciplined ABI rather than a bag of benchmark formulas, and supports clean fallback whenever a key is incomplete. Exact entries remain exact; approximate entries carry explicit domains and error estimators.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Add registry lookup after `extract_topology()`; add macroelement/enrichment hooks in `assembly.cpp` and `hp_assembly.cpp`; serialize provenance and certificate data into testlab results; make deterministic key construction part of geometry tests.
- *Killer risk:* Designing a stable interface/parameter schema broad enough to reuse but narrow enough to certify.

**[OR-02] Nondimensional enrichment cache with residual admission** — `original-combination` · impact M · effort L · risk M
- *What:* Cache local global–local/SBFEM enrichment spaces by dimensionless groups such as $r/t$, fillet radius ratio, notch angle, $
u$, neighbor spacing, and normalized interface traction modes. For a new feature, map the cached modes into physical coordinates and admit them only if their local residual falls below a threshold.
- *Why it wins:* Scale invariance lets one training solve serve many absolute CAD sizes, while residual admission prevents silent extrapolation. Cache misses generate new modes using PolyMesh's own fine solver and improve the library over time.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Derive dimensionless descriptors in `cad_topology.cpp`; generate misses through testlab/local solves; orthogonalize modes in the energy inner product; store deterministic cache artifacts keyed by solver/formulation version.
- *Killer risk:* The chosen dimensionless descriptor may omit a nearby boundary or load interaction that controls the response.

**[OR-03] Certified feature-action broker** — `original-combination` · impact H · effort L · risk M
- *What:* For each recognized feature, choose among **ignore with bound, exact symmetry, exact condensation, analytic enrichment, SBFEM cell, local FCM, or ordinary hp mesh** by predicted cost and the strongest available guarantee. The broker must prefer a boring meshed fallback whenever no action satisfies the requested certificate.
- *Why it wins:* No single technique family covers industrial CAD; selecting the cheapest admissible physics treatment is the actual outside-the-box leverage. It also prevents a fragile analytic feature library from becoming a second, less trustworthy solver.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Add planning between CAD import and `volume_mesh()` in `scene.cpp`; consume `FeatureInstance` records and user tolerances; expose decisions in `result.json`; train cost models only from testlab timings, never hardcoded benchmark answers.
- *Killer risk:* A bad cost/eligibility model can route the hardest cases to the least mature formulation.

**[OR-04] Import-time substitution patch test** — `original-validation` · impact M · effort M · risk L
- *What:* Before trusting any cached macroelement or enrichment on a novel part, solve a tiny local interface problem for rigid-body and constant-strain/traction modes using both the substitute and a coarse explicit patch. Reject entries that violate symmetry, positive semidefiniteness, energy reciprocity, or prescribed error thresholds.
- *Why it wins:* It catches corrupt transforms, port permutations, units, stale material keys, and incompatible enrichment at the point of use. The test is cheap compared with a full solve and converts silent library misuse into deterministic fallback.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Reuse element patch-test machinery around `tests/test_fe_vem_assembly.cpp`; run admission checks during scene construction; record matrix symmetry/eigenvalue/residual diagnostics; never mutate the registry from product code.
- *Killer risk:* Passing low-order patch modes does not prove accuracy for higher-frequency interface data.

**[DF-03] Interaction-aware feature-cluster restoration** — `certified-defeaturing` · impact M · effort L · risk M
- *What:* Treat nearby holes/fillets/bosses as a cluster when individual defeaturing indicators cease to be additive; restore the cluster or evaluate joint residual terms. Use graph distance, physical separation-to-size ratio, and estimator cross-terms to decide when isolation is unsafe.
- *Why it wins:* It avoids the classic failure where ten individually “small” features interact into a large compliance or stress change. Cluster-level decisions still permit aggressive removal of genuinely independent details.
- *Prior art:* Multiple-feature elasticity estimators and adaptive geometric restoration are developed by Antolin & Chanon ([arXiv](https://arxiv.org/abs/2212.03141)); second-order multi-feature defeaturing estimators were also studied in earlier CAD error work.
- *Fit to PolyMesh:* Build a feature proximity graph from `CadTopology`; aggregate estimator contributions in `adapt::error`; let `scene.cpp` restore whole connected groups; sweep spacing/size cases in testlab.
- *Killer risk:* Dense interaction can collapse most candidates into one cluster and erase the speedup.

## Rigorous verdict: where zero or bounded added error is actually possible

### 1. Exact condition for static condensation

Partition the **assembled discrete linear static** system into retained/interface DOFs $b$ and interior DOFs $i$:

$$
\begin{bmatrix}K_{bb}&K_{bi}\\K_{ib}&K_{ii}\end{bmatrix}
\begin{bmatrix}u_b\\u_i\end{bmatrix}
=
\begin{bmatrix}f_b\\f_i\end{bmatrix}.
$$

If:

- the problem is linear and the same $K$ and $f$ would be used in the full model;
- **all coupling of the substructure to the rest of the model occurs through retained DOFs $b$**;
- $K_{ii}$ is nonsingular (internal mechanisms/rigid modes are constrained or retained); and
- no approximation is made in forming/applying $K_{ii}^{-1}$,

then

$$
S=K_{bb}-K_{bi}K_{ii}^{-1}K_{ib},\qquad
\hat f_b=f_b-K_{bi}K_{ii}^{-1}f_i
$$

and solving $S u_b=\hat f_b$, followed by

$$u_i=K_{ii}^{-1}(f_i-K_{ib}u_b),$$

produces **exactly the same discrete solution** as the uncondensed FE system, up to floating-point/linear-solver tolerance. This is exact elimination, not model reduction. It is **not zero continuum discretization error**: it preserves whatever error existed in the fine source mesh. Interior loads are allowed only if their contribution $K_{bi}K_{ii}^{-1}f_i$ is condensed consistently; interior stresses require back-substitution/recovery.

Exact reuse further requires identical geometry, material tensor, topology, port/interface basis and ordering, with rigid transforms handled exactly. Parameter interpolation, port truncation, interface mesh mismatch, nonlinear material, contact state changes, topology changes, or unrepresented interior loads make it approximate. Craig–Bampton is unnecessary for current statics; it becomes relevant when inertia/frequency content exists.

### 2. When an analytic enrichment gives zero error

For a coercive Galerkin problem, if the exact continuum solution $u$ lies in the admissible enriched trial space $V_h^{\mathrm{enr}}$, essential BCs and geometry are represented exactly, and all bilinear/linear forms are integrated exactly, then Galerkin returns $u$ exactly (modulo roundoff). A PU can reproduce an analytic mode $\phi$ over a fully enriched region because $\sum_i N_i\phi=\phi$; at the edge of a locally enriched patch, partially enriched **blending elements** generally destroy that reproduction unless corrected.

That theorem is much narrower than “one exact-mode element makes a feature exact”:

- **Kirsch:** exact for an infinite homogeneous isotropic plate with a circular hole and the matching remote loading assumptions. In a finite 3D part, it is a useful mode, not the whole solution.
- **Williams crack/notch modes:** exact terms of a local asymptotic expansion; finitely many terms do not equal the global solution.
- **Boussinesq/Cerruti:** exact half-space Green fields; a point load is singular and not a finite-energy $H^1$ solution at its point, so use finite traction patches.
- **Fillet/notch eigenfunctions:** exact radial/angular modes only for the ideal local wedge/interface eigenproblem. A finite-radius fillet is smooth and its response depends strongly on the surrounding geometry.

Even when the mode is right, nonpolynomial/cut quadrature, essential BC enforcement, near-linear dependence, basis scaling, blending corrections, and solver conditioning can dominate. Stable GFEM and corrected XFEM are requirements, not polish.

### 3. Feature classes and achievable guarantees

| Feature class | Strongest honest guarantee | Conditions |
|---|---|---|
| Exact mirror/cyclic repeat | **Zero added continuum and discrete error** | Geometry, material, loads and BCs invariant/equivariant; exact transformed constraints/ties |
| Fixed repeated subdomain with standard port | **Zero added discrete error** by full condensation | Same fine $K,f$, interface basis and topology; $K_{ii}$ invertible; exact back-substitution |
| Canonical hole/crack/notch analytic mode | **Zero error for that mode only**; whole solution exact only if it lies in the complete enriched span | Exact ideal geometry/material/loading; corrected blending; exact-enough quadrature and BCs |
| Untrimmed NURBS spline domain | **Zero geometric approximation error** | Exact CAD patch and valid volume parameterization; PDE approximation error remains |
| SBFEM star-convex/singular cell | **No radial discretization error for the semi-analytical radial solution**, but boundary discretization remains | Valid scaling center; sufficiently accurate boundary FE space/eigen-solve |
| Small Neumann/Dirichlet CAD features | **Reliably/certifiably bounded added energy or QoI error** | Feature belongs to estimator's assumptions; valid extension/equilibrated reconstruction; stable QoI |
| Parametric reduced/port component | **A posteriori bounded error relative to the high-fidelity FE model** | In certified parameter domain; coercivity lower bound/residual estimator valid |
| FCM/CutFEM/immersed CAD | No inherent zero-error claim; potentially high-order convergence without fitted volume mesh | Accurate CAD classification and cut quadrature; stable weak BCs; small-cut remedy |
| ML-recognized feature | **No physics guarantee at all** | It may propose candidates; deterministic geometry checks and a numerical certificate must authorize action |

### 4. What OpenCASCADE gives—and does not give

OCCT provides the right low-level machinery:

- `ShapeAnalysis_CanonicalRecognition` recognizes planes, cylinders, cones, spheres, lines, circles and ellipses to a requested tolerance and reports the gap ([official API](https://dev.opencascade.org/doc/refman/html/class_shape_analysis___canonical_recognition.html)).
- B-rep traversal/adaptors expose faces, loops, coedges, adjacency, surface/curve types and differential geometry.
- `BRepAlgoAPI_Defeaturing` / `BOPAlgo_RemoveFeatures` can remove selected feature faces and extend neighboring faces ([official API](https://dev.opencascade.org/doc/refman/html/class_b_o_p_algo___remove_features.html)).
- Fillet/chamfer/rib/groove APIs are primarily **construction** tools.

It does **not** provide a turnkey semantic recognizer that robustly says “these faces are bolt-hole instance 7 of circular pattern 2” for arbitrary imported STEP. PolyMesh must build that semantics from canonical geometry, attributed adjacency, graph matching and verification, while preserving face provenance through defeaturing.

## Top-5 shortlist

1. **DF-01 — certified adaptive Neumann defeaturing.** Best accuracy-per-engineering-effort leverage; directly removes meshing poison and gives an honest error story.
2. **SYM-01 — exact mirror/cyclic sector solves.** Cheapest true zero-added-error win; implement before any surrogate library.
3. **SC-01 + SC-02 — exact repeated-feature condensation with recovery.** The rigorous “pre-solved feature” mechanism for truly congruent, fixed-interface instances.
4. **FR-01 + FR-02 — verified hybrid recognition and congruent-pattern extraction.** Required enabler; deterministic verification matters more than classifier accuracy.
5. **SB-01 — singular SBFEM feature cells.** Best polyhedral-native route for cracks/notches and a defensible complement to VEM.

`RB-01` is the ambitious second wave after exact condensation proves demand. `GF-03` Kirsch enrichment is the best small research pilot, but not the best product architecture.

## Concrete architecture for the best option: certified adaptive defeaturing

### A. Import and feature graph

1. In `src/geom/src/cad_topology.cpp`, augment `extract_topology()` to build a face AAG: canonical surface/curve type, radius/axis, loops, signed dihedral, area, bounding box, adjacency and source OCC face ID.
2. Add a `FeatureCandidate` model (hole, fillet chain, chamfer, boss, groove, protrusion) with member faces, dimensional parameters, neighboring support faces, provenance, and confidence/verification status in `src/geom/include/geom/cad_topology.hpp`.
3. Use `ShapeAnalysis_CanonicalRecognition` to recover analytic primitives hidden as B-splines where the reported gap is below a scale-aware tolerance. Deterministic rules propose candidates; exact surface/loop/topology checks verify them.

### B. Eligibility gate before suppression

For the first production cut, admit only candidates that:

- carry homogeneous Neumann/traction-free boundary conditions;
- do not intersect prescribed-displacement, traction, or load selection boxes;
- do not change material interfaces;
- are not part of a requested peak-stress QoI;
- satisfy the topology/geometric assumptions of the implemented estimator; and
- are separated or explicitly placed in an interaction cluster.

This gate belongs in `src/pipeline/src/scene.cpp`, because it needs CAD, selections, material and analysis intent together. Any uncertainty means “mesh it normally.”

### C. Reversible OCC defeaturing with provenance

Use `BRepAlgoAPI_Defeaturing`/`BOPAlgo_RemoveFeatures` in `src/geom/src/cad_model.cpp` to create a simplified `CadModel`, but retain:

- original feature shape;
- removed and neighboring faces;
- original-to-simplified face history;
- extension/lifting geometry required by the estimator; and
- a deterministic feature ID.

BC selection must be re-evaluated against provenance, not merely geometric boxes, in `src/fea/src/boundary_faces.cpp` and `src/fea/src/traction.cpp`.

### D. Two-error solve and genuine certificate

1. Mesh/solve the simplified model through the existing `scene.cpp` → `assembly.cpp` → `solve.cpp` path.
2. Keep current ZZ recovery in `src/fea/src/zz.cpp` as an **adaptivity indicator only**.
3. Add an equilibrated, statically admissible stress reconstruction in/alongside `src/adapt/src/error.cpp`. The certificate should combine numerical FE error with defeaturing residual/extension terms in the elastic energy norm $\|e\|_E=(\int \varepsilon(e):C:\varepsilon(e))^{1/2}$; a ZZ least-squares stress fit is not a guaranteed upper bound.
4. Produce `FeatureErrorContribution {feature_id, eta_def, assumptions, validity}` and a global bound. For a QoI, add the dual solve and DWR-weighted feature terms from `DF-02`.

### E. Adaptive restoration loop

In `src/adapt/src/hp_driver.cpp` or a sibling driver:

1. compare $\eta_{num}$ and $\eta_{def}$ against separate budgets;
2. if numerical error dominates, use existing $h/p/$shape adaptation;
3. if defeaturing error dominates, restore the largest Dörfler set of feature/cluster contributions;
4. rebuild only the affected CAD/mesh state if feasible, otherwise deterministically remesh; and
5. stop only when the combined certified criterion is met or all features are restored.

This is the geometric analogue of PolyMesh's existing Dörfler loop and is directly supported by Antolin–Chanon's adaptive restoration strategy.

### F. Evidence-first harness

Extend `apps/testlab/main.cpp` and `scripts/analyze_campaign.py` to record:

- original versus retained feature count;
- mesh/DOF/time savings;
- $\eta_{num}$ and $\eta_{def}$ separately;
- observed error against full-feature reference;
- certificate effectivity (bound / observed error);
- every rejected assumption/fallback; and
- deterministic feature/action hashes.

Use `tests/test_kirsch_plate.cpp`, `tests/test_goodier_cavity.cpp`, and new procedural hole/boss/fillet clusters as controlled elasticity checks. The acceptance test is not “the simplified answer looks close”; it is **observed error never exceeds the claimed bound over the supported feature class**, with explicit fallback outside it.

## Do not bother

- **Do not market a Kirsch/Williams/Boussinesq basis as “zero discretization error for the feature.”** It is exact only for its ideal mode/problem; finite boundaries and interactions leave a regular/global correction.
- **Do not interpolate raw condensed stiffness matrices over arbitrary geometry parameters without stability and error control.** Entrywise interpolation can violate symmetry/positive definiteness and has no rigorous reuse guarantee.
- **Do not build Craig–Bampton now.** PolyMesh has no dynamic problem, so exact static condensation dominates it.
- **Do not rewrite the whole solver around full 3D IGA first.** STEP supplies trimmed boundary patches, not a bijective trivariate spline volume; trimming and volume parameterization are the real project.
- **Do not launch a full FCM/CutFEM replacement before a local prototype.** On this workstation, 3D adaptive boundary quadrature and conditioning could cost more than current meshing.
- **Do not let an ML feature label trigger suppression or substitution.** MFCAD-style machining semantics are neither an exact geometry proof nor an elasticity certificate.
- **Do not treat current ZZ recovery as a certified defeaturing bound.** It is a useful heuristic estimator, but it is neither equilibrated nor a guaranteed upper bound.
- **Do not condense through contact, nonlinear material, or changing topology and call it exact.** The Schur operator is state-dependent there; a fixed library entry is wrong.
