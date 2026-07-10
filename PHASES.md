# PHASES — Guided DAG Plan

Legend: ⛔ GATE = stop, summarize, wait for human approval before proceeding.
Each phase runs the `/loop` command internally and must pass `/audit` at its gate.

```
P0 ──> P1 ──> P2 ──┬──> P3 ──> P5 ──> P6 ──> P6.5 ──> P7
                   └──> P4 ──────┘
```

## P0 — Decisions & scaffolding
- Resolve Open Decisions D1–D5 in SPEC.md (human input required).
- Cargo workspace, CI (fmt, clippy, test), VTU export stub, PROGRESS.md.
- ⛔ GATE 0: decisions ratified, repo skeleton reviewed.

## P1 — Reference solver on standard elements (the trustworthy baseline)
- Linear elasticity assembly + direct sparse solve for tet4/tet10, hex8/hex20.
- Tier-0 gates green; Tier-1 Lamé cylinder + Timoshenko cantilever passing on
  externally generated meshes (e.g. Gmsh via file import — we don't mesh yet).
- MMS harness with seeded random fields; convergence-order verification tooling.
- ⛔ GATE 1: convergence plots reviewed. This baseline is frozen as the
  benchmark comparator — later phases may not modify it, only add alongside.

## P2 — Mesh core + tet meshing + validity
- Face-based polyhedral mesh data structure (per D4), validity checker,
  Delaunay/frontal tet generation from STL, sizing-field interface.
- Full Tier-1 suite passing end-to-end (our mesh → our solver).
- ⛔ GATE 2.

## P3 — Geometric feature analysis → a priori hybrid meshing
- Discrete feature detection: dihedral-angle sharp edges, curvature, thin-wall
  proximity. Region classification → element-type + sizing field.
- Hex/prism regions where sweepable, pyramid/polyhedral transitions, geometric
  grading toward detected edges/corners.
- Exit: Kirsch + L-domain peak-stress error beats uniform tet baseline at equal DOFs.
- ⛔ GATE 3.

## P4 — Polyhedral elements (VEM) [parallel with P3 after P2]
- VEM k=1,2 for arbitrary polyhedra behind the same `Element` trait;
  stabilization; Tier-0 eigenvalue checks; MMS convergence at k=1,2.
- ⛔ GATE 4.

## P5 — Adaptive loop (the product)
- ZZ error estimation, Dörfler marking, local h-refinement with polyhedral
  transition handling, p-elevation on smooth regions, stopping criteria,
  automatic settings selection.
- Exit: Tier-3 targets met on the public geometry suite (≥5× DOF, ≥3× time).
- ⛔ GATE 5: full `/audit` including holdout geometries.

## P6 — Performance engineering
- rayon parallel assembly, iterative solver + AMG for large N, memory profiling,
  mesh generation profiling. GPU acceleration (wgpu compute) for assembly and
  iterative solves where it wins, f64 only (ADR-0003).
  No accuracy regressions permitted (full suite re-run).
- ⛔ GATE 6.

## P6.5 — GUI (ADR-0006)
- `gui` crate: wgpu 3D viewport + egui panels, dark CAD-style theme matching
  the owner's desktop CAD app (theme tokens in one module, tuned against
  screenshots at phase start).
- Geometry import, mesh preview colored by element type/order, load/BC setup,
  run control, stress + error-field visualization. No physics logic in `gui`.
- ⛔ GATE 6.5: owner reviews look/feel against the CAD app.

## P7 — OSS release readiness
- License finalized (D2), API docs, examples, CONTRIBUTING.md, benchmark report
  published in README with reproduction instructions.
- ⛔ GATE 7: ship.
