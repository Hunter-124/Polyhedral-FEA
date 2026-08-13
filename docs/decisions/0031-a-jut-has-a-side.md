# ADR-0031: A jut has a side

Status: accepted
Date: 2026-08-13

## Context

Reported as "the sphere is busted and there's still missing faces/elements".

The graded sphere had craters in it: pits one element deep, with the pit floor
reported as the boundary. Measured on `tests/fixtures/parts/sphere.step` at
h = 8 mm, `--mesher graded`:

| metric | value |
|---|---|
| boundary nodes deeper than 0.25 h inside the fitted sphere | 12 |
| deepest | 9.46 mm = 1.19 h |
| same part through the showcase solve | one pit 12.18 mm deep |
| open edges | 0 |
| non-manifold edges | 0 |
| boundary shell components | 1 |
| renderer cells skipped | 0 |

Every crater was watertight. That is the point: the shell census introduced in
ADR-0028 answers "is the boundary closed", and a pit is closed. `hybrid` on the
same part at the same h kept its worst boundary node 0.65 mm off the sphere, so
the defect was not the geometry, the CAD import, or the renderer.

## The defect

`graded_tet_fill_surface` runs an S5 void carve. It peels the tets of *juts* —
boundary nodes still further than `0.15 * hc` from the surface after the first
snap round. The intent is sound: a jut is a Cartesian stair chord left hanging
inside a CAD hole, the snap could not pull it onto a wall, and peeling its tets
is how the hole opens.

But distance alone does not say which **side** of the surface the node is on,
and there are two ways to end up far from it:

- **outside** the solid, poking into a void — peel it, that is the contract;
- **inside** the solid, short of the surface, because projecting it outward
  would invert a skin tet — peeling it does not remove a spike, it digs a pit.

The second is the exact mirror of the first, and the carve treated them the
same. A sphere has no holes, so every jut on a sphere is the mirror case, and
the carve gouged it. This is the same shape of error as ADR-0030: a test that
was *nearly* the right test, applied to a population that included its own
mirror image.

## Decision

The distance proposes and the surface confirms. A jut is a node that is far
from the surface **and** outside it.

The inside/outside oracle — nearest triangle, its outward normal, the sign of
the offset — already existed inlined in the child carve twenty lines up. It is
now one lambda, `outside_solid`, used by both call sites; the inlined copy is
gone. Its ambiguous answers (a point exactly on the surface, a point with no
nearest triangle) return *inside*, because every caller uses it to authorise
deleting material, and the ambiguous answer has to be the one that keeps it.

## Consequences

Exact BRep fidelity, 10,000 trimmed-face UV samples, sphere at h = 8 mm:

| | before | after |
|---|---|---|
| mesh boundary → CAD, p99 | 3.529 mm | 0.313 mm |
| mesh boundary → CAD, max | 9.520 mm (1.190 h) | 0.471 mm (0.059 h) |
| CAD → mesh boundary, max | 3.436 mm (0.430 h) | 0.465 mm (0.058 h) |
| boundary normal angle, max | 90.00° | 6.53° |
| fill `rel_err` | 0.01196 | 0.007429 |

Under-carving was the risk. It is the failure mode that once made holes vanish,
and the rejected "whole tet in void" variant of ADR-0030 died of it. So the
whole graded matrix was re-measured, and every hole-bearing part came back
**bit-identical** — which is itself the evidence that their juts really were
outside the solid all along:

| part | h (m) | rel_err | part | h (m) | rel_err |
|---|---|---|---|---|---|
| tube_s0 | 0.0006 | 8.75e-05 | box_hole_s0 | 0.00278 | 0.0001773 |
| tube_s0 | 0.00125 | 0.001168 | perforated_plate_s0 | 0.0032 | 0.0006716 |
| sphere_box_s0 | 0.0036 | 0.0009914 | l_bracket_s0 | 0.006 | 2.737e-14 |
| sphere_box_s0 | 0.0072 | 3.504e-05 | channel_s0 | 0.0075 | 0.004538 |
| stepped_shaft_s0 | 0.00253 | 0.01814 | plate_hole | 0.0056 | 0.0001077 |

All `open=0 nonmanifold=0`.

## The test

`tests/test_geometry_fidelity.cpp`, "a graded sphere has no boundary node carved
into its interior". It takes centre and radius from the boundary **bounding
box**: a crater is an inward defect and cannot move the box, so the reference
stays honest on the very mesh being accused — which a least-squares radius fit
would not, because the craters drag the fit. Run against the pre-fix mesh it
reports 11 carved nodes and fails; against the fixed one, 0.

Evidence figure: `docs/validation/figures/sphere_jut_crater.png`.

## What this does not fix

`stepped_shaft_s0` at h = 0.00253 remains the worst graded volume error at
`rel_err` 0.01814, unchanged by this work.
