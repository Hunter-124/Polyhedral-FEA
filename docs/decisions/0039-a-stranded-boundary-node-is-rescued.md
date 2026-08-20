# ADR-0039: A stranded boundary node is rescued, not abandoned

- Status: accepted (2026-08-19)
- Completes the open fidelity item of
  [ADR-0035](0035-boundary-conformity.md) §6 (the cone's sharp-rim residual)
  measured against the matrix of
  [ADR-0037](0037-a-box-selection-is-a-region.md) §4.

## 1. The report

`icecream_cone.step` was the one showcase fixture failing its own surface
fidelity bar: `fidelity.mesh_boundary_to_brep.p99_over_bbox` measured
1.230e-4 against the 1e-4 bar (graded, h = 8 mm), with the worst sample
172.5 um off the BRep. Every other fixture sat an order of magnitude inside.

## 2. Where the tail lives

`POLYMESH_FIDELITY_DUMP=<path>` (new, in `evaluate_brep_geometry_fidelity`)
writes the 64 worst mesh→BRep samples as `kind distance x y z` lines, so a
tail is located on the part instead of inferred from quantiles. On the cone
every one of the 64 worst samples sat on one ring: z = 47.95 mm, r = 18.29 mm,
four-fold symmetric, 171 um *inside* the cone wall — while every boundary
neighbour of those nodes measured 0.0 um off the wall.

The ring is one reflection orbit of four boundary corner nodes (plus their
incident edge mids and face centroids, which is what inflates four nodes into
a whole percentile tail). Each node:

- is a boundary corner of the shipped mesh, 171 um inside the exact cone;
- has a 37-tet star (a healthy boundary node carries 15–20) whose worst cells
  are all-boundary cap tets pinned at quality 0.0201 — the shared 0.02 floor;
- cannot move toward the surface by any fraction: four incident tets, all of
  them caps sharing one edge, flip their signed volume before the node
  reaches the wall, and the caps start at the floor, so the exterior gate's
  "never below min(before, floor)" march refuses every rung.

That is the ADR-0035 §6 residual, measured end to end: the compatibility
snap's last resort is a full retreat to the raw lattice site, the node was
left there, the surrounding cells became caps as its neighbours snapped out,
and no later pass — smoother, sliver relaxation, exterior gate — can move a
node whose star is saturated at the floor. The star is saturated *because*
the node is inside; the node cannot leave *because* the star is saturated.

Three repairs were tried on the shipped mesh and are recorded so they are not
tried again:

- **Edge collapse into the on-surface neighbour.** 32 renamed cells mostly
  improve, but one renamed cell inverts and its free corners cannot be nudged
  without breaking their own stars (the interior ring is saturated too).
- **Peeling the star.** A 3-tet peel relocates the straggler one layer in
  (the newly exposed node strands at 337 um); a full-star peel strands 14
  newly exposed nodes at 0.6–1.2 mm. This is the documented pit cascade.
- **Sliding the collapse target tangentially on the surface.** Best case
  leaves a renamed cell at detJ 2.7e-10 — positive, far under the floor.

The defect is not h-phase luck: at h = 7.0/7.5/8/8.5/9 mm the same class of
saturated straggler appears (p99/bbox 9.2e-5, 9.0e-5, 1.23e-4, 1.84e-4,
2.02e-4). It is systematic: the compatibility snap strands a few nodes at
every resolution, and nothing downstream can recover them.

## 3. The fix: two-stage snap

`snap_round` in `graded_tet_fill_surface` now runs two stages.

**Stage 1 is the compatibility path, unchanged.** Its all-or-nothing
semantics are what the downstream collapse and relaxation rounds are built
on: a stair cell may dip thin mid-snap because `repair_round` lifts it
afterwards. This was measured the hard way — gating the bulk stage on the
shape floor pinned 28 boundary nodes 1.45 mm off the sphere behind transient
stair caps (star quality exactly 0.0200), because the partial keeps the
floor gate forces *create* the thin caps that then block every further keep.

**Stage 2 rescues exactly the stranded nodes** — boundary nodes the exact
oracle places more than 0.01 hc from their current position after stage 1 —
with an orbit-locked fraction ladder:

- One ladder for the whole reflection orbit, accepted only when *every*
  member's star accepts it. The shared culprit-aware path in
  `snap_boundary_nodes` cannot be used here: its per-node quality gates read
  values that tie across a mirror pair in exact arithmetic and diverge in
  floating point, so one member stops at 0.5 and its image at 0.75. Measured
  on cylinder/feature at h = 12 mm: a per-node rescue shipped a 0.930
  mirrored-tet fraction against the == 1.0 gate (ADR-0036 §9.2). The
  neighbourhood relaxation is orbit-locked for the same reason.
- The exact target is offered first under the pipeline's standing contract —
  inversion blocks, thinness is repaired downstream. A floor-gated bisection
  that stops a bracket-width short would park nodes 1.35e-9 m off the BRep
  where the fidelity tests demand 1e-12·h (measured on sphere/graded).
- Otherwise the ladder {0.75, 0.5, 0.25} plus bisection, gated on the
  bad-cell test: inverted is always bad; below the shape floor is bad only
  when all four corners are on the boundary. A thin cell with an interior
  corner is repairable later; a thin all-boundary cap never is, so it must
  block here.
- CAD-backed only. Against a tessellation there is no exact oracle and a
  node 0.01 hc off the facets is within facet error, not stranded: the same
  rescue on `test.stl` moved nodes onto facet noise and dropped the hole
  plate composite score 0.48 → 0.25.

## 4. Two classification defects the dump exposed

The same instrument showed the second-worst cluster at the foot rim
(z = 0, r = 6 mm): quadratic boundary patches dipping up to 140 um *below*
the base plane. Two misclassifications in the projection oracle, both
measured at h = 8 mm:

**Crease capture by absolute slack alone.** An unowned node was captured by
a sharp edge when `edge_dist <= 0.55 h` and `edge_dist <= face_dist +
0.08 h`. A cone-wall edge midpoint 4 um from its home face was 580 um from
the foot rim; 580 <= 4 + 640 passed, the mid was pinned onto the rim, and
the patch through it bowed 140 um below z = 0 (a parabola through
z = {1.12 mm, 0, 0} dips to -140 um at t = 3/4). Capture now also requires
`edge_dist <= 4 · face_dist + 1e-3 h`: a genuine crease node has the two
distances within a small multiple; a face mid does not.

**A rim chord's mid is not the rim chain's edge.** When the two endpoints of
a boundary edge both sit on the same sharp BRep edge but latch different
provenance *kinds* (a rim node is on the edge and on a face at once), the
mid free-classified to the face at distance ~0 — the chord lies in the face
plane — and bowed 74 um off the rim circle on a 1.9 mm chord. The mid now
inherits the common sharp edge when both endpoints project within 1e-3·h of
it (context carries the topology table for exactly this).

## 5. Measured, graded, `polymesh diag --no-solve`

| part (h) | p99/bbox before | after | q_min before | after |
|---|---|---|---|---|
| icecream_cone (8 mm) | 1.230e-4 | **8.21e-5** (inside the 1e-4 bar) | 0.0201 | 0.0209 |
| sphere (8 mm) | 8.23e-6 | 8.23e-6 | 0.0634 | 0.0634 |
| cylinder (8 mm) | 2.387e-5 | **1.42e-5** | 0.0528 | 0.0255 |
| plate_hole (6 mm) | 4.78e-6 | 4.18e-6 | 0.0516 | 0.0236 |
| cantilever (10 mm) | 1.4e-17 | 1.4e-17 | 0.1350 | 0.1350 |

Cone rms 5.02 → 3.41 um, worst sample 172.5 → 82.1 um, forward sharp-edge
metric 3.35e-6 → 7.57e-6 of bbox (both inside the feature test's 0.10·h
ceiling). Zero inverted and zero below-floor cells on every fixture.
Mirrored-tet fraction exactly 1.0 on all seven symmetry cases (347,742
assertions). ctest 447/447.

Two honest movements, reported not tuned:

- Cylinder and plate q_min fell (0.0528 → 0.0255, 0.0516 → 0.0236). The
  rescue's exact placements keep thin-but-positive caps that repair lifts
  less high than the old flow's. Both remain above the 0.02 floor with
  margin, and both fixtures' boundary fidelity improved. The trade is the
  contract working as designed; the numbers are here, not smoothed over.
- The cone's worst node is still 82 um off the BRep — one orbit the rescue
  improves but cannot fully place (its caps invert at full travel). The bar
  is a p99 and is met; the max is this paragraph.

## 6. What this does not fix

- The cantilever's worst cell (0.135) is a flat-walled box trade from the
  ADR-0037 smoother, untouched by anything here.
- The cylinder's rim-adjacent max (739 um on a small set near the rims)
  predates this change and is essentially unchanged.
- `tet`'s floor-bounded uniform stair residual (ADR-0035 §6) is a different,
  documented regime.

## 7. Consequences

- `mesh::BoundaryProjectionContext` now optionally carries the sharp-edge
  topology table; `make_boundary_projection` fills it. STL paths pass null
  and behave exactly as before.
- `POLYMESH_FIDELITY_DUMP` is the standing way to locate a fidelity tail.
- The showcase meshes move for every curved part; the showcase, its manifest
  numbers and SHOWCASE.md are regenerated in the companion change.
- The v8 advisor retrain gains one more reason: solve costs and mesh-derived
  labels shifted again on curved parts.
