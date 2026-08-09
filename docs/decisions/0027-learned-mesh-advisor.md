# ADR-0027: Learned mesh advisor — action-conditioned outcomes, not a winner classifier

- Status: accepted (2026-08-09); telemetry + pilot campaign in progress, no model trained yet
- Decision: D27
- Related: ADR-0023 (measure-first), ADR-0026 (metric adaptivity — the advisor's actuator), ADR-0022 (experiment warehouse)
- Program: [docs/plans/variable-everything-and-advisor.md](../plans/variable-everything-and-advisor.md) §Phase 3
- Research: [prior art](../research/ideabank/meshmlpriorart.md) · [corpora](../research/ideabank/caddatasets.md) · [labels](../research/ideabank/syntheticlabelgen.md) · [deployment](../research/ideabank/inferencedeployment.md)

## Context

`adapt::HpDriverPolicy` carries fifteen hand-set constants — turn angle 15°,
thin-wall fraction 0.35, smooth-surplus ratio 0.45, costs (8.0, 2.5, 3.5),
Dörfler 0.3 — described in the header as "calibrated" and in the integration
comment as "campaign-fitted costs land later". They are guesses. Nothing in the
repository fits them to measured data.

Meanwhile `resolve_mesh_size`, mesher selection, `element_tendency`,
`skin_layers`, grading flags, `adapt_passes`, `eta_target` and `p_elevate` are
all chosen by fixed defaults regardless of the part or its boundary conditions.

The literature says the individual pieces are learnable. MeshingNet3D learns a
scalar tetrahedral size field for 3-D linear elasticity from geometry + BC +
material and reports parity with ZZ-guided meshes and clear gains over uniform
ones. E2N learns a goal-oriented error indicator and halves total adaptation
runtime at matched QoI error. Gillette–Keith–Petrides learn only the *marking
threshold* and reach target error with 18–61 % of the cumulative DOFs of the
best fixed threshold, transferring from 2-D corners to a 3-D Fichera corner
without retraining. Fidkowski–Chen learn an SPD metric's anisotropy.

No published or commercial system learns the **joint** action — mesher family,
global h, shape mix, grading, skin layers, order, adaptive schedule — for a
general CAD solid under structural BCs against measured accuracy per DOF and per
second. Ansys "Mesh Agent", Siemens PhysicsAI, Altair shape recognition and
Cadence "AI-driven meshing" are workflow automation, failure diagnosis, or field
surrogates; none discloses a learned mesh-parameter optimiser with benchmarks.

## Decision

### 1. Learn action-conditioned outcomes, never a "best config" label

The dataset row is

```
(context features, action) -> {rel_err, DOF, mesh_ms, solve_ms, peak_mem, quality, failure}
```

and the models are regressors for `log(error)`, `log(DOF)`, `log(seconds)` plus
a feasibility probability. Ranking happens at query time against the user's
budget.

A single-winner label is rejected: near-ties flip under timing noise or a small
utility-weight change, and an action that was never evaluated can never win.
Failures are labels (`failure_type`, time-to-failure), not missing rows.

### 2. A second head predicts the sizing field

`log(h*(x)/L)` sampled from converged adaptive expert meshes, normalised by the
bbox diagonal. This is the MeshingNet3D/LAMG/AMBER target, and after ADR-0026 it
has a real actuator: `pipeline::RefinementPlan::size_field`. We do **not**
generate metric-tensor labels until a metric-aware mesher exists.

### 3. Context features are scale-free and cheap

`pipeline::extract_case_features` runs on the imported model and the BC regions
only — no mesh, no solve, well under a second. Every length is divided by the
bbox diagonal. Actions never leak into the feature vector.

### 4. Reference truth

Per (CAD, BC) case, one converged adaptive quadratic-tet reference, stopped when
two successive refinements move compliance, strain energy and loaded-interface
displacement by < 0.25 % and the estimated energy error is < 0.5 %; amortised
across every candidate action for that case. A 10 % audit subset gets a further
level plus Richardson-style QoI extrapolation. MMS cases are an independent
audit set, not ordinary training data.

Scoring a raw nodal stress maximum remains forbidden
(`docs/research/campaign-metrics.md`): use face-mean SCF, a volume-weighted
percentile, or energy.

### 5. BC sampling varies topology, not magnitude

Linear elastostatics scales: multiplying every load by a constant scales the
answer and changes nothing about where resolution is needed. Sample fixture and
load **face topology**, direction, and footprint. Reject setups with residual
rigid modes, near-zero strain energy, overlapping load/support, or point
singularities — and record the rejection reason.

### 6. Corpus and licensing

Primary, commercially clean: **SFEM** (MIT, ~16k STEP with fixed-facet masks,
loads and FEniCSx elastic fields), **MFCAD++** (CC BY, 59,665 STEP), one **ABC**
chunk behind an OCCT solid gate and a licence-provenance gate, plus our own
procedural OCC/CadQuery template families.

**SimJEB** and the **Fusion 360 Gallery** are non-commercial: they live in an
isolated evaluation lane and must not touch a shipped checkpoint. **GrabCAD is
not a usable corpus** — its §6.2 cross-licence is non-sublicensable,
non-transferable and non-commercial, there is no bulk API, and provenance cannot
be established at scale. Every training manifest records the union of input
licences.

### 7. Deployment: LightGBM C API, no Python at runtime

Train in Python (LightGBM, pinned), ship inference through the LightGBM C API
from vcpkg — about 4 MiB on Windows, single-threaded, deterministic, with a
schema manifest and Python/C++ parity vectors checked at load. ONNX Runtime is
deferred until a geometry encoder demonstrably earns its ~20 MiB.

### 8. The advisor proposes; the estimator disposes

An advisor suggestion is a starting point, never an answer. ZZ, the health
gates, the load-area check and the resource guards all still run. An
out-of-distribution query — detected by feature-space distance to the training
manifold and by regressor interval width — falls back to today's deterministic
defaults and says so in the mesh note.

## Consequences

- The campaign harness must emit schema `advisor-row-v2` with explicit
  `features` and `action` objects, and persist per-pass adapt traces. Without
  that, only a case-level chooser is trainable — the current 40 warehouse runs,
  all at `order=1`, cannot train anything.
- The action grid in `campaign.json` must actually sweep the action space.
  Today it sweeps `{mesher, feature_refine, order=1}`.
- Determinism survives: inference is single-threaded on a fixed model artifact,
  and the chosen `SimSetup` is quantised before use so a float wobble cannot
  change the mesh.

## Rejected

- **Deep RL over per-element refine/coarsen actions.** Expensive, unstable, and
  aimed at anticipatory time-dependent refinement we do not have in linear
  statics. If a policy is learned at all, learn the *global marking controls*
  (Gillette-style) over the trusted estimator first.
- **A neural surrogate that replaces the solver.** It destroys the evidence-first
  claim; MeshGraphNets rollouts decohere and remain unverifiable per-result.
- **Training on scraped GrabCAD.** No bulk mechanism, no commercial grant,
  non-transferable rights, unestablishable provenance.
- **Treating published FEA datasets' meshes as optimal-mesh labels.** They
  contain one chosen discretisation per geometry; that is a physics prior, not a
  mesh-policy label.
