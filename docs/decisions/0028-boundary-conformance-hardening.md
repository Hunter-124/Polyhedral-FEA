# ADR-0028: Boundary-conformance hardening across meshing, solving, probes, and evidence

- Status: accepted (2026-08-11); complete — projection and evidence-path changes shipped, and the truth rerun plus retrains it gated on landed (ADR-0029, [advisor 0006–0008](../advisor/0008-v4-corpus-retrain.md))
- Decision: D28
- Related: ADR-0012 (hybrid/graded tet), ADR-0022 (experiment warehouse), ADR-0023 (measure-first), ADR-0027 (learned advisor)
- Evidence: commits `18ee534`, `92455f9`, `f23b4d1`, `cb73261`, `08f9f55`; `tests/test_brep_fidelity.cpp`; `bench/results/gmsh-peer.json`

## Context

Order elevation put every quadratic mid-edge node at the chord midpoint, so a
curved order-2 rim missed the exact B-rep by the chord sagitta. The first
owner-aware projection in `18ee534` fixed that geometry, but its validity gate
sampled corner volumes and reference edge-midpoint locations. A graded
selective-p-elevation truth run then reached `element_stiffness: non-positive
Jacobian`: stiffness integrated at points the gate had never sampled.

The audit found adjacent evidence failures. The analytic box-hole SCF truth is
the Kirsch **peak**, 3.0, but the probe was an area-weighted mean over the rim,
creating a flat approximately 0.66 error floor. `scripts/vtu_wire_png.py`
rendered cells outside node counts 4/5/6/8 as zig-zag polygons. Pre-fix
`advisor-row-v2` campaigns then preserved labels from the old engine and probe.

A matched-order external baseline makes the cost of these mistakes concrete.
For `box_hole_s0_c0`, order 1, `h_rel=0.08`, the checked-in
`bench/results/gmsh-peer.json` reports Gmsh mesh + our solver at relative error
0.364 with 978 active DOF; our default hybrid at 0.664 with 2,808 DOF and SCF
approximately 1.01; and our feature-graded tet at 0.190 with 7,608 DOF. Graded
is most accurate; Gmsh is by far the best accuracy per DOF.

**Evidence reconciliation.** `18ee534`'s first implementation reached residuals
0.076 h and 0.0014 h, but its edge-midpoint guard was too weak. `f23b4d1`
deliberately traded some geometric accuracy for stiffness validity: final
residuals are 0.094 h and 0.034 h after quadrature-rule backoff. The code uses
corner signed-volume epsilon \(10^{-14}h^3\), Jacobian epsilon
\(10^{-8}h^3\), and the API `fea::default_rule`. The final plate-hole test
captures counts diagnostically rather than freezing them. The checked-in survey
runs all seven fixtures at 0.20/0.12 and only cylinder and plate-hole at 0.08;
the broader 7 x 3 sweep was ad hoc, not the committed regression grid.

## Decision

### 1. Project quadratic boundary mid-edge nodes onto exact CAD

`pipeline::make_boundary_projection` and
`pipeline::project_quadratic_boundary_mids` are the shared contract. Corners
owned by the same CAD edge project on that edge; the same face projects on that
face; the same vertex projects onto that vertex. Mismatched or unknown owners
classify afresh through the existing oracle, which writes provenance on first
use.

### 2. The validity guard follows stiffness quadrature

Two scaled checks define an acceptable incident quadratic element. Tet corner
signed volume, or every hex corner triple, must exceed \(10^{-14}h^3\). At
every `element_stiffness` integration point from `fea::default_rule`, both
Jacobians must be finite, the saved mesh must have `det(J)>0`, and the moved
mesh must have `det(J)>10^{-8}h^3`. Reference edge-midpoint samples are not a
substitute for the solver's rule.

These checks apply first to the full CAD displacement and then to each of
six bisection backoff steps. If the full move is invalid, keep the largest valid
fraction instead of treating every limited move as a revert. On plate-hole the
measured outcomes are:

- `h_rel=0.12`: 704 boundary mids, 702 full / 2 partial / 0 reverted; maximum
  residual 0.368 h -> 0.094 h.
- `h_rel=0.10`: 960 boundary mids, 958 full / 2 partial / 0 reverted; maximum
  residual 0.0876 h -> 0.034 h.

True reverts are therefore exceptional; a partial move is an intentional
validity-constrained improvement, not a projection failure.

#### Process lesson: campaign before retrain

Two defects passed the unit suite and surfaced only under real workloads. The
truth campaign found the weak projection guard; the external Gmsh comparison
found the ZZ recovery defect below. Campaigns and external baselines are
therefore engine-validation gates, not presentation after the model is trained.
Truth must be rerun **before** retraining whenever they expose an engine change.

### 3. Do not ship the void-jut repair

A merge-only `kHybrid` jut repair was measured in an ad hoc sweep of seven STEP
fixtures at `h_rel` in `{0.20, 0.12, 0.08}`. It fired zero times and exact-B-rep
deviation never exceeded 0.059 h, so it was deleted rather than shipped as dead
topology mutation.

The historical approximately 0.4 h jut in `tests/test_curved_mesh_quality.cpp`
is a graded-tet defect, not hybrid. The post-smoothing projection round is
load-bearing: deleting it regressed `icecream_cone` from 0.059 h to 0.191 h, so
it stays. The checked-in, deliberately looser guard rail is
`dist_max <= 0.25 h` and `dist_p99 <= 0.10 h` over the narrower grid above.

### 4. A peak truth requires a peak probe

Add `peak_vm` and `peak_vm_over_nominal`: maximum element von Mises inside the
selected box, optionally divided by nominal stress. The four
`box_hole_s*_c0` references use the latter. A mean remains valid only when its
truth is also a mean.

### 5. Render topology, not raw connectivity order

`vtu_wire_png.py` dispatches on VTK type. Tet10 and hex20 use corner topology;
VTK_POLYHEDRON (42) consumes `faces`/`faceoffsets`; VTK_CONVEX_POINT_SET (41)
is hulled; unsupported types are skipped. Every file prints a cell-type census
and skip count instead of drawing fictitious zig-zag cells.

### 6. Every order-2 producer projects

Every `fea::p_elevate` or other order-2 producer must use the shared helper, or
document why CAD projection is unavailable or inappropriate.
The exhaustive production inventory is:

- the two adaptive `SolveJob` selective-elevation sites in
  `src/pipeline/src/scene.cpp` project through the shared helper immediately
  after promotion;
- both private CLI selective-elevation sites in `apps/cli/main.cpp` do the
  same;
- synchronous order-2 campaign promotion in `apps/testlab/main.cpp` also
  projects before the solved-stage geometry-volume measurement and solve;
- the Gmsh reader preserves source-authored Tet10/Hex20 coordinates because
  an imported `.msh` has no associated `CadModel` at that layer; and
- the D6 synthetic L-domain benchmark has planar boundaries and no BRep, so
  its straight mids already lie on the exact geometry.

### 7. Engine changes require a row-schema bump and retraining

Geometry, validity, and probe changes invalidate advisor labels. Emission moves
from `advisor-row-v2` to `advisor-row-v3`; loaders reject stale rows loudly.
Pre-fix campaigns remain under `archive-v2/`, outside the live
`bench/campaigns/advisor-*` dedup glob. Retraining uses only post-fix rows.

### 8. Keep the external baseline as a tradeoff, not a winner claim

This matched linear comparison is context, not a universal winner claim.
Feature-graded tet wins absolute accuracy here; Gmsh wins accuracy per DOF by a
wide margin; default hybrid misses the concentration despite more DOF than
Gmsh. Future claims report error and active DOF at matched order and probe.

### 9. LEB projection may not destroy the progress measure

Longest-edge bisection hung narrowly on graded `box_hole_s2` at
`h=0.019757645353151405`. The LEPP walk and tie-break were deterministic, but
closest-point projection moved a free-edge midpoint crossing the hole void
almost onto an endpoint: a 0.011685674 m parent produced one child essentially
as long as the parent and one below 1e-12 m. Marks stayed at 867 while nodes and
tets grew every iteration: more than 32,800 splits preceded a 30 s test timeout,
and a CLI run exceeded 300 s at 99.6 % CPU with flat approximately 280 MiB RSS.
Neighbouring `h_rel` 0.09 and 0.07 both completed in under 5 s, despite the
latter producing more elements, so this was geometric pathology rather than
monotonic cost.

A projected midpoint is accepted only when **both** child edges are at most
0.75 times the parent. Otherwise LEB uses the validity-checked Euclidean chord
midpoint, restoring a decreasing propagation measure. An exact repeated-edge
LEPP walk throws a diagnostic error rather than spinning. Geometric projection
must never invalidate a refinement scheme's progress guarantee.

### 10. ZZ patch recovery may not extrapolate an under-determined fit

`recover_zz` samples one centroid stress per incident element, fits a nodal
least-squares patch, then evaluates it at the node. A p-elevated mid-side node
may have only four or five incident elements—as few samples as fit
coefficients—so the unsmoothed fit can extrapolate wildly. On
`box_hole_s2_c0`, order 2, the probe maximum was 10.79 MPa at a mid-side node
whose edge corners were 1.44/1.72 MPa; p95 was 2.24 MPa, p99 3.01 MPa, and the
corner-only maximum 3.02 MPa against Kirsch 3.0.

High-leverage extrapolation is bounded by an L2 gain guard, an SVD basis, and a
patch-mean fallback. Measured maxima moved from 10.786 to 2.979 MPa on s2 and
7.484 to 3.939 MPa on s3. The external `box_hole_s2_c0`, `h_rel=0.08`, order-2
row moved from 2.595 relative error to 0.0072.

## Consequences

- Rare partial nodes intentionally remain off exact CAD: projection and element
  validity compete, and both outcomes are reported.
- The guard pays for stiffness-rule Jacobians on the full move and up to six
  backoff trials so assembly does not discover invalidity later.
- Fresh classification grows shared provenance; callers preserve its context
  through elevation.
- The post-smoothing projection stays. We ship no merge-only jut repair; the
  committed guard rail records its narrower grid rather than claiming 7 x 3.
- Peak stress is noisier and more mesh-local than a mean, but is reachable by
  peak truth. Convex-point-set rendering relies on VTK's convexity contract;
  unknown cells remain visible in the skip count.
- `advisor-row-v2` compute is preserved but cannot train the new advisor:
  storage is sacrificed to provenance rather than stale labels.
- This ADR does not replace the default mesher with Gmsh, add a Gmsh
  dependency, or generalise the three-point baseline beyond the recorded case.
- The ZZ guard engages on approximately 16–21 % of order-2 nodes and up to
  approximately 20 % on some order-1 meshes. It changes recovered stresses,
  error estimates, and the adaptivity path—not a rare corner—because ZZ drives
  `mark_smooth` and therefore which elements p-elevation promotes. The running
  truth campaign was halted and restarted rather than mixing already-computed
  rows from the old recovery.
- LEB now prefers guaranteed progress over exact surface projection when the
  two conflict. The chord fallback remains validity checked, and a repeated
  propagation edge becomes a diagnostic failure rather than an infinite loop.

## Rejected

- **Chord-midpoint geometry.** It raises order while retaining a linear boundary.
- **A reference edge-midpoint sampler.** It can pass while the stiffness
  integration points fail; the consumer's quadrature must also be checked.
- **Keeping the zero-fire collapse as insurance.** Dead topology mutation adds
  risk without measured benefit.
- **Scoring a mean against a peak.** The target is unreachable by construction.
- **Guessing edges or mixing row schemas.** Both turn known incompatibility into
  plausible-looking but false evidence.
