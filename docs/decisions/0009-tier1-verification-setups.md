# ADR-0009: Tier-1 analytical verification setups (Kirsch / Goodier / L-domain)

- Status: accepted (2026-07-10, P1 / GATE 1)
- Decision: how each remaining Tier-1 closed-form case is posed for the
  frozen P1 baseline solver

## Decision

### Kirsch plate (SCF = 3)
Quarter annular plate, hex20 mesh in (r, θ, z) parameter space mapped to a
circular hole, **exact Kirsch traction** on the outer arc, free hole, symmetry
on the cut planes, plane-strain `u_z = 0`. Using the continuum traction field
as the Neumann data makes the infinite-plate solution exact on the finite
annulus, so residual error is pure discretization (no domain truncation).

**C5 / equal-DOF grading (2026-07-10):** GATE 3 peak-stress claim uses the same
annular BC setup on **tet10** with two a priori radial maps at identical
topology (exact equal free DOFs): linear `r` (uniform baseline) vs logarithmic
`r = a (b/a)^ξ` (feature-aware concentration at the hole). Product Cartesian
tet fill is not used here — it stair-cases the hole (ADR-0015). Test:
`tests/test_kirsch_c5_graded.cpp`.

### Goodier cavity (SCF = 3(9−5ν)/(2(7−5ν)))
Spherical-shell octant, hex20 with **logarithmic radial spacing** (nodes
clustered at the cavity), polar angle starting above zero to keep Jacobians
positive near the pole. Remote uniaxial strain corresponding to `σ_zz = T` is
imposed as Dirichlet on the outer sphere (Saint-Venant stand-in for the
infinite body); symmetry on the coordinate planes; cavity free. Peak
tangential stress is recovered by nodal averaging. P1 acceptance: relative
SCF error ≤ 12% at `b/a = 15`. Tighter checks (exact Goodier-field BCs, ZZ
recovery) are deferred to P2+/P5.

### L-domain (Williams λ ≈ 0.5445)
L-shaped prism built as two glued hex blocks, hex20, fixed wall on `x = 0`,
uniform traction on the free end, plane strain. No closed-form energy is
available; verification is **self-convergence of strain energy** under uniform
h-halving. Conforming Galerkin energy increases monotonically; successive
energy gaps converge at rate `2λ ≈ 1.09` (±0.35 on the coarse meshes used at
P1). Peak von Mises at the re-entrant corner must grow under refinement
(unbounded continuum singularity).

### Gmsh import
ASCII `.msh` format 2.2 only (file-type 0). Volume types tet4/10 and hex8/20;
surface types tri3/6 and quad4/8 with physical-group tags preserved for BC
selection. Node ordering matches `fea/nodal_mesh.hpp` for these types. Binary
and v4 meshes are rejected with a clear error — external generation can emit
2.2 ASCII.

## Alternatives
- Rely solely on externally generated Gmsh files for every Tier-1 case: blocked
  until a system `gmsh` binary is part of the dev toolchain; structured
  parametric meshes keep the suite self-contained and deterministic.
- Exact Goodier-field Neumann data on a moderate shell: preferred long-term,
  deferred because the full stress-field bookkeeping is large and the
  Saint-Venant Dirichlet setup already hits the P1 bar.
- Plane-stress thin plate for Kirsch with free top/bottom: more faithful to
  the classical statement, but mid-plane-only z-restraint left the load path
  fragile; plane strain shares the same in-plane Airy stresses and matches the
  Lamé test pattern.

## Why
P1 freezes the baseline solver. Each case must (a) load reference values only
from `bench/reference/*.json`, (b) demonstrate the physics the benchmark is
meant to stress (concentration / singularity / curved body), and (c) pass
deterministically without external meshing tools. The setups above meet those
constraints with the current element zoo and stress recovery.
