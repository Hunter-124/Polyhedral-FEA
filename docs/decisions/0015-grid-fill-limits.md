# ADR-0015: Cartesian grid-fill limits (not Delaunay / frontal)

- Status: accepted (2026-07-10)
- Track: B1 (documented limits; true Delaunay deferred)

## Context
Product meshers (`tet_fill_surface`, `hex_fill_surface`, `graded_tet_fill_surface`,
`transition_fill_surface`, `prism_fill_surface`) fill a **Cartesian lattice** over
the axis-aligned bbox of a closed triangle surface, classify cells inside/outside
by ray cast, and emit tet4 / hex8 / pyramid5 / prism6 connectivity. Optional
limited surface snap (≤ 0.35 h) pulls boundary lattice nodes toward the STL with
Jacobian unsnap safety (B3 / ADR-0013).

This is **not** constrained Delaunay, advancing-front, or CAD-aware meshing.
`prism_fill_surface` / `VolumeMesher::kPrismSweep` is **not** CAD extrusion
detection: it is an AABB lattice of prism6 wedges along the longest bbox axis.

## Decision
Document and ship Cartesian grid fills as the v1 product path. Do **not** claim
analytical mesh quality, boundary-fitted DOF efficiency, or Tier-1 accuracy on
*our* mesh until a true volume mesher (or imported Gmsh/meshio path) exists.

## What the fills are
| Path | Algorithm | Boundary |
|------|-----------|----------|
| tet | Kuhn 6-tet split of inside voxels | Stair-cased + limited snap |
| hex | Inside voxels → hex8 | Stair-cased (no snap yet) |
| graded tet | Fine skin / coarse core on lattice | Stair-cased (no snap) |
| hex+pyramid | Interior hex, boundary cell → 6 pyramids | Limited snap + unsnap |
| prism sweep | Inside voxels → 2× prism6 (base diag), sweep = longest axis | Stair-cased (no snap) |

## Lattice construction (bbox-fitted)
`make_bbox_grid` sets \(n_a=\lceil L_a/h\rceil\) and \(\Delta_a=L_a/n_a\) so the
lattice **exactly** spans the AABB (nodes land on bbox faces). Cell sizes may be
anisotropic when the requested \(h\) does not divide an axis. This removes the
systematic underfill gap when \(n h > L\) left a partial exterior layer empty.

**Cell budget:** default max is \(512\cdot 1024\) cells. If the requested \(h\)
would exceed that (common for graded tet, which builds a fine lattice at
\(h/2\)), the grid is **auto-coarsened** rather than throwing
`grid too fine`. Graded fill also pre-floors \(h\) via `min_h_for_cell_budget`
(subdivision=2). Mesher notes may say `h raised to cell budget`.

## Ray parity (shared-edge dedupe)
Inside tests use even-odd ray casting. Coplanar face diagonals / shared edges
used to register **two** identical crossings, flipping parity and punching
diagonal tunnels through cubes and other AABB bricks (cells with \(c_x\approx c_y\)
marked outside). Crossings are now **deduplicated** within a small z-ε so each
surface hit counts once (`mesh/grid_classify.hpp`).

## Staircasing and when they fail
- **Staircasing:** free surface follows lattice faces, not the CAD/STL, except
  where limited snap moves nodes ≤ 0.35 h (may still leave residual gap ~O(h)
  on *curved* surfaces; AABB bricks fill solid with exact bbox coverage).
- **Feature loss:** thin walls, fillets, and re-entrant corners thinner than ~h
  are under-resolved or dropped (inside test uses cell centres). Thin plates
  with thickness \(t<h\) still mesh when \(n_z=\lceil t/h\rceil\ge 1\) and
  \(\Delta_z=t/n_z\).
- **Empty / open surfaces:** ray parity fails → `ValidityError` or empty volume.
- **Non-watertight STL:** undefined interior; validate surface first.
- **High aspect domains:** uniform target h over bbox wastes DOFs in large empty
  exterior grid spans (still allocated as nodes only where cells are inside).
- **Not quality-optimal:** min dihedral / aspect not optimized; slivers can
  appear near jagged snaps (mitigated by unsnap, not by remeshing).

## What product / e2e may claim
**Allowed**
- Mesh is structurally valid: finite nodes, legal connectivity, **positive**
  tet volumes / hex centre Jacobian / pyramid tet-split volumes after snap.
- Deterministic for fixed (surface, h, mesher, snap flag).
- Smoke / fixture load → mesh → (optional) solve without inverted-element throws.
- GUI/CLI “mesh preview” and DOF counts on public fixtures.

**Not allowed (yet)**
- “Constrained Delaunay”, “boundary-fitted”, or “frontal” marketing language.
- Tier-1 analytical error bars (Lamé SCF, Kirsch, Goodier, L-domain energy gap)
  attributed to *product* grid meshes — those GATE 1 numbers use imported /
  structured reference meshes (ADR-0009).
- Claiming optimal adaptive rates or competitive DOF efficiency vs CalculiX on
  curved geometry until B1 true mesher or fair imported-mesh e2e (E4) lands.

## Follow-on
True constrained Delaunay / frontal tet (or robust external mesher import with
feature preservation) remains open under ROADMAP B1. Graded + feature sizing
and adapt seed remesh reduce DOFs but do not remove staircasing.

## Consequences
- Mesher notes and README state the grid-fill caveat in one line.
- Catch2 L-domain fixture test documents validity only (no analytical check).
- B1 closed as **documented limits** until a real volume mesher ships.
