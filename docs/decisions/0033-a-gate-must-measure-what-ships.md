# ADR-0033: A gate must measure what ships

Status: accepted
Date: 2026-08-15

## Context

Reported symptom: "weird defects, especially with the sphere and the ice cream
cone." Measured with `polymesh diag` across `hybrid`, `graded` and `tet` at
h = 0.008 / 0.005 / 0.003 m, the sphere and `icecream_cone` hybrid meshes came back
with `quality_min` **negative** — `-0.837` (sphere, h=0.003), `-0.779`
(icecream_cone, h=0.003), `-0.742` (icecream_cone, h=0.008). `plate_hole` at
h=0.003 measured `-0.129`, so this was never curvature-specific; it was
resolution-specific, and the curved parts reach it first.

A negative `fea::cell_quality` means the cell's isoparametric map is inside out.
The meshes were shipping folded cells, the solve did not fail, and every mesher
gate reported them healthy. Three independent causes, all the same mistake.

### 1. The pyramid gate measured a different cell than the diagnostics

`icecream_cone` at h=0.008, hybrid: **960 of 24,286 pyramids** scored below zero
under `fea::cell_quality` — every one of them corner-driven (the minimum base-corner
scaled Jacobian, not the volume term) — while the mesher's
`validity::pyramid_shape_quality` rated all 960 at **≥ 0.0608**. The mesher measured
the two assembly split tets; the diagnostics measured the base-corner Jacobian. Both
numbers were right about their own question, and no offender collector ever saw a
problem, so no repair round ever ran.

Making the snap gate use the corner measure "fixes" the sign and wrecks the mesh:
exact-BRep p99/h on `icecream_cone` h=0.008 goes **0.019 → 0.107** and max/h
**0.029 → 0.361**, because the only lever the snap has is to pull boundary nodes
back off the surface.

### 2. `hex_fill` gated on sampled signs, and nothing else

`hex_fill_surface` asked only whether the trilinear determinant was positive at the
centre and the eight 2×2×2 Gauss points. Measured: it shipped `fea::cell_quality`
**-0.3154** on the hole plate at h=0.10·extent and **-7e-17** (a flat cell) on the
sphere at h=0.15·extent with all nine determinants positive.

### 3. `snap_boundary_nodes` computed a whole-mesh proof and threw it away

The culprit-aware path ends with a recovery pass that re-pushes each fully restored
node as far toward the surface as its own incident star allows. Those are *local*
decisions taken in sequence, so a later node's push can re-break a cell an earlier
node shares, and the earlier node is never revisited. The function then ran

```cpp
offenders.clear();
collect_offenders(offenders); // mandatory final whole-mesh proof
```

and returned. Nothing reads that set — no caller of `snap_boundary_nodes` receives
it. The "proof" proved nothing. Measured on the new `ellipsoid_boss_s1` at auto-h,
hybrid: **4 hex8 cells** left the snap at `fea::cell_quality` **-0.9939**, with 7 of
their 8 nodes recovered to 0.5–0.7 h of travel.

## Decision

**A gate measures the cell that ships.** Where the shipped representation and the
measured one disagree, change the representation — do not weaken the measure, and do
not buy the measure with geometry.

1. **Corner-folded pyramids ship as their two assembly tets.**
   `element_stiffness` already assembles a `kPyramid5` as exactly two tets split
   along `validity::pyramid_split_diagonal` (`fea/src/assembly.cpp:77-115`). A
   pyramid whose base corner has folded, but whose two split tets have positive
   volume, *is* the union of those two tets. Emitting them is the same geometry, the
   same stiffness, and the same nodes — with no folded map. Cost: zero.
   `icecream_cone` h=0.008 `quality_min` **-0.742 → +0.0201** with p99/h
   **0.0190 → 0.0189** and max/h identical.

   The base quad is shared by the fans of the two lattice cells across it, so the
   mark propagates to the partner: triangulating one side only leaves the shared
   face non-conforming (measured: 2,152 boundary edges used by three or more faces).
   Splittability is a *validity* test (both split tets positively oriented), not a
   shape floor — a floor there refused splits whose tets were no worse than cells
   the mesh already ships, and left a `-0.082` pyramid on `cylinder_prism`.

2. **`validity::pyramid_shape_quality` is now the full `fea::cell_quality` measure**
   (split-tet aspect, base-corner scaled Jacobian, volume collapse). It is the
   honest report and the decomposition trigger. The snap and smoothing gates use
   `validity::pyramid_split_shape_quality` — split-tet aspect only, which is what
   the integrator sees. Folding the collapse term into the *gate* took the hybrid
   sphere's M1max from 1.7e-16 to 0.037 at h=0.15·extent, so it stays out.

3. **`hex_fill` keeps the assembly-aligned gate and gains interior relaxation.**
   A hex8 is assembled isoparametrically, so its corner Jacobian is not what the
   solver integrates, and a hex has no conformity-free decomposition to escape into.
   Gating on the corner measure costs 3 decades of fidelity for shape the solver
   never uses (M1max 0.0007 → 0.108 sphere, 9.6e-12 → **2.559** hole plate: half a
   cell off the surface). The real cure is room: interior nodes now relax toward
   their lattice-neighbour centroid between two snap rounds, each move kept only
   while every incident hex stays valid. Sphere M1max **6.9e-4 → 1.1e-16**,
   composite 0.8494 → 0.8503; hole plate 9.6e-12 → 7.2e-12.

4. **`snap_boundary_nodes` acts on its final sweep.** Every recovered node that
   still participates in a bad cell retreats all the way back, and the mesh is
   re-proved, to a fixed point. Each iteration permanently drops at least one node,
   so it terminates in at most one pass per recovered node. `ellipsoid_boss_s1`
   hybrid `quality_min` **-0.9939 → +0.0200**, 4 inverted cells → 0, p99/h
   0.342 → 0.319.

5. **The graded fill relaxes interior slivers (S6).** S4 collapses sliver caps and
   S5 peels the flakes that gain a free face, so both are boundary-facing by
   construction; a sliver wedged in the interior survives them. Its non-boundary
   nodes now relax toward their edge-neighbour centroid, a move kept only when the
   worst aspect over the node's whole incident star strictly improves. Boundary
   nodes are frozen, so it cannot cost a micron of fidelity, and monotone
   acceptance means it cannot make any cell worse than it found it.
   `cylinder` h=0.005 worst aspect **4.17e-05 → 5.86e-04** (14×) at identical
   element count, p99/h and volume error. It is a no-op wherever quality is already
   sound: the three scorecard geometries measure graded 0.8007 / 0.7924 / 0.5279
   with M6 0.02545 / 0.05093 / 0.03059 — unchanged to every digit.

## Open: the graded sliver chain

S6 raised the floor; it did not close the hole. `cylinder` at h=0.005 with the
graded mesher still ships a mesh `solve_elastostatics` cannot solve: both
preconditioners break down at a true relative residual of 1.3e6, on 194,098 valid
tets with a closed shell, zero inverted cells and a 0.001 relative volume error.
The cause is scale, not validity — min edge 1.9e-05 m against a nominal 8.5e-03
(0.004 h), and element stiffness scales with edge length, so the matrix spreads
over three extra decades. `hybrid` and `tet` solve the same part at the same h
(hybrid's own worst cell is 2.1e-06 and CG converges, so a sliver alone is not
sufficient — the short EDGE is what hurts).

Measured stage by stage, min tet aspect through the graded carve/snap alternations:

| stage | min edge / h | min aspect |
| --- | --- | --- |
| entering the alternation loop | 0.0021 | 1.20e-04 |
| after carve 1 | 0.0174 | 2.76e-04 |
| after snap 1 | 0.0059 | 1.58e-05 |
| after snap 2 | 0.0008 | 6.79e-06 |
| after snap 3 | 0.0007 | 7.53e-07 |
| after the final repair round | 0.0020 | 4.17e-05 |

Each `snap_round` costs a decade. Its offender predicate is
`tet_signed_volume > 1e-14·h³` — a machine-degeneracy test, precisely what
`mesh/cell_validity.hpp` was written to replace, and the graded fill never got that
treatment. Two fixes were tried and **both reverted, because neither made the solve
work**:

- a `1e-3` normalized shape floor on that predicate: min edge improved
  9.5e-06 → 1.9e-05 but min aspect went 4.17e-05 → 1.58e-05 and CG still broke down;
- a short-edge collapse phase in S4 (any edge below 0.05 h, through the existing
  `try_collapse` with its tear and volume censuses): produced a byte-identical mesh,
  because the offending edge is created *after* the last repair round.

The table also shows the mesh already enters the alternation loop at 1.2e-04, so the
chain starts upstream of the carve/snap loop, in the LEB refine + first snap. Curing
it means giving the graded snap a real shape floor AND a repair that runs after the
final carve/pull without re-burying faces — the ordering the current code
deliberately avoids ("the census gate stays last"). That is a re-engineering of the
graded snap pipeline, not a threshold change, and it needs a corpus-scale
regeneration to validate. Reachable only well below the training grid (h=0.005 on
`cylinder` is h_rel 0.024 against a grid of 0.10–0.20), which is why no labelled row
has hit it.

## Open: the ellipsoidal boss defeats both boundary snaps

The three new families were added to test non-circular curvature, and two of them
pass cleanly and converge. `lobed_shaft` and `twisted_loft` — a C2 periodic
B-spline section and a doubly-curved twisted NURBS loft — mesh with positive worst
cell quality at every resolution tried, and hybrid converges properly on
`lobed_shaft`: exact-BRep p99/h 0.0191 → 0.0105, boundary normal p99 26.8° → 0.2°,
relative volume error 0.0067 → 0.0017 from h_rel 0.06 to 0.03. Curvature-driven
sizing on a non-circular spline wall works.

`ellipsoid_boss` does not, in either mesher, at any resolution measured:

| run | h_rel | worst cell | inverted | p99/h | normal p99 |
| --- | --- | --- | --- | --- | --- |
| hybrid | auto | +0.0200 (hex8) | 0 | 0.342 | 85.0° |
| hybrid | 0.06 | **-0.2607 (pyramid5)** | **5** | 0.781 | 90.0° |
| hybrid | 0.03 | +0.0200 (hex8) | 0 | 0.968 | 85.1° |
| graded | auto | +5.6e-06 | 0 | 0.638 | 89.8° |
| graded | 0.06 | +2.1e-04 | 0 | 0.893 | 89.5° |
| graded | 0.03 | +1.3e-03 | 0 | **1.844** | — |

A p99/h approaching or exceeding 1 means the boundary is a whole cell off the CAD
surface for most of its nodes — the snap is refusing nearly every move — and a
boundary-normal p99 of 85–90° means the facets it did place are close to
perpendicular to the surface they represent. Refinement makes it *worse*, not
better, which rules out under-resolution as the explanation. The graded run at
h_rel 0.03 then fails to solve at all (CG breaks down, residual 2.1e7), consistent
with the sliver chain above.

The 5 inverted pyramids at h_rel 0.06 survive the decomposition of decision 1
because their two assembly split tets are not positively oriented either, so there
is no valid representation to ship them as — those cells are broken in every
measure, and the snap gate that should have refused the move that broke them did
not. That is the thread to pull first.

What distinguishes this part from `sphere_box`, which the same code meshes well, is
that the boss's two principal curvatures differ by a factor of (a/c)² ≈ 2 at the
pole and vary continuously over the surface, so the projection's local
closest-point problem has no constant scale. Nothing else in the corpus has that,
which is why nothing else caught it.

## Consequences

- The hybrid sphere and `cylinder_prism` meshes used to contain no `tet4` at all, so
  `test_curved_mesh_quality`'s M6 fell back to its free-face measure — which is
  deliberately excluded from `composite_score`. Those meshes were scored on five of
  six metrics *and* shipped inverted cells. They now contain the decomposed assembly
  tets, so M6 is tet-measured and enters the composite: sphere 0.8434 → 0.8200,
  cylinder 0.8222 → 0.7938, hole plate 0.5344 → **0.5476**. Every frozen floor still
  passes; none was moved. M1max moves for the same reason (sphere 1.7e-16 → 0.0012 =
  0.008 h): the free-face sample is two triangle centroids per warped quad instead of
  one quad centroid, and a triangle centroid of a warped quad is not on the surface.
  No boundary node moved and node counts are identical (3,161 / 8,185).
- `worst_cell_quality` (min `fea::cell_quality` over shipped cells, every element
  type) is asserted positive for graded and hybrid on all three scorecard
  geometries. That is strictly stronger than the composite-floor margin it replaces.
  `kHexFill` is exempt, and `hex_bad` says why.
- `polymesh diag` reports `quality_min_type` and `n_inverted_cells`. A single worst
  number cannot distinguish a fill defect from a snap defect, or one folded cell
  from a thousand.
- Element counts rise slightly where folds existed (`icecream_cone` h=0.008:
  25,230 → 27,254 cells at identical DOF). Every advisor label carrying an element
  count for a hybrid mesh with folds is therefore stale; the corpus needs a
  regeneration before the next retrain.
- `relative_volume_error` moves a little where cells were split (`icecream_cone`
  h=0.008: 0.00404 → 0.00440). The new number is the volume the solver actually
  integrates; the old one was measured on a cell the assembly never used.
