# What the showcase film shows, beat by beat

The film (`advisor_cinema.mp4`, `advisor_cinema.gif`) puts four rows of text on
screen: a plain-English headline, the numbers that matter on that beat, the one
disclosure that applies to it, and a provenance stamp. That budget is deliberate
— see [ADR-0043](../../decisions/0043-a-film-someone-can-read.md) — and this file
is the other half of the deal. Every claim the film makes, the struct field or
function behind it, and every disclosure that used to be stacked six deep in 13 px
grey are here.

Nothing in the film is a mock-up. It is recorded from the GUI's own framebuffer by
`polymesh-gui --auto` (`apps/gui/cinema.cpp`), driven by
[`scripts/render_cinema.py`](../../../scripts/render_cinema.py), which also writes
`manifest.json` beside the video with the exact command, the encoder, the frame
geometry read out of the first PNG's IHDR, and every number the GUI printed on
stdout.

## The composition

| | |
|---|---|
| Take | 3600 frames at a fixed 1/60 s virtual timestep = 60.000 s. The clock is set from the frame **index**, never accumulated from real frame time. |
| Frame | 1920×1080. `--size` sets Xvfb and `POLYMESH_GUI_SIZE`; the recorded resolution is measured from the PNG rather than assumed. |
| Acts | `skeleton` 0.13, `deliberate` 0.15, `build` 0.18, `mesh_hold` 0.09, `solve` 0.45. At 60 s these are 7.8 / 9.0 / 10.8 / 5.4 / 27.0 s. |
| Analysis pane | 0.42 of the width. Exact-CAD evidence → four wide activation lanes → cell audit → active-equation graph, with direct opacity handoffs. During build the measured final pass remains visible while the later real mesh lands. |
| Bottom ledger | Constant height, horizontally composed: headline/numbers left, plain-language disclosure right, provenance full-width below. |
| Camera | Fit before frame zero by solving the perspective inequalities on all eight corners at the settled pane aspect, then re-fit to the exact rest∪fully-exaggerated result envelope. It never moves during the captured take. |
| Mechanics overlay | Two support glyphs and one force arrow are anchored to the selected CAD regions and persist through every act. Their soft pulse is presentation-only. During the exact linear load ramp the arrow length and stated resultant scale by the displayed λ; direction comes from the real resultant. |
### What is interpolated

Time, opacity, the shrink-toward-centroid reveal, the spatial handoff front, the
pre/post-filter spacing-glyph morph, the presentation-only pulse phases, and the
load factor λ. That is the whole list.

No displayed **number** is ever interpolated. The opening rings interpolate
marker diameter and colour between two measured target-h evaluations; their
before/after millimetre ranges are computed values, and no intermediate value
is labelled as another measurement. During result handoffs, the old and new
fields each retain their own measured scalar values and own normalization; only
the narrow front feather blends display colours. No activation, element count,
error indicator, stress value or progress value is ever synthesised. Where a
source is missing the film says which one and shows nothing in its place.

λ is the one interpolated quantity that is also displayed, and it is displayed
because interpolating it is exact rather than approximate — see
[the load ramp](#load-ramp) below.

## Act 1 — `skeleton`: the part

- **Outline source** is stated on screen and is one of:
  - `geom::extract_topology(*model.cad, 32)` — the STEP file's exact edge
    curves and the same 32-sample curvature traces used by product sizing.
  - `geom::detect_sharp_edges(model.surface, 30°)` — the tessellation's crease
    network, for mesh input that carries no BRep. The film calls this out as **not**
    a CAD skeleton when it happens.
  - unavailable, with the extractor's own message drawn verbatim.
- `skeleton_polylines` / `skeleton_points` are the counts of what was extracted
  and pushed to the viewport, not an estimate of the part's complexity.
- Nothing is drawn as mesh in this act or the next. Nothing has been meshed yet.
  The points on the part are explicitly **target-spacing rings**, not elements:
  ring diameter is proportional to target `h`, orange means finer and cyan
  coarser.
- `prepare_cinema_features` calls the production
  `pipeline::build_refinement_plan` twice with the same resolved `h`, geometry
  and BC/load regions: once with spectral filtering disabled and once with the
  final `SimSetup`. The two `size_field` functions are evaluated at a
  deterministic, bounded walk over `Model::surface.vertices` and at every
  sample of the selected CAD edge. The exact sample count and pre/post-filter
  target-h ranges are printed by the GUI and copied into the manifest.
- When feature grading is enabled, `SpectralSizingReport` supplies modes,
  retained energy and density changes. The wishbone fallback records
  `feature_grading=false`; its FFT is labelled geometry analysis only.
- The upper chart is one real `CadEdge::kappa_samples` trace and the output of
  `geom::lowpass_signal(..., 0.995)`. A scan cursor advances over the same edge
  samples highlighted on the part. The lower chart is the non-DC first
  conjugate half of the exact even-reflected FFT: retained modes stay teal and
  discarded modes dim as the inverse reconstruction takes over. The DC bar is
  omitted because it is mean curvature, always retained, and excluded from
  `modes_total`; showing it would flatten every non-DC mode that actually
  explains spacing variation. No spectrum is invented or decoratively seeded.
- The last opening beat sweeps the pre/post-filter `h(x)` rings over the part,
  so the viewer can see where the frequency-space change affects the eventual
  cell spacing rather than infer it from a chart alone. The completed field
  remains at full opacity across the chapter boundary, dims to a 0.22-alpha
  input map over the first 1.3 s of advisor scoring, and is not cleared.
- The analysis panel starts opening at 0.624 s and reaches full width at
  1.716 s, leaving about 6.08 s fully open on the default take.

## Act 2 — `deliberate`: choosing a mesh

The first 65% of the act shows one real forward pass per beat in chooser order:
108 candidate actions and one final re-score. At the default 60 s duration the
109 measured passes use a 53.7 ms display beat; the remaining 3.15 s holds the
final state, and the 1.6 s decision lead at the start of the next act extends
that inspectable hold. Candidate-specific prose does not flash during the pass
lane: the strip keeps one stable explanation while the network itself carries
motion.

The feature panel and full on-part target-spacing field enter this act intact.
Over the first 1.3 s the panel cross-fades into four wide activation lanes while
the rings settle to their carry opacity. The network therefore appears as the
consumer of the field just shown, not a fresh scene over an empty wireframe.

- **The node fills are the graph's own tensors**, read out of the ONNX session:
  `advisor::ActivationFrame::input` / `fc1` / `fc2` (post-GELU) / `heads`. Not a
  re-implementation of the forward pass.
- **Node radius and fill are `|a| / max|a| within each layer`**, not against one
  shared scale. Trunk and head magnitudes differ by roughly 10×, and one shared
  scale flattens the trunk into a uniform grey column. The same reason
  `scripts/advisor/figures.py` scales its activation heatmap per row.
- **Colour is the sign** of the activation, through
  `gui/colormap.hpp::signed_colormap` (ColorBrewer RdBu reversed, agreeing with
  matplotlib's `RdBu_r` to 0.0115 in unit RGB). Blue negative, near-white zero,
  red positive. Zero lands on the neutral centre, so the range mapped onto
  [−1, 1] is symmetric.
- **Connections**: the 180 strongest of 18,912, ranked per frame by
  `score = activation_norm × abs(weight)`, then drawn through the branch-colored
  `activation_layout.json` edge blocks (96×81, 96×96, 20×96). Opacity is the
  replay channel during build; stroke width remains the measured normalised
  `|w_ji · a_i|` so animation does not change the encoded magnitude.
- **Lane motion is a timing cue, not another value.** During candidate scoring,
  a subdued band advances input → hidden 1 → hidden 2 → outputs once per pass.
  During mesh construction the same bands follow `activation_wave`. Node fill,
  radius, sign and every connection rank remain the recorded tensors.
- **Head names are plain language.** Every head remains a measured circle in the
  output lane; the selected head is named in the decision chip, while this file
  carries the complete mapping. `activation_layout.json` is not rewritten.

  | Plain label | Graph tensor |
  |---|---|
  | predicted error | `rel_err` |
  | error vs this part's median | `rel_err_rel` |
  | mesh-to-CAD distance | `geo_chamfer` |
  | mesh-to-CAD worst 1% | `geo_p99` |
  | unknowns | `dof` |
  | meshing time | `mesh_ms` |
  | solve time | `solve_ms` |
  | portable solve work | `solve_flops` |
  | portable data traffic | `solve_bytes` |
  | host-normalized meshing work | `mesh_work` |
  | failure risk | `failure_logit` |
  | cell size | `policy_h_rel` |
  | refinement passes | `policy_adapt_passes` |
  | error target | `policy_eta_target` |
  | order 1 (linear) | `policy_order_logit_1` |
  | order 2 (quadratic) | `policy_order_logit_2` |
  | mesher: graded tets | `policy_mesher_logit_graded_tet` |
  | mesher: hex | `policy_mesher_logit_hex` |
  | mesher: hybrid VEM | `policy_mesher_logit_hybrid_vem` |
  | mesher: hybrid, hex + pyramids | `policy_mesher_logit_hybrid_zoo` |

- During the pass lane the strip reports only measured pass/candidate/gate data.
  The final state names one of four outcomes without conflation: applied action,
  advisor abstention, unrecognised action, or unavailable advisor.
- For the wishbone's new descriptor combination, the OOD distance is unavailable.
  The advisor abstains and the configured baseline remains authoritative.

## Act 3 — `build`: the mesher executing the decision

The advisor outcome is held for `CinemaState::kDecisionLead` (1.6 s). The
wishbone is outside the calibrated descriptor envelope, so the configured
baseline remains authoritative: structural steel, a fine wall-resolving tet4
target, complete ZZ verification without an implied remesh, and explicit direct
LDLT. The base mesh resolves each thin member with 4–6 elements; the opening FFT
is geometry analysis, not a sizing input.

- During build the measured OOD-check state remains visible. A restrained
  halo/connection wave travels through the fixed tensor values; only after it
  reaches the output lane do the later real snapshot's cells land. A compact
  bridge pill, five moving dots and a pulsing target ring show presentation
  direction without claiming concurrency. The strip says
  **aligned replay · computed sequentially**.
- The 0.22-alpha target-spacing map carried from advisor scoring fades only as
  real cells replace it in place. There is no clear/reset.
- An accepted decision still writes mesher, `h = h_rel × bbox diagonal`, adapt
  passes, η target and order into `SimSetup`. An abstention or unrecognised
  mesher never silently changes the configured baseline.
- Order above 2 maps to the one supported quadratic promotion
  (tet4/hex8 → tet10/hex20), and the executed order is displayed.
- **Stages are the mesher's own.** `pipeline::MeshStage` snapshots retain their
  real ids, pass, element/node counts and order in the manifest. The film folds
  them into the advisor-outcome handoff rather than reserving a separate
  “converting to solver elements” chapter:

  | Manifest label | Stage id | What had finished |
  |---|---|---|
  | laying down the cell grid | `lattice` | raw grid sites |
  | splitting hexes into pyramids | `expand` | conformity transition expansion |
  | pulling the surface onto the CAD | `snap` | free-surface projection |
  | removing flattened cells | `peel` | invalid transition fans removed |
  | re-projecting stragglers | `reproject` | residual projection |
  | evening surface spacing | `smooth` | tangential relaxation |
  | snapping what moved, again | `resnap` | second projection |
  | pinning CAD edges and corners | `pin` | exact feature constraints |
  | cells become the solve mesh | `fill` | solver element output |
  | final checks | `ship` | conform/ship/orphan gates |

  A step that did not run never emits. A step that ran and moved nothing still
  emits, so two consecutive stages can be identical meshes — that is what the
  mesher did, and reporting it is preferred over hiding a round that ran.
- **Cells appear in the mesher's own emission order.** The presentation reveal
  begins after the activation/refusal wave reaches the output lane; it does not
  claim runtime progress. The source snapshot and displayed count are real.
- Cells the viewport cannot triangulate are counted and called out, never hidden.
- The reveal shrink pulls each cell toward its centroid as it lands, then closes.
  Dense-mesh edges stay at 0.8 px / 0.18 opacity so shaded geometry survives.
- **The cell microscope** reads the captured `NodalMesh`, not a second model. It
  reports the actual type histogram, displays order-1 corners beside order-2
  midside nodes, and shows `fea::summarize_cell_quality` once per snapshot.
  `CinemaMeshInsight` is computed when worker snapshots are drained, never per
  frame.

- The panel names varyhedron, restricted-CVT poly-VEM and octahedral as
  **experimental alternatives — not used in this verified solve**. Presence in
  the codebase is not presented as evidence that this take exercised them.

## Act 4 — `mesh_hold`: the finished mesh

5.4 s on the authoritative mesh consumed by the first solve. It opens every
cell by 0.10 toward its own centroid, holds the exploded topology, closes it,
then leaves the delivered mesh still for the final fifth of the act.

Counts come from `SolveStage::trace`; type mix and min/mean shape quality come
from the aligned `CinemaMeshInsight`. On quadratic CAD runs the viewport source
is `SolveStage::result.volume_mesh`, not the linear construction scaffold.

## Act 5 — `solve`: the answer, in the order it is computed

Intermediate passes are real `pipeline::SolveStage` callbacks. After worker
finalisation, `CinemaState::adopt_final_result` replaces only the last snapshot
with `SolveJob::take_result()`, so film, Studio and export use the same
configured-order result. Beat order follows the real stage count:

```
pass i:  stress sweep, stress hold
         [i == 0 only] gradient sweep, gradient hold
         error, error hold
         [another pass follows] refine, refine hold
after the last pass:  load ramp, hold
```

The number of solve stages is data, not a storyboard constant. The recorder
emits each stage's pass index, element/node/DOF count, global η and mark counts
into `manifest.json`; `for_each_solve_beat` scales that sequence uniformly into
the 27.0 s solve act without dropping a phase.

The published wishbone stage is 40,170 tet4 cells / 9,796 nodes / 29,388 DOF,
with 0.0200003 minimum and 0.249598 mean cell quality. The distributed 47.17 kN
proof load gives 33.160 MPa true peak stress, 17.425 MPa p99 stress and
0.0032879 mm physical peak displacement. `global_eta = 0.206942`; the film
states that 20.69% verification result and never calls this take reference truth.

There is no per-element solve order. The take replays completed fields, not a
fabricated iteration timeline; the recorded solver token is described below.

### The sweeps are handoffs, not physical animations

Every moving field beat starts from the state already on screen. A plane travels
across the part; behind it the arriving field carries its own values at its own
colour scale, while ahead of it the preceding measured field remains at its own
scale. The first stress beat is the sole exception: its carry state is the
authoritative mesh over the neutral grey used before any solved scalar exists.
The mesh overlay fades during the first 42% of that front instead of disappearing
on the solve boundary.

Stress therefore hands to the recovered gradient; gradient hands to pass-0 ZZ
error; later-pass stress hands to that pass's ZZ error; and the final ZZ map hands
to the load-scaled stress field. Within the leading band, display colours blend
and lift toward white by up to 0.65 so the boundary reads as a moving front.
Outside that narrow feather, both fields are returned byte-for-byte from their
own colormap evaluations. No scalar or displayed number is interpolated.

The front is eased (`smoothstep`) precisely because it is presentation and not a
physical time variable — a linear front starts and stops with a visible jerk.

**Which way it travels** is resolved from the real load case
(`resolve_sweep_axis`): the axis is the resultant of every
`SimSetup::LoadSpec::force` in the take, and its sign is flipped when the loaded
faces' own mean position sits at the far end of that axis, so the front always
starts at the loaded end. That is a comparison of two measured numbers against the
part's bounding box, not an assumption about which face of a part is usually
loaded. With no load case there is no load axis: the reveal runs along the part's
longest bounding-box edge and the film says that is all it is.

### Stress

`SolveStage::result.von_mises`, per node, Pa, drawn on the geometry that pass
actually solved, at zero displacement exaggeration. The colour scale is the
measured nodal p99 so a constrained-node singularity cannot make 99% of the part
dark; `max_von_mises` remains the true peak printed on screen and in the manifest.

### The stress gradient

`fea::nodal_scalar_gradient_magnitude(result.volume_mesh, result.von_mises)`
(`src/fea/src/stress.cpp`), in Pa/m, shown as MPa/mm. For each node it fits
`s(x) ≈ s_i + g·(x − x_i)` over that node's own element patch by unweighted linear
least squares and reports `|g|`.
The display graph and viewport use the measured gradient p99; the steepest
recovered value is still stated numerically. The final histogram bin is labelled
`≥ p99` because it contains the complete upper tail.

- It is a **recovery**, so it is exact for a field that is linear in x and
  first-order accurate on a curved one. Measured: rate 0.82 / 0.69 / 0.78 on
  `exp(x)·sin(3y)·(1+z²)` over a perturbed hex lattice at h = 1/8 → 1/64.
- A node whose patch cannot determine a gradient (fewer than three usable
  neighbours, or a patch whose normal matrix is rank-deficient at a relative
  eigenvalue floor of 1e-10) reports exactly 0.0 and is counted. The film states
  the count on screen when it is nonzero, so a zero that means "flat field" and a
  zero that means "could not tell" are distinguishable.
- When the recovery returns nothing at all, the gradient beats keep the **stress**
  field on screen and the strip says the gradient is unavailable. Nothing is drawn
  in place of a gradient that could not be computed.

### The error field

`SolveStage::result.nodal_eta` — the Zienkiewicz-Zhu error field from that solve.
Colour uses the measured nodal p99 and the true `max_nodal_eta` remains data.
The strip reports `PassTrace::global_eta`, the configured target and the real
`n_h_mark` / `n_p_mark` counts.

The error beat itself is a spatial handoff from the field immediately before it,
not an instantaneous recolour. On pass 0 that carry field is the recovered
gradient; on later passes it is von Mises stress because the gradient is shown
only once. Each uses its own measured p99 display cap while the front crosses.

### Refine

The viewport compares the previous and next `SolveStage::result.volume_mesh`
snapshots by topological family plus corner coordinates quantized at
`max(1e-10 × union_bbox_diagonal, 1e-12 m)`. Tet4 and tet10 share the tetrahedron
family and compare only their four corners, so final polynomial promotion does
not masquerade as wholesale h-refinement.

The manifest records the measured kept/removed/added counts for this take. The
transition's first 32% performs the causal field→mesh handoff: the exact ZZ map
that produced the marks fades only as the exact old→new topology diff rises over
it. Persistent cells remain rendered and briefly open by 0.06 toward their
centroids; they never disappear. During the first 40% removed cells
collapse/fade; during the remaining 60% added cells spawn in the next mesh's
storage order. The ending frame is exactly the mesh the next pass solved.

### Load ramp

λ from 0 to 1, linear and never eased.

The force overlay follows the same exact λ: arrow length and its numeric kN
label scale from zero to the recorded full resultant, while its direction stays
fixed. The small support/force glows and advisor-lane pulses are explicitly
presentation timing cues; they encode no additional mechanical quantity.

`u(λ) = λ·u` and `σ(λ) = λ·σ` are **exact**, so every frame of the ramp is the
real solution of a real load case and not an interpolation between two pictures.
That is established from the code, not assumed:

1. `K` depends only on mesh geometry and `Material`, never on `u` or the loads
   (`src/fea/src/assembly.cpp:64-239`), and `solve_elastostatics` is a single
   assembly and a single linear solve — no Newton loop, no increments
   (`src/fea/src/solve.cpp:467-607`).
2. The right-hand side is linear in the loads (`solve.cpp:559-562`); the one
   additive term, the Dirichlet correction `rhs -= K_fc·u_c`
   (`solve.cpp:590`), does not scale with the loads, which is the physically
   correct behaviour for supports held fixed.
3. Both solve paths are linear operators on that right-hand side: LDLT
   back-substitution (`solve.cpp:453-462`) and the CG recurrence from `x0 = 0`
   (`solve.cpp:161-270`).
4. `σ = D ε(u)` with `ε` linear in `u` (`assembly.cpp:42-60`, `stress.cpp:113`),
   and `von_mises` is homogeneous of degree one in stress
   (`stress.cpp:177-183`).

Scope note: this holds unconditionally for the ordinary case of fixed supports
and a scaled force. A caller that also scaled non-zero prescribed displacements
would have to do it in lockstep; nothing in `solve_elastostatics` does that
automatically, and the film's case has none.

The colour ramps with the shape because stress scales with load exactly as
displacement does. The scalar buffer stays the pass's own field and the measured
**p99 display cap is divided by λ**, making the drawn colour `λ·s / p99`; the
true maximum remains stated numerically. λ itself is never floored — the number
and displacement are the real λ, including exactly 0 — but the divisor is
floored at 1e-3, where every drawn colour is already within one part in a
thousand of the bottom of the colormap.

**The shape is exaggerated, and the physical answer is not.** After
`CinemaState::adopt_final_result`, the studio computes
`scale = 0.12 × bbox_diagonal / max_displacement` from that same final result,
and draws exactly `x + scale·u`. True displacement, shown displacement, factor
and fraction are printed into the manifest and remain on screen through the
ramp and hold. Before frame zero, the camera includes both rest and fully
exaggerated node positions.

### Material

The recorder sets `material 200 0.3` before advisor inference and solving.
`SimSetup::poissons_ratio` enters the advisor feature row, then the solve builds
`fea::Material{E, ν}` from the same setup. `Material::d_matrix()` evaluates
Lamé parameters
`λ = Eν / ((1+ν)(1−2ν))` and `μ = E / (2(1+ν))`; assembly uses
`K_e = ∫ BᵀD(E,ν)B dV`, and stress recovery applies the same `D` to `ε = Bu`.
Invalid E or ν now fails before assembly. The equation board shows the actual
E = 200 GPa and ν = 0.3 rather than relying on GUI defaults or an unlabeled
constitutive matrix.

### Which solver ran

The recorder prints a `solver` token and the manifest records it.

`fea::SolveOptions::on_note` is the authoritative channel. CG names itself and
its convergence there; direct solves emit `direct LDLT selected for N free
DOFs`. That note lives on the `SolveResult` it describes, so the final quadratic
re-solve replaces the linear pass's provenance instead of inheriting or losing it.

## The active-equation graph

The closing act shows one relation the solver is using now, one plain-language
explanation, and one graph from the same `SolveStage`. It does not dim six
unrelated equation groups around the active one. The relations and citations
remain:

| On screen | Code | Citation |
|---|---|---|
| `K u = f` | the assembled system, Dirichlet DOFs eliminated into `K_ff u_f = f_f − K_fc u_c` | `fea/solve.hpp:136`, `fea/src/solve.cpp:574-606` |
| `K_e = ∫ Bᵀ D B dV` | `k.noalias() += b.transpose() * d * b * (det * qp.weight)` | `fea/src/assembly.cpp:139` |
| `ε = B u` | the 6×3n strain-displacement matrix, **engineering** shear (γ = 2ε, no ½ factor) | `fea/src/assembly.cpp:39-60` |
| `σ = D ε` | isotropic Hooke, Voigt order xx, yy, zz, yz, xz, xy | `fea/src/material.cpp:6-18` |
| `λ = Eν / (1+ν)(1−2ν)`, `μ = E / 2(1+ν)` | `Material::lambda()`, `Material::mu()` | `fea/material.hpp:24-31` |
| `σ_vm = √(½[(σ₁₁−σ₂₂)² + (σ₂₂−σ₃₃)² + (σ₃₃−σ₁₁)²] + 3(σ₁₂² + σ₂₃² + σ₁₃²))` | `von_mises()` | `fea/src/stress.cpp:177-183` |
| `\|∇σ_vm\|`, `g = argmin Σ (σ_j − σ_i − g·d)²` | `nodal_scalar_gradient_magnitude()` | `fea/src/stress.cpp` |
| `η_e = ‖σ* − σ_h‖_{E,e} / ‖σ_h‖_{E,Ω}`, `η = √(Σ η_e²)` | `e_sq = el_vol · diffᵀ D⁻¹ diff`, normalised by `ref_sq` | `fea/zz.hpp:27-35`, `fea/src/zz.cpp:270-286` |
| `‖σ‖²_E = ∫ σᵀ D⁻¹ σ dV` | the energy norm the indicator is measured in | `fea/zz.hpp:8-12` |
| `mark the largest η_e until Σ η_e² ≥ θ Σ η²` | Dörfler set selection, θ = `dorfler_theta` (0.3), applied to the h-marked set | `adapt/src/error.cpp:11-38`, `adapt/src/hp_driver.cpp:377-394` |
| `f → λf ⇒ u → λu, σ → λσ` | established from the code, see [the load ramp](#load-ramp) | |

Two things the board does **not** show, and why. The per-element
`utility = benefit / cost` maximisation over {h, p, shape} that
`adapt/src/hp_driver.cpp:130-197` actually runs is nine expressions with eleven
policy constants; the film shows the Dörfler inequality that focuses the h-marked
set and points here for the rest. And the board omits the MPC master/slave
transform `K_system = TᵀKT` (`solve.cpp:555-563`), which is empty on this case —
`LinearConstraints` is non-empty only when an unsupported linear element remains
beside a promoted edge.

Sub- and superscripts on the board are composed from scaled, raised runs of
ordinary digits rather than Unicode sub/superscript codepoints: Liberation Sans,
the first font fallback on Linux, has no U+2081, and an equation full of tofu
boxes is worse than no equation.

## Reproducing it

```sh
python3 scripts/render_cinema.py --all
```

Needs a built `polymesh-gui` with `-DPOLYMESH_WITH_ADVISOR=ON`, `ffmpeg` with an
h264 encoder, and `Xvfb` for a headless run. `--list` shows the cases, `--only`
re-encodes from frames already on disk, and `manifest.json` records what the run
actually did — including, when the GUI printed nothing for a field, that it
printed nothing rather than a plausible value.
