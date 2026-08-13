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
low oblique angle across the whole plate (h = 3 mm, 35,386 nodes / 174,234
elements, 106,158 DOF; min-x face fixed, conserved +x resultant on max-x).
von Mises is shown on the surface with displacement warped ×5000. The colour
range spans the full field, 0 – 3.84e6 Pa. Exact values per image live in
[`manifest.json`](assets/showcase/manifest.json).

```sh
python scripts/render_showcase.py --only hero
```

## Gallery — per-part stress renders

Each render is a full import → mesh → solve → export pass on a STEP part from
[`tests/fixtures/parts/`](../tests/fixtures/parts), coloured by von Mises stress
with the displacement field warped for visibility. Every caption in
[`manifest.json`](assets/showcase/manifest.json) states the element size, node /
element / DOF counts, the boundary conditions, the warp factor, the colour range
**and the percentile it was clipped at**, plus the true unclipped peak nodal
value — which sits at a clamped face on every one of these parts and is a
boundary-condition singularity, not a physical stress. The exact `polymesh
solve` invocation per part is in the manifest too.

### `gallery_plate_hole.png`

![plate_hole](assets/showcase/gallery_plate_hole.png)

Flat plate with a central hole — the canonical stress-riser benchmark geometry.
Feature-aware grading concentrates elements around the hole where the gradient
lives. h = 3 mm, 35,386 nodes / 174,234 elements, 106,158 DOF.

```sh
python scripts/render_showcase.py --only plate_hole
```

### `gallery_cantilever.png`

![cantilever](assets/showcase/gallery_cantilever.png)

End-loaded cantilever: linear bending stress distribution, maximum at the
clamped root. h = 30 mm, 8,761 nodes / 44,832 elements, 26,283 DOF. This is the
geometry behind the Timoshenko tip-deflection verification (1.50% error,
[bench/reports/p1-gate1-convergence.md](../bench/reports/p1-gate1-convergence.md)).

```sh
python scripts/render_showcase.py --only cantilever
```

### `gallery_cylinder.png`

![cylinder](assets/showcase/gallery_cylinder.png)

Curved-wall solid imported from STEP. Curvature-driven sizing refines the
cylindrical face while the bulk stays coarse. h = 12 mm, 8,823 nodes / 42,443
elements, 26,469 DOF.

```sh
python scripts/render_showcase.py --only cylinder
```

### `gallery_sphere.png`

![sphere](assets/showcase/gallery_sphere.png)

Closed curved B-rep — the hardest case for a Cartesian grid fill. Shows the
stair-cased boundary honestly, with the feature-graded skin layers absorbing the
curvature ([ADR-0015](decisions/0015-grid-fill-limits.md)). h = 8 mm, 4,646
nodes / 22,698 elements, 13,938 DOF.

```sh
python scripts/render_showcase.py --only sphere
```

### `gallery_icecream_cone.png`

![icecream_cone](assets/showcase/gallery_icecream_cone.png)

One watertight 3D Boolean solid: a round truncated cone fused into an
overlapping spherical scoop. The committed STEP is reloaded through
OpenCASCADE, meshed at h = 10 mm, and solved with a conserved downward
resultant on the scoop: 3,250 nodes / 15,301 elements, 9,750 DOF.

```sh
python scripts/render_showcase.py --only icecream_cone
```

## Mesher comparison

![Mesher comparison](assets/showcase/compare_meshers.png)

**`compare_meshers.png`** — the same plate at h = 3 mm through three meshers:
`tet` (11,770 nodes / 53,760 cells), `graded` (35,386 / 174,234) and `hybrid`
(113,088 / 329,094), whose finer run includes conforming transition cells.
Labels and counts are burned into the tiles. All three are Cartesian grid-fill
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

| Mesher | mesh→BRep p99 / h | BRep→mesh p99 / h | sharp BRep edge→mesh p99 / h | normal p99 |
|---|---:|---:|---:|---:|
| graded | 0.0175 | 0.00392 | 0.322 | 20.9° |
| hybrid | 0.00999 | 0.0201 | 0.311 | 4.67° |
| varyhedron | 0.0174 | 0.00361 | 0.322 | 20.9° |

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

**`compare_grading.png`** — uniform (`--no-feature`, h = 3.8 mm) versus
feature-graded sizing (h = 5.6 mm) on the same part and mesher, with `h` tuned
so the two legs land on a **matched element budget**: 43,360 vs 45,308 cells,
4.5% apart (26,988 vs 29,055 DOF — the grid quantizes too hard to hit equal DOF
exactly, so the honest control is element count; both figures are printed in
the tile footers and the manifest). The comparison is therefore about *where*
the elements went, not how many there are. Same principle as the Kirsch
equal-DOF result, which had the
finer control of a structured annular mesh: at an identical 648 free DOFs,
logarithmic radial grading cut SCF error from **3.06%** to **0.70%**
([docs/progress.md](progress.md)).

```sh
python scripts/render_showcase.py --only compare_grading
```

## Benchmark charts

All three charts are plotted directly from committed JSON/reports — the script
reads the files, it does not carry the numbers.

```sh
python scripts/plot_benchmarks.py
```

### `bench_dof_time.png`

![DOF and time benchmark](assets/showcase/bench_dof_time.png)

D6 L-domain instrument: geometry-graded tet10 versus PolyMesh's **own frozen
uniform tet10 baseline** at matched strain-energy accuracy (energy deficit
0.0854% uniform vs 0.0888% graded).

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

## Architecture diagram

![Architecture](assets/showcase/architecture.png)

**`architecture.png`** — the pipeline as a dark-theme diagram: STEP/B-rep import
→ feature analysis → sizing field → hybrid meshers (tet / hex / prism / pyramid
/ polyhedron) → mixed FE+VEM assembly into one global stiffness matrix → linear
solve (SimplicialLDLT or CG) → Zienkiewicz–Zhu recovery and error estimate →
hp-adapt driver → back to the sizing field, or out to VTU. The same graph is
kept as mermaid source in the [README](../README.md#architecture).

```sh
python scripts/render_showcase.py --only architecture
```

## GUI

![PolyMesh Studio](assets/showcase/gui_studio.png)

**`gui_studio.png`** — *PolyMesh Studio* with a solved part: viewport, study
setup (material, element size, fixtures, loads), and the results panel with
stress, deflection, and the ZZ indicator η. Captured in-app: **F12** (or *File →
save screenshot*) writes the window framebuffer to a PNG, and
`POLYMESH_GUI_SHOT=/abs/path.png` writes to a fixed path, which is how the
headless capture is scripted.

```sh
POLYMESH_GUI_SHOT=$PWD/docs/assets/showcase/gui_studio.png \
  ./build/apps/gui/polymesh-gui tests/fixtures/parts/plate_hole.step
```

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
  GUI itself. Displacement is warped by a stated factor (×200 to ×5000) so
  deflection is visible, and the colour range is clipped by one rule applied to
  every render — the 99th percentile of the visible surface field — stated
  identically in every footer alongside the true unclipped peak nodal value:
  clamped faces are stress singularities under point-fixed boundary conditions,
  so an unclipped scale would render one hot node and an otherwise dark part.
  Nothing about the underlying solution is altered — only the mapping from value
  to colour.
- **Charts** (`bench_dof_time.png`, `bench_tier1.png`, `bench_mms.png`) are
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
- **Advisor figures on this page predate that rebuild.** Any accuracy-derived
  advisor number shown here was measured against the retired references and on a
  split later found to leak; the corpus is being regenerated. Structural findings
  survive, magnitudes will move. See the
  [model card](advisor/0004-model-card.md) for which is which.

## Reproduce

Three entry points, all pure Python 3 on top of the built CLI:

```sh
# 1. Everything: solves, renders, comparison grids, diagram, charts, manifest.
#    Idempotent — skips solves whose VTU already exists unless --force.
python scripts/render_showcase.py --all

# 2. Benchmark charts only, straight from committed bench data.
python scripts/plot_benchmarks.py

# 3. Generic labeled tiler, reusable for any set of PNGs.
python scripts/make_compare_grid.py --out OUT.png --title "..." \
  --labels tet,graded,hybrid img1.png img2.png img3.png
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
