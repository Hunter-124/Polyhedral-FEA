<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Outside-the-box leverage across the whole program

## Ranked idea bank

The order below is the ranking: early ideas are the best bets after balancing accuracy, credibility, implementation cost, and the actual i7-9750H/RTX 2070 workstation. “Guaranteed” always means *under explicit assumptions*—linear elasticity, admissible loads/BCs, adequate quadrature, valid geometry, and a correct implementation—not marketing shorthand.

**[WL-01] Compile manufactured elasticity problems into regression gates** — `verification` · impact H · effort M · risk L · **SHIPPED** (`tests/test_mms_convergence.cpp`, `tests/support/mms.hpp`, MMS coverage in `test_hp_assembly.cpp`/`test_vem.cpp`, energy rates p=1..4 gated in CI)
- *What:* Build a symbolic MMS generator that starts from a smooth displacement field, differentiates $\sigma=C:\varepsilon(u)$, and emits body forces, tractions, Dirichlet data, and exact displacement/stress norms. Run systematic refinement for every production element family, VEM order, and geometry path, fitting observed convergence order rather than merely checking one answer.
- *Why it wins:* MMS catches sign, Jacobian, quadrature, assembly, BC, and stress-recovery defects that reference-answer tests can all share; an observed-order gate is far harder to game than a single tolerance.
- *Prior art:* Aycock, Rebelo & Craven, “Method of Manufactured Solutions Code Verification of Elastostatic Solid Mechanics Problems in a Commercial Finite Element Solver” ([arXiv:1902.07608](https://arxiv.org/abs/1902.07608)).
- *Fit to PolyMesh:* Add generation to the test harness around `src/fea/src/assembly.cpp`, `vem.cpp`, `hp_assembly.cpp`, `stress.cpp`, and `apps/testlab/main.cpp`; record $L^2$, energy, and stress-norm slopes in campaign JSON and gate them in CI.
- *Killer risk:* Incorrect symbolic-to-PolyMesh BC/load translation can make the verifier test itself instead of the solver.

**[WL-02] Make the conforming hierarchical `HpModel` the production discretization** — `correctness / high order` · impact H · effort L · risk H
- *What:* Retire selective tet4→tet10/hex8→hex20 promotion and route production through the already implemented modal `HpModel`/`assemble_hp`, preserving its shared-entity minimum rule. Add constraint-aware result evaluation and support only the element/order combinations that pass MMS before broadening coverage.
- *Why it wins:* This removes the silent nonconforming-interface defect and unlocks true variable $p$ up to hex 6/tet 4, yielding much higher accuracy per element on smooth fields.
- *Prior art:* Brown et al. demonstrate high-order solid mechanics reducing cost to engineering tolerances in “Performance Portable Solid Mechanics via Matrix-Free $p$-Multigrid” ([arXiv:2204.01722](https://arxiv.org/abs/2204.01722)).
- *Fit to PolyMesh:* Clean cutover through `src/fea/include/fea/hp_assembly.hpp`, `src/fea/src/hp_assembly.cpp`, `hierarchical.cpp`, `pipeline::Model`, and `src/adapt/src/hp_driver.cpp`; delete the unconstrained nodal-promotion path after campaign parity.
- *Killer risk:* Mixed tet/hex/poly interfaces and BC/result transfer may expose conformity gaps not covered by the current shared-entity rule.

**[WL-03] Goal-oriented DWR adaptivity for engineer-selected outputs** — `error control` · impact H · effort L · risk M
- *What:* Let the user choose a differentiable quantity of interest (QoI)—tip displacement, compliance, reaction force, or area-averaged stress—and solve an enriched adjoint alongside the primal problem. Weight element and face residuals by adjoint sensitivity, then drive $h/p$/shape decisions from their signed QoI contributions rather than the global ZZ energy proxy.
- *Why it wins:* DOFs are spent where local error actually changes the number being reported; an ugly region irrelevant to the QoI stops stealing refinement from a small but influential load path.
- *Prior art:* Becker & Rannacher, “An optimal control approach to a posteriori error estimation in finite element methods” ([DOI:10.1017/S0962492901000010](https://doi.org/10.1017/S0962492901000010)).
- *Fit to PolyMesh:* Add a QoI/adjoint layer beside `src/fea/src/solve.cpp`, residual evaluation beside `assembly.cpp`/`vem.cpp`, and DWR indicators as new `ElementHpSignal`s consumed by `drive_hp()` in `src/adapt/src/hp_driver.cpp`.
- *Killer risk:* A same-space adjoint gives a useless zero by Galerkin orthogonality; reliable enrichment and residual terms for every hybrid/VEM interface are real work.

**[WL-04] CAD-exact curved boundary elements, not merely projected nodes** — `representation / geometry error` · impact H · effort L · risk H
- *What:* Carry OCC face identity and parametric coordinates into boundary elements, evaluate boundary quadrature points/normals/Jacobians directly on the B-rep, and use geometry order independent of solution order. Start with NEFEM-style curved boundary tets while leaving interior elements conventional.
- *Why it wins:* Refinement no longer wastes DOFs converging a faceted approximation to a curved fillet or hole; geometry error ceases to dominate high-order solution error.
- *Prior art:* Sevilla, Fernández-Méndez & Huerta, “3D NURBS-enhanced finite element method (NEFEM)” ([DOI:10.1002/nme.3164](https://doi.org/10.1002/nme.3164)).
- *Fit to PolyMesh:* Extend `geom::CadTopology` (`src/geom/include/geom/cad_topology.hpp`), OCC tessellation provenance, boundary `mesh::Face`, quadrature, element mapping, and stress/result sampling; validate with curved-domain MMS.
- *Killer risk:* Robustly parameterizing trimmed STEP faces and preventing inverted curved elements near seams is substantially harder than projecting midside nodes.

**[WL-05] Certified two-sided QoI intervals via admissible stresses and CRE** — `certification` · impact H · effort XL · risk H
- *What:* Reconstruct a statically admissible, approximately symmetric $H(\mathrm{div})$ stress field on vertex/element patches, combine it with the kinematically admissible displacement, and compute constitutive-relation-error bounds. Repeat for the adjoint to report, for example, “area-averaged fillet stress = 241 MPa, discretization interval [236, 247] MPa,” not an unqualified scalar.
- *Why it wins:* This changes accuracy from an estimator with unknown effectivity into a result-specific, auditable interval; it is the strongest product differentiator here when the assumptions hold.
- *Prior art:* Gallimard, “A constitutive relation error estimator based on traction-free recovery of the equilibrated stress,” including lower/upper bounds for local QoIs ([DOI:10.1002/nme.2496](https://doi.org/10.1002/nme.2496)); equilibrated estimates provide constant-free guaranteed bounds in their setting ([arXiv:1812.06678](https://arxiv.org/abs/1812.06678)).
- *Fit to PolyMesh:* Add local mixed patch solves beside `src/fea/src/zz.cpp`, exact traction/body-force projection, primal/dual admissibility checks, and interval fields in `pipeline::fill_result_fields()` and the GUI.
- *Killer risk:* A rigorous stress reconstruction across tet/hex/prism/pyramid/VEM interfaces—and inclusion of geometry, quadrature, and algebraic error—is a research project, not a ZZ refactor.

**[WL-06] Frontal constrained-Delaunay tetrahedralization as the dependable baseline** — `meshing` · impact H · effort L · risk M
- *What:* Add a true B-rep-conforming restricted/constrained Delaunay refiner with protected sharp curves, off-centre insertion, sizing-field conformance, and exact predicates. Make it the “boring reliable” tet path against which all exotic hybrid/poly meshers compete.
- *Why it wins:* It raises the practical accuracy ceiling by making high-quality, locally graded, boundary-faithful meshes available on arbitrary CAD; every estimator and high-order method becomes more trustworthy on top of it.
- *Prior art:* Engwirda, “Conforming restricted Delaunay mesh generation for piecewise smooth complexes” ([arXiv:1606.01289](https://arxiv.org/abs/1606.01289)); Shewchuk, “Constrained Delaunay Tetrahedralizations and Provably Good Boundary Recovery” ([author PDF](https://people.eecs.berkeley.edu/~jrs/papers/cdtbasic.pdf)).
- *Fit to PolyMesh:* New `pipeline::VolumeMesher` backend consuming `CadTopology` sharp curves and the sizing field, with TetGen/CGAL/JIGSAW evaluated as dependency options; feed native tets to the existing FEA and campaign harness.
- *Killer risk:* Robust conformity to dirty trimmed STEP models and license/dependency constraints can erase the apparent implementation advantage.

**[WL-07] Matrix-free $p$-multigrid with an AMG coarse solve** — `solver` · impact H · effort XL · risk M
- *What:* Apply high-order element operators without assembling the full matrix, coarsen polynomial order down to $p=1$, then use AMG on the coarse assembled elasticity system. Implement CPU first; the same operator interface becomes the GPU path later.
- *Why it wins:* Sum factorization on hexes reduces memory traffic and operator complexity, while $p$-multigrid removes the condition-number growth that would otherwise make the newly activated `HpModel` unusable.
- *Prior art:* Brown et al., “Performance Portable Solid Mechanics via Matrix-Free $p$-Multigrid” ([arXiv:2204.01722](https://arxiv.org/abs/2204.01722)).
- *Fit to PolyMesh:* Refactor `HpSystem`, `hp_element_stiffness()`, `src/fea/src/solve.cpp`, and `backend.cpp` around an operator/preconditioner interface; retain assembled fallback for tets/poly VEM until kernels mature.
- *Killer risk:* The major gains are easiest on tensor-product hexes; arbitrary tets and polyhedral VEM can leave an awkward, slower hybrid operator.

**[WL-08] Static-condense all high-order interior modes** — `solver` · impact H · effort M · risk L
- *What:* Partition each high-order element matrix into boundary and interior modes, eliminate interiors locally, solve the global Schur complement, then recover element interiors. Cache local factorizations for repeated load cases.
- *Why it wins:* Global unknown count and sparse fill fall sharply as $p$ rises, making the production `HpModel` useful on this workstation rather than merely correct.
- *Prior art:* Static condensation is an established high-order FE technique; MFEM documents the smaller condensed system and higher-order efficiency ([MFEM solver guide](https://mfem.org/tutorial/solvers/)).
- *Fit to PolyMesh:* Add condensation/recovery to `src/fea/src/hp_assembly.cpp` around its existing edge/face/interior mode partition and expose condensed matrices to `solve.cpp`.
- *Killer risk:* Poorly conditioned local interior blocks on distorted elements can make recovery numerically fragile.

**[WL-09] Separate discretization, algebraic, quadrature, and geometry error budgets** — `error control / honesty` · impact H · effort L · risk M
- *What:* Stop CG when estimated algebraic error is safely below discretization error, and report separate bars for algebraic residual, FE discretization, quadrature, and CAD/tessellation geometry. Never spend iterations driving a residual six orders below a mesh error—or present that residual as solution accuracy.
- *Why it wins:* It saves solve time and prevents a common credibility failure: conflating linear-system convergence with continuum-solution convergence.
- *Prior art:* Ern & Vohralík, “Algebraic and discretization error estimation by equilibrated fluxes,” explicitly motivates stopping when algebraic error falls below discretization error ([HAL primary manuscript](https://inria.hal.science/hal-00851822/document)).
- *Fit to PolyMesh:* Couple `solve.cpp` iteration history to a new estimator API, CAD chordal metrics from `CadTopology`, quadrature checks, and result metadata in `apps/testlab`/GUI.
- *Killer risk:* The decomposition is only honest if cross-terms and estimator assumptions are exposed rather than summed as if independent certainties.

**[WL-10] GPU matrix-free elasticity kernels, not another CSR SpMV micro-optimization** — `performance` · impact H · effort L · risk M
- *What:* Fuse basis evaluation, gradients, constitutive action, and scatter for high-order hex/tet batches on the RTX 2070, keeping vectors and geometry factors resident. Use CUDA CSR only for the coarse AMG level and legacy low-order cases.
- *Why it wins:* The GPU sees arithmetic-rich element kernels instead of bandwidth-bound sparse rows; it also avoids storing high-order global matrices that would exhaust 8 GB VRAM.
- *Prior art:* Brown et al. report order-of-magnitude efficiency improvements from matrix-free high-order GPU solid mechanics ([arXiv:2204.01722](https://arxiv.org/abs/2204.01722)).
- *Fit to PolyMesh:* Extend `src/fea/src/spmv.cpp`/`backend.cpp` into a device operator backend shared with `HpModel`; campaign by QoI error per second, not kernel GFLOP/s.
- *Killer risk:* RTX 2070 double throughput is weak, so small/moderate models may lose to the CPU after launch and transfer overhead.

**[WL-11] Mixed-precision preconditioning behind an explicit ADR exception** — `solver / performance` · impact M · effort M · risk M
- *What:* Keep assembly, residuals, updates, convergence tests, and final solution in FP64, but permit FP32 (or experimentally FP16) inside the multigrid preconditioner with residual scaling and iterative refinement. This requires a documented exception to PolyMesh’s double-only rule; silently changing precision is unacceptable.
- *Why it wins:* Preconditioner accuracy need only reduce error, not define the final answer, so lower precision can cut memory traffic while FP64 outer correction preserves requested accuracy.
- *Prior art:* Oo & Vogel report FP64-accurate iterative refinement with low-precision geometric multigrid and up to 2.5× overall speedup ([arXiv:2007.07539](https://arxiv.org/abs/2007.07539)).
- *Fit to PolyMesh:* Add typed preconditioner vectors/kernels under `backend.cpp`, residual replacement in `solve.cpp`, and MMS/effectivity gates comparing full-FP64 and mixed modes.
- *Killer risk:* The RTX 2070 lacks the same useful FP64/FP16 balance as datacenter GPUs, so the complexity may yield little wall-clock gain.

**[WL-12] Recycle Krylov subspaces across loads, adjoints, and refinements** — `solver / workflow` · impact M · effort M · risk L
- *What:* Preserve approximate low eigenmodes/deflation vectors between multiple load cases, primal/adjoint pairs, and nearby adaptive meshes, transferring vectors when topology changes. Use block solves when several RHS share one stiffness matrix.
- *Why it wins:* Design studies and DWR create sequences of closely related systems; paying to rediscover the same slow modes every solve is avoidable.
- *Prior art:* Parks et al., “Recycling Krylov Subspaces for Sequences of Linear Systems” ([primary author manuscript](https://personal.math.vt.edu/sturler/publications/SISC_KrylovRecycling_2006.pdf)).
- *Fit to PolyMesh:* Add a recyclable solver context around `solve.cpp`, keyed by matrix/geometry lineage, plus prolongation from `adapt::drive_hp` refinement maps.
- *Killer risk:* Bad transferred subspaces can cost more orthogonalization and memory than they save on workstation-sized systems.

**[WL-13] Bank–Weiser local enrichment as the practical ZZ replacement** — `error control` · impact M · effort M · risk L
- *What:* Solve a tiny hierarchical Neumann correction on each element in an enriched space and use its energy as the local indicator. Deploy this before full certification because it is local, parallel, and naturally compatible with hierarchical $p$ modes.
- *Why it wins:* It measures missing approximation content rather than smooths the computed stress and can supply better $h$ versus $p$ evidence than ZZ with modest cost.
- *Prior art:* Bulle et al., “Hierarchical a posteriori error estimation of Bank-Weiser type in the FEniCS Project” ([arXiv:2102.04360](https://arxiv.org/abs/2102.04360)).
- *Fit to PolyMesh:* Reuse `hp_modes()`/`hp_element_stiffness()` to construct local complements, replace or augment `recover_zz()` outputs, and feed `ElementHpSignal` smoothness/error fields.
- *Killer risk:* Saturation assumptions can fail near singularities, so it must be effectivity-tested rather than called a bound.

**[WL-14] Define a stress-singularity contract instead of refining toward infinity** — `results / honesty` · impact H · effort S · risk L
- *What:* Detect re-entrant corners, point/edge loads, zero-area supports, and sharp material/BC discontinuities, then forbid “certified peak nodal stress” there. Offer mesh-objective alternatives: area/volume-averaged stress, hot-spot extrapolation at declared distances, reaction force, or a fitted singular amplitude.
- *Why it wins:* It prevents endless refinement from making the headline peak larger while falsely implying convergence, and it gives DWR/CRE a bounded, meaningful QoI.
- *Prior art:* `NONE FOUND — original proposal` (the mathematical singularity is known; the proposed enforceable product contract and automatic QoI substitution are the original part).
- *Fit to PolyMesh:* Combine `CadTopology` sharp-edge/re-entrant classification, BC/load support geometry, `stress.cpp`, GUI result selection, and campaign convergence checks.
- *Killer risk:* Users may reject a tool that refuses the familiar maximum-von-Mises number even when refusal is the honest answer.

**[WL-15] Finite-cell high-order escape hatch for CAD that defeats meshing** — `representation / meshing bypass` · impact H · effort XL · risk H
- *What:* Embed the solid in a Cartesian/octree background, integrate cut cells adaptively against the OCC B-rep, and impose Dirichlet conditions weakly. Use it as a fallback/reference solver, not an immediate replacement for body-fitted meshes.
- *Why it wins:* Geometry complexity moves from topology-sensitive volume meshing into local quadrature; smooth regions retain high-order convergence on simple cells.
- *Prior art:* Schillinger et al., “A review of the finite cell method for nonlinear structural analysis of complex CAD and image-based geometric models” ([arXiv:1807.01285](https://arxiv.org/abs/1807.01285)).
- *Fit to PolyMesh:* Reuse OCC point-in-solid/distance queries, add cut-cell octree quadrature, Nitsche BCs, ghost/aggregation stabilization, and a new `VolumeMesher`-independent analysis path.
- *Killer risk:* Tiny cut cells create severe conditioning and integration costs unless aggregation/preconditioning is excellent.

**[WL-16] Selective isogeometric patches only on CAD-smooth regions** — `representation` · impact M · effort XL · risk H
- *What:* Keep general hybrid FEM/VEM in the volume, but identify untrimmed or simply trimmed NURBS surface/volume-like regions where CAD basis functions can represent geometry and solution directly. Couple those patches to standard FE through mortar/Nitsche interfaces.
- *Why it wins:* It captures smooth curved features with exact geometry and high continuity without requiring arbitrary STEP solids to become one global spline volume.
- *Prior art:* Hughes, Cottrell & Bazilevs, “Isogeometric analysis: CAD, finite elements, NURBS, exact geometry and mesh refinement” ([DOI:10.1016/j.cma.2004.10.008](https://doi.org/10.1016/j.cma.2004.10.008)).
- *Fit to PolyMesh:* Extend `CadFace` provenance, add spline basis/quadrature and nonmatching coupling in FEA, and have sweep/feature recognition select eligible regions.
- *Killer risk:* Robust volumetric parameterization and trimmed-patch coupling can consume years while covering few real parts.

**[WL-17] Graded isosurface stuffing as a fast independent tet generator** — `meshing` · impact M · effort L · risk M
- *What:* Convert the B-rep to a signed-distance oracle and fill an adaptive BCC lattice with a finite stencil set, projecting boundary vertices to CAD. Use it for instant previews and as an algorithmically independent cross-check against Delaunay meshes.
- *Why it wins:* It is fast, robust, parallel-friendly, and has angle guarantees in its original setting; independence is valuable when two meshers agreeing is evidence against a shared meshing pathology.
- *Prior art:* Shewchuk, “Isosurface Stuffing: Fast Tetrahedral Meshes with Good Dihedral Angles,” with stated 10.7°–165° angle bounds ([Stanford abstract](https://graphics.stanford.edu/courses/ba-colloquium/spring07/talk1.html)).
- *Fit to PolyMesh:* Build an OCC signed-distance/classification service, adaptive lattice templates, and a new `kIsoStuff` backend feeding existing tet FEA.
- *Killer risk:* CAD feature preservation below lattice scale and watertight sign classification are weaker than a true B-rep-constrained method.

**[WL-18] Medial-axis sheet extraction and automatic sweep decomposition** — `meshing / representation` · impact H · effort L · risk H
- *What:* Compute a robust interior distance field/medial approximation, detect thin sheets and tubular regions, pair opposite CAD faces, and partition them into sweepable blocks. Mesh those regions with prisms/hexes through thickness while leaving junctions to CDT or VEM.
- *Why it wins:* Thin walls get the right topology and anisotropy—few well-aligned layers through thickness and long elements in-plane—instead of exploding isotropic tet counts.
- *Prior art:* Xia et al., “Fast equal and biased distance fields for medial axis transform with meshing in mind” ([ScienceDirect primary article](https://www.sciencedirect.com/science/article/pii/S0307904X11002952)).
- *Fit to PolyMesh:* Replace/augment uniform-grid wall-thickness ray casting, enrich `CadTopology` with paired-face/sweep-region data, and extend `kPrismSweep`/`kHybrid` partitioning.
- *Killer risk:* Medial axes are unstable under small CAD defects, and junction decomposition—not thickness detection—is the genuinely hard part.

**[WL-19] A boundary-layer advancing front for load paths and thin walls** — `meshing` · impact M · effort L · risk H
- *What:* Seed high-quality prism/hex layers from selected CAD faces, advance them using curvature/thickness constraints, then terminate into a Delaunay tet core with pyramids/polyhedra. Restrict the first version to confidently detected two-sided sheets rather than promising arbitrary advancing-front hex meshing.
- *Why it wins:* Boundary stresses and bending through thickness converge with aligned layers at far fewer DOFs than isotropic tet refinement.
- *Prior art:* Engwirda’s Frontal-Delaunay work combines advancing-front point placement with Delaunay robustness ([arXiv:1606.01289](https://arxiv.org/abs/1606.01289)).
- *Fit to PolyMesh:* Extend `kPrismSweep` and `kHexPyramid`, consume curvature/wall-thickness fields and CAD normals, and certify transition cells with existing validity checks.
- *Killer risk:* Front collisions and termination around ribs/fillets can create exactly the slivers and invalid cells the method is intended to avoid.

**[WL-20] Frame-field/polycube hex meshing as an offline campaign challenger** — `meshing` · impact M · effort XL · risk H
- *What:* Optimize a volumetric octahedral frame aligned with CAD surfaces, principal stress/curvature directions, and thin-wall axes; integrate it into a parameterization and extract hexes. Run this offline in `testlab` until it consistently beats CDT rather than making it a product dependency.
- *Why it wins:* When successful, aligned hexes can deliver excellent bending and anisotropic accuracy per DOF; the frame itself is also a useful anisotropy field for other meshers.
- *Prior art:* Palmer, Bommes & Solomon, “Algebraic Representations for Volumetric Frame Fields” ([arXiv:1908.05411](https://arxiv.org/abs/1908.05411)); Gregson, Sheffer & Zhang, “All-Hex Mesh Generation via Volumetric PolyCube Deformation” ([DOI](https://doi.org/10.1111/j.1467-8659.2011.02015.x)).
- *Fit to PolyMesh:* New offline mesher/field optimizer fed by `CadTopology` and stress/curvature directions, with output scored by the campaign Pareto pipeline.
- *Killer risk:* Field singularities and integer-grid parameterization failures make general automatic all-hex meshing an open problem, not a normal feature ticket.

**[WL-21] Represent the discretization as a queryable field and lineage DAG** — `representation / workflow` · impact M · effort L · risk M
- *What:* Store stable geometric entity IDs, refinement parent/child lineage, local basis/order, CAD provenance, and lazy field evaluators `value(x)`, `stress(x)`, `error(x)`, `cell(x)` rather than treating each remesh as an unrelated static array. Materialize GUI buffers and export arrays only on demand.
- *Why it wins:* Solution/error transfer, Krylov recycling, progressive rendering, cache reuse, and reproducibility become first-class instead of repeated nearest-neighbour hacks and copies.
- *Prior art:* `NONE FOUND — original proposal` (adaptive mesh forests and field APIs exist; the proposed CAD-to-result lineage contract across PolyMesh’s heterogeneous remeshers is the original composition).
- *Fit to PolyMesh:* Refactor `mesh::PolyMesh`, `pipeline::Model`, result storage in `apps/gui/main.cpp`, and adapt/remesh outputs around immutable IDs plus lazy evaluators.
- *Killer risk:* A sweeping data-model refactor can delay numerical improvements and increase memory if lineage is retained indiscriminately.

**[WL-22] Observed-order campaign gates on real curved and singular benchmarks** — `verification` · impact H · effort M · risk L
- *What:* Beyond MMS, maintain benchmark families with known asymptotic behavior: smooth curved elasticity, bending-dominated beams, near-incompressibility, re-entrant singularities, thin walls, and mixed meshes. Regress slopes, effectivity, monotonic QoI behavior, and cost-to-tolerance across commits.
- *Why it wins:* It catches regressions that still pass a final-value tolerance and makes claims such as “$p=3$ improves accuracy” reproducible over a family, not a showcase part.
- *Prior art:* Roache et al. describe MMS plus systematic refinement as a theorem-like code-verification process ([ASME article](https://asmedigitalcollection.asme.org/fluidsengineering/article/124/1/4/462791/Code-Verification-by-the-Method-of-Manufactured)).
- *Fit to PolyMesh:* Extend `apps/testlab`, `bench/reference/*.json`, `results.jsonl`, and `scripts/analyze_campaign.py` with robust log-slope/effectivity gates and confidence intervals.
- *Killer risk:* Pre-asymptotic meshes can make valid code appear to have the wrong order unless the gate identifies the asymptotic range honestly.

**[WL-23] Independent-solver triangulation plus metamorphic invariants** — `verification` · impact H · effort M · risk L
- *What:* For each verification case, compare PolyMesh to CalculiX and, where feasible, a tiny independent reference implementation; also rerun after rigid rotation, translation, unit scaling, node permutation, and equivalent load decomposition. Require invariant energies/reactions and transformed displacement/stress fields, not just one matching scalar.
- *Why it wins:* Cross-code agreement catches PolyMesh-specific defects, while metamorphic transforms catch shared-input and coordinate-frame bugs that code-to-code agreement can miss.
- *Prior art:* `NONE FOUND — original proposal` (code-to-code comparison and metamorphic testing are established separately; this specific invariant ensemble for hybrid CAD FEA is the proposal).
- *Fit to PolyMesh:* Use the existing CalculiX wiring and campaign runner; add transform generation, canonical field comparison, and provenance to `result.json`.
- *Killer risk:* Two codes can agree because they share the same modeling mistake, especially in BC interpretation and stress extrapolation.

**[WL-24] An evidence ledger, not a magical “trust score”** — `product / honesty` · impact H · effort M · risk M
- *What:* Display a decomposed result card: equilibrium residual, rigid-body constraint status, mesh validity, geometry deviation, algebraic tolerance, estimator/effectivity status, refinement trend, cross-code/MMS coverage, and assumption violations. A traffic-light summary may exist, but every component and rule must remain inspectable and versioned.
- *Why it wins:* Engineers can distinguish “solver converged” from “answer credible,” and PolyMesh can support claims with a machine-readable audit trail rather than a proprietary scalar.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Aggregate evidence from `CadTopology`, mesh validity, `solve.cpp`, adapt/error estimators, campaign references, and GUI result panels; serialize the ledger beside every result.
- *Killer risk:* Collapsing heterogeneous evidence into one score invites false confidence and gaming; the decomposition must be primary.

**[WL-25] Adversarial BC sanity checking before solve** — `workflow / correctness` · impact H · effort M · risk L
- *What:* Diagnose empty/near-zero selected support area, supports collapsing to points/curves, duplicate/conflicting constraints, unbalanced loads, unintended free rigid modes, load/support overlap, and high sensitivity to CAD selection tolerance. Perturb selection tolerances and report whether the constrained faces and reactions are stable.
- *Why it wins:* Preventing a plausible-looking solve of the wrong boundary-value problem beats any mesh improvement, and directly targets the known curved-part default-BC degeneration.
- *Prior art:* `NONE FOUND — original proposal` (automated CAD–CAE assignment exists, but the perturb-and-prove adversarial audit is the original part).
- *Fit to PolyMesh:* Add checks around B-rep face-tag/selection-box BC resolution in `pipeline::Model`, a six-rigid-mode rank test before `solve_elastostatics`, and clear GUI blockers/warnings.
- *Killer risk:* Aggressive warnings can block legitimate mechanisms or symmetry models unless users can supply explicit intent.

**[WL-26] Progressive preview and QoI bracketing while the fine solve runs** — `workflow` · impact M · effort M · risk L
- *What:* Immediately solve a coarse, robust CDT/finite-cell model, stream mesh and displacement updates, then launch adaptive stages that preserve a stable result lineage. Display QoI history and estimated interval versus elapsed time rather than replacing one opaque final mesh with another.
- *Why it wins:* Users discover bad BCs/materials in seconds, and can stop when the engineering QoI—not an arbitrary mesh tier—has stabilized.
- *Prior art:* `NONE FOUND — original proposal` (multilevel solvers are established; the product-level progressive QoI/credibility stream is the original composition).
- *Fit to PolyMesh:* Orchestrate `pipeline::Model`, `adapt::drive_hp`, background solver jobs, lineage fields, and `apps/gui/main.cpp`; reuse each level as the next initial guess.
- *Killer risk:* An attractive coarse preview can anchor users on a wrong answer unless uncertainty and provisional status are impossible to miss.

**[WL-27] Content-addressed solve cache plus exact local reanalysis** — `workflow / performance` · impact M · effort M · risk M
- *What:* Hash normalized B-rep topology/geometry, material, BCs, loads, mesher version/seed, discretization, and solver policy; reuse meshes, symbolic factorizations, preconditioners, and solutions at the narrowest valid level. For local non-topological changes, update only affected factor-tree paths or use low-rank/reanalysis methods.
- *Why it wins:* Parameter sweeps and GUI iteration stop paying full CAD→mesh→assemble→solve cost when most inputs are unchanged.
- *Prior art:* “An exact reanalysis algorithm for local non-topological high-rank structural modifications in finite element analysis” updates only affected factor-tree paths ([ScienceDirect primary article](https://www.sciencedirect.com/science/article/abs/pii/S0045794914001588)).
- *Fit to PolyMesh:* Add canonical hashing/provenance across `geom`, `pipeline`, `fea`, and `apps/testlab`; cache only immutable artifacts with explicit version keys.
- *Killer risk:* A false cache hit caused by incomplete normalization is a silent wrong-answer defect worse than no cache.

**[WL-28] Adjoint design sensitivities as a normal result field** — `workflow / design` · impact H · effort L · risk M
- *What:* For each differentiable QoI, compute sensitivity to load magnitude, elastic constants, thickness/feature parameters, and selected CAD control parameters using the same adjoint introduced for DWR. Expose gradients with validity warnings near topology changes and nonsmooth maxima.
- *Why it wins:* One adjoint gives derivatives with cost largely independent of parameter count, turning PolyMesh from a one-shot analyzer into a design-iteration engine.
- *Prior art:* The adjoint mechanism and local sensitivity interpretation are central to Becker & Rannacher’s DWR framework ([DOI](https://doi.org/10.1017/S0962492901000010)).
- *Fit to PolyMesh:* Differentiate load/material assembly first, then OCC parametric features; add sensitivity fields to result JSON/GUI and finite-difference verification cases to `testlab`.
- *Killer risk:* CAD topology changes make shape derivatives discontinuous, so gradients can be numerically precise and operationally misleading.

**[WL-29] Estimator disagreement as an uncertainty auction for DOFs** — `adaptivity / original` · impact H · effort M · risk M
- *What:* Run cheap, diverse signals—ZZ, hierarchical correction, DWR, residual jumps, geometry error, conditioning, and ML-advisor uncertainty—and allocate the next DOF budget to regions/actions with both high predicted benefit and high disagreement. Record which signal won each “auction” and whether the realized QoI improvement justified it.
- *Why it wins:* No single imperfect estimator dominates outside its regime; disagreement becomes actionable epistemic uncertainty and the campaign produces calibration data automatically.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Replace hand-set `drive_hp` weights in `src/adapt/src/hp_driver.cpp` with a budgeted policy fed by all estimator channels and evaluated through `apps/testlab` successive halving.
- *Killer risk:* Without strict online calibration, the auction can become a complicated heuristic that is harder to trust than the existing weights.

**[WL-30] Randomized residual probes for probabilistic whole-field error checks** — `error control / weird` · impact M · effort L · risk M
- *What:* Solve a small fixed set of random adjoint problems and project the primal residual onto them to estimate error norms or many QoIs with explicit high-probability bounds. Use it as an independent smoke detector and for batch parameter studies, not as a substitute for deterministic certification.
- *Why it wins:* A handful of probes can detect error components missed by ZZ without solving one adjoint per output, and the probability statement is more honest than an unexplained ML confidence.
- *Prior art:* Smetana, Zahm & Patera, “Randomized residual-based error estimators for parametrized equations” ([arXiv:1807.10489](https://arxiv.org/abs/1807.10489)).
- *Fit to PolyMesh:* Reuse the adjoint/operator interface and Krylov recycling; store deterministic RNG seeds and probabilistic coverage in campaign/result metadata.
- *Killer risk:* Reduced/random dual spaces trained on the wrong problem distribution can give tight-looking but poorly calibrated estimates.

**[WL-31] Optimize the whole discretization recipe per part with CMA-ES—offline only** — `meshing / optimization` · impact M · effort M · risk H
- *What:* Treat mesher family, seed placement, size-field parameters, element tendency, $p$ map, solver policy, and tolerances as a mixed black-box vector; minimize measured QoI error/time/invalidity over a fixed budget using CMA-ES or Bayesian optimization. Train a cheap policy on the resulting campaign archive, but never run hundreds of production solves by default.
- *Why it wins:* It can discover non-obvious interactions among knobs that hand-tuned `drive_hp` weights and one-factor sweeps miss.
- *Prior art:* Jacobian-based mesh-quality optimization is established ([OSTI primary report](https://www.osti.gov/biblio/3213/)); `NONE FOUND` for jointly optimizing PolyMesh’s heterogeneous mesher+$h/p$/shape+solver recipe, so that composition is original.
- *Fit to PolyMesh:* Wrap the existing `apps/testlab` campaign space and Pareto analyzer, using reference/MMS labels and strict compute budgets on the workstation.
- *Killer risk:* Per-part search spends far more solves than it saves and overfits to an unavailable “truth” reference.

**[WL-32] Neural/operator warm starts that can never change the final equations** — `ML / solver` · impact M · effort L · risk M
- *What:* Train on PolyMesh’s own campaign solutions to predict a prolongated displacement or low-frequency error correction, then feed it only as the initial guess/optional nonlinear preconditioner to CG/FCG. The residual-based FP64 solver remains authoritative and falls back to zero start when prediction uncertainty is high.
- *Why it wins:* Repeated part families and parameter sweeps may shed Krylov iterations while preserving the exact discrete equations and deterministic convergence test.
- *Prior art:* Chen, “Graph Neural Preconditioners for Iterative Solutions of Sparse Linear Systems” ([arXiv:2406.00809](https://arxiv.org/abs/2406.00809)).
- *Fit to PolyMesh:* Export graph/operator features from assembled systems, train offline from `results.jsonl`, and integrate through `solve.cpp` behind a measured iteration-count gate.
- *Killer risk:* Classical AMG/recycling may outperform it with none of the distribution-shift, model-loading, and reproducibility burden.

**[WL-33] A dual-mesh mimetic shadow solve as an independent conservation check** — `weird / verification` · impact M · effort XL · risk H
- *What:* Construct a primal/dual complex from selected polyhedral meshes and solve a low-order mimetic elasticity-like equilibrium problem whose discrete divergence/gradient identities differ from the primal FEM/VEM implementation. Compare reactions, energy, and coarse displacement/QoIs as an independent shadow result rather than replacing the production solver.
- *Why it wins:* Agreement across fundamentally different discrete operators is stronger evidence than two element variants sharing assembly and quadrature code; local conservation defects become visible.
- *Prior art:* Lipnikov et al., “Mimetic finite difference method,” reviews conservation-, symmetry-, positivity-, and duality-preserving schemes on polygonal/polyhedral meshes ([DOI:10.1016/j.jcp.2013.07.031](https://doi.org/10.1016/j.jcp.2013.07.031)).
- *Fit to PolyMesh:* Reuse `mesh::PolyMesh` face/cell incidence and VEM geometry, add a separate mimetic operator path, and run it only on campaign/verification tiers.
- *Killer risk:* Stable 3D linear elasticity on arbitrary duals is difficult, and a crude shadow solver may disagree for discretization reasons too large to diagnose.

**[WL-34] CAD-tolerance and selection-jitter ensembles** — `uncertainty / workflow` · impact M · effort M · risk L
- *What:* Re-import and solve a small ensemble under controlled OCC sewing tolerances, tessellation tolerances, BC-selection tolerances, and sub-resolution feature suppression. Report whether the QoI is numerically stable to ambiguity in the actual CAD-to-analysis pipeline.
- *Why it wins:* It exposes a neglected uncertainty source: two apparently identical analyses can differ because topology repair or face selection changed, not because the FE mesh improved.
- *Prior art:* `NONE FOUND — original proposal`.
- *Fit to PolyMesh:* Parameterize STEP/BREP import and BC selection, use content-addressed caches/reanalysis, and add a “CAD/selection sensitivity” band to campaign and GUI evidence ledgers.
- *Killer risk:* The ensemble can be expensive and may confuse numerical sensitivity with legitimate changes in the interpreted solid.

**[WL-35] Pre-solved CAD-feature operators with certified remainder coupling** — `representation / model reduction` · impact H · effort XL · risk H
- *What:* Recognize recurring features such as holes, fillets, bosses, ribs, and thin ligaments, replace a feature neighborhood by a precomputed boundary-to-boundary Schur/influence operator parameterized by geometry/material, and couple it to the surrounding FE mesh. Require an online residual/CRE check that rejects the reduced operator when the surrounding load state lies outside its certified envelope.
- *Why it wins:* Repeated local 3D fields are paid for once, while the global solve sees only interface DOFs; unlike a raw surrogate, the online check can force a full local discretization when needed.
- *Prior art:* `NONE FOUND — original proposal` for parametric CAD-feature operators with online certified rejection; it is related to substructuring/model reduction but materially stronger than a lookup table.
- *Fit to PolyMesh:* Feature recognition in `CadTopology`, an offline `testlab` generator using the working solver for labels, interface Schur coupling in FEA, and a fallback that restores ordinary meshing without changing the problem.
- *Killer risk:* Feature response depends strongly on nonlocal boundary conditions and neighboring features, so the certified validity domain may be too small to provide useful reuse.

## Top-5 shortlist with implementation sketches

### 1. WL-01 — MMS compiler and observed-order gates
1. Define symbolic displacement families that exercise normal/shear coupling and non-axis-aligned coordinates; derive body force and traction automatically.
2. Start on cubes/tets with exact planar geometry, then add a sphere/cylinder for CAD-boundary tests.
3. Run 4–6 deterministic refinement levels for tet4/tet10, hex8/hex20, VEM $k=1/2$, and `HpModel`; compute $L^2$, energy, and stress errors by independent quadrature.
4. Fit slopes with a declared asymptotic-window rule; fail CI on slope loss, not harmless constant-factor noise.
5. Publish generator input, expected order, raw errors, mesh sizes, and regression fit in campaign artifacts. This is medium effort and should precede every ambitious numerical claim.

### 2. WL-02 — conforming hierarchical $hp$ production cutover
1. Inventory every production callsite of nodal promotion and route one end-to-end solve through `HpModel`.
2. Enforce shared edge/face order and orientation maps; treat unsupported hybrid/poly interfaces as explicit $p=1$ boundaries or reject them—never silently mismatch.
3. Add static condensation (WL-08) immediately enough that $p>2$ does not create an absurd global system.
4. Prove each supported $p$ with WL-01, patch tests, rigid-body invariance, and adjacent unequal-order cases.
5. Remove the nonconforming promotion path only after result export, stress evaluation, BC application, and adaptivity all consume the hierarchical solution.

### 3. WL-03 — DWR QoI adaptivity
1. Ship a small QoI DSL: displacement component/average, compliance, reaction, and area-averaged stress; explicitly exclude raw pointwise maxima.
2. Implement the adjoint using the same symmetric stiffness operator and a separately assembled QoI derivative.
3. Compute enriched adjoints on $p+1$ or a refined overlay; include volume residuals, interelement traction jumps, Neumann mismatch, and VEM consistency terms.
4. Localize signed contributions, mark by absolute contribution while retaining cancellation diagnostics, and compare against uniform refinement.
5. Calibrate effectivity on MMS/reference cases before exposing an “estimated QoI error”; DWR is targeted, not automatically guaranteed.

### 4. WL-04 — exact/curved CAD boundary geometry
1. Preserve `CadFace` ID and $(u,v)$ provenance for every boundary facet and node.
2. Implement OCC surface evaluation and derivatives in boundary-element quadrature, beginning with non-seam, simply trimmed faces.
3. Generate/project high-order boundary nodes and run Jacobian/inversion guards; fall back to lower geometry order when guards fail, with a visible warning.
4. Separate geometry order from solution $p$ and include geometry residual/deviation in the error budget.
5. Demonstrate expected high-order convergence on spherical/cylindrical MMS before claiming general trimmed-STEP support.

### 5. WL-05 — certified QoI intervals
1. Limit the first target to linear isotropic elasticity on conforming tets with polynomial loads and a regularized scalar QoI.
2. Build local patch reconstructions that satisfy equilibrium and interelement traction continuity; numerically verify admissibility identities.
3. Compute primal energy bounds, then primal/adjoint CRE terms for two-sided QoI bounds.
4. Add algebraic-solver and quadrature stopping checks so omitted errors cannot exceed the displayed interval; declare CAD geometry error separately until curved certification exists.
5. Expand element families only after bound containment and effectivity are demonstrated against MMS exact solutions. Expect XL effort: the first honest narrow certificate is more valuable than a broad fake guarantee.

**Which missing meshing algorithm changes the ceiling most?** WL-06, a protected Frontal/constrained-Delaunay tet mesher. It is less glamorous than general all-hex meshing, but it applies to far more STEP parts, provides the robust refinement substrate needed by $h/p$/DWR/certification, and creates a trustworthy baseline. Frame-field hex meshing can win spectacularly on suitable parts, but its failure rate and implementation risk make it an offline challenger, not the ceiling-raising foundation.

## Do not bother—at least not yet

- **General automatic all-hex as a product promise:** frame singularities, parameterization, and junction topology remain too fragile; first extract sweepable sheets and keep CDT at junctions.
- **A learned preconditioner before matrix-free $p$-multigrid/AMG and Krylov recycling:** classical baselines are more deterministic, easier to debug, and likely better on the available training volume.
- **CMA-ES on every user solve:** it converts one analysis into hundreds. Use it offline to discover policies, never as the default mesher.
- **A neural operator as the final displacement/stress answer:** it weakens evidence-first claims. Restrict learning to warm starts, proposal ranking, or reduced feature operators with residual-based rejection.
- **FP16/FP32 hidden inside the current “double-only” policy:** no silent exception. Benchmark an ADR-governed low-precision preconditioner only after the FP64 algorithm is correct.
- **“Certified peak nodal stress” at sharp corners, point loads, or point supports:** the continuum quantity may be singular or mesh-dependent; certify an averaged/hot-spot/singularity-amplitude QoI instead.
- **Full arbitrary-STEP isogeometric analysis:** volumetric parameterization and trimmed coupling are disproportionate. NEFEM-style exact boundary treatment captures much of the value earlier.
- **A wholesale dual-mesh solver rewrite:** useful as an independent campaign shadow solver, but not competitive with fixing conformity, geometry order, and error control in the existing solver.
- **More ZZ tuning:** changing patch radii or fit weights cannot turn recovery into a guarantee and risks overfitting benchmark effectivity.
- **GPU sparse-direct work on the RTX 2070:** VRAM and weak FP64 make it a poor target. Invest in matrix-free kernels and CPU coarse/direct solves instead.

## The single biggest credibility improvement

The single change that would most improve the **credibility** of PolyMesh’s accuracy claims is **WL-01: an automated, published MMS verification matrix with observed-order CI gates across every production element, geometry, BC, stress, and adaptivity path**. A certified per-result interval is ultimately stronger, but only after the reconstruction and solver have themselves been verified; today, MMS gives the fastest independent evidence that PolyMesh solves the equations it claims at the order it claims, catches small implementation defects that benchmark answers miss, and produces raw, reproducible evidence outsiders can audit. The credibility rule should be blunt: an element/mesher/order path that lacks an MMS convergence record may be experimental, but it may not be marketed as accurate.
