# Showcase asset index

One line per asset. Machine-readable provenance (part, mesher, `h`, DOF count,
wall time, peak von Mises, exact commands) is in
[`manifest.json`](manifest.json). Captions and methodology:
[`../../SHOWCASE.md`](../../SHOWCASE.md).

| File | What it shows | Source data / command |
|---|---|---|
| `hero.png` | Flagship render: `plate_hole` von Mises across the whole plate from a low oblique angle (h = 6 mm, 13,887 DOF, warp ×200, range clipped at p99.5) | solve VTU from `polymesh solve` (graded mesher); `python3 scripts/render_showcase.py --only hero` |
| `gallery_plate_hole.png` | Plate with central hole, von Mises + displacement; grading concentrated at the riser (h = 6 mm, 4,629 nodes / 18,887 elems, 13,887 DOF) | solve VTU, `tests/fixtures/parts/plate_hole.step`; `render_showcase.py --only plate_hole` |
| `gallery_cantilever.png` | End-loaded cantilever, linear bending stress peaking at the clamped root (h = 30 mm, 3,327 / 13,008, 9,981 DOF) | solve VTU, `tests/fixtures/parts/cantilever.step`; `render_showcase.py --only cantilever` |
| `gallery_cylinder.png` | Curved-wall solid from STEP with curvature-driven sizing (h = 12 mm, 3,719 / 17,186, 11,157 DOF) | solve VTU, `tests/fixtures/parts/cylinder.step`; `render_showcase.py --only cylinder` |
| `gallery_sphere.png` | Closed curved B-rep; stair-cased Cartesian fill with feature-graded skin (h = 8 mm, 4,775 / 23,399, 14,325 DOF) | solve VTU, `tests/fixtures/parts/sphere.step`; `render_showcase.py --only sphere` |
| `gallery_icecream_cone.png` | Smooth dome blended into a sharp cone apex — two length scales in one sizing field (h = 10 mm, 5,410 / 25,255, 16,230 DOF) | solve VTU, `tests/fixtures/parts/icecream_cone.step`; `render_showcase.py --only icecream_cone` |
| `compare_meshers.png` | Same plate at h = 6 mm through `tet` (1,884 nodes / 6,840 cells) \| `graded` (4,629 / 18,887) \| `hybrid` (21,100 / 54,720), labeled tiles | mesh-only VTUs; `render_showcase.py --only compare_meshers` |
| `compare_grading.png` | Uniform (`--no-feature`) vs feature-graded sizing at a **matched element budget** — 18,912 vs 18,944 cells (0.2% apart), 12,426 vs 13,719 DOF | mesh-only VTUs; `render_showcase.py --only compare_grading` |
| `bench_dof_time.png` | D6 L-domain: 6384 → 1248 DOF and 2.762 s → 0.227 s vs the frozen uniform-tet10 baseline at matched energy accuracy | [`bench/results/polymesh-d6-l-domain.json`](../../../bench/results/polymesh-d6-l-domain.json); `python3 scripts/plot_benchmarks.py` |
| `bench_tier1.png` | Relative error vs tolerance on the five closed-form Tier-1 cases (Lamé, Timoshenko, Kirsch, Goodier, L-domain) | [`bench/reports/p1-gate1-convergence.md`](../../../bench/reports/p1-gate1-convergence.md); `plot_benchmarks.py` |
| `bench_mms.png` | Manufactured-solution energy-norm convergence: frozen P1 elements at 0.997 / 0.997 / 2.000 / 2.000 vs theory 1/1/2/2, hierarchical p-basis at 1.02 / 1.99 / 2.98 / 3.98 vs theory 1/2/3/4 | [`docs/progress.md`](../../progress.md) + the GATE-1 convergence report; `plot_benchmarks.py` |
| `architecture.png` | Dark-theme pipeline diagram: STEP → features → sizing field → hybrid meshers → FE+VEM assembly → solve → ZZ estimate → hp-adapt loop → VTU | drawn programmatically in the Studio palette; `render_showcase.py --only architecture` |
| `gui_studio.png` | PolyMesh Studio with a solved part: viewport, study setup, results panel (stress / deflection / η) | captured in-app via F12 or `POLYMESH_GUI_SHOT=<abs path>`; see [`../../SHOWCASE.md#gui`](../../SHOWCASE.md#gui) |
| `manifest.json` | Provenance record for every image above (schema in [`../../SHOWCASE.md`](../../SHOWCASE.md)) | written by `render_showcase.py --all` |

Every stress render states its displacement warp factor, the colour range, the
percentile the range was clipped at, and the true unclipped peak nodal value —
the peak sits at a clamped face on all of these parts and is a boundary-condition
singularity, not a physical stress. Speed and DOF comparisons are against
PolyMesh's own frozen uniform-tet10 baseline, never against another solver
([README § Limitations](../../../README.md#limitations)).
