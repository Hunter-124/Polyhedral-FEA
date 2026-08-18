# ADR-0036: A symmetric part gets a symmetric tiling

- Status: accepted (2026-08-18)
- Companion to [ADR-0035](0035-boundary-conformity.md) (the boundary lands on the
  BRep) and [ADR-0032](0032-stl-order-determinism.md) (order-dependent accept /
  reject passes need a canonical visiting order).

## 1. The report

> our meshes still aren't coming out with symmetrical elements/features even on
> symmetrical parts like the sphere has groups of distinct patterns and same with
> the hole plate but like the patterns aren't symmetrical with the actual geometry

ADR-0035 closed the *position* question: boundary nodes sit on the exact BRep to
machine precision. It said nothing about the *pattern* the elements draw, and the
pattern was wrong in a way no fidelity or quality metric can see.

## 2. What was actually wrong

Measured before anything was changed, on `cantilever.step` — a plain box, the
most symmetric input the corpus has — at h = 10 mm with the uniform `tet` fill:

| about the bbox mid-plane normal to | nodes with an exact mirror partner | tets whose mirror image is also a tet |
|---|---|---|
| x | 100.00% | **0.00%** |
| y | 100.00% | **0.00%** |
| z | 100.00% | **0.00%** |

Not one of the 73326 tets had a mirror image, on a mesh whose every node did.
That is the whole defect in one line, and it explains why it survived so long:
every node-level check passes, the volume is exact, the quality floor holds, and
the fidelity residual is zero. Only the tiling is asymmetric.

The cause is the split, not the lattice. `make_bbox_grid` fits the lattice to the
bbox exactly, so the lattice *planes* are mirror-symmetric by construction. The
product then cut every cell with Kuhn/Freudenthal's six tets about the *same*
cube main diagonal. A single-orientation Kuhn tiling is translation invariant and
has no mirror symmetry at all: reflecting a cell maps its main diagonal to the
anti-diagonal, which the tiling never contains. Every cell leans the same way,
which reads on a curved wall as diagonal banding and on a plate as a pattern that
does not line up with the part's own axes.

Two further passes then broke what symmetry remained:

- **Longest-edge bisection tie-breaking.** `local_refine.cpp:longest_edge` broke
  length ties on the lexicographically smaller *node index pair*. Node indices do
  not mirror, so mirror-image tets were handed non-mirror-image edges and the
  refinement closure diverged. Ties are not an edge case here: on the graded box
  the LEB reached only 22–76% mirrored tets even after the split was fixed.
- **Seed decimation anchor.** `decimate_sources` bucketed size sources on a
  lattice anchored at *world zero*, so a cell wall falls at an arbitrary place
  inside the part and a source could survive decimation while its exact mirror
  did not.

## 3. The fix

**Rotate the Kuhn diagonal per cell.** Conformity fixes which orientations may
sit side by side. A Kuhn cell's diagonal on each face is the projection of its
main diagonal a–b; writing that direction as signs (s1,s2,s3) modulo global
negation, the x-face diagonal is fixed by s2·s3, the y-face by s1·s3 and the
z-face by s1·s2 — so s2·s3 must be constant along x, s1·s3 along y and s1·s2
along z. Solving that system for the *mirror-antisymmetric* case gives

    s = ( (-1)^(i+j), 1, (-1)^(j+k) )

i.e. four of the eight Kuhn orientations in a fixed pattern
(`mesh/lattice_split.hpp`, `kLatticeTetsKuhn` + `lattice_cell_variant`).
Reflecting an axis negates the corresponding sign; with an **even** cell count on
that axis the index parity negates it too, so the reflected cell receives exactly
the reflected variant. Even counts are therefore a requirement, not a
preference — `classify_cells_feature_aware` takes `even_cells` and the tet fills
ask for it, which also puts every bbox mid-plane on a lattice plane. The mixed
hex/pyramid fill stays on the odd-permitting lattice: its cells are self-mirror
and its 2:1 closure was tuned there.

**Break LEB ties in a reflection-equivariant frame.** Fold every candidate edge
into the octant of its own tet's centroid (`MirrorFold`) and take the
lexicographically smallest folded edge. A tet and its mirror image fold to the
same point set, so the same folded edge wins in both and the two answers are
exact mirror images. Length comparison moved to a relative epsilon for the same
reason: mirrored edges are equal only to a few ulp, and an absolute epsilon on
squared lengths ranked one of them strictly longer. Because cell counts are even,
no cell straddles a mid-plane, so no tet centroid sits on one and the octant sign
is never ambiguous.

**Anchor decimation and boundary visiting order on the part, not the world.**
`decimate_sources` buckets about the source set's own bbox centre — that
partition maps onto itself under reflection, and min-h is commutative.
`tet_boundary_nodes` returns nodes sorted by quantised distance-from-centre per
axis (ties by node id), so a node and its mirror image are adjacent in the
snap/smooth order with the same predecessor set up to reflection. This supersedes
the ascending-node-id order ADR-0032 installed for cross-platform determinism;
the new key is also platform-independent.

## 4. Measured

Mirror-image tet fraction, `--no-curved` so the comparison is the tiling itself,
tolerance 1e-6 of the bbox diagonal:

| part | h | axis | before | after |
|---|---|---|---|---|
| cantilever (box), `tet` | 10 mm | x/y/z | 0.00% / 0.00% / 0.00% | **100% / 100% / 100%** |
| cantilever (box), `graded` | 10 mm | x/y/z | 56.95% / 64.09% / 64.09% | **100% / 100% / 100%** |
| plate_hole, `graded` | 6 mm | x/y/z | 40.14% / 40.11% / 73.30% | 82.01% / 82.90% / 97.39% |
| sphere, `graded` | 8 mm | x/y/z | — | 51.65% / 71.55% / 69.33% |

Fidelity and quality at the same h, `polymesh diag --mesher graded`:

| part | h | q_min before → after | surface p99/bbox before → after | normal p99° before → after |
|---|---|---|---|---|
| sphere | 8 mm | 0.02542 → 0.02891 | 7.12e-6 → 7.93e-6 | 0.335 → 0.238 |
| icecream_cone | 8 mm | 0.02105 → 0.05766 | 5.71e-5 → **1.46e-4** | 2.77 → 4.98 |
| cylinder | 8 mm | 0.03250 → 0.05338 | 5.66e-6 → 5.39e-6 | 0.195 → 0.337 |
| plate_hole | 6 mm | 0.02110 → 0.02065 | 4.83e-5 → 4.55e-6 | 0.773 → 0.347 |

Zero inverted and zero sub-floor cells throughout.

The cone regression is reported, not smoothed over. It is a sampling effect, not
a systematic loss: sweeping h with the new tiling gives surface p99 = 7.73e-5,
9.56e-5, **1.46e-4**, 7.71e-5, 9.52e-5 at h = 6, 7, 8, 10, 12 mm. The residual on
this part lives in the 5e-5–1.5e-4 band and depends on where lattice nodes fall
relative to the rim; h = 8 mm is the top of that band with the new even lattice
and was the bottom of it with the old odd one. The cone's own open item — CAD →
mesh sharp-edge coverage, ADR-0035 §6 — is what sets that band.

## 5. Rejected alternative, measured rather than argued

The alternating **five-tet** ("checkerboard BCC") split: one regular central tet
plus four corner tets, chosen by (i+j+k) parity. It is equally mirror-symmetric,
needs the same even counts, is one element per cell cheaper, and is far better
shaped — q = 1.0 for the central tet and 0.5 for the corners against Kuhn's
uniform 0.2722. It measured better on fidelity too: sphere q_min 0.0637, sphere
normal p99 0.166°, plate_hole surface p99 3.5e-6, and higher mirrored-tet
fractions than the shipped scheme (sphere 69.94/97.62/91.88%).

It was rejected because its central tet is regular, so all six of its edges tie
for longest and longest-edge bisection loses the cell-local terminal edge that
makes graded refinement local. On a 4³ unit box, doubling the level-1 cell count
(16 → 32 cells) produced a **bit-identical** 768-tet mesh: any local mark refined
the whole lattice. Element counts rose 2.4× on the sphere at matched h for the
same reason. Grading locality is worth more than base shape quality, so the
six-tet cell stays. `tests/test_size_field_fill.cpp` now asserts the level counts
alongside the totals so this failure mode cannot return silently.

## 6. What is still open

**The tessellation is the ceiling.** A mesh cannot be more mirror-symmetric than
the surface its decisions are read from, and OCC's triangulation of a symmetric
part is not symmetric. Measured on the product's own deflection settings
(`5e-4 · diag`, 0.2 rad), fraction of tessellation vertices with an exact mirror
partner:

| part | x | y | z |
|---|---|---|---|
| sphere | **0.00%** | 99.69% | 1.33% |
| plate_hole | **5.97%** | 100% | 100% |
| cylinder | 100% | 100% | 100% |

The sphere's seam meridian and poles put its facets in completely different
places on the two sides of the yz-plane. Everything downstream that still reads
`Model::surface` inherits that: `adapt::geometry_size_sources` (per-vertex
discrete curvature and thickness), `mesh::stamp_curvature_cells` (the per-cell
h·κ turning-angle stamp), `classify_cells_inside` (z-ray parity), and the jut /
carve thresholds. The boundary *snap target* is already the exact BRep
(ADR-0035), which is why the residual is 50–80% mirrored rather than ~0%.
Ablation confirms the split of blame: with `--no-feature --no-spectral`, i.e. no
size-field sources at all, the sphere still measures 68–72% — so roughly a third
of the remaining asymmetry is the mesher's own tessellation-driven stamping and
carving, not the sizing plan.

Closing this meant taking curvature from the exact BRep — per-face principal
curvature samples on `geom::CadTopology`, the face analogue of the
`CadEdge::kappa_samples` that `spectral_edge_sources` already uses. That landed
as the follow-up in §8; what is genuinely still open is listed at its end.

## 7. Consequences

- `src/mesh/include/mesh/lattice_split.hpp` is the single owner of the lattice →
  tets decision. `hybrid_fill.cpp` and `tet_fill.cpp` both consume it; neither
  keeps a local table.
- `classify_cells_feature_aware` gains `even_cells`. Passing it changes cell
  counts on any axis whose extent/h was odd, so element counts, DOF and every
  mesh-derived advisor label move on those parts.
- `kCurvedDofPerElement` is re-measured at 4.6 (was 4.4). Measured 3·nodes /
  elements at auto h: sphere 4.44, icecream_cone 4.41, cylinder 4.36, plate_hole
  4.51, cantilever 4.47. The old constant let the sphere ship 1.9% over a
  300k-DOF interactive ceiling.
- `tests/test_graded_fill.cpp` gains `[mesher][symmetry]`: the uniform and graded
  tilings must be 100% mirror-symmetric about all three bbox mid-planes on a box
  with a radially symmetric size field. The pre-fix tiling scored 0.0.
- `tests/test_size_field_fill.cpp` asserts that more level-1 cells produce more
  elements, which is the guard against a split whose LEB has lost its cell-local
  terminal edge.
- `tet_boundary_nodes` takes node positions. ADR-0032's ascending-node-id order
  is superseded by the mirror-canonical order, which is equally deterministic
  across standard libraries.

## 8. Follow-up: the order-dependent passes and the tessellation ceiling

Two further pieces landed on top of the split, each measured the same way.

**Every accept/reject pass is now reflection-equivariant.** The stage-by-stage
attribution on `cylinder.step` (whose tessellation is exactly mirror-symmetric,
so any asymmetry is the mesher's own) put the losses at, in order: the tangential
smoothing pass (−7 to −11 points of mirrored tets), the sliver-collapse round
(−1.5 to −3 points, and the seed the smoothing then amplified ~5×), and the
snap's coupled retreat (nothing measurable on this part). The fixes share one
mechanism, `mesh::MirrorKeyFrame` (quantised per-axis distance from the node-set
bbox centre — mirror-invariant, so a mirror pair keys bit-identically):

- `snap_boundary_nodes`' retreat loop picks the worst offender by moved distance
  with the key, not the node id, breaking ties — a node and its mirror offend by
  the same amount, and the id tie-break retreated one of the pair and kept the
  other.
- `smooth_boundary_nodes` visits nodes in mirror-canonical order
  (`sort_mirror_canonical`), as does the feature-pin pass.
- The collapse round's phase A ranks merge candidates by (distance, key, id);
  phase B visits caps worst-aspect-first with key tie-breaks, tries candidate
  edges shortest-first with key tie-breaks, and — when both collapse directions
  are legal — keeps the direction whose incident star survives in better shape
  (`collapse_score`, the star's worst post-collapse aspect; aspect is
  mirror-invariant, so mirrored caps collapse in mirrored directions).

Visit order was the one place a symmetric choice lost to a quality choice, and
the measured record matters: index order fails the sphere scorecard (M1max
0.035 h), centre-out radial order passes the sphere (0.0095 h) but destroys the
hole plate (M1max 4.76 — centre-out starts at the hole rim, the one boundary
that must not move first), and worst-aspect-first passes both (sphere 0.0095 h
against the 0.0123 h baseline, hole plate composite 0.530 at parity). Quality
order also beats the baseline it replaced.

**Curvature sizing reads the exact BRep.** `geom::CadFace` carries `samples` /
`kappa_samples` — max |principal curvature| from `BRepLProp_SLProps` on a uv grid
inside the trimmed face, so the value is a property of the surface, not of the
tessellation (a sphere comes out at κ = 20.000 1/m everywhere; the same part's
tessellation-derived estimate spans 17.7–145.9). `build_refinement_plan` prefers
it whenever `Model::cad` is populated and reports `geo_curv=brep|tessellation`;
the thin-wall term still comes from the tessellation. On the sphere this removes
all 55 tessellation-noise seeds (was refining a uniformly-curved surface
non-uniformly), and it is what moved the sphere's y/z mirror fraction to
95.9/83.1%.

Final mirrored-tet fractions (`--no-curved`, 1e-6·diag tolerance) and the
curved-geometry fidelity matrix:

| part | h | x | y | z |
|---|---|---|---|---|
| cantilever, `tet` | 10 mm | 100% | 100% | 100% |
| cantilever, `graded` | 10 mm | 100% | 100% | 100% |
| cylinder, `graded` | 8 mm | 87.6% | 89.7% | 89.7% |
| sphere, `graded` | 8 mm | 75.4% | 95.9% | 83.1% |
| plate_hole, `graded` | 6 mm | 81.7% | 83.8% | 94.3% |

| part | q_min | below floor | surface p99/bbox | normal p99° |
|---|---|---|---|---|
| sphere | 0.0207 | 0 | 6.9e-6 | 0.253 |
| icecream_cone | 0.0205 | 0 | 1.42e-4 | 5.12 |
| cylinder | 0.0221 | 0 | 1.8e-5 | 0.347 |
| plate_hole | 0.0210 | 0 | 4.5e-6 | 0.366 |

Reported, not smoothed over: the collapse rework lets the cylinder's q_min fall
from 0.053 to 0.022. The collapse gate can never create a cell below its star's
pre-collapse minimum (healthy cells may not drop below 0.04, sub-0.04 cells may
only improve), so the 0.022 cell was born in the snap/LEB stages and merely
survives a different survivor selection — but the margin above the 0.02 floor is
now thin on curved parts and worth watching. The cylinder's surface p99 moved
5.6e-6 → 1.8e-5 for the same reason, still well inside the 1e-4 bar.

**Still open.** What remains asymmetric is what still reads `Model::surface`:
`stamp_curvature_cells`'s per-cell turning angle (worth 12 points of plate
symmetry when ablated), `classify_cells_inside`'s z-ray parity, and the
jut/carve thresholds — all fed by a tessellation that is itself 0–6%
mirror-symmetric on the sphere and plate. Closing those means either an exact
BRep inside/outside classifier for boundary cells or a symmetry-exploiting
tessellation; neither is small, and the renders are no longer visibly wrong, so
they are left open here rather than half-done.

