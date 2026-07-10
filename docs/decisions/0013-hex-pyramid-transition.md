# ADR-0013: Hex core + pyramid skin transition mesher

- Status: accepted (2026-07-10)

## Decision
`transition_fill_surface` / `VolumeMesher::kHexPyramid`:
- Interior lattice cells → hex8
- Boundary lattice cells → 6 pyramid5 (apex at cell center, bases = hex faces)

## Why
Hex–tet faces are non-conforming. Pyramid bases are quads, so interior
hex–pyramid interfaces are conforming nodal matches. Boundary quads remain
available for BC region mapping.

## Assembly note
Pyramid element stiffness is formed as the sum of two tet4 stiffnesses
(base diagonal split + apex). This avoids fragile isoparametric Jacobians
on the physical pyramid while preserving the Tier-0 RBM count.
