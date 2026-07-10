# ADR-0013: Hex core + pyramid skin transition mesher

- Status: accepted (2026-07-10); product FE path amended (C1, 2026-07-10)

## Decision
`transition_fill_surface` / `VolumeMesher::kHexPyramid`:
- Interior lattice cells → hex8 (topology)
- Boundary lattice cells → 6 pyramid5 (apex at cell center, bases = hex faces)

## Why
Hex–tet faces are non-conforming. Pyramid bases are quads, so interior
hex–pyramid interfaces are conforming **nodal** matches. Boundary quads remain
available for BC region mapping.

## Nonconformity (why native hex8 + pyramid5 fails the patch test)
Pyramid5 stiffness is the sum of **two tet4** stiffnesses (base diagonal
0–2 + apex). Hex8 stays true isoparametric trilinear (GATE 1 freeze). On a
shared face the hex side is bilinear while the pyramid side is piecewise
linear on two triangles; nodal forces for constant stress do not cancel, so a
joint constant-strain patch fails (typical max error ~1e-5 on a small lattice).

Pure-hex (Tier-0) and pure-pyramid lattices pass independently.

## Product FE path (C1 honesty)
`expand_hex_core_to_pyramids` converts every interior hex8 into **six pyramid5**
with apex at the cell centroid, using the same face order and base-diagonal
convention as the skin. Pipeline `VolumeMesher::kHexPyramid` always runs this
expand before emitting `fea::NodalMesh` elements.

Result: the FE mesh is all-pyramid5 (tet-split stiffness) with matching face
diagonals across former hex–pyramid interfaces. Constant-strain patch on a
mixed lattice (nonempty hex core + pyramid skin) is exact (max error < 1e-12).

Display / mesher notes still report lattice hex count; the solve mesh is the
expanded product path. GATE-1 pure hex8 is unchanged.

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

### Jacobian safety (B3)
After the limited snap, any moved node that participates in a non-positive
cell is **reverted** to its pre-snap lattice position (iterate to fixed point):
- **Hex8:** non-positive isoparametric Jacobian det at reference centre or at
  any of the 8 product-Gauss points used by assembly.
- **Pyramid5:** either tet of the base-diagonal split (0–1–2–apex / 0–2–3–apex)
  has |volume| ≤ ε (collapsed). Assembly may flip orientation; zero volume is
  the hard fail.

If a cell remains inverted after all offenders are unsnapped, the mesher
throws `ValidityError` rather than hand the solver a bad element.
