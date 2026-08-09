# ADR-0026: Anisotropic metric-field adaptivity, and making the size field real

- Status: accepted (2026-08-09); `adapt::MetricField` + size-field-driven fills in progress
- Decision: D26
- Related: ADR-0016 (LEB), ADR-0018 (graded LEB conformity), ADR-0019 (mixed FE/VEM adaptive order), ADR-0023 (measure-first)
- Program: [docs/plans/variable-everything-and-advisor.md](../plans/variable-everything-and-advisor.md) §Phase 1
- Idea bank: [docs/research/ideabank/meshknob-brainstorm.md](../research/ideabank/meshknob-brainstorm.md)

## Context

A 2026-08-09 source audit established two facts that invalidate the current
"variable size / density" claim.

**1. The scalar size field is computed and then discarded.**
`adapt::GradedSizing` is a correct min-plus, gradient-limited, isotropic field
\(h(x)=\mathrm{clamp}(\min_i[h_i+\beta\lVert x-x_i\rVert],h_{\min},h_{\max})\).
`pipeline::build_refinement_plan` builds the source list from curvature,
thin-wall and BC/load regions with genuinely different per-source \(h_i\) — and
then `adapt::seed_plan` throws every \(h_i\) away, returning only the source
*positions* plus a single ball radius \(1.5\,h_{coarse}\)
(`src/adapt/src/graded_sizing.cpp:172-184`). The graded, hybrid and varyhedron
meshers therefore refine by "is this cell inside any ball", not by \(h(x)\).
`kCvtPoly` is the only mesher that ever calls `size_at`
(`src/pipeline/src/scene.cpp:1791-1796`).

**2. There is no anisotropy anywhere.** No metric tensor, no directional sizing,
no aspect-ratio control. A thin wall in bending, a boundary layer, or a
stress field with a strong principal direction can only be resolved by shrinking
\(h\) in *all three* directions.

Independent research (three concurrent literature passes) converged on the same
conclusion: a solution-driven Riemannian metric field connected to a mature 3-D
metric remesher is the single highest-leverage missing capability for
accuracy-per-DOF, because it adds size, aspect ratio and orientation where we
have only size. Evidence: Wallwork et al.'s PETSc–ParMmg study, where Hessian
metrics in a solve/adapt loop retained the expected \(L^2\) rate on a sharp
solution that uniform refinement could not, with element volumes spanning eight
orders of magnitude (<https://arxiv.org/pdf/2201.02806>).

## Decision

### 1. `adapt::MetricField` is the new sizing contract

One SPD \(3\times3\) metric per point. A unit edge in the metric satisfies
\(\sqrt{e^{\top}Me}=1\), so \(\lambda_i = 1/h_i^2\). The type provides
log-Euclidean interpolation, eigenvalue and aspect clamps, metric intersection
by simultaneous reduction, Alauzet gradation limiting, continuous-mesh
complexity \(C=\int\sqrt{\det M}\,\mathrm dx\) with global normalisation, and a
**conservative** scalar projection (`isotropic_size()` returns the *smallest*
eigen-size) so every existing scalar consumer keeps working and can never
under-resolve.

Determinism is a hard requirement: eigenvalues sorted ascending, eigenvector
signs canonicalised by largest-magnitude component, fixed node traversal order,
and gradation sweeps run forward *and* backward for symmetry.

### 2. The scalar field must reach the default meshers before anisotropy does

`graded_tet_fill_surface` and `mixed_fill_surface` take an optional
`mesh::SizeFieldFn` and derive each cell's refinement level from
\(\mathrm{clamp}(\mathrm{round}(\log_2(h_{cell}/h(x))),0,L_{max})\) instead of a
binary ball test. An empty field reproduces today's behaviour bit-for-bit — the
existing 149-test suite is the regression gate. Seed-derived and field-derived
levels union by maximum, so callers passing both get the finer.

The 2:1 conformity guarantee is **not negotiable**. If a field-derived level
jump would break balance, the level is reduced; the existing closure pass runs
unchanged.

`h_floor` is the mesher's own minimum-cell-budget \(h\), so a field can never
demand more cells than the budget allows, and the clamp is reported in the
mesher note.

### 3. We adapt Mmg3d; we do not write an anisotropic remesher

Mmg/Omega_h/Pragmatic encode years of split–collapse–swap–relocate robustness.
Our differentiated work is CAD tag preservation across remeshing, metric
construction from the physics, and validation — not the kernel. Benchmark
adapters if needed, but **ship one**.

### 4. Error drives size quantitatively

The fixed `h_next = 0.75·h` is replaced by the equidistribution update
\(h_{new} = h\,(\eta_{target,e}/\eta_e)^{1/p}\), clamped and gradation limited.
Binary Dörfler marking survives only as a fallback when the field is unusable.

## Consequences

- `pipeline::RefinementPlan` gains a `size_field` and the per-source \(h\)
  values stop being dead data.
- Campaign rows can finally record what grading actually happened, which is a
  precondition for ADR-0027's learned advisor: the advisor's principal actuator
  is exactly this field.
- Anisotropy is *enabled*, not yet *shipped*. `MetricField` lands first with
  unit-level proof; the Mmg3d adapter and Hessian recovery are the following
  slice, gated on metric-edge histograms, CAD Hausdorff/normal error, tag
  preservation, positive Jacobians, repeatability, and an error-vs-DOF campaign.
- Claiming "anisotropic meshing" because a mesh contains stretched prisms is
  forbidden. The learned or computed output must control **orientation-dependent**
  size before that word is used.

## Rejected

- **Writing a native 3-D anisotropic remesher.** Multi-month robustness risk for
  a solved problem.
- **Metric-tensor labels for the advisor before the mesher can consume them.**
  Labels with no executable action are waste.
- **Keeping the seed+ball path as the primary mechanism.** It cannot express a
  smooth size transition, which is precisely what gradation limiting is for.
