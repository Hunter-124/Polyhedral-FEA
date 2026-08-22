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
| Acts | `skeleton` 0.13, `deliberate` 0.15, `build` 0.18, `mesh_hold` 0.09, `solve` 0.45. At 60 s these are 7.8 / 9.0 / 10.8 / 5.4 / 27.0 s. The GUI prints exact frame spans into the manifest. |
| Left panel | 0.42 of the width. It opens once, then changes in place: exact-CAD spectrum → deployed network → actual-cell microscope → equation board. Every view cross-fades directly from the view before it; the solve boundary no longer replays the network. Pane geometry never moves after opening. |
| Bottom strip | Constant height. Over-wide rows shrink to fit; they never wrap or clip. Labels remain stable while fast computation animates above them. |
| Camera | Set once, at `cinema on`, to the union of the skeleton and the mesh bounds (`Viewport::frame_content(kCinema)`, yaw 0.70, pitch 0.72, 0.90 fill) and then locked. There is no cut anywhere in the film. |

### What is interpolated

Time, opacity, the shrink-toward-centroid reveal, the spatial handoff front, the
pre/post-filter spacing-glyph morph, and the load factor λ. That is the whole
list.

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
  sample of the selected CAD edge. The default yields 1,753 on-part samples:
  target h is 2.426–7.092 mm before filtering and 2.657–5.953 mm after filtering.
- The final plan's `SpectralSizingReport` supplies modes kept/total, retained
  energy, denoised curve seeds, predicted density before/after, and exact-BRep
  provenance.
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
one per candidate and one final re-score. The default records 38 + 1 = 39 passes
at 0.15 s each. The remaining 3.15 s holds the final re-score/refusal state at
full size, and the 1.6 s decision lead at the start of the next act extends that
inspectable hold. Candidate-specific prose does not flash during the pass lane:
the strip keeps one stable explanation while the network itself carries motion.

The feature panel and full on-part target-spacing field enter this act intact.
Over the first 1.3 s the panel cross-fades directly into the deployed network
while the rings settle to the restrained carry opacity. The network therefore
appears as the consumer of the field just shown, not as a fresh scene over an
empty wireframe.

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
- **Connections**: the 180 strongest of 15,936, ranked per frame by
  `|w_ji · a_i|` from the exported weight blocks
  (`activation_layout.json` edge blocks 96×53, 96×96, 17×96). Ranking by weight
  alone would be wrong — a large weight on a silent unit carries nothing. Line
  opacity and width are both `|w_ji · a_i| / max`. The count is drawn from the
  constant that selects it (`kDrawnConnections`), so shrinking it shrinks the
  on-screen sentence too.
- **Head names are shown in plain language.** The mapping is a table in
  `apps/gui/cinema.cpp` (`kHeadNames`); `activation_layout.json` is not rewritten,
  because it is the exporter's record of what the graph emits. An unmapped label
  falls through **verbatim** rather than being de-underscored by a rule — a
  mechanical transform would have produced a confident-looking label nobody chose.

  | On screen | Graph tensor |
  |---|---|
  | predicted error | `rel_err` |
  | error vs this part's median | `rel_err_rel` |
  | mesh-to-CAD distance | `geo_chamfer` |
  | mesh-to-CAD worst 1% | `geo_p99` |
  | unknowns | `dof` |
  | meshing time | `mesh_ms` |
  | solve time | `solve_ms` |
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

- During the pass lane the strip reports only the measured forward-pass index,
  total pass count, candidate count and failure-gate threshold. During the final
  hold it names the settled decision/refusal. Candidate scores remain visible on
  their real head units.
- The default complex part is outside the advisor's validated operating
  envelope. The OOD refusal is the deployed result, not an error in the film:
  `AdvisorDecision::vetoed` is shown and its action is not applied.

## Act 3 — `build`: the mesher executing the decision

The advisor outcome is held for `CinemaState::kDecisionLead` (1.6 s) before the
first cell appears. The default part is refused as out of distribution, so the
explicit fallback remains authoritative: E = 200 GPa, ν = 0.3, graded tet,
h = 12 mm, spectral sizing, one measured adaptive pass with no early η target,
and quadratic CAD geometry. The strip labels this **Advisor abstained — verified
fallback**; it never presents the vetoed hybrid action as executed.

- During the lead, the network holds its final measured pass. It then
  cross-fades to the cell microscope as construction begins.
- The 0.22-alpha target-spacing map carried from advisor scoring fades over the
  larger of the 1.6 s decision lead or 18% of this act. Actual emitted cells
  occupy the same part as those targets replace them; there is no clear/reset.
- An accepted decision still writes mesher, `h = h_rel × bbox diagonal`, adapt
  passes, η target and order into `SimSetup`. A refusal or unrecognised mesher is
  never substituted silently.
- Order above 2 maps to the one supported quadratic promotion
  (tet4/hex8 → tet10/hex20), and the executed order is displayed.
- **Stages are the mesher's own.** One beat per `pipeline::MeshStage` of the
  initial fill (`pass == 0`), in emission order, each carrying that stage's whole
  `fea::NodalMesh`. The stage ids are `pipeline::kMeshStageNames`; the film draws
  a plain-language name for each:

  | On screen | Stage id | What had finished |
  |---|---|---|
  | laying down the cell grid | `lattice` | hex/pyramid/tet lattice at raw grid sites, unsnapped |
  | splitting hexes into pyramids | `expand` | ADR-0013 hex→pyramid product expansion |
  | pulling the surface onto the CAD | `snap` | free-surface boundary nodes projected onto the CAD surface |
  | removing the cells the snap flattened | `peel` | snap-flattened transition fan tets deleted, boundary faces rebuilt |
  | re-projecting the stragglers | `reproject` | per-node re-projection of the residual outliers |
  | evening out the surface spacing | `smooth` | tangential relaxation of boundary-node spacing on curved walls |
  | snapping what moved, again | `resnap` | second snap over the set the peel and smoothing left |
  | pinning CAD edges and corners | `pin` | CAD vertices and sharp-edge curves hard-pinned (ADR-0035) |
  | converting to solver elements | `fill` | the mesher's output in the solver element zoo |
  | final checks | `ship` | exterior conform + ship gate + orphan compaction |

  A step that did not run never emits. A step that ran and moved nothing still
  emits, so two consecutive stages can be identical meshes — that is what the
  mesher did, and reporting it is preferred over hiding a round that ran.
- **Cells appear in the mesher's own emission order** — their index in
  `mesh.elements`. Nothing is sorted, and no cell is drawn before the stage that
  built it. The count on screen is `reveal × cinema_element_count()`: exact
  arithmetic on two real numbers, not an estimate of progress.
- **Cells the viewport could not triangulate** (degenerate connectivity, faceless
  poly-VEM cells) are counted by `Viewport::cinema_skipped_element_count()` and
  called out on screen when nonzero, rather than the reveal being quietly
  narrowed.
- **The reveal shrink** pulls each cell toward its own centroid as it lands,
  then closes over the first third of the stage beat. Cell edges are 0.8 px at
  0.18 opacity so the dense 30k-cell take remains shaded geometry rather than a
  black wire mass.
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

Intermediate passes are the real `pipeline::SolveStage` callbacks. After worker
finalisation, `CinemaState::adopt_final_result` replaces only the last snapshot
with `SolveJob::take_result()` and refreshes its trace/quality. That makes final
quadratic promotion, re-solve, VTU result and film byte-for-byte the same field.
Beat order follows the adaptive loop with a hold after every moving result:

```
pass i:  stress sweep, stress hold
         [i == 0 only] gradient sweep, gradient hold
         error, error hold
         [another pass follows] refine, refine hold
after the last pass:  load ramp, hold
```

The default has two solve stages. Their nominal 44.0 s beat sequence is scaled
uniformly into the 27.0 s solve act (factor 0.613636), so no phase is dropped:
both stress/error results and holds, the first-pass gradient, the incremental
refine/re-refine hold, the load ramp and the final hold all remain present.

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
actually solved, at zero displacement exaggeration. The colour scale is that
pass's own `max_von_mises`.

### The stress gradient

`fea::nodal_scalar_gradient_magnitude(result.volume_mesh, result.von_mises)`
(`src/fea/src/stress.cpp`), in Pa/m, shown as MPa/mm. For each node it fits
`s(x) ≈ s_i + g·(x − x_i)` over that node's own element patch by unweighted linear
least squares and reports `|g|`.

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

`SolveStage::result.nodal_eta` — the Zienkiewicz-Zhu error field from that solve,
colour-scaled by `max_nodal_eta`. The strip reports `PassTrace::global_eta` and
the real `n_h_mark` / `n_p_mark` counts. With no configured adapt target it says
**verification pass** rather than printing a fictitious 0% target.

The error beat itself is a spatial handoff from the field immediately before it,
not an instantaneous recolour. On pass 0 that carry field is the recovered
gradient; on later passes it is von Mises stress because the gradient is shown
only once. Both use their own maxima while the front crosses the part.

### Refine

The viewport compares the previous and next `SolveStage::result.volume_mesh`
snapshots by topological family plus corner coordinates quantized at
`max(1e-10 × union_bbox_diagonal, 1e-12 m)`. Tet4 and tet10 share the tetrahedron
family and compare only their four corners, so final polynomial promotion does
not masquerade as wholesale h-refinement.

The default measured transition preserves 27,808 cells, removes 2,688 and adds
8,143 replacements. Its first 32% also performs the causal field→mesh handoff:
the exact ZZ map that produced the marks fades only as the exact old→new topology
diff rises over it. Persistent cells then remain rendered and briefly open by
0.06 toward their centroids so internal changes can be seen; they never
disappear. During the first 40% only removed cells collapse/fade; during the
remaining 60% only added cells spawn with the existing centroid/reveal animation
in the next mesh's storage order. The ending frame is exactly the mesh the next
pass solved.

### Load ramp

λ from 0 to 1, linear and never eased.

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

The colour ramps with the shape because the stress scales with the load exactly as
the displacement does. The scalar buffer stays the pass's own field and the
**maximum is divided by λ** instead, making the drawn colour `λ·s / s_max`: the
λ-scaled field against a fixed full-load legend, with no second copy of the field
anywhere. λ itself is never floored — the number on screen and the displacement
are the real λ, including exactly 0 — but the divisor is, at 1e-3, where every
drawn colour is already within one part in a thousand of the bottom of the
colormap.

**The shape is exaggerated, and the physical answer is not.** After
`CinemaState::adopt_final_result`, the studio computes
`scale = 0.12 × bbox_diagonal / max_displacement` from that same final result.
The viewport then draws exactly `x + scale·u`. This take's true maximum is
0.0008111 mm; the reported 26,221× scale shows 21.27 mm, exactly 12% of the model
diagonal. True displacement, shown displacement, factor and fraction are printed
into the manifest, and true/shown values remain on screen through the ramp and
hold.

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

`fea::SolveOptions::on_note` is the solver's authoritative channel. A CG run
names itself there. With no note, `cinema_solver_token` may prove direct LDLT
only when total DOF is already below the free-DOF threshold; otherwise it records
`note_absent` rather than guessing. The default final quadratic result takes that
honest `note_absent` path, rendered as **solver method not reported** on screen.

## The equation board

The closing act replaces the network with the relations the solver actually
evaluates, with the one this beat is computing lit and its own live numbers beside
it. Every equation below is the expression the cited code implements.

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
