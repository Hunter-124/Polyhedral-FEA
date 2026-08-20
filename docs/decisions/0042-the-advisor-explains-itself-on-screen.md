# ADR-0042: The advisor explains itself on screen

- Status: accepted (2026-08-20)
- Supersedes: `docs/advisor/figures/activation_map.png` and the
  `activation_map()` generator in `scripts/advisor/figures.py`, both deleted
- Touches: `scripts/advisor/export_onnx.py`, `src/advisor/*`, `src/mesh/*`,
  `apps/gui/*`, `scripts/render_cinema.py`, `docs/assets/cinema/*`

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
decision, beside the mesher building the mesh that decision asked for:
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

- **Time.** The take runs on a virtual clock at a fixed 1/60 s per frame, and the
  acts get fixed fractions of it. Nothing on screen is a wall-clock measurement,
  so nothing on screen is a performance claim; the solve's real cost is in the
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
sequence, the element geometry and the element counts.

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

The film records `box_hole_s0_c0`: `bench/geometries/corpus/primitives/box_hole_s0.step`
under axial tension, the x_lo face clamped and the x_hi face loaded.

That case is not a stylistic choice. The advisor refuses parts it does not
recognise, and the refusal is not rare — measured with
`polymesh solve <part> --advisor bench/advisor` against the 6.30 operating point,
`box_hole_s0` scores an out-of-distribution distance of 3.57 and is advised,
`tests/fixtures/parts/plate_hole.step` scores 3.73 and is advised, and
`sphere`, `icecream_cone`, `cantilever`, `smoke_bar` and `pipe` score 12.6 to
90.9 and are vetoed. On any of those the advisor panel would be a refusal notice.
Recording the showcase flagship instead would have put the film closer to the
gate for no gain.

`box_hole_s0_c0` is also the exact input the retired heatmap was computed on: its
title read "run 30 on box_hole_s0_c0 · cfg-116b3958". The replacement therefore
supersedes the figure on the figure's own case, rather than on a case chosen to
flatter the replacement.

Two details of the translation into GUI verbs are worth recording, because both
are places where a plausible guess would have been wrong:

- **The load is 89.257 N, not a round number.** The case JSON specifies a 1e6 Pa
  traction over an `expected_area` of 8.925720996e-05 m² on the x_hi face, and
  the `loadface` verb takes newtons, so the faithful translation is that
  resultant.
- **Face ids are GUI face ids and were verified, not assumed.** They are assigned
  when the viewport loads the part and have nothing to do with the case JSON's
  selection boxes. `fix 0` / `loadface 5` was confirmed by solving with them and
  reading the VTU back: the part's x extent is [−0.03, 0.03] m, the max-|u| node
  sits at (0.03, 0, 0) — the loaded end — with u = (3.113e-07, −4.0e-19,
  2.37e-09) m, and the top displacement decile is 99.92% aligned with x. That is
  axial tension on x_hi with x_lo clamped, which is what the case specifies. The
  ids stay CLI parameters (`--fix-face`, `--load-face`) because a change to face
  discovery would renumber them.

The `h` verb in the recorded command is a fallback only. When the advisor
advises it sets `h` itself, so the mesh in the film is the advisor's own action
rather than an element size the script chose. On this case it advises
`hybrid_zoo` at `h_rel` 0.08 — 5.345 mm on this part — order 1, one adapt pass
and an η target of 0.02, inside the distribution at a Mahalanobis distance of
3.57 against the 6.30 operating point.

That is a different action from the one the same part gets from
`polymesh solve box_hole_s0.step --advisor bench/advisor`, which reports
`h_rel` 0.2, order 2 and no adapt passes. Both are real forward passes; they
differ because they are different *cases*, not different models. The CLI
invocation above takes its default boundary conditions (a transverse load), the
film applies `box_hole_s0_c0`'s axial tension, and the case columns are model
inputs. The out-of-distribution distance is identical at 3.57 in both, which is
the gate behaving as documented: it tests the part, not the load case.

## 6. Packaging and provenance

- **Three artifacts, because one format cannot do the job.** GitHub does not
  render a repo-relative `<video>` reliably, so the README embeds a
  palette-optimised GIF of a subset of the take and links the h264 mp4 for the
  full one; `poster.png` is the first fully-composed frame, for renderers that
  show neither. The GIF's width and frame rate are whatever the byte budget
  allowed, and `manifest.json` records which rung of the ladder that was.
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
  mp4, the frame count, the frame rate, and the per-act frame spans the GUI
  reported.

## 7. Consequences

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
