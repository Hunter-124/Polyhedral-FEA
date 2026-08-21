# ADR-0043: A film someone can read

- Status: accepted (2026-08-21)
- Revises: [ADR-0042](0042-the-advisor-explains-itself-on-screen.md) — the film
  keeps its subject and its honesty rules; §6 (concurrent acts) and the ticker
  design are superseded here
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

1. a plain-English headline — "Peak stress 2.489 MPa", "Where does stress change
   fastest?", "Rebuilding where the error was";
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

The strip is 203 px instead of 270, and it is 203 px on frame 1 and frame 1800,
so the machinery that used to probe every act and every beat for a high-water
mark is gone.

Above the rows is a four-word chapter bar — *the part · choose a mesh · build it
· solve it* — with the current chapter lit and a progress fill. It is the one
piece of pure orientation in the composition, and it is there because a
first-time viewer of a 30 s take should not have to read a clock to know where
they are.

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

## 3. Sequence, not simultaneity

The pass lane now runs inside the deliberation act alone. From the first frame of
the build act to the last frame of the film it **holds** the forward pass that
scored the recommended action (`ActivationFrame::recommended`, falling back to
the final re-score, which is a pass over exactly the action being built), and the
`policy_mesher_logit_*` unit the decision came out of is highlighted.

Measured in the recorded frames: the panel's ink count takes seven distinct
values across the deliberation act and exactly one value across the 7.8 s of
build plus mesh-hold. The activations beside the growing mesh are the activations
of the pass that chose that mesh.

ADR-0042 §6 argued for the overlap on the grounds that it was honest — the film
said the two lanes were separate recordings — and that it bought screen time. It
did buy screen time. It also asked the viewer to hold a disclaimer in mind while
looking at a picture that contradicted it, which is a cost the film was paying in
the one currency it cannot afford.

## 4. Holding on results

Four new beats and one new act, all of them stills:

| Beat | Length | What is held |
|---|---|---|
| `mesh_hold` (a whole act) | 2.1 s | the finished fill, complete, with its counts |
| `stress_hold` | 1.1 s | that pass's von Mises field |
| `gradient_hold` | 1.1 s | the recovered stress gradient |
| `error_hold` | 0.8 s | the ZZ error field |
| `refine_hold` | 1.0 s | the mesh the next pass solved |
| `hold` | 1.8 s | the finished answer at full load |

Verified frame by frame on the recorded take: every one of those windows is a
**bit-exact** freeze of the viewport (zero pixels changed between consecutive
frames), and every moving beat between them moves. The take is 30 s rather than
20 s, and the closing act takes 0.54 of it rather than 0.63, because the holds
are what the extra ten seconds buy.

## 5. Stress arriving, and its gradient

Two beats animate a **spatial reveal** of a field that is already complete: a
plane travels across the part, the field's own colours are uncovered behind it,
and ahead of it the surface is a neutral grey chosen to be unreachable through
`fea_colormap` (measured: 0.67 minimum unit-RGB distance from any colour the map
can produce, so an unswept region cannot be misread as a low field value). A
leading band brightens toward white by up to 0.65, which is what makes the front
read as a front.

Nothing about the field changes as the front passes and no number on screen is
tied to the front's position, which is why the front is the one motion in the
film that is eased rather than linear: it is a camera move, and a linear front
starts and stops with a visible jerk. The strip says what it is in those terms —
"the field is already complete; what moves is how much of it has been uncovered".

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
down. So the panel's **content** cross-fades over 0.5 s into a board of the seven
relations the solver evaluates, with the one this beat is computing lit, a rule
down its left edge, and its own live numbers beside it: the unknown count, the
peak stress, the steepest gradient, the estimated error against its target, the
marked-cell count, λ.

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
- `Viewport::FieldSweep` participates in the result bake key, so a moving front
  re-bakes. The load ramp already re-baked every frame, so per-frame baking is
  established cost, not new.
- The default take is 1800 frames. A shorter `--frames` still works: the GUI
  paces its own acts inside whatever it is given, so every beat compresses in
  proportion rather than the end being truncated.
- `cinema_ticker_chips` / `cinema_ticker_body` / `cinema_ticker_height` /
  `cinema_ticker_reserve` / `draw_cinema_ticker` are gone, replaced by
  `cinema_caption` and `draw_cinema_strip`. `CinemaState::ticker_reserve` is gone
  with them.
