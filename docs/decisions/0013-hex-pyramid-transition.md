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
Pyramid5 stiffness is the sum of **two tet4** stiffnesses (base diagonal
0–2 + apex), with orientation flip applied to both the tet connectivity and
the local DOF scatter map. Hex8 stays true isoparametric trilinear (GATE 1
freeze).

## Patch test / hybrid nonconformity
- Pure pyramid lattices pass the constant-strain patch test exactly.
- Hex–pyramid hybrids do **not** (yet): hex faces are bilinear while
  tet-split pyramids present two linear triangles, so constant-stress nodal
  forces disagree on shared faces. Engineering rule #2 is enforced on each
  element type in isolation (hex via Tier-0, pyramid via pure-pyramid patch);
  a conforming hybrid formulation (matching Kuhn diagonals on both sides, or
  mortar) is future work.

## Orientation
Mesher orders pyramid bases so the apex lies on the +normal side of the
right-handed base winding (isoparametric Jacobian positive if that path is
used for body-load quadrature).

## Surface snap
Free-boundary lattice nodes (pyramid base corners on exterior faces) are
optionally pulled toward the STL by at most `0.35 h` (same budget as tet
fill). Pyramid apices stay at cell centers so hex–pyramid interfaces remain
conforming. Residual max distance is reported in `boundary_max_distance`
and the pipeline mesher note.
