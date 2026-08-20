# ADR-0041: A deformed render carries its undeformed outline

- Status: accepted (2026-08-20)
- Supersedes: ADR-0038 §6 on the ice-cream cone's fixture (see §5)
- Touches: `scripts/render_showcase.py`, `docs/assets/showcase/*`

## 1. The report

> the deformed result isn't actually showing on many of our examples, and some
> of the results don't seem correct since the base of this should have stress on
> it from the normal forces or something

The image attached was `docs/assets/showcase/gallery_cantilever.png` at
`f847607`. Two claims, and they turned out to be one defect: the second is what
the first *looks like* to a reader.

## 2. The warp was applied. The camera cancelled it

`render_stress` warps the grid before extracting the surface, and it did so
correctly. On the shipped `build/showcase/cantilever.vtu`, `warp_by_vector` at
the caption's ×200 moves the tip from z = 0 to z = −40.03 mm; the solve itself
is right to 0.08% (tip deflection 2.0015e-4 m against PL³/3EI = 2.0000e-4 m).

The camera threw it away. `fit_camera` builds the screen-vertical axis by
projecting `up` out of the view direction. For `view = (−0.42, −1.00, 0.46)`:

| `up` | x̂ · û (screen-vertical per metre of beam) | ẑ · û |
|---|---:|---:|
| `(0, 0, 1)` (shipped) | **+0.1512** | +0.9206 |
| `(0, vz, −vy) = (0, 0.46, 1.00)` | **0.0000** | +0.9085 |

With the shipped `up`, a *straight* 1 m beam climbed 151 mm up the frame from
root to tip. The real warped droop contributes 200 × 2.0e-4 × 0.9206 = 37 mm in
the opposite sense. So the deflection was a 24% correction to a fake tilt 4.1×
its size: the figure showed a beam rising to the right, the actual bending was
invisible inside that rise, and the clamped end — yellow, carrying the peak —
read as the *drooping free* end. Hence "the base should have stress on it": the
reader identified the wrong end as the base, and the figure gave them no way not
to.

`up = (0, vz, −vy)` is not a new invention. It is perpendicular to the view by
construction (`vy·vz + vz·(−vy) = 0`) and has no world-x component, so the part's
x-axis lands exactly horizontal, and `render_showcase.py` already documented and
used it — on the plate, four Part entries above the cantilever. The cantilever
was the outlier.

## 3. The other five figures showed nothing, for a different reason

The cantilever at least had deflection on screen and mislabelled it. On the rest
of the gallery the deformation is along the part's own long axis — tension on the
plate and hero, axial compression on the cylinder, sphere and cone — so warping
by an honest 2–5% of the bbox diagonal changes the silhouette by a few percent
*of a shape the reader has never seen undeformed*. There is nothing to compare
against, so it reads as nothing at all. No warp factor fixes that; a bigger one
just shears the geometry into a shape the solve never predicted (the note above
`warp_frac` already refuses to do this, and is still right).

What was missing is the reference. The Studio viewport has had it since it had
results — a **undeformed outline** toggle, drawn from the rest node positions
with the depth mask off (`apps/gui/viewport.cpp`). The still renders never grew
the equivalent.

## 4. The rule

Every stress render now composites the **view silhouette of the rest surface**
over the warped body:

- Silhouette, not wireframe. The GUI can afford a full boundary-edge overlay
  interactively; 44,832 cells of it in a 1920×1440 still is mush. `vtkPolyDataSilhouette`
  with `EnableFeatureAngle(0)` gives one crisp closed curve per part. Crease
  edges were tried and rejected: they also emit the part's hidden back edges,
  which turns a reference curve into a box wireframe laid over the field.
- Composited, not depth-tested. The case worth seeing is exactly the one where
  the body has moved in *front* of where it used to be (the plate's loaded end,
  the sphere's equator), and a depth-tested reference is invisible there.
- Framed with the body. `fit_camera` now fits the union of the warped and rest
  point sets, because on the cantilever the reference is the half that sits
  *above* the deformed body and would otherwise be cropped.

Every footer changed from "Deformation warped ×N for visibility" to "Deformation
warped ×N, against the undeformed outline", so the grey curve is named rather
than left as decoration.

## 5. The cone's fixture, and a correction to ADR-0038

Verifying the second half of the report found one figure whose headline number
was, in fact, wrong — not the cantilever, the cone.

ADR-0038 §6 kept the cone's patch fixture on the grounds that "its foot has no
flat face to clamp". That is contradicted by §5 of the same ADR, which correctly
describes `icecream_cone.step` as a *truncated* cone whose "foot is a flat disc
of radius 6 mm". The mesh agrees with §5: at h = 10 mm, 341 boundary nodes sit at
exactly z = 0 with r ≤ 6.000 mm, and the first node layer up the wall is at
z = 0.273 mm.

So the showcase was clamping `z <= 12 mm` — that disc *plus* the wall out to
r = 9.5 mm — and the top of that band is an artificial clamped-patch boundary in
the middle of a conical face. It owned the figure. Von Mises against height,
h = 10 mm, nodal max per 3 mm band:

| z | 0 mm | 4 mm | 8 mm | **12 mm** | 16 mm | 20 mm | 30 mm |
|---|---:|---:|---:|---:|---:|---:|---:|
| `z <= 12 mm` (before) | 0.73 | 1.16 | 2.75 | **8.67** | 3.65 | 2.73 | 1.89 |
| foot face (after) | 12.20 | 7.17 | 5.78 | 4.58 | 3.63 | 2.83 | 1.87 |

Before, all 200 of the hottest nodes lay in z = 11.1…13.9 mm — a one-element
ring straddling the box plane, non-monotone in both directions, 2.4× the field
one element above it. The part's reported peak was a property of the box.

After, the field decays monotonically from the fixture into the part and the peak
sits at r = 6.03 mm, z = 0.3 mm: the foot's own CAD rim, which a cone standing on
its point really has. 12.20 MPa against a mean bearing stress of
1000 N / π(6 mm)² = 8.84 MPa is a 1.38× rim concentration, which is the answer
that model has. Fixed nodes 1,801 → 341; strain-free elements 0 → 0 of 87,284
(ADR-0038's 8 corner elements are gone with the patch); nodes at exactly 0 Pa
0 → 0; mesh unchanged at 127,765 nodes / 87,284 elements.

The box top is 0.1 mm rather than 0, for the same reason the cylinder's is 2 mm
rather than 0: snapped foot nodes land on z = 0 exactly, and a hair of tolerance
costs 0.03 mm of radius if a future h puts a wall node inside it.

## 6. What this does not change

- **No solver, mesher or BC code was touched.** §2 and §4 are `scripts/render_showcase.py`;
  §5 is one `fix_box` tuple in its Part table. The cantilever VTU is byte-identical
  (sha256 `a162d2b1f103`), as are the plate, cylinder and sphere.
- **The cantilever result was never wrong.** Nodal von Mises on the top fibre
  against PL(x)c/I: 5.41 vs 5.40 MPa at x = 0.1 m, 4.50 vs 4.50 at 0.25, 3.00 vs
  3.00 at 0.5, 1.50 vs 1.50 at 0.75, 0.602 vs 0.600 at 0.9. The clamped face
  itself reads 5.03 MPa against beam theory's 6.00 because the clamp suppresses
  the Poisson strain there — expected, and the peak (5.89 MPa) sits 7 mm in at
  the root corner, which is what the caption already said.
- **The sphere keeps its cap boxes.** Its peak (4.31 MPa) is also a clamped-patch
  ring — all 200 hottest nodes in z = −40.2…−38.9 mm, against 0.71 MPa two
  elements below — but a sphere is one CAD face, so there is no face to clamp
  instead. The patch *is* the model, the caption prints the peak node so the
  reader can see where it is, and nothing here pretends otherwise.
- **The clip rule is untouched.** The cone's colour range moves 4.68 → 9.42 MPa
  because its 99th percentile moved; no figure got a range chosen to flatter it.

## 7. Consequences

- Every stress render in `docs/assets/showcase/` now shows a reference outline,
  and its footer names it.
- A `Part` whose deflection is transverse to its long axis states an `up` that
  puts that axis on the screen horizontal. `up = (0, vz, −vy)` is the rule; a
  bare world axis is the exception and needs a reason.
- A fixture box whose face is a coordinate plane cutting through the middle of a
  CAD face owns the part's reported peak. Clamp the face.
