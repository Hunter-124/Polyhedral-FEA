# ADR-0037: A box selection is a region, and a smooth field is sampled on element sizes

- Status: accepted (2026-08-19)
- Companion to [ADR-0036](0036-a-symmetric-part-gets-a-symmetric-tiling.md) (a
  symmetric part gets a symmetric tiling) and
  [ADR-0035](0035-boundary-conformity.md) (the boundary lands on the BRep).

## 1. The report

> we are at the finish line, last thing to fix is like better uniformity now that
> we have symmetry, like the stress line on the ball that should be straight has a
> noticeably jagged edge which isn't correct

The image was `docs/assets/showcase/gallery_sphere.png` — the von Mises band just
below the clamped-and-loaded upper cap of `sphere.step`, solved at h = 8 mm. The
load is axisymmetric (`--fix-box z <= -40 mm`, `--load-box z >= 40 mm`, a -z
resultant of 3141.59 N), so every iso-line of the answer is a circle of latitude.
The rendered one is not: it scallops, with a four-lobed envelope and
element-scale roughness on top of it.

ADR-0036 had just driven the mirrored-tet fraction to exactly 1.0 on every
fixture, so the pattern is not asymmetric any more. It is non-uniform, and the
question is which non-uniformity a smooth field is actually reading.

## 2. Measuring the jag

The instrument is the iso-line's own position. Take every node on the sphere skin
(all 16610 of them, mid-side nodes included, selected as |r - R| < 1e-9 of the
bbox diagonal, which they satisfy exactly), bin by azimuth into 180 sectors, and
in each sector find where von Mises crosses a fixed level by linear
interpolation in z. A perfect answer gives a constant. What it gave, at h = 8 mm:

| iso level | mean z | rms of z | peak-to-peak |
|---|---|---|---|
| 500 kPa | 37.53 mm | 0.391 mm (0.102 h) | 1.617 mm (0.420 h) |
| 600 kPa | 38.41 mm | 0.295 mm (0.077 h) | 1.160 mm (0.302 h) |

The azimuthal spectrum of that wander is the diagnostic. Every mode with
appreciable amplitude was a multiple of four (m = 4, 8, 12, 20, 36, 52), with no
broadband floor to speak of. A four-fold azimuthal structure about z is the
signature of the axis-aligned background lattice, not of noise.

## 3. Two causes, measured separately

### 3.1 The load stopped on a staircase

The first cause was not in the mesher at all. `--load-box` names a *region* of the
boundary surface, and the CLI turned it into a *set of faces* with
`fea::faces_within`: accept a face when every one of its nodes is inside the box.
On an unstructured skin that patch does not end on the plane z = 40 mm; it ends on
whichever element edges happen to be inside, which is a staircase.

| | |
|---|---|
| patch edge z, per azimuth sector | mean 41.14 mm, rms 1.06 mm, peak-to-peak 4.63 mm (0.28 h and 1.20 h) |
| loaded area per sector | 25.7% rms, 115% peak-to-peak |
| total patch area | 2.9481e-3 m^2 against the cap's 3.14159e-3 m^2 — 6.2% short |

So the traction magnitude was 6.2% high, over a region whose boundary wandered by
more than an element. Low-pass the iso-line wander and the patch-edge staircase to
the wavenumbers a viewer can resolve (m <= 24) and they correlate +0.92 at the
500 kPa level and +0.88 at 600 kPa.

`fea::consistent_region_load` integrates over the intersection of each face with
the box instead. Each reference triangle of the face's parameter domain is split
into 4^3 sub-triangles; each sub-triangle is clipped by the six box planes with
Sutherland-Hodgman in its own linearisation, so a vertex created on a plane sits
exactly on it and the next plane's sign test is consistent; the clipped polygon is
fan-triangulated and the unit-triangle rule is mapped onto the pieces. A face the
region does not cut is detected from the same lattice samples and gets the plain
`face_rule` back, so a load region whose boundary misses every face integrates bit
for bit what it integrated before.

Subdivision level is chosen on measured convergence of the clipped area, sphere
skin, `--load-box z >= 40 mm`, 1312 candidate faces:

| level | clipped area (m^2) | relative to level 5 |
|---|---|---|
| 0 | 3.137614e-3 | -1.3e-3 |
| 1 | 3.140484e-3 | -3.5e-4 |
| 2 | 3.141320e-3 | -8.5e-5 |
| 3 | 3.141527e-3 | -1.9e-5 |
| 4 | 3.141575e-3 | -3.9e-6 |
| 5 | 3.141588e-3 | — |

A clean factor of four per level, and level 5 sits 1.5e-6 under the analytic
2 pi R (R - 40 mm) = 3.1415927e-3 m^2. Level 3 spreads its 1.9e-5 of area over a
0.188 m patch edge: 3.2e-7 m, or 1.8e-6 of the bounding-box diagonal, 55 times
inside the 1e-4-of-bbox bar the mesher itself is held to. The shipped C++ reports
a clipped area of 3.14152661e-3 m^2 on the h = 8 mm tet10 skin; an independent
NumPy implementation of the same algorithm, written from the same description and
not from the code, reports 3.141526610e-3 — agreement to ten significant figures.

Result on the picture: iso-line peak-to-peak 1.617 mm -> 1.245 mm (-23%), and the
staircase's own modes collapse — m=8 2.61e-4 -> 6.4e-5, m=12 2.30e-4 -> 7.4e-5,
m=20 2.29e-4 -> 1.1e-4.

### 3.2 The wall smoother was not equalising anything

The second cause is the mesh, and the pass that exists to fix it was solving the
wrong problem. `smooth_boundary_nodes` relaxed each free wall node toward the
centroid of its 1-ring and re-projected. The fixed point of that iteration is "I
sit at my neighbours' centroid", which on a surface of irregular valence — and a
lattice skin with per-cell Kuhn diagonal rotation has valences 4 through 8 — is
not "my elements are the same size". The pass therefore did not converge slowly
toward uniformity; it converged to something else. Sphere skin, equivalent
equilateral edge length, p95/p05:

| passes | 3 | 8 | 20 |
|---|---|---|---|
| neighbour centroid | 2.15 | 2.20 | 2.33 |

Worse with more work is the signature of a wrong objective, not a weak one.

The replacement is the area-weighted mean of the incident faces' centroids. It
decomposes exactly — writing `c_f` for face centroids, `A_f` for areas, `A` for
their mean:

```
  sum(A_f c_f) / sum(A_f) - p  =  sum((A_f - A) c_f) / sum(A_f)  +  (sum(c_f)/n - p)
```

The first term is a pure area-imbalance signal: zero when every incident face has
the same area, and otherwise pulling the node *into* the oversized face and
shrinking it. The second is plain 1-ring regularisation. Both halves were built
and measured on their own, and each alone is worse than the sum, because size
uniformity and triangle shape are separate properties and one term buys each:

| free-node target | size p95/p05 | max/min | azimuthal spread in band | min angle p01 | normal p99 vs exact sphere |
|---|---|---|---|---|---|
| neighbour centroid (old) | 2.15 | 3.46 | 14.5% | 26.7 deg | 1.90 deg |
| area imbalance only | 1.67 | 2.11 | 10.3% | 20.3 deg | 4.23 deg |
| equal edge lengths | 1.77 | 2.38 | 10.6% | 22.2 deg | 3.55 deg |
| **area-weighted centroid** | **1.78** | **2.52** | **10.6%** | **26.4 deg** | **2.55 deg** |

The two size-only objectives reach the best spacing and pay for it by skewing
triangles by a quarter of their worst angle. The sum reaches the same spacing at
the old target's shape.

Pass count stays at 3 and is now capped at 10. It is shared with the crease-chain
relaxation, which slides nodes *along* a sharp edge and is not idempotent: on
`cantilever.step` at h = 10 mm, 8 passes take the worst cell's shape quality to
0.093 and 20 passes to 0.058, on a box whose planar walls have no spacing left to
win.

## 4. Four-fixture matrix

Graded mesher, standard fixture sizes, `n_below_shape_floor` = 0 and no inverted
cells in every row.

| part | q_min before | after | boundary p99/bbox before | after | normal p99 before | after |
|---|---|---|---|---|---|---|
| sphere (h=8mm) | 0.0545 | **0.0634** | 9.10e-6 | **8.23e-6** | 0.238 deg | 0.320 deg |
| icecream_cone (8mm) | 0.0214 | 0.0201 | 1.429e-4 | **1.230e-4** | 5.67 deg | **4.46 deg** |
| cylinder (8mm) | 0.0527 | 0.0528 | 1.368e-5 | 2.387e-5 | 3.25 deg | 4.16 deg |
| plate_hole (6mm) | 0.0512 | 0.0516 | 4.46e-6 | 4.78e-6 | 0.598 deg | **0.575 deg** |
| cantilever (10mm) | 0.1838 | 0.1350 | 1.4e-17 | 1.4e-17 | 0 | 0 |

Boundary-to-BRep distance improves in the *bulk* on both curved-wall fixtures
(cylinder rms -13% and p95 -17%; cone rms -14% and p95 -6%), and the cone — the
one fixture still outside the 1e-4-of-bbox bar, a pre-existing ADR-0035 item —
improves at every percentile, its worst point halving from 3.38e-4 to 1.73e-4 m.

Two regressions, recorded rather than tuned away:

- The cylinder's boundary-distance tail worsens even though its bulk improves:
  p99 3.35e-6 -> 5.85e-6 m, max 6.32e-4 -> 7.32e-4 m. A small set of points near
  the pinned rims sits further from the BRep.
- The cantilever's single worst cell falls 27%, to 0.135 against the 0.02 shape
  floor, with mean quality unchanged at 0.267. A box has no wall spacing to win,
  so the pass only trades there.

Mirror symmetry is unaffected: incident-face lists are sorted on the
reflection-invariant `MirrorKeyFrame` key so a node and its image accumulate the
same area-weighted sum in the same order, and `tests/test_graded_fill.cpp` still
measures a mirrored-tet fraction of exactly 1.0 on all seven CAD cases.

## 5. What is left, and what it is

The residual is discretisation error at a traction discontinuity, and it is
labelled that way rather than called a defect.

The loaded region ends on a plane, so the applied traction jumps from 1.0 MPa to
zero across the circle z = 40 mm. That is a genuine singularity in the elasticity
solution, and the visible band *is* that singularity. Two measurements pin the
residual down.

It is element-scale, not a coherent wave. Remove the axisymmetric part of the skin
von Mises field (a binned mean in z) and the residual has rms 3.9e4 Pa, 5.6% of
the local mean, in the band. The rms difference between the residuals of two
*adjacent* skin nodes is 1.21 times that — an uncorrelated field would give
sqrt(2) = 1.41, a spatially coherent one would give much less. So most of what a
viewer sees is one-element roughness with a four-fold envelope, and the primal
displacement already carries it: uz scatters 1.9% of its own band mean at
z = 37.5 mm, before any stress recovery.

It converges. Solving the same case at four sizes, iso-line rms at the 600 kPa
level:

| h | 12 mm | 10 mm | 8 mm | 6 mm |
|---|---|---|---|---|
| iso-line rms | 0.298 mm | 0.231 mm | 0.251 mm | 0.154 mm |
| as a fraction of h | 0.077 | 0.060 | 0.065 | 0.040 |

First order in the iso-line's position across the range (0.95 measured over the
2x span), with individual azimuthal modes moving around as the lattice phase
changes relative to the cut. Nothing here is a fixed pattern that survives
refinement, which is what a mesher defect would look like.

## 6. Consequences

- Any new box-selected load must go through `fea::consistent_region_load` and
  `fea::faces_touching`. `fea::faces_within` remains correct for a selection that
  really is a face set — a CAD region, a slab, the normal-aligned fallback — and
  wrong for a volume of space.
- `--force` and `--traction` both now report a `clipped area` in the `load:` line
  when the selection came from a box. The number changed on every box-selected
  fixture, because the old one was measuring the staircase.
- The graded, hex and tet fills, and both pipeline smoothing sites, share the new
  free-node target; the crease-chain branch is untouched.
- Showcase renders and their manifest numbers move: same commands, different
  applied traction and different skin spacing.
