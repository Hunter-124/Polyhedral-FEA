# ADR-0040: A boundary condition names the exact closure of a CAD face

- Status: accepted (2026-08-20)
- Context: [ADR-0037](0037-traction-is-integrated-not-lumped.md) (loads are
  integrated over a stated region), [ADR-0038](0038-a-fixture-is-applied-to-the-boundary.md)
  (fixtures constrain boundary nodes, never a volume)

## 1. The report

The Studio capture `gui_studio.png` (plate with hole, h = 6 mm, one end face
fixed, the opposite end pulled with 1000 N) showed three artifacts a user
could point at: a salt-and-pepper dark band with a hot corner where the
clamped end should read as a smooth low-stress band, and two cold dents on
one long edge only — on a part and load case that are mirror-symmetric in
that direction, so any one-sided feature is numerics, not physics.

## 2. Cross-checking the GUI against the CLI

A new `--auto` verb, `savevtu <path>`, exports the solved nodal fields
exactly as the viewport interpolated them, so the Studio result can be
diffed node-for-node against the CLI solve of the same case. The two meshes
were bit-identical (52,080 tet10, 77,940 nodes). The fields were not:

- Of the 657 boundary nodes on the fixed end face, the Studio solve held
  **405** at zero displacement; the rest — the face's perimeter ring plus a
  scatter of interior mid-edge nodes — were simply unconstrained.
- The field broke the part's y-mirror symmetry: 2,868 mirrored surface pairs
  differed by 0.08 MPa median, **1.33 MPa** worst. The CLI field's mirror
  pairs agree to 0.000.
- Max von Mises differed: 3.236 MPa (Studio) vs 3.128 MPa (CLI).

The render path (8×-subdivided boundary, barycentric from nodal values) was
cleared by construction; the corruption was in the boundary conditions the
GUI assembled.

## 3. Root cause A: nearest-triangle region roulette (GUI / SolveJob)

`SolveJob` mapped a picked CAD face to mesh nodes through
`boundary_node_region`, which assigns every boundary node to exactly one
region — the region of the *nearest display triangle*. A node on a face's
perimeter sits on two or three CAD faces at once, so the roulette hands it
to whichever neighbour's tessellation happens to win, deterministically but
asymmetrically (the display tessellation is not mirror-symmetric). Fixing a
face fixed its interior and lost its ring; the free ring is what speckled
the clamped-end band (0.05…1.83 MPa where the smooth reference is
0.28…0.54 MPa), and the asymmetric ring is what dented one edge.

The mid-edge inheritance after p-elevation (`extend_boundary_regions`)
papers over this only when both endpoints of an edge won the *same* region,
which rim edges never do.

## 4. Root cause B: the CLI's default end slab is not a face

The same investigation turned on the CLI's default (box-less) end selection,
used by the plate and cantilever showcase solves. The 0.51·h slab selects
boundary *nodes* near the end plane, and `faces_within` then returns every
facet whose nodes all fit in the slab — including side-wall rim facets
0.51·h up the walls. Two measured consequences on plate_hole at h = 6 mm:

- **As a fixture**, the slab clamps an artificial patch boundary one slab
  deep into the side walls: the dominant hot band in the old field sat
  exactly at `x = xmin + 0.51·h` (2,510 nodes above 1.5 MPa), while the
  physical clamp-corner singularity was smoothed away by the extra
  constraint.
- **As a load**, the end traction was integrated over the side rim facets
  too, applying it as in-plane *shear* on the free side walls: the loaded
  face read 1.03…2.02 MPa where uniform normal traction delivers a uniform
  1.00 MPa.

On the cantilever (h = 30 mm) the same ridge held the old gallery figure's
peak: 8.64 MPa at x = 22 mm — just past the slab edge — instead of at the
root. The physical root-corner field peaks at 5.89 MPa; far-field bending
was unaffected (mid-beam top fibre 3.07 vs 3.09 MPa, analytic 3.0 MPa).

## 5. The fix

**SolveJob (GUI and every library caller).** When the model is CAD-backed,
BC membership is now derived from the exact BRep owner of every boundary
node — the same oracle the mesher snaps with (`make_boundary_projection`,
now captured with its topology table). A node belongs to a face when its
owner *is* the face, or is an edge or vertex bordering that face
(`build_exact_membership` in `scene.cpp`). Fixtures therefore cover the
face's full closure — perimeter ring and mid-edge nodes included — and load
facet sets close exactly: `faces_within` over the exact member set includes
the face's rim facets and excludes the neighbours', whose interior nodes are
not members. Classification is fresh on the mesh in hand at every
`apply_bcs` (never carried across a remesh, where node ids are reused for
different positions); a node the oracle cannot classify keeps its roulette
region, so exact coverage is never narrower than the legacy path it
replaces. Non-CAD models keep the roulette — a tessellation has no exact
owners to ask.

**CLI.** The default slab branch of `select_end` now keeps only end-facing
facets (`|n·x̂| ≥ 0.7`, the convention the degenerate-slab fallback already
used) and takes the node set as those facets' closure — the end face with
its perimeter ring, nothing up the walls. When nothing end-facing is found
it falls through to the existing normal-aligned band fallback. Explicit
`--fix-box` / `--load-box` selections are untouched: a box is a stated
region with exact clipping (ADR-0037), and the peer harness pins it.

## 6. Measured

plate_hole, h = 6 mm, graded, same mesh (52,080 tet10) everywhere:

| quantity | before (GUI) | before (CLI) | after (both paths) |
| --- | --- | --- | --- |
| fixed end-face nodes held | 405 / 657 | 657 / 657 | **657 / 657** |
| y-mirror pair gap (med / max) | 0.08 / 1.33 MPa | 0 / 0 | **0 / 0** |
| loaded-face von Mises | 0.21…1.18 MPa | 1.03…2.02 MPa | **0.979…1.000 MPa** |
| max von Mises | 3.236 MPa | 3.128 MPa | **3.140 MPa**, hole rim |

After both fixes the GUI and CLI fields agree to solver tolerance at every
mirrored node (max |Δ| < 1e-5 MPa) — two independent BC paths, one answer.

Cantilever, h = 30 mm: peak 8.64 → **5.89 MPa**, now at the physical
root-corner singularity instead of the slab-edge ridge; mid-beam top fibre
3.09 MPa against the 3.0 MPa hand calc. The gallery figure's quoted peak
moves for the honest reason that the old number measured an artifact.

Gates: mirrored-tet fraction exactly 1.0 (347,742 assertions), `[feature_pin]`
49 assertions, `[traction]` 1,509 assertions, `ctest --test-dir build-fpoff`
448/448.

## 7. What this does not fix

- A fully clamped edge is a true stress singularity: exact-closure fixtures
  make clamp corners read *warmer* than the old over-clamped slab did
  (plate clamp rim: up to 1.46 MPa against a 3.14 MPa scale). That is the
  stated boundary condition's physics, and the p99 colour clip keeps it
  from owning the render.
- Explicit box selections still mean what they say; a box that reaches up a
  wall still loads the wall (the showcase cylinder's top 5 mm of wall is
  deliberate and captioned).
- The one remaining GUI↔CLI difference is modelling intent, not mechanics:
  the capture recipe fixes region 0 (the min-x end) and pulls region 5
  (max-x) toward +x, matching the gallery's tension case; the face ids are
  now documented from measurement, not assumption.

## 8. Consequences

- `polymesh-gui --auto` gains `savevtu <path>`; the Results panel's
  *export VTU* button and the verb share one helper, so a headless capture
  can always be cross-checked against the CLI byte-for-field.
- `docs/SHOWCASE.md`'s GUI recipe and face-id note corrected (region 0 is
  the min-x end face, region 5 the max-x end face, on this build).
- `gallery_plate_hole.png`, `gallery_cantilever.png`, `hero.png` and
  `gui_studio.png` regenerated from the fixed selections; the manifest and
  quoted numbers re-synced (plate peak 3.128 → 3.140 MPa, cantilever peak
  8.64 → 5.89 MPa).
- `VolumeMeshOutput::boundary_node_region` remains as the legacy/fallback
  map and for non-CAD models; new BC code should ask for exact membership
  instead of walking it.
