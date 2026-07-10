# SPEC — Adaptive Hybrid Polyhedral Mesher + Co-Designed FEA Solver

## Problem statement
Existing open meshers treat meshing and solving as separate concerns. Uniform
tet meshes waste enormous compute on regions that a single large hex would
resolve to 99%+ accuracy, while under-resolving stress concentrations at edges,
corners, and fillets — exactly where results matter. We want a mesher that
reads the geometry, classifies regions by their geometric/physical criticality,
and automatically chooses element **shape, size, and polynomial order** per
region, iterating with solver feedback until a target accuracy is hit at
minimum cost.

**North star (owner, 2026-07-09):** an aggressively heterogeneous
hp-adaptive mesh co-optimized with its FEA solver — minimize error on complex
geometry where large elements can't quantify stress, maximize compute
performance in both meshing and solving, and offload to GPU wherever it wins
(f64, accuracy never traded away).

## Goals
1. **Hybrid element zoo**: tets, hexes, prisms/wedges, pyramids (transition
   elements), and general convex polyhedra in one conforming mesh.
2. **Geometry-driven a priori sizing**: automatic feature detection (sharp
   edges via dihedral angle, corners, high-curvature faces, thin walls /
   proximity) produces a sizing + element-type field with zero user tuning.
3. **Solution-driven a posteriori adaptivity**: solve → estimate error per
   element → refine h (size), p (order), or swap element type locally →
   re-solve, until a global energy-norm error target is met.
4. **Automatic settings**: the user supplies geometry + loads + a target
   accuracy; everything else is chosen by the system.
5. **Verified accuracy**: every capability is validated against closed-form
   analytical solutions and manufactured solutions with demonstrated
   convergence rates (see BENCHMARKS.md).
6. **Wins to demonstrate**: vs. a uniform 2nd-order tet baseline (our own
   solver, same tolerance), ≥5× fewer DOFs and ≥3× lower end-to-end wall time
   at equal or better energy-norm error on the benchmark geometry suite.

## Non-goals (v1)
- Nonlinear materials, plasticity, contact, dynamics — v1 is linear elastostatics
  (3D Hooke's law), because that's what our analytical verification suite covers.
- Shell/beam elements, CFD, thermal (design the element traits so these can come later).
- A GUI *during the solver phases*. The GUI is in scope for the v1 release but
  built after the adaptive core works (phase P6.5, ADR-0006); CLI + VTU export
  remain the automation path throughout.
- Distributed/MPI solves. Single node, multi-threaded; GPU acceleration of
  assembly/solves is a P6 performance-phase topic (ADR-0003).

## Architecture (pinned)
```
STEP/B-rep or STL in
   └─> geom: geometry kernel interface (see Open Decision D1)
         └─> feature analysis: dihedral angles, curvature, medial-axis proximity,
             small-feature detection  →  sizing field + region classification
               └─> mesh: hybrid generation
                     - structured/swept hex & prism regions where classification allows
                     - polyhedral/tet fill in irregular regions
                     - pyramids or polyhedral transition layers at interfaces
                     └─> fea: assembly + solve (linear elasticity)
                           └─> adapt: ZZ/residual error estimate per element
                                 └─> hp-refinement marks  →  back to mesh (local remesh)
                                       loop until error target or budget
```

## Key technical positions (pinned unless a phase proves otherwise)
- **Polyhedral elements use VEM (Virtual Element Method)** rather than trying
  to build shape functions on arbitrary polyhedra (Wachspress/mean-value
  coordinates are the fallback, but VEM handles non-convex cells and hanging
  nodes naturally and has clean hp theory). Tets/hexes/prisms use standard
  isoparametric FEM; the assembly layer treats both through one `Element` trait.
- **Error estimation**: Zienkiewicz–Zhu superconvergent patch recovery as the
  workhorse (cheap, effective for elasticity), with an explicit residual
  estimator as a cross-check in the audit.
- **Conformity between mixed element types**: prefer geometric conformity via
  transition elements (pyramids / polyhedral buffer cells). Hanging nodes only
  if the VEM path makes them trivially cheap.
- **Refinement decisions**: Dörfler marking on the error indicator; choose p vs h
  per element by local smoothness estimate (smooth → p-raise, singular/near-edge → h-refine).
- **Corner/edge singularities**: geometric grading toward re-entrant edges
  (a priori, from feature analysis) because uniform refinement can never
  recover optimal rates there — this is the core of the "critical parts get
  special treatment" idea from the original discussion.

## Decisions — ratified at GATE 0 (2026-07-09; full rationale in docs/decisions/)
- **D1 Geometry kernel** (ADR-0001): OpenCASCADE for B-rep/STEP behind the
  `POLYMESH_WITH_OCC` CMake option; STL loader + discrete feature detection
  always compiled and used for P1–P2.
- **D2 License** (ADR-0002): **AGPL-3.0-or-later**. Dual commercial licensing
  kept open; CLA/assignment policy needed before accepting external PRs.
- **D3 Element formulations** (ADR-0003): unified `Element` interface —
  isoparametric FEM at adaptive order p=1..4 on tets/hexes/prisms/pyramids,
  VEM k=1,2 on general polyhedra. Double precision only.
- **D4 Mesh data structure** (ADR-0004): face-based owner/neighbour.
- **D5 Benchmark baseline** (ADR-0005): our own uniform tet10 path, frozen at
  GATE 1, plus CalculiX cross-check inside `/audit`.
- **Language** (ADR-0007): C++20, CMake + Ninja, Eigen for linear algebra.
- **CUDA** (ADR-0008): optional runtime-selected backend for parallelizable
  kernels (batched stiffness, SpMV, error indicators); CPU path is the
  always-present reference, parity-tested against every GPU kernel.
- **GUI** (ADR-0006): in scope for v1, built as phase P6.5 after the adaptive
  core — native viewport, dark CAD-style theme matching the owner's CAD app.
