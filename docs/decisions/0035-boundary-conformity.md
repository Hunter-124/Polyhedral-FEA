# ADR-0035: Boundary nodes belong on the BRep, not near it

- Status: accepted (2026-08-17)
- Supersedes the boundary-snap half of [ADR-0015](0015-grid-fill-limits.md)
  ("limited snap ≤ 0.75 h … may still leave residual gap ~O(h) on curved
  surfaces"). The staircase caveat stands; the residual does not.
- Companion to [ADR-0033](0033-a-gate-must-measure-what-ships.md) (a gate must
  measure the cell that ships) and [ADR-0034](0034-spectral-sizing-and-coarsening.md)
  (the Fourier tools this reuses).

## 1. The report

The defect was reported from the rendered figures, not from a metric: *round
surfaces have weird random defects, and edges are the same — a curved edge
looks chamfered instead of a straight 90°.* The renders were not lying.
`scripts/render_showcase.py` draws the mesh VTU verbatim
(`extract_surface(algorithm="dataset_surface")`, `smooth_shading=False`,
`show_edges=True`); there is no filter in the path that could invent or hide a
facet. What the figures showed was the mesh.

## 2. What was actually wrong

Four structural causes, none of which h can fix.

**(a) The snap target was the tessellation, not the BRep.** Every Cartesian
fill called `snap_boundary_nodes(const geom::TriSurface&, …)`. The exact OCC
oracle (`geom::project_point_on_surface/_on_face/_on_edge/_on_vertex`,
`geom/cad_model.hpp`) existed and was used only by diagnostics and two late
passes.

**(b) Only four of six meshers even had the oracle.** `volume_mesh_impl` built
the `BoundaryProjectionContext` for hybrid / hybrid-VEM / graded / varyhedron.
Plain tet fill and hex fill were handed an empty feature-edge span and no
context at all — zero crease awareness, and `compare_meshers.png` renders the
tet tile.

**(c) Nothing pinned a sharp edge.** `project_point_on_edge` was never called
by any mesher. A crease node was projected to the nearest point of a *face*,
which is by definition off the crease; varyhedron's "edge attraction" then
moved it 35 % of the remaining way. A 35 % blend toward an edge *is* a chamfer
of the other 65 %. That is the chamfer in the figures, and it is width-h, so it
never shrinks relative to the cell.

**(d) The unsnap ladder threw projections away silently.** When no fraction in
{0.75, 0.5, 0.25, 0} kept every incident cell valid, the node reverted to its
raw Cartesian lattice site. The common case was worse and quieter: a *partial*
keep. Measured on icecream_cone at h = 8 mm, only 9 of 584 moved nodes fully
retreated, yet the worst boundary node sat 0.60 h off the CAD, because the
ladder settled at 0.25 of the move and reported nothing.

## 3. The fix

**Exact oracle everywhere.** The four-mesher gate is deleted. Any model with a
live `CadModel` builds the oracle, and tet/hex fill take a `mesh::BoundaryFit`
(model + topology + oracle). `make_boundary_projection` now also hands back the
`CadTopology` it already extracts, and extracts it at 32 samples per edge
rather than 10: the polyline is the pin's capture test, and a 10-sample circle
has a 4.9 %·R chord sag — a fifth of a cell on a small bore.

**Hard feature pinning** (`mesh/feature_pin.hpp`). CAD vertices claim their
nearest boundary node within 0.75 h and freeze it; sharp CAD edges collect the
nodes within 0.5 h of their curve, order them by arclength, and pin each onto
the exact curve. Pins are **all-or-nothing**. A partial pin leaves the node
neither on its curve nor on the surface it came from, and measurably so: with
fractional pins the worst plate_hole boundary node went to 0.31 h, three times
its pre-pin value.

**Fourier chain re-spacing.** A closed crease chain (a bore rim, a cap seam)
comes out of the lattice unevenly spaced, because the lattice samples the curve
wherever its cells happen to cross. That unevenness is the sawtooth. For closed
chains of ≥ 8 nodes the deviation of each node's arclength parameter from a
perfectly uniform chain is a periodic signal; it is low-passed at 0.995 energy
with `geom::lowpass_signal_periodic` and the node is re-pinned at the corrected
parameter. **Fourier chooses only where along the curve a node sits — never
where the curve is.** The pin target is always the exact OCC projection, and a
re-spaced target that would move a node more than one cell falls back to the
plain nearest-point pin.

The transform itself moved from `adapt::spectral` to `geom/signal_fft.hpp`
(`adapt` links `mesh`, so a mesh pass could not reach an adapt header);
`adapt::spectral` re-exports the names, so ADR-0034's callers are unchanged.

**Relaxation instead of retreat.** `snap_boundary_nodes` takes a
`RelaxNeighborhoodFn` and calls it whenever the projection cannot be kept
*whole* — not only on full failure. Interior nodes of the blocked node's star
relax toward their neighbour centroids, capped at 0.25 h and validity-gated, so
boundary fidelity cannot be traded for it. When the stair cell has no interior
corner at all (documented in `hex_fill.cpp`: 7 of 8 corners in boundary quads),
the fallback slides the star's other *boundary* nodes tangentially and
re-projects them through the same oracle: they stay on the CAD, just spaced
differently, which is the only freedom such a cell has left. With the oracle
present, tet fill raises its travel cap from 0.75 h to the full 1.25 h the snap
allows — a stair node on a slanted wall must be able to cross a half-cell
diagonal (0.87 h) to reach the CAD at all.

**A gate that measures the cell that ships.** The graded pin gate now scores
`tet_shape_quality` against `kCellShapeFloor` rather than a positive volume; a
sign-only gate let a pin flatten a cylinder skin tet to quality 1e-4. And after
the mesher branch, `relax_cells_below_shape_floor` measures every emitted cell
with the product's own `fea::cell_quality`, relaxes interior nodes of the ones
below the floor, and **reports whatever is left** in the mesher note and in
`diag --json` as `mesh.n_below_shape_floor`. This is ADR-0033's rule applied to
the last hand-off: a pyramid gated as a pyramid may ship as two assembly tets.

**A metric that separates placement from discretisation.**
`BRepGeometryFidelity` gained `mesh_boundary_nodes_to_brep_surface` (`diag`:
`fidelity.mesh_boundary_nodes_to_brep`). The combined statistic mixes boundary
nodes with facet centroids and edge midpoints, and a straight facet spanning a
curve always carries the chord sag h²κ/8. That sag is a discretisation
property, not a placement error; mixing them made it impossible to tell "the
mesher missed the surface" from "a flat facet spans a curve".

## 4. Measured, h = 8 mm, `polymesh diag`

Boundary-node distance to the exact BRep, as a fraction of h.

| part / mesher | p99 before | p99 after | max before | max after |
|---|---|---|---|---|
| sphere / graded | 0.039 | **2.9e-15** | 0.059 | **5.9e-15** |
| sphere / varyhedron | 0.029 | **3.1e-15** | 0.037 | **5.9e-15** |
| cylinder / graded | 0.045 | **2.8e-15** | 0.180 | **0.0052** |
| cylinder / varyhedron | 0.038 | **5.4e-15** | 0.180 | **6.3e-15** |
| plate_hole / graded | 0.0074 | **0** | 0.098 | **0.00030** |
| plate_hole / varyhedron | 0.0074 | **0** | 0.098 | **1.2e-15** |
| plate_hole / tet | 0.218 | **0** | 0.650 | **1.1e-16** |
| icecream_cone / graded | 0.052 | **3.4e-15** | 0.196 | 0.196 |
| icecream_cone / varyhedron | 0.039 | **3.7e-15** | 0.156 | 0.156 |

(The "before" p99 column is the combined mesh→BRep statistic, which is the only
one that existed then; the node-only column did not exist before this ADR. The
comparison is therefore conservative — the old combined number is bounded below
by its node subset.)

Normal-angle p99 (mesh facet vs BRep normal): sphere graded 30° → **4.4°**,
cylinder varyhedron → **2.9°**, plate_hole graded → **3.6°**.

Inverted and below-floor cells, the ADR-0033 family:

| case | quality_min before | after |
|---|---|---|
| sphere hybrid h = 3 mm | **−0.837** | +0.0022, 38 cells reported below floor |
| icecream_cone hybrid h = 8 mm | **−0.742** | +0.020, none below floor |
| icecream_cone hybrid h = 3 mm | **−0.779** | +0.020, none below floor |
| cylinder graded h = 8 mm | 0.0032 | 0.043 |

No mesh in the table ships an inverted cell.

## 5. What is still open, and why it is not hidden

**The Kuhn tet lattice on a truncated cone.** `icecream_cone` (conical surface,
6 mm base radius, spherical cap R = 35 mm, planar bottom) still leaves a
handful of nodes up to 0.196 h off the BRep under graded/varyhedron, and up to
0.50 h under the plain tet fill, at h = 8 mm — the p99 is machine-zero, so this
is a small set, not a surface. Its bottom disc has a 6 mm radius against a
7.8 mm cell: the feature is smaller than the element, and no node placement
fixes an under-resolved feature.

**Spurious mesh creases on smooth walls.** `mesh_feature_to_sharp_brep_edge`
p99 stays near 18 h on icecream_cone: the mesh carries ~220 boundary segments
whose dihedral exceeds 30° while the CAD has one small sharp edge. Every node
in those segments is on the BRep to machine precision, so this is a
*connectivity* artifact — facets folding across a 2:1 LEB boundary transition —
not a placement error. The honest reading is that the reverse direction, which
is the one that answers "does the mesh reproduce the CAD's edges", is
`sharp_brep_edge_to_mesh_feature` p99 = **0.049 h**. Removing the forward
number needs boundary-conforming re-triangulation at 2:1 transitions, which is
a mesher change, not a snap change, and is not attempted here.

**The chordal floor.** With every node exactly on the surface, the remaining
mesh→BRep distance is the sagitta of a straight facet across a curve, h²κ/8.
That is the floor of a linear-element mesh and it is why the combined statistic
(0.05–0.20 h on curved parts) does not go to zero even when the node statistic
does. Quoting node conformity as "the mesh matches the CAD" is only honest with
that sentence attached.

## 6. Consequences

- Two regression tests in `tests/test_brep_fidelity.cpp` (`[feature_pin]`):
  boundary nodes on the exact BRep for eight part × mesher pairs with ceilings
  at 1e-12·h where the mesher is exact, and sharp BRep edges reproduced by mesh
  feature segments in the CAD → mesh direction.
- `diag --json` gains `fidelity.mesh_boundary_nodes_to_brep` and
  `mesh.n_below_shape_floor`.
- Mesher notes carry a `conformity` block (snap moved/unsnapped/relax-rescued,
  pin counts, worst node and its owner class) and a `ship_gate` line when any
  cell is left below the floor.
- The mesher's output changed on every curved part, so every mesh-derived
  advisor label is stale — the corpus is regenerated and the model retrained,
  the same consequence ADR-0032 had.
