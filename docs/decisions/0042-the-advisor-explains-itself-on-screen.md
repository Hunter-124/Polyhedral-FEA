# ADR-0042: The advisor explains itself on screen

- Status: accepted (2026-08-20)
- Revised (2026-08-20, round two): the acts run concurrently (§6), the solved
  fields animate in the order they are computed (§7), and the recorded case moved
  to `sphere_box_s0_c0` (§5)
- Supersedes: `docs/advisor/figures/activation_map.png` and the
  `activation_map()` generator in `scripts/advisor/figures.py`, both deleted
- Touches: `scripts/advisor/export_onnx.py`, `src/advisor/*`, `src/mesh/*`,
  `src/pipeline/*`, `apps/gui/*`, `scripts/render_cinema.py`,
  `docs/assets/cinema/*`

## 1. What the heatmap could not show

The advisor's only picture of itself was `activation_map.png`: one row of cells
per layer, each row normalised to its own maximum, for one canonical input. It
was honest about what it drew — its own footer said "per-row scaling: trunk and
head magnitudes differ ~10×" — and it still could not answer any question a
reader has about the advisor.

- **It showed one input.** The advisor does not evaluate one input. It enumerates
  a candidate grid, scores every candidate with a forward pass, drops the ones
  the DOF budget or the feasibility gate rejects, and ranks the rest. A single
  frame of activations cannot show that a unit was warm *for a particular
  candidate*, which is the only sense in which the network decides anything.
- **It had no edges.** Rows of cells carry no connectivity, so nothing in the
  image says which weight carried a value from one layer to the next. "Warm
  units" is not a mechanism.
- **It came from the training checkpoint, not from what ships.** The values were
  read out of `bench/advisor/runs/<NNN>/activations.json`, a PyTorch-side
  artifact of the training run. The thing that runs in a solve is `model.onnx`
  through onnxruntime in C++.
- **It stopped at the network.** The advisor's output is an action — a mesher, an
  `h`, an order, an adapt schedule. The figure ended one step before the only
  consequence anybody cares about: the mesh that action produces.

So it was replaced by a recording of the deployed graph running the real
decision, beside the mesher building the mesh that decision asked for and the
solver's own answer appearing in the order it is computed:
`docs/assets/cinema/`, produced by `scripts/render_cinema.py`.

## 2. The activations come from the graph, not from a second implementation

The obvious way to draw a network in C++ is to re-implement its forward pass in
C++: read the weights, multiply, apply GELU, draw. It is also the way to end up
with a picture of a network that is not the one deployed.

`export_onnx.py` therefore appends three tap outputs — `trunk_input`,
`trunk_fc1`, `trunk_fc2` — *after* the nine contract outputs, and
`Advisor::explain()` returns exactly what onnxruntime produced for them on each
candidate pass. There is one inference path in the product and the film draws its
internals; there is no arithmetic in the GUI that could disagree with it.

This is [ADR-0033](0033-a-gate-must-measure-what-ships.md)'s rule applied to a
figure rather than to a gate. A gate that measures a different cell than the one
that ships reports health on a mesh nobody has; a picture that draws a
re-derived forward pass shows a network nobody runs. Both failures are invisible
in the artifact, which is what makes them worth a structural fix instead of a
review habit. The margin available for such drift is real: the C++ path already
matches the Python model to 2.158e-06 relative at opset 17, which is small
enough that a plausible-looking re-implementation error would sit *above* the
parity the repo currently claims and still look like a network.

The static layer geometry — layer names, sizes, unit labels and the three fully
connected weight blocks — is exported alongside as `activation_layout.json` and
loaded into `NetworkLayout`. So the nodes are the deployed graph's units and the
lines between them are its weights, read from the same export that produced the
`.onnx` the solve loads. `Advisor::has_activations()` is false when either half
is missing, `explain()` throws rather than guessing, and the surface says the
advisor is unavailable rather than drawing an unlit graph that looks like a quiet
network.

## 3. The mesh evolves at the granularity the mesher has

The mesh half of the film had a tempting cheat available: elements are drawn one
by one, so give each element a timestamp and spawn them at a pleasing rate. That
number does not exist. The volume fill does not emit elements on a clock; it
completes construction stages, and inside a stage the elements come into being
together.

So the pipeline grew a sink instead of a stopwatch. `pipeline::MeshStageSink`
receives one `MeshStage` per completed stage of a fill — stage name, emission
index, adapt pass, and the mesh at that instant in solver types — and
`pipeline::SolveJob::on_mesh_stage` forwards it to whoever is watching. The
stage boundaries in the film are the mesher's own; the number of stages is
however many it ran. Nothing is interpolated between them, and if the mesher
changes its stage sequence the film changes with it rather than keeping a
schedule the code no longer has.

The sink lives in `volume_mesh_impl`, one level *above* the fill functions, and
`src/mesh` was deliberately left untouched. The first attempt did thread a
callback into `graded_tet_fill_surface` itself and was reverted, for a reason
worth recording: the advisor recommends `hybrid_zoo` on every part it will
advise on at all, so the graded tet fill is not the code path the film records,
and the hybrid product path's real construction boundaries — the mixed lattice
fill, the ADR-0013 hex-to-pyramid expansion, the post-expand snap, the fan peel,
the re-projection, the smoothing, the second snap and the carve rounds — are
already visible at the pipeline level. Instrumenting the mesher core would have
bought nothing the film uses and put a callback inside a 2000-line function that
every determinism test depends on. The sink is const views and cannot alter what
the mesher produces; unset, it costs a null check.

Within one stage the elements are revealed in `mesh.elements` order, which is the
mesher's emission order, not a sort chosen for looks. The *rate* of that reveal
is screen time (§4). The counts on screen are `Viewport::cinema_element_count()`
— the elements actually uploaded — so a reader can pause the video and check the
number against `polymesh mesh` on the same part.

## 4. What is cosmetic, exactly

Everything in this list changes when a pixel is drawn, never what it says:

- **Time, including which things share it.** The take runs on a virtual clock at
  a fixed 1/60 s per frame; the acts get fractions of it and, since round two,
  those fractions overlap (§6). Nothing on screen is a wall-clock measurement, so
  nothing on screen is a performance claim; the solve's real cost is in the
  showcase manifest, where it is measured.
- **Opacity and easing.** Fades between acts, the skeleton's alpha, the mesh's
  alpha.
- **The element shrink.** Elements are drawn scaled toward their own centroids so
  the interior of the fill is legible. The nodes are the mesh's nodes; the gap
  between cells is drawing, not geometry, and it goes to zero at `shrink = 0`.
- **Colour.** The diverging blue → near-white → red ramp (`signed_colormap`,
  matching `scripts/figstyle.py`'s convention) maps a signed activation to a
  hue. The value it maps is the graph's.
- **Layout and camera.** Node positions in the network panel, and the camera fit
  in the viewport.

Everything else is measured: which candidate a pass scored, its score, whether
the feasibility gate passed it, whether the DOF budget dropped it, which action
was recommended, the activations themselves, the weights on the edges, the stage
sequence, the element geometry, the element counts, the per-pass displacement and
von Mises fields, the error field each pass recovered, the refinement it asked
for, and the load factor each deformed frame is drawn at (§7).

Two rules follow, and both are enforced rather than documented:

- **No data, no picture.** If the taps or the layout are missing, or the part is
  refused as out of distribution, the surface says so in words. It never
  synthesises a frame to fill the gap.
- **No partial takes.** `render_cinema.py` verifies exactly N contiguous
  nonzero-byte frames before it encodes, and exits nonzero on a missing frame, a
  zero-byte frame, a stale frame from an earlier take, a framebuffer the GUI
  resized mid-run, or a GUI that exited nonzero. A short video is a defect, not a
  shorter video.

## 5. The case is chosen by the gate, not by what looks good

The film records `sphere_box_s0_c0`:
`bench/geometries/corpus/primitives/sphere_box_s0.step` under axial tension, the
x_lo face clamped and the curved x_hi face loaded. 11,692 elements, 13,146 DOF,
max von Mises 2.489 MPa, 38 candidates enumerated and 39 forward passes counting
the final re-score.

That case is not a stylistic choice. The advisor refuses parts it does not
recognise, so the gate picks the shortlist and only then does the film pick from
it. Swept over all 44 corpus primitives with
`polymesh solve <part> --advisor bench/advisor` against the operating point the
shipped `bench/advisor/ood.json` enforces — 5.034, that fit's training 99th
percentile — 23 are advised and 21 refused, and the refusals are whole families:
`perforated_plate` 11.36–17.65, `tube` 13.55–19.02, `ellipsoid_boss`
19.95–32.16, `twisted_loft` 74.51–76.56 and `lobed_shaft` 77.66–80.19 are in no
training row and are refused in every regime. On any of those the advisor panel
would be a refusal notice rather than an explanation.

Of the 23 that are advised, `sphere_box_s0` gives by far the most elements,
because a curved wall is what actually drives curvature and feature grading. It
scores 3.34 against the 5.034 operating point and is not vetoed.

**The definition came from case selection, not from overriding the model.** Round
two asked for a higher-definition mesh, and there were two ways to get one:
override the advisor's `h_rel` and mesh finer than it asked for, or record a part
the advisor itself asks for a fine mesh on. The first would have made the film a
picture of a mesh the product does not build — §2's failure, one layer out — so
the case moved instead. On `sphere_box_s0_c0` the advisor advises `hybrid_zoo` at
`h_rel` 0.08, order 1, one adapt pass and an η target of 0.02, and the film
records that action unmodified: 20x the 568 elements of the previous case, at the
advisor's own recommendation.

`box_hole_s0_c0` is retained and stays reproducible with
`scripts/render_cinema.py --part box_hole_s0_c0`, because it has one property the
new case cannot have: it is the exact input the retired heatmap was computed on,
its title reading "run 30 on box_hole_s0_c0 · cfg-116b3958". The replacement
therefore still supersedes that figure on the figure's own case, and the recorder
keeps the case that proves it. It is advised as well, at 3.57 against 5.034 —
`hybrid_zoo`, `h_rel` 0.08, order 1 and one adapt pass on the axial tension of
`box_hole_s0_c0`, versus `h_rel` 0.2, order 2 and no adapt passes from
`polymesh solve box_hole_s0.step --advisor bench/advisor`, whose default
boundary conditions are a transverse load. Same part, same model, different case
columns, identical distance: the gate tests the part, not the load case. Its 568
elements are simply why it is no longer the default — the mesh runs out of
geometry to build long before the network runs out of candidates to score.

Two details of the translation into GUI verbs are worth recording, because both
are places where a plausible guess would have been wrong:

- **The load is 528.197958 N, and the area behind it is a measurement rather
  than an authored number.** The case JSON specifies a 1e6 Pa traction on x_hi
  and, unlike the old case, carries no `select.expected_area` to read: this
  loaded face is curved (`"load_face_boundary": "curved_surface"`) and
  `scripts/gen_primitive_corpus.py` emits an authored area only for planar ends.
  The CLI measured the exact CAD area of that face as 0.000528197958 m², which at
  1e6 Pa is a resultant of 528.197958 N, and the `loadface` verb takes newtons.
  `render_cinema.py` formats that verb with `.9g` for the same reason the number
  is not rounded here: `%g`'s six significant digits would have handed the GUI
  528.198 N.
- **Face ids are GUI face ids and were verified, not assumed.** They are assigned
  when the viewport loads the part and have nothing to do with the case JSON's
  selection boxes. `fix 0` / `loadface 5` was confirmed by solving with them and
  reading the VTU back: the part's x extent is [0, 0.0729] m, the max-|u| node
  sits at the max-x end — the loaded one — and the top displacement decile is
  99.73% aligned with x. That is axial tension on x_hi with x_lo clamped, which
  is what the case specifies. The ids stay CLI parameters (`--fix-face`,
  `--load-face`) because a change to face discovery would renumber them.

The `h` verb in the recorded command is a fallback only. When the advisor advises
it sets `h` itself from `h_rel`, so the mesh in the film is the advisor's own
action rather than an element size the script chose. The recorded fallback is
6.888 mm, which is 0.08 of this part's 86.10 mm bounding-box diagonal (x
[0, 0.0729] m, y and z [−0.0162, 0.0162] m) — the same size the advisor's own
`h_rel` resolves to here, so a refusal would fall back to a comparable mesh
rather than to a different film.

## 6. The two halves share the clock, because the causal link is the point

Round one ran the film as a sequence: the network scored candidates, the act
ended, and then the mesher built the winner. That ordering is true of the code —
the recommendation is complete before the fill starts, because the fill needs the
`h` it sets — and it is exactly the wrong way to *show* it. A reader watching two
separate acts sees two demos. The claim worth making is that these are one
decision, and the only way to make it visible is to have the activations still
firing while the elements the chosen action produces come into being.

So the acts overlap. The take is `skeleton`, `deliberate`, `build`, `solve` —
0.06, 0.11, 0.20 and 0.63 of it — and the candidate sweep is one continuous 1..39
pass lane running across `deliberate` *and* `build`, while the fill's stage lane
runs inside `build`. For the whole of `build` both counters advance: the network
is still firing while the fill is being built. The beats are shorter too, because
they no longer have to fill two acts: at the recorder's 20 s default a forward
pass gets 0.159 s and a pass-0 construction stage 0.340 s, against 0.238 s and
0.540 s in a 30 s take and 0.292 s per pass in the old sequential one.

**The overlap is a replay, and the film says so.** It would be easy to read two
advancing counters as a claim that the network was still deciding while the
mesher built, and that is not what happened: `Advisor::explain()` runs to
completion in `load_cinema_advisor`, before `solve` is issued, so every pass on
screen — including the ones still to come — had already happened when the mesher
emitted its first stage. What preserves the causal order under the overlap is
that the decision is locked and on screen from the first frame of `build`, with a
lead-in before the first element appears: `CinemaState::kDecisionLead` is 0.6 s,
capped at a fifth of the `build` act, so at the recorder's 1200-frame default it
is the full 0.6 s — 36 frames of decided action with zero elements drawn — and it
only shortens below roughly 450 frames. The cinema replays two recorded sequences
on one clock; the ticker states that, and the ADR states it here.

Nothing about the data changed. Each activation frame still belongs to the
candidate pass that produced it, each mesh stage still belongs to the stage that
emitted it, and both still carry their own index on screen. What changed is which
pixels share a frame, which §4 already lists as cosmetic: time and layout.

One consequence for the recorder. `render_cinema.py` used to cut the inline GIF
from the midpoint of the `advisor` act to the midpoint of the `mesh` act, a rule
that presumes a cut to straddle and that no longer matches any act name the GUI
prints. It then cut the loop to the `build` act, and that was still wrong for the
README's purpose: `build` is the overlap, but the *decision* has already happened
by the time it opens, so the inline loop showed a mesh appearing and never showed
the network that chose it — the one half of this film a static mesh figure cannot
carry. The loop now **opens on the `deliberate` act and runs 0.20 of the take
forward**, which on the committed take is 1.2..5.2 s of 20 s: the pass lane
scoring its 38 candidates, then the fill of the action it picked, with both
counters advancing across the join. `build` alone is kept as the next rule, then
the longest act reported, then `deliberate`'s own scheduled fractions,
0.06..0.26 of the take, for a `--only gif` run with no act table to read — which
on this take lands on the same 1.2..5.2 s the act rule does. The longest-act rule
is deliberately far down: the longest act is `solve` at 0.63, so a rule that
preferred it would inline the answer without the decision that produced it.
Either way the window is computed from what the GUI printed and the rule that
produced it is named in `manifest.json` as `window_source`, so the inline loop is
never a hand-picked range that quietly stopped matching the film.

## 7. The fields animate in the order the answer is computed

Round two asked for the stress and deformation to animate "in the way and order
it is actually solved". There are exactly three real orderings available on this
case, and the film is restricted to them.

**The adaptive loop, which is the order the answer is computed.** A solve, the
error field recovered from that solve, the refinement that field asked for, then
the next solve. `adapt_passes` is 1 on this case, so that is two real solves, and
the loop is observable rather than reconstructed: `pipeline::SolveStage` carries
the pass index, the pass's own `PassTrace`, its `SolveResult` — mesh,
displacement, von Mises, nodal and element η — and the linear solver's note, and
`pipeline::SolveJob::on_solve_stage` delivers one per completed pass. It is the
same shape as the mesh-stage sink of §3 and for the same reason: the film shows
boundaries the code actually has, and unset the callback costs a null check.

On screen that becomes the `SolvePhase` sequence — `kField`, the pass's own von
Mises field on its own mesh; `kError`, the ZZ field recovered from that same
solve; `kRefine`, the mesh the *next* pass solved on, revealed element by
element; then `kLoadRamp` and `kHold` — one phase per real step, named in the
ticker so a viewer can tell which step they are looking at.

**The load factor, which is exact rather than interpolated.** In linear
elastostatics u(λ) = λ·u identically, so ramping λ from 0 to 1 draws the true
solution of the problem at every intermediate load — not a tween between a
start and an end state. That is why the ramp is allowed at all, and why λ is on
screen and labelled as the linear response: a viewer who reads it as a
load-stepped nonlinear solve would be reading a claim the solver never made.

**CG iterates, which this case does not have.** The obvious animation for "as it
is solved" is a residual falling over iterations, and it is unavailable here, for
a measured reason. `fea::SolveMethod::kAuto` chooses from the free DOF count
against `SolveOptions::cg_threshold`, which is 50,000; this case has 13,146 free
DOF, so it is factorised by direct sparse LDLT and there is no iteration sequence
to draw. The threshold is not arbitrary — the solver header records CG losing to
LDLT by two orders of magnitude below it, measured at 11,040 DOF on a plate hex
mesh: 179 s against 0.9 s.

Forcing CG to obtain a prettier animation was considered and rejected. It would
have made the film a recording of a solver path the product does not take on this
part, which is the same defect as drawing a re-implemented forward pass (§2) or a
mesh the advisor did not ask for (§5), and it would have cost a 200x slowdown to
do it. Iterate animation is supported if a case ever selects CG —
`SolveOptions::on_progress` already reports `(iter, max_iter, rel_resid)` — and
until one does, the surface names the method that actually ran and why. The GUI
prints it as a `solver` token on its `cinema: record` line, out of a closed
vocabulary of four: `direct_ldlt`, `cg`, `note_absent` when the passes arrived
without a note to read, and `no_solve_stage` when no pass was observed at all, so
the absence of a solver claim is itself a value rather than a blank.
`render_cinema.py` records it, and `solve_stages`, in `manifest.json`; the film
cannot imply iterations it never performed.

Three things are therefore prohibited, and their absence is the point of this
section: a per-element "solve order" reveal, which does not exist — the
factorisation does not produce the answer element by element; an iteration
counter on a case that was factorised; and a ramp shaped to look nonlinear, which
would misrepresent an exactly linear response.

## 8. Packaging and provenance

- **Three artifacts, because one format cannot do the job.** GitHub does not
  merely render a repo-relative `<video>` unreliably — it drops it. Measured
  2026-08-21 against GitHub's own renderer (`POST /markdown`, `mode: gfm`):
  `<video src="...">`, an absolute `raw.githubusercontent.com` src, and a
  `<video><source><img fallback></video>` nest all render as an empty `<p>`, and
  the nested `<img>` fallback is stripped with the element that contained it. A
  bare raw URL degrades to a plain link. An animated `<img>` is the only markup
  that survives sanitisation and plays inline, and GitHub marks it
  `data-animated-image`. So the README embeds a palette-optimised GIF of a subset
  of the take, centred at full column width, linked to the h264 mp4 for the full
  one; `poster.png` is the first fully-composed frame, for renderers that show
  neither. The GIF's width and frame rate are whatever the byte budget allowed,
  and `manifest.json` records which rung of the ladder that was.
- **The mp4's encoder is recorded, not assumed.** libx264 at CRF 18 is the
  reference path; a distribution ffmpeg without it (Fedora's `ffmpeg-free` ships
  libopenh264 and the NVENC wrappers and no libx264) falls to a named
  alternative, and the manifest says which encoder produced the file rather than
  labelling every h264 file the same.
- **The frames carry the same stamp as every figure in the repo.**
  `POLYMESH_CINEMA_STAMP` is exported as `git <rev> · model.onnx sha256 <12 hex>`
  from `figstyle.git_revision()` and `figstyle.digest()`, and the GUI draws it
  verbatim in the footer. A frame grabbed out of the video can be matched to a
  still figure and to `manifest.json`, which additionally records the exact GUI
  command, the part, the model directory, the sha256 of `model.onnx` and of the
  mp4, the frame count, the frame rate, the per-act frame spans the GUI reported,
  and the `solver` token naming the linear solver the take's solves ran through.

## 9. Consequences

- The advisor's static activation figure is gone, and nothing in the docs
  regenerates it. `scripts/advisor/figures.py` writes `training_curves.png` and
  nothing else; the per-unit view lives in the video and in the interactive
  dashboard.
- Three new taps are part of the ONNX export contract. An export without them
  leaves `has_activations()` false and the cinema unable to run, which is a
  loud failure rather than a blank panel.
- A fill stage is now observable. `graded_tet_fill_surface` and
  `pipeline::volume_mesh` take an optional sink that defaults to empty, so a
  caller that does not ask observes nothing and pays nothing: existing callers
  and the determinism tests are bit-identical.
- Frames are build output, not documentation. They land in `build/cinema/frames`,
  which the repo-root `/build*/` rule already ignores.
- An adaptive pass is now observable too, on the same terms.
  `pipeline::SolveJob::on_solve_stage` defaults to empty and costs one
  `SolveResult` copy per pass only when a consumer sets it, so the CLI, the
  benchmarks and the determinism tests are unaffected.
- The recorded case is `sphere_box_s0_c0` and `box_hole_s0_c0` is now a
  `--part` option rather than the default. Any figure or table quoting the film's
  element count quotes 11,692 rather than 568, and the retired heatmap's own case
  remains recordable for the comparison that justified the replacement.
- The GUI's stdout contract gained three tokens on the `cinema: record` line,
  appended after the existing ones so every prior spelling and position is
  unchanged: `skipped` (elements the viewport could not triangulate),
  `solve_stages` and `solver`. `render_cinema.py` parses all three and records a
  null with a stated reason where one is absent, so an older GUI still renders
  rather than failing a match it cannot satisfy.
