# PolyMesh Showcase

Every image on this page is generated from committed data by a script in
[`scripts/`](../scripts) — no hand-drawn figures, no screenshots of other tools.
Solver renders come from real VTU output produced by the PolyMesh CLI; charts
come from committed benchmark JSON. Per-image provenance (part, mesher, element
size, DOF count, wall time, peak von Mises, and the exact solve/render commands)
is recorded in [`assets/showcase/manifest.json`](assets/showcase/manifest.json).

Regenerate everything:

```sh
python3 scripts/render_showcase.py --all
```

A one-line index of the same assets lives in
[`assets/showcase/OVERVIEW.md`](assets/showcase/OVERVIEW.md).

---

## Hero

![Hero stress render](assets/showcase/hero.png)

**`hero.png`** — `plate_hole` solved on the feature-graded mesher, shot from a
low oblique angle across the whole plate (h = 6 mm, 4,629 nodes / 18,887
elements, 13,887 DOF; min-x face fixed, traction on max-x, the CLI default BC
set). von Mises on the surface with displacement warped ×200. The colour range
is 0 – 1.24e7 Pa, clipped at the 99.5th percentile of the visible surface: the
true peak nodal value is 3.6e13 Pa at the clamped face, which is a boundary
condition singularity rather than a physical stress, so an unclipped range would
show one hot node and a uniformly blue part. Exact values per image live in
[`manifest.json`](assets/showcase/manifest.json).

```sh
python3 scripts/render_showcase.py --only hero
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
lives. h = 6 mm, 4,629 nodes / 18,887 elements, 13,887 DOF.

```sh
python3 scripts/render_showcase.py --only plate_hole
```

### `gallery_cantilever.png`

![cantilever](assets/showcase/gallery_cantilever.png)

End-loaded cantilever: linear bending stress distribution, maximum at the
clamped root. h = 30 mm, 3,327 nodes / 13,008 elements, 9,981 DOF. This is the
geometry behind the Timoshenko tip-deflection verification (1.50% error,
[bench/reports/p1-gate1-convergence.md](../bench/reports/p1-gate1-convergence.md)).

```sh
python3 scripts/render_showcase.py --only cantilever
```

### `gallery_cylinder.png`

![cylinder](assets/showcase/gallery_cylinder.png)

Curved-wall solid imported from STEP. Curvature-driven sizing refines the
cylindrical face while the bulk stays coarse. h = 12 mm, 3,719 nodes / 17,186
elements, 11,157 DOF.

```sh
python3 scripts/render_showcase.py --only cylinder
```

### `gallery_sphere.png`

![sphere](assets/showcase/gallery_sphere.png)

Closed curved B-rep — the hardest case for a Cartesian grid fill. Shows the
stair-cased boundary honestly, with the feature-graded skin layers absorbing the
curvature ([ADR-0015](decisions/0015-grid-fill-limits.md)). h = 8 mm, 4,775
nodes / 23,399 elements, 14,325 DOF.

```sh
python3 scripts/render_showcase.py --only sphere
```

### `gallery_icecream_cone.png`

![icecream_cone](assets/showcase/gallery_icecream_cone.png)

Mixed curvature plus a sharp apex in one part: a smooth dome blended into a
converging cone, so a single sizing field has to handle both a curvature-driven
and a feature-driven length scale. h = 10 mm, 5,410 nodes / 25,255 elements,
16,230 DOF.

```sh
python3 scripts/render_showcase.py --only icecream_cone
```

## Mesher comparison

![Mesher comparison](assets/showcase/compare_meshers.png)

**`compare_meshers.png`** — the same plate at h = 6 mm through three meshers:
`tet` (1,884 nodes / 6,840 cells), `graded` (4,629 / 18,887) and `hybrid`
(21,100 / 54,720), the default element zoo. Labels and counts are burned into
the tiles. All three are Cartesian grid-fill topologies, not Delaunay
([ADR-0015](decisions/0015-grid-fill-limits.md)). This is the visual form of the
`--mesher` dial documented in the [README CLI section](../README.md#cli); the
element menu and transition strategy are described in
[docs/solver-core.md](solver-core.md) and
[ADR-0012](decisions/0012-hybrid-graded-tet.md).

```sh
python3 scripts/render_showcase.py --only compare_meshers
```

## Grading comparison

![Grading comparison](assets/showcase/compare_grading.png)

**`compare_grading.png`** — uniform (`--no-feature`) versus feature-graded
sizing on the same part and mesher, with `h` tuned so the two legs land on a
**matched element budget**: 18,912 vs 18,944 cells, 0.2% apart (12,426 vs 13,719
DOF — the grid quantizes too hard to hit equal DOF exactly, so the honest
control is element count; both figures are printed in the tile footers and the
manifest). The comparison is therefore about *where* the elements went, not how
many there are. Same principle as the Kirsch equal-DOF result, which had the
finer control of a structured annular mesh: at an identical 648 free DOFs,
logarithmic radial grading cut SCF error from **3.06%** to **0.70%**
([docs/progress.md](progress.md)).

```sh
python3 scripts/render_showcase.py --only compare_grading
```

## Benchmark charts

All three charts are plotted directly from committed JSON/reports — the script
reads the files, it does not carry the numbers.

```sh
python3 scripts/plot_benchmarks.py
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
python3 scripts/render_showcase.py --only architecture
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
  is the solver's own `fea_colormap` (blue → cyan → green → yellow → red) so
  colours mean the same thing in the GUI, the CLI output, and these images.
  Displacement is warped by a stated factor (×200 to ×2000) so deflection is
  visible, and the colour range is clipped at a stated percentile of the visible
  surface. Both the percentile and the true unclipped peak nodal value are in
  every caption: clamped faces are stress singularities under point-fixed
  boundary conditions, so an unclipped scale would render one hot node and an
  otherwise blue part. Nothing about the underlying solution is altered — only
  the mapping from value to colour.
- **Charts** (`bench_dof_time.png`, `bench_tier1.png`, `bench_mms.png`) are
  generated by `scripts/plot_benchmarks.py`, which reads committed benchmark
  artifacts — [`bench/results/*.json`](../bench/results) and
  [`bench/reports/p1-gate1-convergence.md`](../bench/reports/p1-gate1-convergence.md).
  If a number changes in the data, the chart changes; the numbers are not
  duplicated inside the plotting script.
- **GUI screenshot** (`gui_studio.png`) is captured inside the application via
  the F12 screenshot path (`glReadPixels` on the default framebuffer, written as
  8-bit RGBA PNG), not by an external screen grabber.
- **Diagram** (`architecture.png`) is drawn programmatically in the Studio
  palette so it matches the application chrome.
- **Honesty rules.** Speed and DOF wins are measured against PolyMesh's own
  frozen uniform-tet10 baseline (ADR-0005), never against a third-party solver.
  Product volume fills are Cartesian grid-fill, not constrained Delaunay
  ([ADR-0015](decisions/0015-grid-fill-limits.md)). Tier-1 analytical numbers
  come from structured parametric meshes. The poly-VEM path is research-gated
  (node M5, not promoted). Full statement:
  [README § Limitations](../README.md#limitations).

## Reproduce

Three entry points, all pure Python 3 on top of the built CLI:

```sh
# 1. Everything: solves, renders, comparison grids, diagram, charts, manifest.
#    Idempotent — skips solves whose VTU already exists unless --force.
python3 scripts/render_showcase.py --all

# 2. Benchmark charts only, straight from committed bench data.
python3 scripts/plot_benchmarks.py

# 3. Generic labeled tiler, reusable for any set of PNGs.
python3 scripts/make_compare_grid.py --out OUT.png --title "..." \
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
