# PolyMesh Showcase

Every image on this page is generated from committed data by a script in
[`scripts/`](../scripts) — no hand-drawn figures, no screenshots of other tools.
Solver renders come from real VTU output produced by the PolyMesh CLI; charts
come from committed benchmark JSON. Per-image provenance (part, mesher, element
size, DOF count, wall time, peak von Mises, and the exact solve/render commands)
is recorded in [`assets/showcase/manifest.json`](assets/showcase/manifest.json).

Regenerate everything:

```sh
python scripts/render_showcase.py --all
```

A one-line index of the same assets lives in
[`assets/showcase/OVERVIEW.md`](assets/showcase/OVERVIEW.md).

---

## Hero

![Hero stress render](assets/showcase/hero.png)

**`hero.png`** — `plate_hole` solved on the feature-graded mesher, shot from a
low oblique angle across the whole plate (h = 6 mm, 77,940 nodes / 52,080 curved
cells, 233,820 DOF; min-x face fixed, conserved +x resultant on max-x).
von Mises is shown on the surface with displacement warped ×5000 against the
undeformed outline. The colour
range is 0 – 2.61 MPa, clipped at the 99th percentile of the visible surface
field; the true peak nodal value is 3.14 MPa at the hole rim — the Kirsch
concentration the part exists to show, not a boundary-condition artifact.
Exact values per image live in
[`manifest.json`](assets/showcase/manifest.json).

```sh
python scripts/render_showcase.py --only hero
```

## Gallery — per-part stress renders

Each render is a full import → mesh → solve → export pass on a STEP part from
[`tests/fixtures/parts/`](../tests/fixtures/parts), coloured by von Mises stress
with the displacement field warped for visibility **against a grey outline of
the undeformed shape** — without that reference an axial 2–5% warp reads as
nothing at all, and a transverse one reads as camera tilt
([ADR-0041](decisions/0041-a-deformed-render-carries-its-undeformed-outline.md)).
Every caption in
[`manifest.json`](assets/showcase/manifest.json) states the element size, node /
element / DOF counts, the boundary conditions, the warp factor, the colour range
**and the percentile it was clipped at**, plus the true unclipped peak nodal
value **with its node coordinates**, so the peak can be matched against the
stated BCs rather than assumed: on the plate it is the hole-rim concentration
(a free surface — no BC acts there), on the cylinder the load box reaches 5 mm
down the wall so the peak sits on the loaded top rim. The exact `polymesh
solve` invocation per part is in the manifest too.

### `gallery_plate_hole.png`

![plate_hole](assets/showcase/gallery_plate_hole.png)

Flat plate with a central hole — the canonical stress-riser benchmark geometry.
Feature-aware grading concentrates elements around the hole where the gradient
lives. h = 6 mm, 52,080 curved cells, **233,820 solved DOF**, 95 s wall. h was
3 mm while the shipped mesh was straight-edged; the exact curved boundary now
renders a smoother hole at ~1/8 the cells.

```sh
python scripts/render_showcase.py --only plate_hole
```

### `gallery_cantilever.png`

![cantilever](assets/showcase/gallery_cantilever.png)

End-loaded cantilever: linear bending stress distribution, maximum at the
clamped root, drooping visibly out of its undeformed outline toward the loaded
tip. h = 30 mm, 44,832 curved cells, **193,971 solved DOF**, 53 s wall.
This is the geometry behind the Timoshenko tip-deflection verification (1.50%
error,
[bench/reports/p1-gate1-convergence.md](../bench/reports/p1-gate1-convergence.md)).

```sh
python scripts/render_showcase.py --only cantilever
```

### `gallery_cylinder.png`

![cylinder](assets/showcase/gallery_cylinder.png)

Curved-wall solid imported from STEP. Curvature-dominant sizing uses a 0.5×
accuracy lattice, then the **authoritative solve mesh** is promoted to
tet10/hex20 and its boundary mids are projected onto the exact BRep. The same
curved volume elements are assembled, exported, and rendered. h = 12 mm,
**688,047 solved DOF**, 780 s wall. Its load box selects the cap plus the top
5 mm of wall, and since [ADR-0037](decisions/0037-a-box-selection-is-a-region.md)
the traction is integrated over exactly that region instead of over whole faces,
so a +z shear runs up to the sharp top rim: the peak nodal value is 3.13 MPa
there, against 2.31 MPa just above the clamped base face and a nominal 0.998 MPa
in the mid-wall (F/A = 1.000 MPa exactly). The fixture is the z = 0 face alone
since [ADR-0038](decisions/0038-a-fixture-is-applied-to-the-boundary.md); it used
to be every node below z = 15 mm, which froze 30.7% of the elements solid.

```sh
python scripts/render_showcase.py --only cylinder
```

### `gallery_sphere.png`

![sphere](assets/showcase/gallery_sphere.png)

Closed curved B-rep — the hardest case for a Cartesian grid fill. The shipped
mesh is no longer a linear solve hidden behind a smoothed render: graded
boundary topology, projected tet10 geometry, stiffness assembly, VTU export and
Studio all consume the same authoritative node set. h = 8 mm, **521,175 solved
DOF**, 112 s wall.

```sh
python scripts/render_showcase.py --only sphere
```

### `gallery_icecream_cone.png`

![icecream_cone](assets/showcase/gallery_icecream_cone.png)
One watertight 3D Boolean solid: a round truncated cone fused into an
overlapping spherical scoop. Sharp CAD-edge paths are recovered through the
boundary graph under the same cell-quality/Jacobian gate, and the projected
quadratic volume mesh is solved directly. h = 10 mm, **383,295 solved DOF**,
118 s wall. Its fixture is the flat 6 mm-radius foot face, not the 12 mm band of
cone wall it used to be: that band's upper edge was an artificial clamped-patch
boundary in the middle of a conical face, and it carried the figure's peak
(8.67 MPa on a one-element ring at the box plane, against 3.65 MPa one element
above it). The peak is now the foot's own CAD rim at 12.2 MPa, a 1.38×
concentration on the 8.84 MPa mean bearing stress, with the field decaying
monotonically from the fixture into the part
([ADR-0041](decisions/0041-a-deformed-render-carries-its-undeformed-outline.md)).

```sh
python scripts/render_showcase.py --only icecream_cone
```

## Mesher comparison

![Mesher comparison](assets/showcase/compare_meshers.png)

**`compare_meshers.png`** — the same plate at h = 6 mm through three meshers,
all three now on curved CAD geometry: `tet` (37,800 DOF), `graded` (233,820) and
`hybrid` (134,748), whose transition zoo carries conforming mixed cells.
All four panels share one camera on a 60 mm window across the hole (6 hole
radii): at whole-plate scale the three topologies read as identical grey
sheets, and the cell zoo only becomes legible at the feature. The faceted rim
of the uniform `tet` fill, the graded rings hugging the bore, and the hybrid's
hex bulk are the three things to compare. All are Cartesian grid-fill
topologies, not Delaunay
([ADR-0015](decisions/0015-grid-fill-limits.md)). This is the visual form of the
`--mesher` dial documented in the [README CLI section](../README.md#cli); the
element menu and transition strategy are described in
[docs/solver-core.md](solver-core.md) and
[ADR-0012](decisions/0012-hybrid-graded-tet.md).

The committed h = 6 mm fidelity matrix also measures `graded`, `hybrid`, and
`varyhedron` against the **live** plate-hole BRep. Mesh→BRep distances use exact
trimmed projection; BRep→mesh uses 10,000 deterministic exact trimmed-face
samples under a hard attempt/storage ceiling; sharp mesh edges are classified
independently by a ≥30° boundary dihedral. These are sampled directional
distributions, not a Hausdorff-distance claim.

The **node** column is the one a mesher fully controls: where it puts a
boundary node. It is zero to machine precision on all three paths since
[ADR-0035](decisions/0035-boundary-conformity.md). The mesh→BRep column stays
non-zero because it also samples facet centroids and edge midpoints, and a
straight facet spanning a curve always carries the chord sag h²κ/8 — a
discretisation property of a linear-element mesh, not a placement error.

| Mesher | boundary node → BRep p99 / h | mesh→BRep p99 / h | BRep→mesh p99 / h | sharp BRep edge→mesh p99 / h | normal p99 |
|---|---:|---:|---:|---:|---:|
| graded | **0** | 0.00939 | 0.00372 | 0.0211 | 3.29° |
| hybrid | **0** | 0.01375 | 0.00697 | 0.1075 | 2.38° |
| varyhedron | **0** | 0.00906 | 0.00340 | 0.0287 | 3.10° |

Before ADR-0035 the same matrix read: graded mesh→BRep 0.0175 with sharp-edge
0.322 and normal p99 20.9°, hybrid 0.00999 / 0.311 / 4.67°, varyhedron 0.0174 /
0.322 / 20.9°. The sharp-edge column improved by an order of magnitude because
crease nodes are now pinned to the exact edge curve instead of to the nearest
point of a face; the second wave then took graded's crease column from 0.0321 to
0.0211 and its normal p99 from 4.54° to 3.29°, and hybrid's normal p99 from 0.52°
to 2.38° — the exterior gate trades a little facet-normal alignment on the hybrid
path for the node placement and crease fidelity in the other columns, and the
table is printed as measured rather than curated.

Source and executable provenance:
[`mesher-fidelity-plate-hole-current.json`](../bench/results/mesher-fidelity-plate-hole-current.json).

The opt-in `cvt_poly` path has a separate, non-promotional RVD-tet audit; it is
not included in `compare_meshers.png` and does not change the default mesher.
At the same h = 6 mm, exact whole-n-gon ownership and scaffold-face coalescing
keep 4,246 cells while reducing displacement vertices by 18.6%. Original-face,
cross-cell, and volume admission rejected the full CAD projection target and
restored the already surface-snapped scaffold coordinates.

| RVD-tet audit | cells | nodes | mesh→BRep p99 / h | BRep→mesh p99 / h | normal p99 | one-run mesh time |
|---|---:|---:|---:|---:|---:|---:|
| base checkout | 4,246 | 85,104 | 0.833 | 0.00403 | 90.0° | 7.81 s |
| admitted candidate | 4,246 | 69,279 | 0.0337 | 0.0202 | 15.6° | 16.54 s |

The forward metric improves because false interior interfaces are no longer
reported as free boundary. The reverse metric and admission-inclusive one-run
mesh time worsen substantially, so this is not a blanket fidelity or speed win.
The distributions are directional samples, not Hausdorff distances. At h =
15 mm, solve-backed historical context also keeps the promotion gate closed:

| Mesher / evidence source | plate-hole SCF error | DOF | solve time | health |
|---|---:|---:|---:|---|
| frozen M9 `hybrid_zoo` | 25.0% | 6,192 | 63.2 ms | ok |
| frozen M9 `varyhedron` | 62.1% | 2,232 | 16.3 ms | ok |
| current experimental `cvt_poly` | 38.3% | 24,111 | 5.19 s | ok |

The M9 timings are historical context rather than a same-executable causal A/B.
Commands, executable/source hashes, deterministic VTU hashes, exact directional
metrics, topology counters, and solve records are committed in
[`cvt-rvd-topology-current.json`](../bench/results/cvt-rvd-topology-current.json).

The separate faceted round-trip evaluator closes and orients the exported skin,
sews 10,976 planar faces into one BRepCheck-valid solid, and measures exact
point-to-shape distances in both directions. Its exact trimmed-face reverse
sample has p99 = 0.118 h under a 500-point / 2,000-attempt ceiling:
[`brep-roundtrip-plate-hole-current.json`](../bench/results/brep-roundtrip-plate-hole-current.json).

```sh
python scripts/render_showcase.py --only compare_meshers
```

## Grading comparison

![Grading comparison](assets/showcase/compare_grading.png)

**`compare_grading.png`** — uniform (`--no-feature`, h = 4.2 mm) versus
feature-graded sizing (h = 5.6 mm) on the same part and mesher, with `h` tuned
so the two legs land on a **matched element budget**: 42,960 vs 45,128 cells,
5.0% apart (190,032 vs 205,128 DOF on the curved geometry — the grid quantizes
too hard to hit equal DOF exactly, so the honest control is element count; both
figures are printed in the tile footers and the manifest).
The comparison is therefore about *where*
the elements went, not how many there are. Same principle as the Kirsch
equal-DOF result, which had the
finer control of a structured annular mesh: at an identical 648 free DOFs,
logarithmic radial grading cut SCF error from **3.06%** to **0.70%**
([docs/progress.md](progress.md)).

```sh
python scripts/render_showcase.py --only compare_grading
```

## Boundary conformity

Every boundary node sits on the exact BRep, not near it
([ADR-0035](decisions/0035-boundary-conformity.md)). This section exists
because the previous figures did not: a reader looking at them reported that
round surfaces carried random defects and a curved-to-planar edge rendered as a
chamfer, and both were real. The renders draw the mesh VTU verbatim
(`extract_surface`, flat shading, element edges on), so there was nowhere for a
defect to hide.

Three structural causes, all removed:

- fills snapped to the **tessellation** rather than the BRep, and two of the six
  meshers were never given the exact oracle at all;
- no mesher called `project_point_on_edge`, so a 90° crease was reconstructed
  from the nearest point of a *face*, and varyhedron's 35 % "edge attraction"
  blend chamfered the remaining 65 % by construction;
- the snap's unsnap ladder silently kept partial projections, leaving nodes up
  to 0.6 h off the CAD while reporting nothing.

Boundary-node distance to the exact BRep, p99 as a fraction of h, at h = 8 mm:

| Part | mesher | before | after |
|---|---|---:|---:|
| sphere | graded | 0.039 | **2.9e-15** |
| cylinder | graded | 0.045 | **2.8e-15** |
| plate_hole | tet | 0.218 | **0** |
| icecream_cone | varyhedron | 0.039 | **3.7e-15** |

Normal-angle p99 (mesh facet vs BRep normal) falls from 30° to 4.4° on the
graded sphere and to 0.52° on the hybrid plate. No mesh in the fixture set
ships an inverted cell any more: the sphere hybrid at h = 3 mm went from
`quality_min` −0.837 to +0.0022, with the 38 cells still under the shape floor
**reported** in the mesher note and in `diag --json` as
`mesh.n_below_shape_floor` rather than passed off as clean.

What a Fourier transform contributes here is spacing, not geometry: a closed
crease chain is re-spaced by low-passing the deviation of its arclength
parameters from a uniform chain, and every pin target is still the exact OCC
curve projection. Fourier chooses *where along the curve* a node sits, never
where the curve is.

### The exterior that ships

The same reader looked again and reported that the sphere and the cone *still*
had visible surface defects, and that was right too. Conforming each mesher's own
lattice skin is not the same as conforming the mesh that leaves: a fan tet peeled
after the snap, a pyramid emitted as two assembly tets or an LEB child carved
late all expose nodes that were interior when the snap ran. On sphere/hybrid the
mixed-fill branch's own worst boundary node read 0.016 h while the shipped mesh
carried nodes 0.085 h off the BRep, in visible flaps. And the owner oracle's
fallback to the tessellation reported ~0 for exactly those nodes, because OCC's
own facets are 0.085 h off this sphere at its deflection.

A mesher-independent gate now conforms the extracted exterior, marching each node
onto the BRep in whatever fraction keeps every incident cell integrable by
`fea::element_jacobians_positive` — the assembly's own det J > 0 test, which
`fea::cell_quality` cannot stand in for: a quality-accepted move once shipped
det J = −6.1e-09. A hex saturated at the shape floor has no interior corner to
give, so it is fanned into six pyramids over its own six faces (the bases *are*
the hex faces, so nothing else sees a change).

The last visible artifact was not placement at all but **spacing**. The showcase
cone shipped adjacent facet planes differing by up to 77.8°, because grading
transitions leave needle facets beside bulk ones. Sliding the face-owned nodes
along their own surface — never the crease or corner nodes — lowers those kinks;
the dihedral detector was also comparing *signed* normals, so an opposite winding
read as a 180° crease and inflated the count from 47 to 177.

Measured at h = 8 mm, second wave, before → after:

| Part | mesher | metric | before | after |
|---|---|---|---:|---:|
| icecream_cone | varyhedron | feature p99 / h | 17.3 | **0.150** |
| icecream_cone | hybrid | feature p99 / h | 1.16 | **0.025** |
| icecream_cone | graded | normal p99 | 20.5° | **9.1°** |
| sphere | hybrid | node p99 / h | 0.0062 | **5.9e-15** |
| sphere | hexpyr | node p99 / h | 0.373 | **0.026** |
| cylinder | hybrid | normal p99 | 19.2° | **0.27°** |

The whole pass is judged on entry and exit — worst cell quality may not drop, the
count of sub-floor cells may not grow, no cell may become non-integrable — and
reverts wholesale otherwise, which is why the experimental packed-poly mesher,
whose cells are already degenerate at ~1e-14, comes out at exactly its previous
numbers instead of slightly worse.

The old display-only workaround is gone. CAD-backed product paths now ship and
solve the curved mesh itself:

- Curvature-dominant BReps use a **0.5× authoritative accuracy lattice**.
- Pyramid transition cells are split with the same conformity-safe diagonal
  used by assembly, then every tet4/hex8 is promoted to tet10/hex20.
- Boundary mids are projected onto exact BRep faces; boundary-graph paths along
  sharp CAD edges are pinned as coupled endpoint moves with local interior-star
  optimisation.
- Every accepted corner, edge and midpoint move must retain
  `quality ≥ 0.02` and positive sampled Jacobians. Invalid combined midpoint
  moves are rolled back and trigger local h-refinement rather than shipping a
  malformed cell.
- Studio tessellates the actual isoparametric faces at eight subdivisions and
  interpolates solved fields with the same shape weights. That rendering is a
  faithful sampling of the solved tet10/hex20 geometry, not a substitute mesh.

At requested h = 8 mm, the product graded path measures p99 surface-to-BRep
error over bbox diagonal of **8.23e-6 sphere**, **8.21e-5 cone**,
**1.42e-5 cylinder** and **9.91e-6 plate-with-hole** — all four inside the
1e-4 (99.99%) target. The cone was the holdout at 1.23e-4 until
[ADR-0039](decisions/0039-a-stranded-boundary-node-is-rescued.md): its
residual was one orbit of boundary nodes stranded at their raw lattice sites
behind floor-pinned cap cells, plus two crease-classification defects at the
foot rim, not chordal error. Their curved-cell `quality_min` values are
0.06338, 0.02085, 0.02550 and 0.03188, with zero inverted and zero sub-floor
cells, and relative volume errors of 3.9e-5, 6.7e-5, 1.2e-5 and 5.3e-6.
Facet-normal p99 is 0.320° / 2.82° / 1.28° / 0.684°; the ~90° maxima on the cone,
cylinder and plate are declared BRep sharp edges and the cone's apex vertex, not
smooth-wall defects.

Two honest residuals remain in the CAD → mesh **edge-coverage** direction, which
asks how far a sampled sharp BRep edge is from the nearest classified mesh
feature curve. Cone coverage p99 is 1.96e-4 of bbox, but cylinder is 1.83e-2 and
plate-with-hole 5.73e-3. The mesh-side error is tiny in the other direction
(4.95e-6 cylinder), so this is not chordal error: a handful of rim chords are
missing from the traced polygon, and a single missing chord puts the rim samples
in its middle at half a chord from the nearest classified curve. Rim tracing is
about 97–99% complete, not 100%.

The recovery pass is priced in the mesher note (`edge_pass=… ms`). Branch-and-
bound worst-quality evaluation brought it from 22.7 s to 6.2 s on plate_hole at
h = 6 mm, where the promotion itself costs a further 2.3 s.

### Reproducing the curved-versus-chordal difference

`polymesh render` rasterizes the *same* isoparametric surface Studio paints
(`fea::tessellate_boundary_surface`), so the claim can be checked without a GPU
or a window, and `--stats` measures each rendered facet normal against the exact
BRep:

```bash
polymesh render tests/fixtures/parts/icecream_cone.step -h 0.012 \
    -o cone_curved.png --stats cone_curved.json
polymesh render tests/fixtures/parts/icecream_cone.step -h 0.012 --no-curved \
    -o cone_chordal.png --stats cone_chordal.json
```

Both runs solve the identical 25 311-cell mesh; only the geometry order differs.
Measured facet-normal deviation from the exact BRep (mean / p99):

| part | curved | chordal (`--no-curved`) |
|---|---|---|
| sphere, h = 12 mm | 0.122° / 0.362° | 0.967° / 2.965° |
| icecream cone, h = 12 mm | 0.438° / 5.195° | 1.816° / 7.711° |

The cone's curved p99 is dominated by facets whose centroid sits on its apex
vertex and rim, where the exact normal is discontinuous by construction; its
85.1° maximum is that apex, not a smooth-wall defect.

## Spectral sizing

The sizing field the meshers consume is FFT-filtered before it is used
([ADR-0034](decisions/0034-spectral-sizing-and-coarsening.md)), and the CLI
prints what the filter did on every `mesh` / `solve` run:

```text
spectral: 55706/262143 modes kept (99.50% energy), 43 denoised edge-curve seeds,
          N_pred 1574 → 1577
```

Three uses, each matched to what a Fourier transform is actually good for:

| Where | What it does |
|---|---|
| CAD edges | κ(s) from OCC carries parameterization noise; an energy-truncated inverse FFT recovers the smooth curvature before it emits chordal size sources |
| Sizing field | the dominant modes carrying 99.5% of the spectral energy are kept and the rest dropped, so isolated seed artifacts merge into the surrounding coarse field |
| Element budget | the Σvol/h³ density integral is the same N_pred contract the CVT path uses, so a cap can be met by one uniform scale after truncation |

A geometry-only floor is re-imposed after filtering, which is why this is safe
to leave on: trimming can raise `h` in a spectrally weak band but never inside a
real curvature or thin-wall demand. On the clean public fixtures that shows up
as a leaner seed set at unchanged mesh output. Two measured A/Bs, each run with
the default and again with `--no-spectral`:

| Part | With spectral | Without |
|---|---|---|
| `sphere.step`, h = 8 mm (`mesh`) | 41 seeds from 51 geometry sources, 9,194 cells | 51 seeds, 9,194 cells |
| `icecream_cone.step`, h = 8 mm (`diag`) | 43 denoised edge-curve sources; 27,368 cells, `quality_min` 0.02098, boundary-node→BRep p99/h 3.7e-15, mesh→BRep p99/h 0.0364, peak VM 2.35799e7 Pa | 27,368 cells, 0.02098, 3.7e-15, 0.0364, 2.35799e7 Pa |

So on fixtures whose curvature was already smooth, the filter is measurably a
no-op in mesh output while emitting a smaller, denoised source set. The payoff
is on noisy real-world curvature; these fixtures are not where it shows, and the
page says so rather than implying otherwise.

`diag --json` carries the same numbers as a `spectral` block for the
self-improve loop, and campaigns opt in per run with `"spectral_smooth": true`.

```sh
polymesh mesh tests/fixtures/parts/icecream_cone.step -h 0.008          # on by default
polymesh mesh tests/fixtures/parts/icecream_cone.step -h 0.008 --no-spectral
```

## Benchmark charts

All four charts are plotted directly from committed JSON/reports — the script
reads the files, it does not carry the numbers.

```sh
python scripts/plot_benchmarks.py
```

### `bench_dof_time.png`

![DOF and time benchmark](assets/showcase/bench_dof_time.png)

D6 L-domain instrument: geometry-graded tet10 versus PolyMesh's **own frozen
uniform tet10 baseline**. The graded mesh does not match the baseline's
strain-energy accuracy — its energy deficit is 0.0888% against the baseline's
0.0854%, i.e. 1.04× as large — so the DOF and time it saves are bought at a
measured 4% higher deficit, not at parity.

| Leg | DOF | Total wall time |
|---|---:|---:|
| uniform tet10 (n=6 baseline) | 6384 | 2.762 s |
| geometry-graded tet10 (h0 = w/8, ρ=2 layers) | 1248 | 0.227 s |
| ratio | **5.12×** | **12.18×** |

Source: [`bench/results/polymesh-d6-l-domain.json`](../bench/results/polymesh-d6-l-domain.json),
[docs/bench/d6-tier3.md](bench/d6-tier3.md). This is a self-relative
measurement, not a comparison against another solver — see
[Limitations](../README.md#limitations).

### `bench_tier1.png`

![Tier-1 error bars](assets/showcase/bench_tier1.png)

Relative error on the five closed-form analytical verifications, each against
its acceptance tolerance:

| Case | Metric | Error | Tolerance |
|---|---|---:|---:|
| Lamé thick cylinder | radial displacement, inner wall | 0.0068% | 1% |
| Lamé thick cylinder | hoop stress, inner wall | 1.36% | 4% |
| Timoshenko cantilever | tip deflection | 1.50% | 3% |
| Kirsch plate | SCF 3.056 vs 3.0 | 1.87% | 5% |
| Goodier spherical cavity | SCF 1.902 vs 2.045 | 7.04% | 12% |
| L-domain re-entrant corner | energy-gap order 1.265 vs 2λ=1.089 | — | ±0.35 |

Source: [`bench/reports/p1-gate1-convergence.md`](../bench/reports/p1-gate1-convergence.md).
These were measured on **structured parametric meshes** (hex20 sectors, annuli,
shell octants per [ADR-0009](decisions/0009-tier1-verification-setups.md)), not
on product grid-fill meshes.

### `bench_mms.png`

![MMS convergence](assets/showcase/bench_mms.png)

Manufactured-solution energy-norm convergence on uniform h-halving, both element
families that the chart plots — the frozen P1 isoparametric elements and the
hierarchical p-basis:

| Family | Theory order | Observed order |
|---|---:|---:|
| frozen P1 · tet4 | 1 | **0.997** |
| frozen P1 · hex8 | 1 | **0.997** |
| frozen P1 · tet10 | 2 | **2.000** |
| frozen P1 · hex20 | 2 | **2.000** |
| hierarchical p = 1 | 1 | **1.02** |
| hierarchical p = 2 | 2 | **1.99** |
| hierarchical p = 3 | 3 | **2.98** |
| hierarchical p = 4 | 4 | **3.98** |

Source: [docs/progress.md](progress.md),
[`bench/reports/p1-gate1-convergence.md`](../bench/reports/p1-gate1-convergence.md).
The manufactured field is generated from a randomized seed at test time so its
coefficients cannot be memorized or hardcoded
([docs/benchmarks.md](benchmarks.md)).

### `bench_advisor_budget.png`

![Advisor under a DOF budget](assets/showcase/bench_advisor_budget.png)

The learned mesh advisor choosing under a hard DOF cap
([ADR-0034](decisions/0034-spectral-sizing-and-coarsening.md)). Each point is one
invoked CLI run — `polymesh solve --advisor bench/advisor --advisor-max-dof N`
— plotted at the DOF budget it was given against the predicted per-case
relative-error score of the action it chose. The advisor enumerates its measured
candidate grid, drops candidates whose predicted DOF exceeds the cap, and ranks
what survives; each coloured step holds one action until a looser cap buys a
better one.

What the shipped sweep records, read off the JSON:

| Part | 2k cap | 4k cap | 8k .. 64k and no cap |
|---|---|---|---|
| `box_hole_s0` | graded_tet p1, 1.6k predicted DOF | hex p2, 2.8k | hybrid_vem p2, 6.4k |
| `plate_notch_s0` | hybrid_zoo p2, 1.9k | hybrid_vem p2, 2.6k | hybrid_vem p2, 2.6k |
| `stepped_shaft_s0` | hybrid_zoo p1, h 0.1, 0.9k | hybrid_zoo p1, h 0.08 + 1 adapt, 2.4k | hybrid_zoo p1, h 0.08 + 1 adapt, 2.4k |

Every capped pick in this sweep predicts a DOF count under its cap — 0 of the 18
capped, non-vetoed runs is over budget — and no run was refused. Earlier text
here described a refusal for `stepped_shaft_s0` at a 2,000-DOF cap; the sweep as
committed does not contain one, and the figure's title states the count it
computes rather than an anecdote.

Two caveats the figure now carries on its face. The DOF figures are the model's
own predictions, and its held-out DOF error is about 0.5 decades, so a cap is a
feasibility filter and not a guarantee. And of the 21 invoked solves, **13 exited
nonzero**: the picked action failed to mesh or solve — every `hybrid_vem p2` pick
plus `plate_notch_s0`'s `hybrid_zoo p2` at the 2k cap. The plotted score is the
model's prediction, not a measured outcome, so the marks stand; the failure count
belongs beside them.

Source: [`bench/results/advisor-budget-sweep.json`](../bench/results/advisor-budget-sweep.json)
(21 runs), regenerated by
[`scripts/sweep_advisor_budget.py`](../scripts/sweep_advisor_budget.py).

## Architecture diagram

![Architecture](assets/showcase/architecture.png)

**`architecture.png`** — the pipeline as a dark-theme diagram: STEP/B-rep import
→ feature analysis (curvature, thin-wall, FFT edge denoise) → spectral-trimmed
sizing field → hybrid meshers (tet / hex / prism / pyramid / polyhedron) → mixed
FE+VEM assembly into one global stiffness matrix → linear solve (SimplicialLDLT,
or CG on the diagonally equilibrated system) → Zienkiewicz–Zhu recovery and
error estimate → hp-adapt driver, which returns a refine, coarsen or p-elevate
decision to the sizing field rather than remeshing from scratch. The learned
mesh advisor hangs off feature analysis: it reads the same case features and
proposes the mesher, `h`, adapt schedule and order, filtered by a DOF budget
when one is given. The same graph is kept as mermaid source in the
[README](../README.md#architecture).

```sh
python scripts/render_showcase.py --only architecture
```

## GUI

![PolyMesh Studio](assets/showcase/gui_studio.png)

**`gui_studio.png`** — *PolyMesh Studio* with a solved part: the live viewport,
material and mesh setup, fixture/load selection, campaign and Test Lab state,
the measured stress legend, and the solve summary in the status ledger. The
capture uses the practical 8 mm Cartesian tet case shown on screen: 4,320 cells,
22,920 unknowns and a 2.478 MPa peak. Captured in-app, **F12** (or *File → save
screenshot*) writes the window framebuffer to a PNG, and
`POLYMESH_GUI_SHOT=/abs/path.png` writes to a fixed path. The fully scripted run
(load, size, mesher, solver, fixtures, load, solve, frame, capture, quit) goes
through `--auto`, which drives the same code paths as the buttons:

```sh
POLYMESH_GUI_SIZE=1920x1080 xvfb-run -a -s "-screen 0 1920x1080x24" \
  ./build/apps/gui/polymesh-gui --auto \
  "load tests/fixtures/parts/plate_hole.step; h 8; mesher tet; solver direct; \
   fix 0; loadface 5 1000 0 0; solve; wire off; frame; \
   shot $PWD/docs/assets/showcase/gui_studio.png; quit"
```
(`wire off` because the results wireframe is baked in a near-black line colour
and covers the shaded field at this zoom; interactively that is the *wireframe
edges* checkbox. Face ids 0 and 5 are the plate's two end faces — 0 the min-x
end, 5 the max-x end — so this is the gallery's tension case: min-x fixed,
+x resultant on max-x. `savevtu out.vtu` additionally exports the solved nodal
fields, which is how the GUI's solve is cross-checked against the CLI's.)

---

## Methodology

- **Solver renders** (`hero.png`, `gallery_*.png`, `compare_meshers.png`,
  `compare_grading.png`) are produced by [PyVista](https://pyvista.org) from
  **real solver output**: the PolyMesh CLI writes a VTU containing the
  `von_Mises` stress and `displacement` fields, and the render script reads that
  file. Nothing is synthesized, retouched, or drawn by hand. The stress colormap
  is viridis (`figstyle.field_cmap("magnitude")`): perceptually uniform, monotone
  in lightness and colour-blind safe. The GUI's own blue → cyan → green → yellow
  → red ramp is not, so it appears only in `gui_studio.png`, which documents the
  GUI itself. Displacement is warped by a stated factor (×200 to ×10000) and
  drawn against the view silhouette of the *undeformed* shape, without which the
  warp is unreadable — five of the six stress cases deform along their own long
  axis, so the silhouette moves by a few percent of a shape the reader has never
  seen at rest. The colour range is clipped by one rule applied to
  every render — the 99th percentile of the visible surface field — stated
  identically in every footer alongside the true unclipped peak nodal value:
  clamped faces are stress singularities under point-fixed boundary conditions,
  so an unclipped scale would render one hot node and an otherwise dark part.
  Nothing about the underlying solution is altered — only the mapping from value
  to colour.
- **Charts** (`bench_dof_time.png`, `bench_tier1.png`, `bench_mms.png`,
  `bench_advisor_budget.png`) are
  generated by `scripts/plot_benchmarks.py`, which reads committed benchmark
  artifacts — [`bench/results/*.json`](../bench/results) and
  [`bench/reports/p1-gate1-convergence.md`](../bench/reports/p1-gate1-convergence.md).
  If a number changes in the data, the chart changes; the numbers are not
  duplicated inside the plotting script.
- **GUI screenshot** (`gui_studio.png`) is captured inside the application via
  the F12 screenshot path (`glReadPixels` on the default framebuffer, written as
  8-bit RGBA PNG), not by an external screen grabber.
- **Diagram** (`architecture.png`) is drawn programmatically with matplotlib
  through `scripts/figstyle.py`, so it matches the rest of the generated figures.
- **Advisor cinema** (`assets/cinema/advisor_cinema.mp4`, `.gif`, `poster.png`)
  is the GUI's own framebuffer at a fixed 1/60 s virtual timestep, encoded by
  `scripts/render_cinema.py`. Its default hero is
  `tests/fixtures/parts/wishbone.step`, generated by
  `scripts/gen_cad_parts.py::make_wishbone` and checked as one valid,
  positive-volume solid. Two internal chassis-bushing bores form separate
  bonded-pin supports; two swept non-coplanar arms and a curved brace converge
  on the internally loaded ball-joint bore. The conserved 4.717 kN design
  resultant combines vertical and inboard components, producing bending and
  torsion rather than token axial stress.

  The recorded result has 40,170 tet4 cells, 9,796 nodes and 29,388 unknowns;
  minimum/mean shape quality is 0.0200 / 0.2496. The distributed bore load
  produces 202.49 MPa true peak von Mises stress (108.82 MPa p99) and 1.198 mm
  physical peak displacement. Its global ZZ indicator is 29.77% and remains
  visible as verification evidence; this is not labelled reference truth.

  The opening starts directly on the analysed shaded CAD body. No neighbouring
  interface hardware is generated. The full BRep edge network lights with
  measured curvature from independently filtered edge traces while one selected
  exact edge shares its cursor with the $\kappa(s)$ plot. Its real
  even-reflected FFT then truncates and reconstructs back over the same edge.
  Because the published run has `feature_grading=false`, it ends at “uniform h
  unchanged” and draws no fabricated spatial sizing field.

  The advisor uses four activation lanes. Every node remains sized/coloured by
  the deployed graph's own tensor, and every shown connection remains one of the
  strongest measured $|w_{ji}a_i|$ paths. All measured candidate passes now fill
  the deliberation window. The final pass locks and the actual `MeshStage`
  snapshot begins 0.55 s later without a chapter or panel reset; a live
  shown/total cell counter follows the emission-order reveal while the chosen
  network remains visible. This preserves the real ordering—chooser first,
  mesher second—without presenting them as unrelated chapters or pretending
  they ran concurrently. The OOD check still abstains outside the calibrated
  envelope, so the configured independently verified baseline is generated.

  The cell microscope owns 10.2 s and now gives the space to one unambiguous
  higher-order element rather than a dense six-tet cube comparison. The tet10
  card shows four corner nodes, all six midside nodes, and every quadratic edge
  as corner→midside→corner. The bottom ledger and manifest continue to identify
  the executed wishbone solve as tet4/p1, so the card teaches supported element
  anatomy rather than claiming that this particular solve ran at p2.

  Mechanics symbols remain absent during exact-CAD analysis, advisor inference
  and meshing. Immediately before gradient recovery and final deformation, each
  support is outlined from its actual internal bushing-bore vertices; the load
  arrow reaches the internal ball-joint bore. This p1 take has no MPC transform,
  so reaction vectors and magnitudes are complete resultants summed from the
  prescribed generalized residual using the exact boundary-condition membership
  that produced the solve. `reactions_complete=false` suppresses arrows on
  MPC-constrained runs rather than mislabeling generalized forces as nodal
  support resultants. During the exact linear ramp, applied and reaction vectors
  scale by the displayed $\lambda$. Final deformation remains limited to 4% of
  the model diagonal with a translucent undeformed CAD surface behind it.

  Framing is solved from all eight corners with the settled viewport aspect and
  includes the complete rest-to-4%-displayed deformation envelope before frame
  zero. The bottom ledger is deliberately sparse: active symbols and necessary
  numbers on the left, optional measurement note on the right, provenance below.
  The poster comes from the first fully composed opening frame.

  Every panel consumes production data: ONNX trunk taps and exported weights;
  `pipeline::build_refinement_plan`; captured `MeshStage`/`SolveStage` meshes;
  `fea::summarize_cell_quality`; structural corner-topology differencing; and
  `SolveJob::take_result()` after the configured solve finalisation.
  Cosmetic work is limited to framing, virtual pacing, opacity, centroid
  separation, spatial handoffs, colour and layout. Full citations and
  disclosures are in [`assets/cinema/NOTES.md`](assets/cinema/NOTES.md);
  commands, exact stage order, solve metrics, display percentiles, model digest,
  encoder and output hashes are in
  [`assets/cinema/manifest.json`](assets/cinema/manifest.json)
  ([ADR-0042](decisions/0042-the-advisor-explains-itself-on-screen.md),
  [ADR-0043](decisions/0043-a-film-someone-can-read.md)).
- **Honesty rules.** Speed and DOF wins are measured against PolyMesh's own
  frozen uniform-tet10 baseline (ADR-0005), never against a third-party solver.
  Product volume fills are Cartesian grid-fill, not constrained Delaunay
  ([ADR-0015](decisions/0015-grid-fill-limits.md)). Tier-1 analytical numbers
  come from structured parametric meshes. The poly-VEM path is research-gated
  (node M5, not promoted). Full statement:
  [README § Limitations](../README.md#limitations).
- **Accuracy truths are independent of this engine.** The advisor corpus's
  reference answers are no longer produced by PolyMesh. 88 of the 96 corpus
  references come from Gmsh 4.13.1 meshing the CAD and CalculiX 2.23 solving it,
  and 8 are closed-form; the previous self-generated references were retired as
  circular ([ADR-0029](decisions/0029-independent-truth-and-honest-gates.md) §1).
  The chain was validated against closed form before adoption, and on one
  identical mesh CalculiX and our solver agree to 3.4e-09 in tip deflection —
  the mesher, not the solver, was the variable. Raw per-rung solver output and
  the per-metric before/after audit are committed under
  [`bench/reference/external/`](../bench/reference/external).
- **The advisor chart plots predictions, not measured accuracy.** Every value in
  `bench_advisor_budget.png` is the shipped model's own head output for the
  action it chose, which is the quantity the chooser actually optimizes; it is
  not a measured error, and the DOF axis is a predicted DOF with roughly
  half-a-decade held-out error. The model behind it is the v4 corpus retrain
  ([`docs/advisor/0008-v4-corpus-retrain.md`](advisor/0008-v4-corpus-retrain.md)).
  Older advisor figures elsewhere in the docs predate that rebuild: any
  accuracy-derived advisor number measured against the retired self-generated
  references is flagged in the
  [model card](advisor/0004-model-card.md), whose structural findings survive
  while magnitudes moved.

## Reproduce

Four entry points, all pure Python 3 on top of the built CLI and GUI:

```sh
# 1. Everything: solves, renders, comparison grids, diagram, charts, manifest.
#    Idempotent — skips solves whose VTU already exists unless --force.
python scripts/render_showcase.py --all

# 2. Benchmark charts only, straight from committed bench data.
python scripts/plot_benchmarks.py

# 3. Generic labeled tiler, reusable for any set of PNGs.
python scripts/make_compare_grid.py --out OUT.png --title "..." \
  --labels tet,graded,hybrid img1.png img2.png img3.png

# 4. The advisor cinema: drives polymesh-gui under xvfb-run itself (do not wrap
#    it in another xvfb-run), verifies the frames, then encodes mp4/gif/poster.
#    Frames land in build/cinema/frames; --only re-encodes without recapturing.
#    The default is icecream_cone; --part selects retained advisor-corpus cases.
python scripts/render_cinema.py --all
python scripts/render_cinema.py --all --part box_hole_s0_c0
python scripts/render_cinema.py --list
```

Useful `render_showcase.py` flags:

| Flag | Effect |
|---|---|
| `--all` | Full pipeline end to end (also shells out to `plot_benchmarks.py`) |
| `--only NAME` | Repeatable; `NAME` is a part (`plate_hole`) or an image stem (`hero`, `compare_meshers`, `architecture`) |
| `--list` | Print the part/image table and exit |
| `--force` | Re-run solves even when the VTU exists |
| `--no-charts` | Skip the `plot_benchmarks.py` shellout |
| `--outdir DIR` | Output directory (default `docs/assets/showcase`) |
| `--stress SOLVE.vtu --out X.png` | Single stress render from an existing solve VTU |
| `--mesh MESH.vtu --out X.png` | Single mesh render from an existing mesh VTU |

Prerequisite: a Release build with OCC enabled (see
[README § Quickstart](../README.md#quickstart-ubuntu)), since every render
starts from a STEP import.
