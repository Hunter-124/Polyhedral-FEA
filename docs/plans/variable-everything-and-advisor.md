# Variable-everything meshing + learned mesh advisor

**Status:** active program (2026-08-09). Supersedes ad-hoc "turn on the variable
knobs" work. Companion ADRs: [0026](../decisions/0026-anisotropic-metric-adaptivity.md),
[0027](../decisions/0027-learned-mesh-advisor.md).
Raw research: [`docs/research/ideabank/`](../research/ideabank/).

## Why this program exists

A source audit (2026-08-09, five independent read-only passes over
`src/adapt`, `src/mesh`, `src/fea`, `src/pipeline`, `apps/`) established that
most of the "variable everything" surface is **declared but not delivered**:

| Claim | Reality (verified) |
|---|---|
| Variable size / density | `adapt::SizingField` / `GradedSizing` are real scalar gradient-limited fields, but the **default meshers never evaluate them**. `build_refinement_plan` collapses every source to a point list plus *one* ball radius `1.5·h_coarse` (`adapt/src/graded_sizing.cpp:172-184`). Only `kCvtPoly` calls `size_at` (`pipeline/src/scene.cpp:1791-1796`). |
| Variable order | Shipped p-adaptivity is selective nodal tet4→tet10 / hex8→hex20 with **no interface treatment**. A promoted element beside a linear neighbour has a midside DOF the neighbour does not carry, so the displacement field is discontinuous across that face. `check_validity` accepts it silently (`fea/src/p_elevate.cpp:34-78`, `fea/src/assembly.cpp:153-177`). A **correct** conforming hierarchical assembler (`HpModel` / `assemble_hp`, hex p≤6, tet p≤4, shared-entity minimum rule) exists and has **no production caller**. |
| Variable shape | `element_tendency` is a **global mesher selector**, not per-cell shape choice. `drive_hp` emits per-element `shape_mark`, and exactly **0** of them change an element; they are averaged into one global vote that only fires on a non-LEB remesh (`adapt/src/hp_driver.cpp:356-388`, `pipeline/src/scene.cpp:3028-3055`). |
| Variable anisotropy | **Does not exist.** No metric tensor, no directional sizing, anywhere in the tree. |
| Adaptive by default | `adapt_passes = 0`, `eta_target = 0`, `bc_grading = false`, `p_elevate = false`. The product default is one mesh, one solve. |

Resolution is additionally capped by: auto-`h` clamped to `[diag/80, diag/6]`
(`pipeline/src/scene.cpp:303-306`), a 589,824-element / 1,769,472-DOF product
ceiling, a 524,288-cell Cartesian grid budget, hybrid's 49,152-element work
convention, and graded-tet's 200,000-tet LEB cutoff.

So: before any AI chooses mesh parameters, **the parameters have to mean
something**. Phase 1 makes them real; phase 3 learns them.

## Phase 1 — make the knobs real

### 1.1 `adapt::MetricField` (new)

> **Shipped under a different name (2026-08-16).** The type `adapt::MetricField`
> below was never built under that name. What landed is `adapt::Metric3d` +
> `adapt::MetricGrid` in `src/adapt/include/adapt/metric_field.hpp`; per
> ADR-0026's own status note, anisotropy is *enabled* but not yet fully
> *shipped* (the Mmg3d adapter and Hessian recovery remain the following
> slice). The capability description below still stands as the design intent.

One SPD 3×3 Riemannian metric field with:
log-Euclidean interpolation, eigenvalue and aspect clamps, metric intersection,
Alauzet gradation limiting, continuous-mesh complexity normalisation
(\(C=\int\sqrt{\det M}\,dx\)), and a conservative scalar projection so every
existing scalar consumer keeps working. Anisotropy is the single highest-leverage
missing capability for accuracy-per-DOF: it adds size, aspect ratio and
orientation where we have only size.

### 1.2 Sizing field end-to-end

`graded_tet_fill_surface` and `mixed_fill_surface` take a `SizeFieldFn` and
derive each cell's refinement level from \(\log_2(h_\text{cell}/h(x))\) instead
of "is this cell inside a fixed ball". Seeds keep working (they synthesise a
field). This is what makes *variable density* real in the default path — and it
is the actuator the advisor writes into.

### 1.3 Conforming variable order

Add `fea::LinearConstraints` (slave DOF = Σ wᵢ·master DOF) applied as a sparse
transform \(K_r = T^\top K T\), \(f_r = T^\top f\) before Dirichlet reduction.
`p_elevate` returns the midside DOFs sitting on a p1/p2 interface; each is
constrained to the average of its edge endpoints. That is the standard minimum
rule and it makes selective promotion a genuine conforming p-adaptive space.
Longer term (phase 2) the `HpModel` path becomes the production route for p>2.

### 1.4 Quantitative sizing from error

Replace the fixed `h_next = 0.75·h` with the equidistribution update
\(h_\text{new} = h\,(\eta_\text{target,e}/\eta_e)^{1/p}\), clamped and gradation
limited. Binary Dörfler marking stays only as the fallback.

## Phase 2 — resolution and shape

Raise the auto-`h` floor and product ceilings behind explicit opt-in; apply
per-element shape marks through agglomeration/local retopology instead of a
single global vote; curved (q=2) CAD boundary geometry so high order is not
capped by a faceted boundary.

## Phase 3 — learned advisor

### What is actually novel

Published work learns **one** of: a scalar size field (MeshingNet /
MeshingNet3D / LAMG / AMBER), an error indicator (E2N), an anisotropic metric
(Fidkowski & Chen, AdaptNet), or a refine/no-refine policy (RL-AMR, ASMR,
Gillette et al.). No published or commercial system jointly picks mesher family,
global h, shape mix, grading, skin layers, order and adaptive schedule for a
general CAD solid under structural BCs, against measured accuracy per DOF and
per second. That joint advisor is the novel piece; every component technique has
precedent.

### Label design (decided)

**Do not** label "the winning config" — near-ties flip under timing noise and an
unevaluated action can never win. Record

```
(context features, action) -> {rel_err, DOF, mesh_ms, solve_ms, peak_mem, quality, failure}
```

and learn action-conditioned regressors for `log(error)`, `log(DOF)`,
`log(seconds)` plus a feasibility probability. Rank the candidate grid at query
time under the user's budget. A second head regresses `log(h*(x)/L)` sampled
from a converged adaptive expert mesh — that head drives 1.2's sizing field.

### Reference truth

Per (CAD, BC) case: one converged adaptive quadratic-tet reference, stopped when
two successive refinements move compliance, strain energy and loaded-interface
displacement by < 0.25 % and estimated energy error < 0.5 %. Amortised over all
candidate configs for that case. MMS cases as an independent audit set. Never
score a raw nodal stress maximum.

### Corpus

> **Superseded (2026-08-16).** The multi-source external corpus below was
> **never ingested** — no SFEM, MFCAD++, ABC, or SimJEB data entered the
> training set. The shipped corpus is 100 % procedurally self-generated
> (`scripts/gen_primitive_corpus.py` + `scripts/build_advisor_dataset.py`);
> per `docs/advisor/0005-data-card.md`, "no third-party CAD corpus is
> included". The licensing analysis below is kept as the record of *why*
> external sources stayed out (GrabCAD's cross-licence; SimJEB's NC-only
> GrabCAD-derived CAD).

| Source | Licence | Take | Role |
|---|---|---|---|
| [SFEM](https://huggingface.co/datasets/cmudrc/SFEM) | **MIT** | ~16k STEP + `fixed_facet_mask` + loads + FEniCSx elastic fields, ≈3 GB STEP | primary commercial-clean geometry+BC corpus |
| [MFCAD++](https://pure.qub.ac.uk/en/datasets/mfcad-dataset-dataset-for-paper-hierarchical-cadnet-learning-from/) | CC BY | 59,665 STEP, 1.5 GB | machining-feature-labelled geometry diversity |
| [ABC](https://deep-geometry.github.io/abc-dataset/) | MIT paper / Onshape terms | one 10k STEP chunk ≤1.73 GB after OCCT solid gate | human-authored realism |
| Procedural (CadQuery/OCC) | ours | 12 template families × N | controllable coverage, known BC faces |
| [SimJEB](https://simjeb.github.io/) | **NC only** | 381 real brackets + full FEA | isolated non-commercial benchmark lane |

GrabCAD is **not** usable as a corpus: the §6.2 cross-licence is
non-sublicensable, non-transferable and non-commercial, there is no bulk API,
and provenance cannot be established at scale. SimJEB's CAD is GrabCAD-derived
and must stay out of any shipped checkpoint.

### Scale

> **Superseded (2026-08-16).** The 1,200-problem / ~29k-solve plan below never
> happened. What shipped (per `docs/advisor/0005-data-card.md`, final for this
> cycle): **96 cases** — 8 procedural families × 4 size regimes × 3 load cases —
> swept into a **2,412-row** `bench/advisor/dataset.csv`, against **96
> reference truths (88 external Gmsh + CalculiX, 8 closed-form)**. The
> literature-saturation argument below did hold at this smaller scale: at six
> families the effect of adding a family on held-out regret was unmeasurable,
> and eight families is where a 0.02-decade effect becomes detectable
> (data card §Composition).

1,200 independent (CAD, BC) problems × 24 mesh actions ≈ 29k candidate solves,
plus 1,200 reference trajectories. ~10³ solver-hours. Start with a 20-geometry
pilot (~190 solver-hours) to measure real cost before committing.
Literature support: ASMR saturates near 100 PDE cases; LAMG's 500-shape/5,000-PDE
model was within 1 % of its 2,500/25,000 model.

### Deployment

Train LightGBM in Python; ship inference through the **LightGBM C API** from
vcpkg (~4 MiB Windows), single-threaded, deterministic, no Python at runtime.
The advisor only *proposes*: the existing ZZ estimator and health gates remain
the guardrail, and an out-of-distribution query falls back to current defaults.


## Phase 4 — pre-solved and exactly-handled CAD features

The owner's idea — recognise recurring CAD patterns and handle them exactly
instead of brute-force meshing them — is sound, but only three of the obvious
routes actually earn a guarantee. Full analysis:
[`docs/research/ideabank/presolvedfeatures.md`](../research/ideabank/presolvedfeatures.md).

| Route | Guarantee | Conditions |
|---|---|---|
| **Mirror / cyclic symmetry** | **Zero added error**, continuum *and* discrete | Geometry, material, loads and BCs invariant or equivariant under the transform; exact transformed tie constraints on the cut faces |
| **Static condensation of a repeated fixed-interface subdomain** | **Zero added *discrete* error** | Linear; *all* coupling passes through the retained interface DOFs; `K_ii` nonsingular; `K_ii^{-1}` applied without approximation; identical geometry/material/interface basis/ordering on reuse |
| **Certified defeaturing** (suppress a small hole/boss/fillet and *bound* the induced error) | **Rigorously bounded** QoI or energy error | The feature is a valid small Neumann/Dirichlet perturbation; an equilibrated reconstruction exists; the QoI is stable |

What does **not** give a free lunch, despite appearances: an analytic enrichment
(Kirsch, Williams, Boussinesq) is exact *only for that mode*, and only if the
ideal geometry/loading assumptions hold, blending elements are corrected, and
the non-polynomial quadrature is accurate. It is a good research pilot, a bad
product foundation. A learned feature recogniser carries **no** physics
guarantee — it may propose candidates; a deterministic geometric check and a
numerical certificate must authorise the action.

Ordering: **symmetry first** (cheapest true zero-error win), then certified
defeaturing (biggest practical meshing relief, and it makes the honesty story
*better* rather than worse), then exact condensation of congruent repeated
features once pattern extraction is verified. SBFEM cells for cracks/notches
are the natural polyhedral-native complement to VEM, and sit behind those.

OpenCASCADE supplies the primitives (`ShapeAnalysis_CanonicalRecognition`,
`BRepAlgoAPI_Defeaturing` / `BOPAlgo_RemoveFeatures`, B-rep adjacency traversal)
but **no** semantic recogniser — "these faces are bolt-hole instance 7 of
circular pattern 2" is ours to build from canonical geometry + attributed
adjacency graph matching, with face provenance preserved through defeaturing.

## Non-goals

Native anisotropic remesher written from scratch (adapt Mmg3d instead);
automatic all-hex via frame fields; full IGA from trimmed STEP; global DG;
a neural surrogate that replaces the solver; reduced integration as a speed
feature; GPU sparse-direct on this hardware.

## Verification contract

Nothing in this program ships on a plausibility argument.
Every element/mesher/order path gets an MMS observed-order record; every
advisor claim is a held-out, family-disjoint campaign showing error-per-DOF and
error-per-second against tuned fixed defaults and against random action
sampling.
