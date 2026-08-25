# External comparisons: Gmsh and CalculiX

Two external comparisons are committed. They isolate different components. The
Gmsh comparison swaps the mesh source and holds PolyMesh's solver, probe, BCs
and truth fixed. The CalculiX comparison swaps the solver and holds the mesh
fixed. Neither is an end-to-end matched-CAD comparison of both mesher and
solver.

## Gmsh: swapping the mesh source

The matrix below is the committed one
([`a25b4ec`](../bench/results/gmsh-peer.json), 336 rows, engine `f372e83`).
Order-1 medians are the clean comparison, and our meshers win all four families
on accuracy:

| Case family | Gmsh mesh | Native default | Native graded | Accuracy winner |
|---|---:|---:|---:|---|
| Box-hole SCF | 0.3780 | 0.4831 | 0.1493 | native graded |
| Stepped-shaft tip deflection | 0.2381 | 0.0148 | 0.1399 | native default |
| Thin-walled tube | 0.1448 | — | 0.0802 | native graded |
| Perforated plate | 0.7622 | 0.3318 | 0.4163 | native default |

The box-hole row moved a long way, and that is a bug fix rather than better
meshing. An earlier README reported Gmsh dominating the hole family at both
orders; that result was measured on an engine
which silently deleted the bore, so the geometry being compared was not the
geometry requested. The number moved because the engine was corrected. Nothing
about the mesher improved, and the previous figures should be read as having
measured the wrong solid rather than as having been beaten.

### Charging for degrees of freedom

The accuracy wins are bought with degrees of freedom, and the ranking flips once
you charge for them. Median DOF at the same rungs: box-hole 738 (Gmsh) versus
6,242 (graded), tube 540 versus 5,142. On median `relative error × DOF`, Gmsh
still wins two of the four families — box-hole 278 versus 849 and tube 84 versus
363 — while native wins stepped-shaft 17 versus 69 and perforated-plate 678
versus 1,171. Neither tool is uniformly better. Ours is more accurate per case;
Gmsh is more economical on the curved-hole families.

### Coverage

Coverage is thin, and the reason matters. Of 336 rows only 159 are `ok`: 166 are
refusals, 9 are outright failures and 2 are honest timeouts. All 9 failures are
Gmsh's own, against zero native failures, and all 9 are thin-walled tubes
concentrated in two parts. They span all three rungs (`h_rel` 0.20 ×4, 0.12 ×4,
0.08 ×1), so coarse sizes dominate without fully explaining them. That geometry
is near-unmeshable by either tool; the difference is that our refusals name the
size they would need instead of emitting a mesh that cannot be solved. Gmsh
records 0 refusals against our 166 because it has no refusal path — it either
meshes or fails — which is the clearest argument in the matrix for having one.
The 2 timeouts are `perforated_plate_s2_c1` graded at `h_rel=0.08`, recorded as
timeouts rather than dropped. Because refusal counts differ per source, the
medians are over each source's own measurable rows and the `n` are unequal (12
Gmsh versus 6 graded on box-hole, for one), so this is not a strict matched set.

### Order-2 pairing

Order 2 remains an approximate pairing: the adaptive path produces a mixed-p
mesh, so an `order=2` native row is not the uniformly quadratic mesh a Gmsh
`order=2` row is. Where the uniform variant is used the pairing is exact, and
full parity was verified on 14 of 14 matched `polymesh-native-uniform-p2` rows.
Order-2 Gmsh meshes use high-order optimisation (`Mesh.HighOrderOptimize=2`; one
row needed the mode-1 fallback). Without it, four meshes contained inverted
tet10 elements that PolyMesh rejects.

### A defect the comparison exposed

The comparison also exposed a real stress-recovery defect on our side: ZZ patch
fits were extrapolated at p-elevated mid-side nodes. Fixing it (`08f9f55`) moved
`box_hole_s2_c0`, `h_rel=0.08`, order 2 from 2.595 relative error to 0.0072,
within 0.72% of Kirsch 3.0, while the spurious node fell from 10.79 MPa to about
1.2 MPa.

### Gmsh run-to-run noise

Gmsh optimisation has run-to-run noise of its own. Two serial, single-threaded
Gmsh 4.13.1 runs with identical order-2 / `HighOrderOptimize=2` inputs kept
connectivity and node/DOF counts but moved coordinates by up to 1.59e-3 m;
`stepped_shaft_s1_c1` at `h_rel=0.20` moved from 0.4103 to 0.4359 relative error
(control: 0.7844 → 0.8067). Single rows therefore vary by a few points, and the
medians absorb part of that noise.

![External mesh-source comparison](advisor/figures/external_comparison.png)

Order-1 median accuracy per case family, by mesh source.

## CalculiX: swapping the solver

CalculiX 2.23 and PolyMesh agree in tip deflection to better than 2e-5% at every
rung on identical structured hex8 cantilever meshes (48 / 216 / 1,200 / 7,776
DOF). Both converge toward the shared reference: 72.19 → 40.20 → 15.13 → 4.88%
error.

## Provenance

Unavailable points carry explicit nulls, and none were fabricated. A pre-fix
snapshot and the per-row attribution of what moved and why are archived under
[`bench/results/archive/`](../bench/results/archive/).

Sources: [Gmsh mesh-source results](../bench/results/gmsh-peer.json),
[CalculiX solver-parity results](../bench/results/calculix-cantilever.json), and
the [benchmark scoreboard](bench/scoreboard.md).

## What this does not establish

Speedups are self-relative, and the external comparisons are scoped. "5.12×
fewer DOFs, 12.2× lower wall time" is against PolyMesh's own frozen
uniform-tet10 baseline
([ADR-0005](decisions/0005-benchmark-baseline.md)). The CalculiX agreement above
validates solver formulation and assembly parity on identical meshes, not an
end-to-end mesher-plus-solver comparison. Elmer and Code_Aster remain unmeasured
([results](../bench/results/calculix-cantilever.json),
[scoreboard](bench/scoreboard.md)).

The default coarse product mesher can miss stress concentrations. At matched
order 1 on `box_hole_s0_c0`, the default hybrid missed the hole concentration
(0.664 relative error), versus 0.364 for a Gmsh mesh consumed by the same
PolyMesh solver and probe, and 0.190 for PolyMesh graded tet. That swaps the
mesh source, not the solver ([results](../bench/results/gmsh-peer.json)).
