# ADR-0030: The ruler was wrong — retracting the fan-transition defect, and drawing the curvature we already compute

- Status: accepted (2026-08-13); all changes shipped and verified by running the product
- Decision: D30
- Related: ADR-0012 (hybrid/graded tet), ADR-0023 (measure-first), ADR-0028 (boundary conformance), ADR-0029 (independent truth, one-rule-one-implementation)
- Evidence: commits `c3fbe62`, `9f6bba3`, `9cb1c8a`, `f224d57`, `2294b00`; `tests/test_quadrature.cpp`, `tests/test_mixed_fill.cpp`, `tests/test_geometry_fidelity.cpp`, `tests/test_cell_quality.cpp`, `tests/test_curved_mesh_quality.cpp`; suite 407/407
- Supersedes: the mixed-level branch of the fill-stage guard introduced in `1d48978`

## Context

The reported symptom was "massive holes and hella defects, especially along the
edges and curved surfaces". Two candidate causes were on the table, both
documented as measured: the hybrid mesher's 2:1 fan transition losing 36–40 % of
solid volume on every mixed-level fill, and a display path suspected of
under-representing curved geometry.

Both turned out to be real problems. Neither was the problem it looked like.

### The fan transition never lost anything

`unit_box.step` at `-h 0.22` fills to 992 hexes plus 24 transition pyramids,
expands to 5,976 `kPyramid5` product cells, and reports `snap max|d| = 2.48e-16
m`. Those cells tile the box exactly: 992 × 0.1³ + 0.2³ = 1.0 m³. The engine
reported `geometry_fill_volume mesh=0.6 cad=1 rel_err=0.4`.

0.6 is not a discretisation artefact. It is a ratio, and it is exact.

`fea::eval_pyramid5` is a **collapsed-brick** map: `N_{0..3} = ¼(1±ξ)(1±η)·t`
with `t = (1-ζ)/2`, and `N_4 = 1-t`. Every base shape function already carries
the factor `t`, so the image cross-section at height ζ shrinks like `t` while ξ
and η keep their full ±1 range. Its parametric domain is the **cube**.

`fea::pyramid_rule` integrated over the reference **pyramid**: points at
`ξ = a·s` with `a = (1-ζ)/2`, weighted by the Duffy Jacobian `a²`. Against a
cube-domain map that double-counts the collapse — it integrates `(1-ζ)⁴` where
the map needs `(1-ζ)²`. On the unit-hex fan it returns 1/10 per pyramid instead
of 1/6. 1/10 ÷ 1/6 = **0.6**.

Both halves are individually defensible and were written against different
mental models of the same element; the comment above each describes its own
model correctly. Nothing in the codebase compared them, because no test coupled
a rule to the shape functions it integrates.

The consequences were not confined to a diagnostic. `pipeline` used the rule to
measure fill volume, `fea::assemble_body_load` used it to apply body forces on
pyramids, and `fea::recover_zz` used it to weight stress recovery on them.

### The guard then argued for the wrong thing, convincingly

`1d48978` had already noticed that the error barely moves as `h` shrinks, and
had reasoned — carefully, from real measurements — that the transition rather
than the resolution must be at fault. It shipped a refusal naming the fan as the
cause and `--mesher graded_tet` as the remedy, and a regression test that ran
the remedy and asserted it worked. The remedy did work. The explanation was
wrong, and the test could not tell the difference, because it verified the
recommendation rather than the diagnosis.

### The curvature was computed, gated, paid for, and then discarded on screen

ADR-0028 projects every order-2 mid-edge node onto the exact B-rep behind a
two-epsilon validity gate. `fea::collect_element_loops` then facets `kTet10`
from `n[0..3]` and `kHex20` from `n[0..7]`. `apps/gui/viewport.cpp` rasterises
exactly that, in both mesh-preview and results mode, and never reads its own
VTU. `scripts/vtu_wire_png.py` did the same thing to VTK types 24 and 25.

So a correctly curved rim drew as the straight chord between corners,
everywhere a human actually looks. On `plate_hole` at h = 0.02 m the wireframe
used 672 of 3,164 nodes and discarded all 2,270 mid-edge nodes, which sit up to
0.204 of an edge length off the chord — up to 25 px of missing curvature on a
1100 px canvas.

## Decision

### 1. A quadrature rule is defined by the shape functions it integrates, and a test must say so

`pyramid_rule` is now tensor Gauss on the cube. `tests/test_quadrature.cpp`
gains the contract that was missing: for every element type, `Σ w·|det J|` must
equal the exact volume of the reference cell **and** of a skewed affine image of
it, and `Σ w` must equal the reference domain's own measure. Weight-sum alone is
not enough — it catches a wrong normalisation but not a wrong domain.

`tests/test_mixed_fill.cpp` asserts a mixed-level hybrid fill is volume-exact at
all six previously-refused `h`, **and** that the mesh still contains pyramids,
so it cannot pass by quietly ceasing to exercise the transition.

| `-h` (unit_box) | before | after |
|---|---|---|
| 0.22 | 0.4000 | 1.010e-13 |
| 0.20 | 0.3926 | 2.517e-13 |
| 0.173205 | 0.3926 | 2.517e-13 |
| 0.16 | 0.3813 | 1.408e-13 |
| 0.14 | 0.3719 | 2.634e-12 |
| 0.125 | 0.3649 | 1.527e-12 |
| 0.11 | 0.3600 | 3.409e-12 |

### 2. A retracted cause is deleted, not reworded

The guard's mixed-level branch is gone. Recommending `graded_tet` for a cause
that does not exist would steer users away from the better mesher on the
strength of a retracted finding. The surviving message covers the one failure
mode that is still real — under-resolution — and names the halving that was
verified: `channel_s0.step` fails at 0.3351 (h = 0.015 m), gets **worse** at
0.3372 (h = 0.012 m), and clears at 1.102e-14 (h = 0.0075 m). The pyramid census
stays in the message as neutral diagnostics with an explicit note that the
transition is not the cause.

"Reducing -h a little does NOT fix this" is kept, because it is true of the
remaining mode and was independently re-measured.

### 3. Display and physics are two different boundary contracts

`extract_boundary_polys` (display) now splits quadratic faces through the
mid-edge nodes they already own: a triangle into four sub-triangles, a quad into
four corner triangles plus the central quad. Only existing nodes are emitted, so
no vertex is invented and nodal result scalars still colour every sub-facet. A
mixed-p face whose edges do not all carry mid-nodes is left whole rather than
torn.

`extract_boundary_faces` (physics — BC/load selection, traction area, B-rep
fidelity sampling) is untouched and stays on the corner topology. The header now
states which is which, because that distinction is where the bug lived.

Measured on a 60° `kHex20` cylinder sector with its outer-wall mid-nodes on the
true radius, worst gap between the drawn surface and the exact cylinder:
**4.019 mm → 1.022 mm**, i.e. `R(1-cos30°)` → `R(1-cos15°)`, 3.93×. The test
asserts the closed form rather than the measurement.

`scripts/vtu_wire_png.py` draws each exterior edge as an 8-segment Lagrange
polyline through end/mid/end, keeping face matching on corner nodes so mixed-p
meshes still cancel. `--straight-edges` reproduces the old output, so the defect
and its removal render from one script. `render_showcase.py` needed no change:
PyVista's `extract_surface` already runs at `NonlinearSubdivisionLevel 1`.

### 4. One definition of a cell's volume

Auditing for this bug class found a second instance of it.
`ElementCentroidStress.volume` was `|det J|` at a single reference point with
the domain measure dropped: exact for `kTet4` only because the next line
overrode it, coincidentally exact for `kPrism6` (reference volume 1), **0.125×**
for `kHex8`/`kHex20` and a non-constant **~0.09×** for `kPyramid5`.

It was dormant — only `.quality`, `.stress` and `.centroid` are ever read, so no
campaign label was corrupted — but it is precisely the landmine that the next
person to volume-weight these samples would step on. The root cause was three
copies of "volume of a cell", of which two were right.

There is now one: `fea::element_volume` / `fea::mesh_volume`. The pipeline's
copy and the duplicated VEM divergence loop are deleted.

### 5. Geometry may only be deleted if the boundary survives it

The same audit, run against the *other* half of the user's complaint, found the
graded mesher shipping genuinely torn shells on curved parts: `tube_s0` at
h = 0.0006 m carried **548** multiplicity-4 edges forming a duplicated skin down
the full 60 mm of the bore, and `sphere_box_s0` went **5 → 47** as `h` halved —
refinement made it worse. The default hybrid mesher was clean on all 11
configurations checked.

Nothing could see it, for the same structural reason as the pyramid bug: a slit
shell has the right volume, because the two torn patches coincide and cancel in
the divergence sum. Four of the seven defective meshes reported a **clean**
volume band. `check_tet_fill_geometry` validates node indices, finiteness and
positive tet volume; a slit violates none of them.

Three sites, one bug — each deletes or suppresses geometry one item at a time
and none checked what that did to the boundary:

| site | what it drops | measured damage |
|---|---|---|
| `hybrid_fill.cpp` local child carve | tets whose centroid lands in a void child | 30 torn edges on a lattice that arrived with 0 |
| `hybrid_fill.cpp` S4 sliver-cap collapse | an edge collapse with no link-condition check | welds the complex to itself |
| `boundary_faces.cpp` `suppress_opposing_partition` | a coarse face against the finer faces tiling it | 6 open edges on a watertight tet complex |

The stage census settled which mattered. On `sphere_box_s0` at h = 0.0072 the
Kuhn lattice and all four LEB passes are perfectly conforming — **0 torn edges
over 26,008 tets** — the child carve alone introduced 30, and the repair rounds
only whittled that down (30 → 17 → 6). The repairs were never the source; they
were mopping up after the carve, which is why fixing them first only moved
3 → 2.

All three now propose and the shell disposes. Where a deletion would strand a
neighbour with two exposed faces, **extend** (delete the stranded spike) is
tried first and **revive** (put a tet back) is the fallback, because at a void
carve reviving backfills material *into the hole* — measured, revive-only left
the `box_hole` bore wall 12 % larger and doubled `cylinder_prism`'s graded
surface residual. Extension is capped so a runaway cannot eat the solid, and if
neither converges the whole proposal is dropped.

Result: **14/14 watertight** across both meshers and 7 parts. `tube_s0` at
h = 0.00125 m with graded went from refused (rel_err 0.2446) to 0.09097.

The pipeline now runs a boundary-shell census after the fill, refuses an open or
non-manifold shell naming the counts, and carries
`boundary_shell edges=/open=/nonmanifold=` in every mesher note. The refusal
says explicitly that a volume check cannot catch this, and prints the mesh's
volume error next to the tear count to make the point unarguable.

**Two scorecard thresholds are re-baselined, and it is not a loosening.** Both
metrics range over the free-face set, so a torn shell was deleting the worst
facets from its own measurement. Pre-fix, graded on `cylinder_prism` at h = 0.12
reported `M1max` 0.007973 (0.066 h) and `M6` 0.01052 on a boundary carrying
**52 open and 5 non-manifold edges out of 8,103**. With the shell closed the
same mesher at the same h reports 0.009718 (0.081 h) and 0.003077, over 861 more
tets. The two fixtures whose graded shells were already sound are unmoved.
`score_volume` now REQUIREs closure *before* it measures anything — a strictly
stronger contract than either threshold, and one a mesher cannot satisfy by
dropping the facets it would have been judged on.

## Consequences

**The advisor's training data is contaminated, and the amount is measured, not
estimated.** Every one of the 141 distinct configurations that the fill-stage
guard refused across the checked-in campaigns was re-run on the fixed engine
(47 distinct `part × mesher × h`, the rest duplicates):

| channel | measurement |
|---|---|
| campaign rows total | 3,548 |
| rows carrying a `fill-stage guard` refusal | 948 (26.7 %) |
| refused configurations that now succeed | 16 of 47 (34 %) |
| **rows whose refusal label is now definitively wrong** | **76 (2.1 % of all rows, 8.0 % of refusals)** |
| still genuinely refused | 30 — `channel`, `tube`, `stepped_shaft`, `l_bracket`, all thin-walled under-resolution |

The second channel is larger. The shell fix changes graded-tet **geometry**, so
its accuracy labels move even where the row succeeded. Sampling 24 distinct
`part × h` graded configurations from the campaigns and re-meshing each:
**17 of 24 changed**, most by under 1 % but several by a great deal —
`stepped_shaft_s0` 8,225 → 10,336 tets (+26 %), `tube_s3` 7,959 → 11,883
(+49 %), `sphere_box_s2` 2,536 → 19,093 (+653 %). The large jumps are where the
carve had been tearing out the most material. `graded_tet` is 1,684 of the
3,548 rows.

Third, **body forces and ZZ recovery on pyramid cells were under-weighted by
0.6×**. Cantilever campaigns are traction-loaded so body force is largely
unexercised, but ZZ fed the adaptivity indicator on every hybrid mesh.

Regenerating the campaigns is hours of compute and is **not** done by this ADR;
it is a decision for whoever owns the next advisor revision. What this ADR
guarantees is that the contamination is enumerable rather than vague: rows whose
`error` contains `fill-stage guard`, and `graded_tet` rows on parts with curved
or stepped features.

The peer matrix is **not** affected — `bench/results/gmsh-peer.json` is
Gmsh-meshed and solved by our solver, so our mesher never touched it. The
showcase was regenerated and came back **bit-identical** (node counts, element
counts, DOF and peak von Mises match to the digit; `plate_hole.vtu` hashes the
same): those fixtures run at fine `h` on parts whose shells were already sound.

## What this says about the method

ADR-0023 says measure first. This is the case where measuring first was not
enough, because the instrument was the defect. Three independent things agreed
with each other and were all wrong together: the guard, the error message, and
the regression test that executed the message's advice.

What finally separated them was checking a quantity two ways that share no
code — the quadrature volume against the boundary-triangle divergence volume —
and noticing that a "discretisation error" was the exact rational 0.6.

The negative results are worth as much as the fix and are recorded so nobody
re-opens them: cell types are not silently dropped by either renderer (0 skipped
across 40 meshes covering VTK 10/12/13/14/24/25); free-face extraction has no
gaps at rims or 2:1 transitions (every boundary edge at multiplicity exactly 2,
including a 27,896-pyramid transition mesh); VTU node ordering for tet10 and
hex20 is correct and unpermuted; and `tet4`, `tet10`, `hex8`, `hex20`, `prism6`,
the traction face rule and the `cell_quality` ideal-volume constants are all
correctly domain-matched. VTK types 41 and 42 remain **untested rather than
cleared** — no VTU in the repository contains one.
