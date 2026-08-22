# ADR-0043: A film someone can read

- Status: accepted (2026-08-21)
- Revises: [ADR-0042](0042-the-advisor-explains-itself-on-screen.md) — the film
  keeps its subject and its honesty rules; §6 (concurrent acts) and the ticker
  design are superseded here
- Revised (2026-08-21): longer curvature and settled-network holds, explicit
  material/deformation telemetry, structural incremental refinement, and an
  on-part spectral-spacing explanation.
- Touches: `apps/gui/cinema.{hpp,cpp}`, `apps/gui/main.cpp`,
  `apps/gui/viewport.{hpp,cpp}`, `src/fea/{include/fea,src}/stress.{hpp,cpp}`,
  `scripts/render_cinema.py`, `docs/assets/cinema/*`, `README.md`
- New: `docs/assets/cinema/NOTES.md`, `tests/test_stress_gradient.cpp`

## 1. What was wrong with the film

ADR-0042 replaced a static activation heatmap with a recording of the deployed
network deciding, the mesher building what it chose, and the solver's answer
arriving. That was the right subject. The execution had four defects, all of
which came from optimising for auditability against an imagined hostile reader
rather than for comprehension by an actual one.

**Nothing could be read.** The composition set its prose at 13–16 px in a
1920×1080 frame and the README embeds a GIF downscaled to roughly half that.
Measured on the shipped asset: body rows arrived at the reader as 5–7 px of ink.
The film's own disclosures — the part of it that makes the honesty claim
checkable — were the least legible thing in it.

**There was too much of it.** The bottom strip carried up to nine wrapped rows,
270 px tall, re-measured every frame and reserved at the tallest act so that it
would not resize the part at act boundaries. The network panel carried four more
paragraphs. A frame of the closing act put roughly 120 words on screen for 0.3 s.

**The two panes did not agree.** The advisor's pass lane swept the candidate grid
across the deliberation act *and* the build act, so while the mesh grew on the
right, the network on the left was lighting up for candidate 31 of 39 — an action
that was not the one being built. The film said so, in a 30-word disclosure. The
disclosure was true and the composition was still misleading: a viewer reads two
panes side by side as one event.

**It cut away from every result.** There was no beat for the finished mesh: the
build act ended on the frame the last cell landed and the closing act began. Each
solved field got one beat and then the next beat started. The film showed a lot
of work and none of its outcomes.

## 2. The four rows

The strip now carries exactly four rows, at a constant height, every act:

1. a concise label — "Peak stress 11.35 MPa", "Stress gradient",
   "Estimated solution error";
2. the two-to-four numbers that matter on this beat;
3. the one disclosure that applies to it;
4. the provenance stamp, and the path to the rest of the disclosures.

Sizes are 40 / 27 / 20 / 17 px at a 1080-line frame, scaled with the frame
height. Measured ink heights in the recorded frames: 27 px for the headline
against 9 px for the old body rows. A row too wide for the strip is **set
smaller** until it fits — never wrapped, because wrapping changes the strip's
height and the leftover is the viewport pane whose height sets the part's
rendered size; and never clipped, because a clipped sentence is a different claim
from the one the film made.

The strip is 203 px instead of 270, and it is 203 px on frame 1 and frame 3600,
so per-act high-water probing is gone.

Above it is a four-label chapter bar — *exact CAD · advisor · mesher · analysis*
— with a progress fill. A first-time viewer of a 60 s presentation should not
have to read a clock to know where they are.

### The disclosures did not disappear

`docs/assets/cinema/NOTES.md` carries every one of them, beat by beat, with the
struct field or function behind each: the per-layer activation normalisation and
why it is per-layer, the connection ranking, the head-name mapping in both
directions, every mesh stage id, the reveal order, the skipped-element count, the
ZZ definition, the sweep axis rule, the exaggeration factor, and the four-step
argument from the code that makes the load ramp exact. The film names that file
on screen in row four.

This is the trade, stated plainly: **a disclosure nobody can read is not a
disclosure.** Six paragraphs of 13 px grey satisfied a rule and informed nobody.
One legible sentence plus a path to the full account informs the reader who wants
it and stops lying to the one who does not. Nothing was deleted; it moved to
where it can be read.

## 3. One subject per chapter

The fast advisor lane runs only inside deliberation. Its strip stays stable —
pass index, total passes, candidate count, gate threshold — while the measured
activations animate.

The advisor act now spends 65% on the 39 real passes and 35% holding the final
re-score/refusal state. That gives the settled network 3.15 s before the build
act, whose 1.6 s decision lead extends the hold before dissolving into the cell
microscope. The complex default is OOD-refused, so the strip says **Advisor
abstained — final network state** and then **Advisor abstained — verified
fallback**. No vetoed action is pictured as executed.

The solve boundary dissolves the completed cell microscope directly into the
equation board. The network is not replayed between them. Pane geometry never
changes; only opacity does.

The opening follows the same rule. A curvature plot by itself explains a signal
processing step but not a meshing decision, so the exact-CAD chapter has five
visible beats: sample κ(s) along a highlighted BRep edge; transform that trace;
show which measured frequency modes survive the 99.5% energy threshold; inverse
reconstruct the trace; and sweep target-h rings over the part. The ring geometry
is evaluated from the production size field before and after filtering at the
same deterministic surface points. Their diameter is proportional to target h
and their colour is an explicit fine-to-coarse scale. They are labelled as
spacing targets, never as generated elements.

That completed state now crosses the chapter boundary instead of being erased.
The feature panel dissolves directly into the network over 1.3 s; the on-part
rings dim to a 0.22-alpha input map, remain through deliberation, and fade only
as the chosen mesher starts emitting real cells on the same part. This adds
continuity, not new data or simultaneous prose.

## 4. Holding on results

The 60 s default spends 7.8 s on exact CAD/spectral sizing, including about
6.08 s with the panel fully open, and 9.0 s on the advisor, including its
3.15 s settled-state hold. Its two-pass closing sequence is scaled uniformly
into 27.0 s:

| Beat | Default length | What is held |
|---|---:|---|
| `mesh_hold` | 5.4 s | exploded topology, then the closed initial solve mesh |
| `stress_hold` | 1.96 s per pass | solved von Mises field |
| `gradient_hold` | 1.84 s | recovered first-pass stress gradient |
| `error_hold` | 1.72 s per pass | ZZ error field |
| `refine_hold` | 2.09 s | next solved mesh after incremental cell replacement |
| `hold` | 3.31 s | finished full-load answer |

Moving beats are 1.47–2.70 s. Every beat is retained; the multi-pass schedule
scales proportionally rather than truncating the ending.

The refinement beat is structural rather than a full redraw. A corner-topology
diff identifies 27,808 persistent, 2,688 removed and 8,143 added cells in the
default take. Persistent cells never leave the framebuffer; they briefly open
by 0.06 toward their centroids so changed interior cells can be seen. Removed
cells collapse/fade first, then only replacements use the established
centroid-spawn animation. Tet4/tet10 compare by corner topology, so p-promotion
does not make every surviving cell look replaced.

## 5. One state hands directly to the next

Moving result beats are **spatial handoffs** between already-complete measured
states. The first stress field grows out of the authoritative mesh: the same cell
rendering remains over the neutral pre-result surface and fades during the first
42% of the front. Every later field keeps its predecessor ahead of the front at
that predecessor's own scale: stress → recovered gradient → pass-0 ZZ error,
later-pass stress → ZZ error, and final ZZ error → load-scaled stress.

Behind the front, the arriving field's own colormap result is returned
byte-for-byte. Ahead of it, the predecessor's own colormap result is returned
byte-for-byte. Only the narrow feather blends those two display colours and
brightens toward white by up to 0.65, so it reads as a handoff rather than an
instantaneous recolour. No scalar and no displayed number is interpolated.

The refinement boundary follows the same causal rule without inventing a scalar
blend: for the first 32% of the beat, the exact ZZ map that made the marks fades
as the exact old→new topology diff rises over it. The structural collapse/spawn
then continues from that carried state. The movie never clears to an empty
wireframe and never replays an earlier chapter to bridge a later one.

The front is eased because it is presentation, not a physical time variable. The
load factor remains strictly linear and continues to define the exact
$u(\lambda)=\lambda u$ response independently of the eased colour handoff.

**Which way it travels** is resolved from the real load case: the axis is the
resultant of every `SimSetup::LoadSpec::force` in the take, and its sign is
flipped when the loaded faces' own mean position sits at the far end of that
axis, so the front always starts at the loaded end. That is a comparison of two
measured numbers against the part's bounding box, not an assumption about which
face of a part is usually loaded. With no load case the reveal runs along the
longest bounding-box edge and the film says that is all it is.

The gradient beat needed a quantity that did not exist. `fea::nodal_scalar_gradient_magnitude`
fits `s(x) ≈ s_i + g·(x − x_i)` over each node's own element patch by unweighted
linear least squares and reports `|g|`; it is exact for a field linear in `x`
(pinned in `tests/test_stress_gradient.cpp`) and first order on a curved one
(measured rate 0.82 / 0.69 / 0.78 on a perturbed lattice). A node whose patch
cannot determine a gradient reports exactly 0.0 and is **counted**, so a zero
meaning "flat" and a zero meaning "could not tell" are distinguishable, and the
film states the count when it is nonzero. When the recovery returns nothing at
all, the gradient beats keep the stress field on screen and say the gradient is
unavailable. Nothing is drawn in place of a gradient that could not be computed.

Verified in the recorded frames: the gradient beat's hue histogram is L1 1.62
(of a possible 2.0) away from the stress beat's, and 1.40 from the error field's.
It is a different field, not a relabelled copy.

## 6. Equations instead of a network, in the act that is arithmetic

The network has said everything it has to say by the time the answer starts
arriving, and what the closing act is doing is arithmetic that can be written
down. So the completed **cell microscope** cross-fades over 0.8 s directly into
a board of the seven relations the solver evaluates. The previous implementation
briefly restored the network at this boundary; that stepped backward in the
story and is removed. The active relation is lit, a rule runs down the board's
left edge, and live numbers sit beside it: E = 200 GPa, ν = 0.3, unknown count,
peak stress, steepest gradient, estimated error, marked-cell count and λ. The
final strip distinguishes the true 0.0008111 mm displacement from the explicitly
exaggerated 21.27 mm / 26,221× presentation.

Every equation is the expression the cited code implements, and the citation
table is in `NOTES.md`. Two things the board deliberately does not show: the
per-element `utility = benefit / cost` maximisation over {h, p, shape}, which is
nine expressions and eleven policy constants (the board shows the Dörfler
inequality that focuses the h-marked set and points at the notes); and the MPC
transform `K_system = TᵀKT`, which is empty on this case.

The panel's **geometry never changes** across the swap. The pane's width sets the
viewport's width, the viewport's height sets the part's rendered size, and the
film is one continuous shot.

Sub- and superscripts are composed from scaled, raised runs of ordinary digits
rather than Unicode sub/superscript codepoints, and the film's face merges a
maths fallback over the UI face. Both are the same lesson, learned by measurement:
Liberation Sans, Fedora's UI default and this project's first font fallback, has
no U+2081, no U+2207 `∇` and no U+21D2 `⇒`, and the first cut of the equation
board drew tofu boxes in the panel whose entire job is to be readable. Swapping
the studio's UI face to fix a film would be the wrong trade; merging the missing
block into the film's own face is not, and ImGui keeps the first glyph added for
a codepoint, so the merge fills gaps and never overrides.

## 7. Plain language everywhere a reader looks

Head units, mesh stages and mesher names are drawn in plain English:
"mesh-to-CAD distance", "pulling the surface onto the CAD", "hybrid: hex bulk,
pyramid skin". The mappings are tables in `apps/gui/cinema.cpp`, and
`activation_layout.json` is **not** rewritten — it is the exporter's record of
what the graph emits, and a film is not a reason to edit a model artifact. Both
directions of the mapping are in `NOTES.md`.

An unmapped label falls through **verbatim** rather than being de-underscored by
a rule. A mechanical `_` → space transform would have produced a
confident-looking label for a head this table has not been taught about, which is
the same class of mistake as inventing a number.

Element counts carry thousands separators, because "11,692 cells" is read at a
glance and "11692 cells" is counted.

The same treatment reached the documentation and the figure generators:
`scripts/figstyle.py` now owns one `QUANTITY_LABELS` table that every generator
draws its axis labels from, and the advisor docs introduce each quantity as
`mesh-to-CAD distance (`geo_chamfer`)` — plain label first, identifier once. Six
hand-typed copies of a label drift, and a figure whose axis disagrees with the
doc beside it is worse than either being wrong alone, because the reader cannot
tell which is the mistake.

## 8. The GIF is the whole take

The inline GIF used to be a four-second window opening on the deliberation act,
cut from the act table the GUI printed. That was the right answer for a film
whose payload was the network deciding and the fill starting. This film's payload
is the sequence — the mesh finishing and being held, stress arriving, the
gradient of it, the refinement, the ramp, the final freeze — and no window narrow
enough to be a loop contains more than one of those.

So the GIF covers the take end to end and meets the 8 MB budget in the ladder
instead, stepping down through width and frame rate. Frame rate is spent before
width, because the take is mostly stills and slow sweeps (a hold compresses to
nearly nothing whatever the rate) and width is what decides whether the headline
survives. The ladder stops at 720 px because below that the numbers row stops
being readable at all, and a GIF nobody can read is the defect this ADR exists to
fix.

## 9. Consequences

- `CinemaAct` gains `kMeshHold`; `SolvePhase` is ten beats instead of five. The
  recorder prints five act windows and `scripts/render_cinema.py` reads the table
  it prints, so neither side hard-codes four.
- `DisplayMode` gains `kResultsGradient`, and `Viewport::set_result` takes an
  optional extra per-node field interpolated onto the boundary samples through
  the same path the von Mises field already uses.
- `Viewport::FieldSweep` participates in the result bake key, including the
  carried field id and its own maximum. A moving front therefore re-bakes both
  measured colormap evaluations. The load ramp already re-baked every frame, so
  per-frame baking is established cost, not new.
- Result mode can composite the existing cinema mesh buffers over a fading
  scalar surface; this is used only for mesh→stress and ZZ→refinement handoffs.
- The left pane adds a production spectral-sizing trace and a cached
  `CinemaMeshInsight`; neither computes per frame.
- The default take is 3600 frames (60 s). A shorter `--frames` still works:
  every act and beat scales in proportion rather than truncating the ending.
  Candidate-specific prose is intentionally absent from the fast pass lane.
- `cinema_ticker_chips` / `cinema_ticker_body` / `cinema_ticker_height` /
  `cinema_ticker_reserve` / `draw_cinema_ticker` are gone, replaced by
  `cinema_caption` and `draw_cinema_strip`. `CinemaState::ticker_reserve` is gone
  with them.
