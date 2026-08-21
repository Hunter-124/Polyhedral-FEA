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
| Take | 1800 frames at a fixed 1/60 s virtual timestep = 30.000 s. The clock is set from the frame **index**, never accumulated from a real frame time, so the same take renders identically on a 400 fps box and inside a 6 fps software-GL Xvfb. |
| Frame | 1920×1080. `--size` sets both the Xvfb screen and, through `POLYMESH_GUI_SIZE`, the GUI window; the recorded resolution is measured off the PNG rather than assumed. |
| Acts | `skeleton` 0.05, `deliberate` 0.14, `build` 0.20, `mesh_hold` 0.07, `solve` 0.54 of the take. The GUI prints the frame span of each; the manifest records what it printed. |
| Left panel | 0.42 of the width, from the end of the opening act to the last frame. It never moves again: the pane's height sets the part's rendered size, so a pane that changed width mid-take would resize the subject inside one continuous shot. Its **content** cross-fades from the network to the equation board over 0.5 s at the `solve` boundary. |
| Bottom strip | A constant height, computed from the four row sizes. A row too wide for the strip is set smaller until it fits — never wrapped (that would change the strip's height, and therefore the part's size) and never clipped (that would drop the end of a sentence the film is making). |
| Camera | Set once, at `cinema on`, to the union of the skeleton and the mesh bounds (`Viewport::frame_content(kCinema)`, yaw 0.70, pitch 0.72, 0.90 fill) and then locked. There is no cut anywhere in the film. |

### What is interpolated

Time, opacity, the shrink-toward-centroid reveal, the spatial sweep front, and
the load factor λ. That is the whole list.

No displayed **number** is ever interpolated. No activation, element count, error
indicator, stress value or progress value is ever synthesised. Where a source is
missing the film says which one and shows nothing in its place.

λ is the one interpolated quantity that is also displayed, and it is displayed
because interpolating it is exact rather than approximate — see
[the load ramp](#load-ramp) below.

## Act 1 — `skeleton`: the part

- **Outline source** is stated on screen and is one of:
  - `geom::extract_topology(*model.cad, 16)` — the STEP file's own edge curves, 16
    samples per edge. The film's part takes this path.
  - `geom::detect_sharp_edges(model.surface, 30°)` — the tessellation's crease
    network, for mesh input that carries no BRep. The film calls this out as **not**
    a CAD skeleton when it happens.
  - unavailable, with the extractor's own message drawn verbatim.
- `skeleton_polylines` / `skeleton_points` are the counts of what was extracted
  and pushed to the viewport, not an estimate of the part's complexity.
- Nothing is drawn as mesh in this act or the next. Nothing has been meshed yet.

## Act 2 — `deliberate`: choosing a mesh

One beat per real forward pass of the deployed graph, in the order
`Advisor::explain()` ran them: one per enumerated candidate action, then a final
re-score of the recommended one. On the film's case that is 38 + 1 = 39 passes at
0.108 s each.

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

- **The number in the headline** is `ActivationFrame::candidate + 1` of
  `frames.size() - 1` — the candidate grid is one shorter than the pass count,
  because the last pass is the re-score and not a candidate.
- **"predicted error"** in the numbers row is `ActivationFrame::score`, i.e.
  `rel_err_rel`: `log10(rel_err)` minus that case's median over the actions
  actually run. It is the ranking key, lower is better, and it is meaningless
  compared across cases
  ([docs/advisor/0001-architecture.md](../../advisor/0001-architecture.md)).
- **"failure risk"** is `σ(failure_logit)`, computed with the same
  branch-on-sign logistic `src/advisor/src/advisor.cpp` uses, because `exp` of a
  large positive logit overflows to `inf` and would turn the probability into NaN.
- **The gate** compares that probability against `AdvisorExplanation::gate_threshold`.
  A dropped candidate is shown as dropped. The candidate-loop bookkeeping
  (`ranked`, `over_budget`) applies only to candidates, never to the re-score pass.

## Act 3 — `build`: the mesher executing the decision

The decision is on screen for `CinemaState::kDecisionLead` (0.7 s, capped at a
fifth of the act) **before the first cell appears**. Without that lead the action
and the thing it produced would arrive on the same frame and a viewer could not
tell which followed which.

- **The pass lane stops.** From the first frame of this act to the last frame of
  the film, the network holds the pass that scored the recommended action
  (`ActivationFrame::recommended`, falling back to the final re-score, which is a
  pass over exactly the action being built). The activations beside the growing
  mesh are the activations of the forward pass that chose that mesh. This is a
  change from the earlier cut of the film, which kept sweeping candidates during
  the fill: two lanes advancing on one clock was honest about being a composition,
  but a viewer reasonably read the lit graph as being about the mesh on the right,
  and it was not.
- **The head unit the decision came out of is highlighted** — the
  `policy_mesher_logit_*` unit matching `ActivationFrame::action.mesher`.
- **The decision was applied.** `load_cinema_advisor` writes the advised mesher,
  `h = h_rel × bbox diagonal`, adapt passes, η target and order into the app's
  `SimSetup` before `solve` is issued, and the strip states what was applied. A
  refusal (`AdvisorDecision::vetoed`) is **not** applied and is shown as a
  refusal. An advised mesher name this build does not recognise is **not**
  substituted for something else: `pipeline::mesher_from_name` refuses it, and
  the strip says the mesh below is the studio's own setup.
- **Order above 2** is executed as quadratic: the solve path has one p-elevation
  step (tet4/hex8 → tet10/hex20). The HUD reports the executed order.
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
- **The shrink** that draws each cell toward its own centroid as it lands is
  cosmetic and is geometry only: the cell is the cell. It collapses over the
  first third of each beat (`kRevealShrink` 0.22, `kRevealShrinkFraction` 0.33)
  and the cell edges are drawn at 1.0 px / 0.30 opacity. Both were measured, not
  chosen: at the previous 1.5 px / full opacity, 22–50% of the part's own painted
  pixels were near-black cell outline on this 11,692-cell case, against 3.4–8.9%
  at these settings, and at half the part being outline the reveal front stops
  reading at all.

## Act 4 — `mesh_hold`: the finished mesh

2.1 s of the completed fill, complete and still, with its own counts. This act
did not exist in the earlier cut, which moved off the mesh on the frame the last
cell landed.

Counts are the app's own HUD line, sourced from `SimSetup` and the delivered
mesh, never from the advisor decision struct — the two agree only when the
decision was applied, and when they disagree the setup is the truth.

## Act 5 — `solve`: the answer, in the order it is computed

Every field is one a real `pipeline::SolveStage` produced. Beat order per pass is
`pipeline::SolveJob`'s own loop order plus a still hold after every moving beat:

```
pass i:  stress sweep, stress hold
         [i == 0 only] gradient sweep, gradient hold
         [another pass follows] error, error hold, refine, refine hold
after the last pass:  load ramp, hold
```

Beat lengths at the 30 s take (`beat_seconds` in `apps/gui/cinema.cpp`, scaled to
the act's own span): stress sweep 1.6 s, stress hold 1.1 s, gradient sweep 1.5 s,
gradient hold 1.1 s, error 1.2 s, error hold 0.8 s, refine 1.7 s, refine hold
1.0 s, load ramp 1.9 s, final hold 1.8 s.

There is no beat for a per-element solve order and no iteration counter, because
a direct sparse factorisation has neither. See [the solver](#which-solver-ran).

### The sweeps are reveals, not animations

The two `*_sweep` beats uncover a field that is **already complete**. A plane
travels across the part; behind it the surface carries that pass's own values at
that pass's own colour scale, ahead of it the surface is a neutral grey
deliberately outside the `fea_colormap` range (0.67 minimum unit-RGB distance from
any colour the map can produce, so an unswept region cannot be misread as a field
value); within the leading band the colour lifts toward white by up to 0.65, which
is what makes the front read as a front.

Nothing about the field changes as the front passes, and no number on screen is
tied to the front's position. The front is eased (`smoothstep`) precisely because
it is a camera move and not a physical quantity — a linear front starts and stops
with a visible jerk.

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

`SolveStage::result.nodal_eta` — the Zienkiewicz-Zhu error field recovered from
that same solve, colour-scaled by `max_nodal_eta`. The numbers row carries
`PassTrace::global_eta` against the run's own `SimSetup::eta_target`, and the
element counts `n_h_mark` / `n_p_mark`: this field, and nothing else, is what
decides the next mesh.

### Refine

`SolveStage::result.volume_mesh` of the **next** pass — the mesh that pass
actually solved — revealed element by element in that mesh's own storage order.
Naming the source is what stops this beat from silently redrawing the pass-0 fill
and calling it refined.

When the delivered element count comes back **unchanged**, the film says so in the
headline and stops calling it a finer mesh: the marks drove a remesh whose ship
gate returned the same count, so what is on screen is a re-fill. On the film's
case this is exactly what happens (11,692 → 11,692), and it is the kind of number
a reader should not have to take on trust.

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

**The shape is exaggerated.** The film states the factor
(`λ × App::deform_scale`) on screen at every frame of the ramp and the final hold.
The true displacement on this case peaks at 1.78e-4 mm and is invisible at any
honest scale.

### Which solver ran

The recorder prints a `solver` token and the manifest records it.

`fea::SolveOptions::on_note` is the only channel the linear solver has, and it
speaks on the CG path and on a memory-budget downgrade. Below `cg_threshold` with
the budget satisfied it says nothing at all — which is the normal outcome at this
project's DOF counts and is exactly what happens on the film's case.

Silence is not a licence to guess. `fea::select_solve_method` sends
`SolveMethod::kAuto` to CG only when the **free** DOF count exceeds
`SolveOptions::cg_threshold` (50,000); the free set is a subset of the pass's
`PassTrace::n_dof`; and the one override that could have changed the choice emits
a note. So `n_dof = 13,146 ≤ 50,000` with no note means the system was factorised
directly (sparse LDLT), and no conjugate-gradient iterations exist on this case —
which is why none are animated. The token is `direct_ldlt` when that argument
closes, `cg` when the solver said so, `note_absent` when neither establishes it,
and `no_solve_stage` when nothing was delivered.

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
