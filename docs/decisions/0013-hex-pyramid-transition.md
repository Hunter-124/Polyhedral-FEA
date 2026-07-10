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
on the physical pyramid while preserving the Tier-0 RBM count. ZZ recovery
uses the same tet split for centroid stress (isoparametric pyramid stress
is skipped).

## Surface snap
Free-boundary lattice nodes (pyramid base corners on exterior faces) are
optionally pulled toward the STL by at most `0.35 h` (same budget as tet
fill). Pyramid apices stay at cell centers so hex–pyramid interfaces remain
conforming. Residual max distance is reported in `boundary_max_distance`
and the pipeline mesher note.
