# ADR-0038: A fixture is applied to the boundary, not to a volume of nodes

- Status: accepted (2026-08-19)
- Companion to [ADR-0037](0037-a-box-selection-is-a-region.md) (a box selection is
  a region), whose finding this completes on the Dirichlet side.

## 1. The report

> im still seeing jagged transitions

The image was a 1072x458 crop of `docs/assets/showcase/gallery_cylinder.png` at
`83cfc83` — the clamped base of `cylinder.step`, solved at h = 12 mm. A flat
purple region fills the bottom of the wall, and its upper edge is a sawtooth with
an amplitude of about one element, wandering with no relation to the geometry.

ADR-0037 had gone after the same word on the sphere and found a load-selection
staircase and a mis-posed smoother. Neither is what this is. This one is not the
load, not the mesher, and not the renderer.

## 2. What the purple region actually was

Purple is not a low stress. It is *exactly zero*, and it is zero because the
elements there have no strain to compute.

`--fix-box` resolved to every mesh node inside the box:

```cpp
for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {   // ALL nodes
    if (inside(box, mesh.nodes[i])) { sel.nodes.push_back(i); }
}
```

so a fixture applied through one prescribed the interior of the solid as well as
its surface. An element whose ten nodes all fall inside the box then has
identically zero strain — hence identically zero stress, whatever the rest of the
part does. The box was a rigid inclusion, not a fixture.

On the shipped showcase solves, as a fraction of all elements:

| part | fully constrained elements | share | nodes reading exactly 0 Pa |
|---|---:|---:|---:|
| cylinder (`--fix-box z <= 15 mm`) | 49,660 of 161,976 | 30.7% | 70,928 |
| sphere (`z <= -40 mm`) | 8,104 of 121,232 | 6.7% | 11,124 |
| icecream_cone (`z <= 12 mm`) | 5,244 of 86,512 | 6.1% | 7,960 |
| plate_hole (default min-x slab) | 1,488 of 52,016 | 2.9% | 2,070 |
| cantilever (default min-x slab) | 768 of 44,832 | 1.7% | 1,090 |

The default `0.51*h` end slab has the defect for the same reason: at a coarse `h`
the slab reaches a lattice layer past the end face.

### Why the edge of that region is ragged

Not because the node selection is ragged. On the cylinder wall the highest
constrained node sat at z = 14.982 mm and the lowest free one at z = 15.017 mm, so
the *node* boundary is the plane z = 15 mm to within 35 µm — a fortieth of an
element.

The raggedness comes from element extents. An element straddling the plane keeps
at least one free node, so it carries stress; the zero-stress region therefore
stops at the *far* side of the straddling layer, and how far that is varies with
the local tiling and cell size. 3,528 elements straddled. The visible boundary is
the union of one element's worth of arbitrary choices, which is exactly what a
sawtooth of one element's amplitude looks like.

The step across it was total. Wall von Mises against height, before:

| z band | 0–5 mm | 10–14.9 mm | 15.0–15.1 mm | 15.1–17 mm |
|---|---|---|---|---|
| mean | 0.000 MPa | 0.127 MPa (min 0) | 1.025 MPa | 1.241 MPa |

## 3. The library never had it

`src/pipeline/src/scene.cpp`'s `collect_bcs()` builds fixtures from
`vol.boundary_node_region`, which is populated by projecting *boundary* nodes onto
the display tessellation. The GUI and every library caller therefore always
constrained surfaces. Only `apps/cli` walked the whole node array, and its own
error message already described a box as the way to "select a real face":

```
solve: the fixture selection is geometrically degenerate — {}.
Select a real face with --fix-box x0 y0 z0 x1 y1 z1.
```

So this is not a design choice being reversed. It is one code path that disagreed
with the rest of the system and with its own diagnostics.

## 4. The rule, and where it lives

The rule moved into the FEA library beside `faces_touching`, so it is testable
rather than buried in `main.cpp`:

```cpp
std::vector<std::uint32_t> boundary_face_nodes(const std::vector<SurfaceFace>&);
std::vector<std::uint32_t> boundary_nodes_within(const NodalMesh&,
                                                const std::vector<SurfaceFace>&,
                                                const LoadRegion&);
```

`boundary_nodes_within` walks the boundary id set — O(N^(2/3)) of the mesh, and
already sorted, so the result comes out ascending without a second sort — and
accepts infinite bounds, which lets the default end slab be the same rule with two
open sides instead of a second loop. `select_end` uses it on both branches.

Two tests pin the invariant, in `tests/test_traction_selection.cpp`:

- a boundary selection never returns an interior node, and takes every boundary
  node the volume rule would have taken;
- across three tilings (n = 3, 4, 6 on the unit box) it leaves **zero** fully
  constrained elements where the volume rule leaves some.

The second is the one that matters: it is the property that decides whether a
solve contains a rigid inclusion, and it is checked on the mesh rather than
asserted in prose.

## 5. What it did to the answer

Every showcase solve changed, because the boundary condition changed. Fixed node
counts, fully constrained elements, and peak von Mises:

| part | fixed nodes | fully constrained elements | nodes at exactly 0 Pa | peak vM |
|---|---|---|---|---|
| cylinder | 74,870 → 4,949 | 49,660 → **0** | 70,928 → 0 | 16.24 → 16.24 MPa |
| sphere | 14,518 → 2,433 | 8,104 → **0** | 11,124 → 0 | 3.18 → 4.31 MPa |
| icecream_cone | 8,561 → 1,561 | 5,244 → **8** | 7,960 → 32 | 4.51 → 8.67 MPa |
| plate_hole | 3,037 → 891 | 1,488 → **0** | 2,070 → 0 | 3.082 → 3.082 MPa |
| cantilever | 1,379 → 417 | 768 → **0** | 1,090 → 0 | 5.85 → 8.64 MPa |

The cone's 8 are not a selection defect and they are reported rather than rounded
to zero. `icecream_cone.step` is a *truncated* cone: its foot is a flat disc of
radius 6 mm, and the fixture patch (`z <= 12 mm`) wraps that disc and the wall
above it. In the corner between them the solid is thinner than one element, so 8
elements of 86,512 (0.009%) have all ten nodes on the boundary surface and are
strain-free by geometry, not by rule; their centroids sit at z = 0.25–1.4 mm,
r = 5.7–6.3 mm. 32 nodes therefore still read exactly 0 Pa there, against 7,960
before. Any fixture patch that encloses a region thinner than an element does
this; the answer is resolution, not selection.

The peaks that moved all moved for the same reason and in the same direction: a
clamped *surface patch* has a stronger edge singularity than a clamped *block*,
because the material under the patch can now strain. The peaks that did not move
are the ones that were never at a fixture — the plate's is the hole's stress
riser, the cylinder's is the loaded top rim (16.24 MPa at node (-20, -46, 200) mm,
against 3.49 MPa just above the clamped base and a mid-wall 0.998 MPa where
F/A = 1.000 MPa).

Wall time went up, and this is the honest cost of the fix: constrained DOF are
eliminated from the system, so freeing 30% of the cylinder's elements gives the
solver 30% more problem. Measured, same machine, same mesh:

| part | before | after |
|---|---:|---:|
| cylinder | 376 s | 780 s |
| plate_hole | 105 s | 156 s |
| sphere | 77 s | 112 s |
| icecream_cone | 86 s | 115 s |
| cantilever | 53 s | 74 s |

Mesh output is untouched: this is a boundary condition, not a sizing input. On the
cylinder, `polymesh mesh -h 0.012` with the new fixture box and with the old one
emit bit-identical VTUs (sha256 `68d2c65498704034` both), from the same 124 BC
grading seeds.

## 6. The picture

The cylinder's base is now a smooth blue wall brightening into a rim
concentration at the z = 0 clamped face, with no flat-coloured region and no
sawtooth anywhere. The cantilever reads as a cantilever for the first time —
bright tension and compression fibres at the root, a dark neutral axis, decaying
to the tip — where before the root third was a purple block.

The showcase cylinder's fixture box was also narrowed from `z <= 15 mm` to
`z <= 2 mm`, i.e. the base face. Under the new rule the 15 mm box clamps the
outer wall up to 15 mm as well, and the upper edge of that strip is an artificial
clamped-patch boundary with a real stress singularity on it: at h = 12 mm it drew
a ring of one-element hot spots a fifteenth of the way up the wall, which no
physical fixture produces. The base face is the standard model, it is what the
caption always claimed, and as measured above it does not move the mesh. The cone
keeps its patch fixture because its foot has no flat face to clamp; its
clamped-edge ring is azimuthally smooth and is the honest answer to that model.

## 7. What this does *not* fix

The sphere's loaded band is unchanged, as expected and as measured. Iso-line
position at the 500 kPa level, before → after: rms 3.758e-4 → 3.761e-4 m, ptp
1.245e-3 → 1.245e-3 m, m=4 amplitude 3.65e-4 → 3.65e-4 m. The fixture is on the
opposite cap; the band sits at a traction discontinuity and its residual is the
element-scale discretisation error ADR-0037 §5 characterises, converging first
order in h.

The renderer was tested and cleared as a contributor. `render_stress` extracts the
surface at VTK's default nonlinear subdivision level of 1, i.e. four flat
sub-triangles per quadratic boundary face. Raising it to 3 draws the sphere's
surface field on 532,480 triangles instead of 33,280, and with element edges shown
the edge ink swamps the image; the band's wobble is present in the nodal values
themselves, which is where it was measured. Higher subdivision would need element
edges drawn as separate curved polylines, and would not change the measurement.

## 8. Consequences

- `--fix-box` and `--load-box` select boundary nodes and faces. A box that lies
  strictly inside the solid selects nothing and the existing degeneracy check
  reports it, instead of silently freezing material.
- Every stored bench result computed through the CLI is one regime stale. Two
  harnesses that mirrored the old rule in Python were corrected in the same
  change: `bench/reference/external_truth.py` now intersects the fix box with the
  boundary node set before handing CalculiX its `*BOUNDARY` cards, and
  `bench/competitive/peers/run_gmsh_peer.py` now *measures* prescribed nodes off
  the exported displacement field instead of re-deriving the box rule — the old
  count over-reported the constrained set and therefore under-reported active DOF
  on exactly the rows that matrix compares, which flattered us.
- The advisor's labels are mesh-derived and unaffected, but every case's solve
  cost moved, so the v8 retrain that was already outstanding now has one more
  reason to happen.
