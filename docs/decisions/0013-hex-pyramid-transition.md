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

## Product FE path (pipeline)
`VolumeMesher::kHexPyramid` emits native `kHex8` + `kPyramid5` elements.
Hex stays GATE-1 isoparametric trilinear; pyramid stiffness is two tet4s.
The hybrid does not pass a joint constant-strain patch (bilinear hex face vs
piecewise-linear pyramid base). Isolated hex (Tier-0) and pure-pyramid
lattices (tests) do.

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
