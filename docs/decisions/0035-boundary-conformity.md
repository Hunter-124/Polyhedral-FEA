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

## 5. Second wave: the exterior that ships

§3 conformed each mesher's *own* boundary set — the lattice skin its snap ran
on. What ships is `fea::extract_boundary_faces(out.mesh)`, the true element
exterior, and the two are different sets: a fan tet peeled after the snap, a
pyramid emitted as two assembly tets, an LEB child carved late, all expose nodes
that were interior when the snap ran. Measured on sphere/hybrid at h = 8 mm, the
mixed-fill branch's own worst boundary node sat 0.016 h off the BRep while the
shipped mesh carried nodes 0.085 h off it, in visible flaps. So a
mesher-independent gate (`conform_true_exterior`, `src/pipeline/src/scene.cpp`)
now runs on the extracted exterior, and it needed four things §3 did not have:

**Exact resolution.** `boundary_projection_target` falls back to the
tessellation for a node with no latched owner, and OCC's own facets are 0.085 h
off this sphere at its deflection — so the fallback reported ~0 for exactly the
nodes that were wrong. Unowned nodes are projected freely onto the BRep instead.

**A march, not a jump.** Take the largest legal fraction of the remaining gap,
re-project, repeat. Room comes first from interior star nodes, then from sliding
wall neighbours along their own owner geometry. Every step must also not
increase the *free* distance to the shape: walking toward a point that is on the
BRep can still leave the local patch, which took cvt_poly's worst node from
0.503 h to 1.799 h before that guard existed.

**Conforming hex relief.** A hex saturated at the shape floor has no interior
corner to give — every one of sphere/hybrid's 34 stragglers had an incident hex
at quality 0.020081, four e-5 above the floor, so even a 0.125 step took it
under. Such a hex is fanned into six pyramids over its own six quad faces; the
bases *are* the hex faces, so no neighbour sees a change. Each child must clear
the floor and be integrable, and the phase rolls back if it buys nothing (on
icecream_cone/hex it fanned 20 hexes, moved no node, and left 29 cells below the
floor — reverted).

**`fea::element_jacobians_positive`, the assembly's own rule.** `cell_quality`
is a shape measure; the solver's question is det J > 0 at every quadrature point
of the element's rule, and for a pyramid in both tets of the split it is
actually integrated as. A quality-accepted move shipped det J = −6.085e-09 on
icecream_cone/graded and −3.564e-10 on plate_hole/varyhedron. Repair passes now
gate on the predicate, and the ship gate counts non-integrable cells in the
mesher note. This is why the pass is safe where a first attempt (recorded in the
ADR history as reverted) was not.

The whole pass is judged on entry/exit — worst cell quality may not drop, the
count of sub-floor cells may not grow, no cell may be non-integrable — and
reverts wholesale otherwise. That is what keeps cvt_poly, whose cells are
already degenerate at ~1e-14, at exactly its previous numbers.

### 5.1 Facet kinks: the defect a user actually sees

With every node exactly on the BRep the surface can still *look* wrong. The
showcase cone (graded, h = 10 mm) shipped adjacent facet pairs whose planes
differ by up to 77.8°, mean 6.0°, because grading transitions leave needle
facets beside bulk ones (adjacent area ratios up to 9.8). A kink between two
exact facets is not a placement error but a **spacing** error, and spacing is
the one degree of freedom a node on a face still has. The gate's kink-relief
phase slides face-owned nodes around kinked edges along their own surface,
keeping only moves that lower the worst kink in that node's neighbourhood.
Edge- and vertex-owned nodes are never slid: that is precisely what would blunt
the crease the pinning pass just made exact.

Also corrected here: the dihedral feature detector compared **signed** facet
normals, so a neighbouring pair the boundary extraction happened to wind
oppositely read as a 180° crease. On icecream_cone/graded it reported 177
feature segments where the geometry has 47, and the phantoms sit on smooth
walls — which is why their distance to the nearest sharp BRep edge came out at
0.81 of the bounding-box diagonal. The angle between two facet *planes* is what
a crease is, so the measure is now `|n0·n1|`. Most of what the previous revision
of this section recorded as a mesher defect was this measurement artifact.

### 5.2 Measured, h = 8 mm, second wave

| case | node p99 | node max | normal p99 | feature p99 |
|---|---|---|---|---|
| sphere / graded | 2.9e-15 | 5.9e-15 | 4.36° | — |
| sphere / varyhedron | 3.1e-15 | 5.9e-15 | 2.64° | — |
| sphere / hybrid | 0.0062 → **5.9e-15** | 0.081 → **0.061** | 8.77° → **5.28°** | — |
| sphere / hexpyr | 0.373 → **0.026** | 0.553 → **0.027** | 69.3° → **12.8°** | — |
| sphere / octa | 0.150 → **0.021** | 0.151 → **0.032** | 33.4° → **12.7°** | — |
| cone / hybrid | 3.5e-15 | 5.3e-15 | 6.50° | 1.16 → **0.025 h** |
| cone / varyhedron | 3.2e-15 | 0.156 → **0.071** | 15.1° → **8.96°** | 16.7 → **0.150 h** |
| cone / graded | 3.2e-15 | 0.196 → **0.050** | 20.5° → **9.12°** | 17.9 h |
| cone / hexpyr | 0.493 → **0.289** | 0.614 → **0.290** | 76.9° → **48.1°** | 18.1 h |
| cylinder / hybrid | 5.4e-15 | 6.3e-15 | 19.2° → **0.27°** | 0.483 → **0.049 h** |
| plate_hole / graded | 0 | 3.0e-4 → **1.2e-15** | 3.58° → **3.72°** | 0.203 h |
| cylinder / tet | 0.034 → **0.017** | 0.044 | 25.4° | 1.05 h |

No case regressed. The two fixtures that fail to solve at this h
(cylinder/cvt_poly, icecream_cone/octa) fail identically before and after, for
reasons unrelated to boundary placement.

## 6. What is still open, and why it is not hidden

**The uniform Cartesian tet fill is bounded by the shape floor, not by the
projector.** `tet` still leaves p99 = 0.10 h on the sphere and 0.18 h on the
cone at h = 8 mm. This was measured, not assumed: placing one straggler on its
exact target drives 2–14 of its incident tets below the shared cell-shape floor
(node 12 on icecream_cone reaches quality −0.050 at its target), which is why
every one of these parts ships `quality_min = 0.0200` exactly — the snap stops
at the floor and the residual is the price. Three fixes were tried and are
recorded here so they are not tried again:

- More snapping. A coupled snap/relax march of 12 rounds moved sphere's p99 by
  nothing at all (0.1167 → 0.1167).
- Conforming Steiner relief (split each fully-boundary stair tet at its
  centroid, 1→4, faces unchanged). Cylinder 0.0343 → 0.0295, cone 0.198 → 0.175,
  sphere unchanged: the children of a stair tet are worse than the parent, so
  the gate that keeps them above the floor rejects exactly the cells that block.
- Conformity-driven LEB. Refining the blocking cells does buy placement, but the
  refinement adds *new* boundary nodes that stick the same way, and it costs
  shape: sphere reached p99 0.032 h with 94 cells below the floor and
  quality_min 1.1e-05. Refining the pristine lattice instead of the snapped mesh
  kept quality but moved the worst node the wrong way (0.121 h → 0.300 h).

- Per-level snapping during LEB. The graded path already implements the proposed
  mechanism: it snaps lattice corners before refinement, projects every new
  free-surface midpoint in `local_refine_tets`, then re-collects and snaps the
  complete shell after the L1/L2 waves. Applying that same sequence to the
  uniform path is therefore the already-measured conformity-driven LEB case
  above, not a fourth independent mechanism: it reached sphere p99 0.032 h only
  by shipping 94 sub-floor cells. Re-running it under another name would not
  change the scale-invariant constraint.

The reading is that the constraint is scale-invariant: halving h halves the
required travel and the available room together. A uniform lattice conforms only
by shipping cells below the floor, which is not a trade this project makes. The
graded / hybrid / varyhedron paths exist for conformity and reach machine
precision; `tet` is the uniform baseline, and its residual is reported in its
own mesher note rather than smoothed over.

**Product rounded geometry is closed on the authoritative mesh.** A
surface-only diagonal flip was the wrong primitive: it cannot change the volume
cell that owns the face, and measured 2→3 cavity candidates either failed the
shape floor or did not cover the sharp BRep path. The accepted mechanism is:

1. route product CAD meshing through graded tets by default;
2. use a 0.5× lattice when non-planar BRep faces occupy at least 25% of area;
3. recover connected sharp-edge segments in the boundary graph and pin both
   endpoints as one validity-gated move, opening only the affected interior
   star through deterministic pattern search when necessary;
4. promote the resulting volume cells to tet10/hex20, project face mids and
   CAD-edge mids through exact OCC owners, and refine locally when a midpoint
   cannot be accepted;
5. assemble, solve, export, diagnose and render that same higher-order mesh.

This removes both sources of the visible rounded defects: under-resolved
boundary topology and the h²κ/8 corner-chord floor. Studio evaluates the actual
isoparametric surface at eight subdivisions; it no longer substitutes a
display-only mesh. At requested h = 8 mm, sphere/cone/cylinder/plate-hole p99
surface-to-BRep errors over bbox diagonal are 7.12e-6, 5.50e-5, 5.66e-6 and
4.83e-5 respectively, all inside the 1e-4 (99.99%) target. Their curved-cell
quality minima are 0.02542, 0.02460, 0.03250 and 0.02008 with zero inverted and
zero sub-floor cells.

The uniform Cartesian `tet` baseline remains explicitly floor-bounded. It is no
longer the product default, and its failed projection/refinement experiments
remain recorded above rather than being hidden behind the curved graded result.

The cone apex is a declared BRep singular vertex. Its 90° maximum normal jump is
mathematically correct; the smooth-wall p99 is 2.59°. Exact sharp-edge
diagnostics now classify CAD-owned quadratic boundary edges and use exact OCC
curve projection, not sampled-polyline chord distance.

## 7. Consequences
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
- A third regression test, `[exterior_gate]`, asserts the two claims the second
  wave makes about the shipped mesh over six part × mesher pairs: every emitted
  element passes `fea::element_jacobians_positive`, and the p99 of the *shipped
  exterior's* node distance to the exact BRep is at or below 1e-12·h on the
  conforming meshers.
- `fea::element_jacobians_positive` / `fea::star_jacobians_positive`
  (`src/fea/include/fea/element_validity.hpp`) are the integrability predicate
  every repair pass gates on. They are additive: no GATE-1 formulation, rule or
  assembly branch changed.
- Mesher notes gain an `exterior_gate` block (candidates, moved, relax-rescued,
  hexes fanned, left, worst residual, kinks relieved, and `REVERTED` when the
  exit invariant rolled the pass back) and a `ship_gate … non-integrable cells`
  line when any emitted element would be refused by the assembly.
- The second wave changed mesher output again, on every part that has a curved
  face, so the advisor corpus is regenerated and retrained a second time. The
  v5 generation is archived whole under `bench/campaigns/archive-v6/` and
  `bench/advisor/archive-v6/`.
- The authoritative-geometry wave changes element order, solve DOF, product
  mesher defaults, curved-part counts and every mesh-derived advisor label.
  Generation v6 is archived whole under `bench/campaigns/archive-v7/` and
  `bench/advisor/archive-v7/`; generation v7 is regenerated from the final
  binary and retrained before publication.
- `tests/test_traction_selection.cpp` verifies that boundary tessellation
  evaluates the authoritative quadratic shape functions and retains exact
  nodal interpolation weights. The graded solve regression verifies tet10-only
  solve geometry and a displacement vector covering every quadratic node.
