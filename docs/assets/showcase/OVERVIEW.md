# Showcase asset index

One line per asset. Machine-readable provenance (part, mesher, `h`, DOF count,
wall time, peak von Mises, exact commands) is in
[`manifest.json`](manifest.json). Captions and methodology:
[`../../SHOWCASE.md`](../../SHOWCASE.md).

| File | What it shows | Source data / command |
|---|---|---|
| `hero.png` | Flagship render: `plate_hole` von Mises across the whole plate from a low oblique angle (h = 6 mm, 233,820 DOF, warp ×5000, full-field range) | solve VTU from `polymesh solve` (graded mesher); `python scripts/render_showcase.py --only hero` |
| `gallery_plate_hole.png` | Plate with central hole, von Mises + displacement; grading concentrated at the riser (h = 6 mm, 233,820 DOF) | solve VTU, `tests/fixtures/parts/plate_hole.step`; `render_showcase.py --only plate_hole` |
| `gallery_cantilever.png` | End-loaded cantilever, linear bending stress peaking at the clamped root (h = 30 mm, 193,971 DOF) | solve VTU, `tests/fixtures/parts/cantilever.step`; `render_showcase.py --only cantilever` |
| `gallery_cylinder.png` | Curved-wall solid from STEP with curvature-driven sizing (h = 12 mm, 688,047 DOF) | solve VTU, `tests/fixtures/parts/cylinder.step`; `render_showcase.py --only cylinder` |
| `gallery_sphere.png` | Closed curved B-rep; every boundary node on the exact BRep (ADR-0035), feature-graded skin (h = 8 mm, 521,175 DOF) | solve VTU, `tests/fixtures/parts/sphere.step`; `render_showcase.py --only sphere` |
| `gallery_icecream_cone.png` | Watertight fused round cone + spherical scoop, solved from STEP (h = 10 mm, 383,295 DOF) | solve VTU, `tests/fixtures/parts/icecream_cone.step`; `render_showcase.py --only icecream_cone` |
| `compare_meshers.png` | Same plate at h = 6 mm through `tet` (37,800 DOF) \| `graded` (233,820) \| `hybrid` (134,748), labeled tiles | mesh-only VTUs; `render_showcase.py --only compare_meshers` |
| `compare_grading.png` | Uniform (`--no-feature`, h = 4.2 mm) vs feature-graded (h = 5.6 mm) sizing at a **matched element budget** — 190,032 vs 205,128 DOF (42,960 vs 45,128 cells, 5.0% apart) | mesh-only VTUs; `render_showcase.py --only compare_grading` |
| `bench_dof_time.png` | D6 L-domain: 6384 → 1248 DOF and 2.762 s → 0.227 s vs the frozen uniform-tet10 baseline at matched energy accuracy | [`bench/results/polymesh-d6-l-domain.json`](../../../bench/results/polymesh-d6-l-domain.json); `python scripts/plot_benchmarks.py` |
| `bench_tier1.png` | Relative error vs tolerance on the five closed-form Tier-1 cases (Lamé, Timoshenko, Kirsch, Goodier, L-domain) | [`bench/reports/p1-gate1-convergence.md`](../../../bench/reports/p1-gate1-convergence.md); `plot_benchmarks.py` |
| `bench_mms.png` | Manufactured-solution energy-norm convergence: frozen P1 elements at 0.997 / 0.997 / 2.000 / 2.000 vs theory 1/1/2/2, hierarchical p-basis at 1.02 / 1.99 / 2.98 / 3.98 vs theory 1/2/3/4 | [`docs/progress.md`](../../progress.md) + the GATE-1 convergence report; `plot_benchmarks.py` |
| `architecture.png` | Dark-theme pipeline diagram: STEP → features → sizing field → hybrid meshers → FE+VEM assembly → solve → ZZ estimate → hp-adapt loop → VTU | drawn programmatically in the Studio palette; `render_showcase.py --only architecture` |
| `gui_studio.png` | PolyMesh Studio with a solved part: viewport, study setup, results panel (stress / deflection / η) | captured in-app via F12 or `POLYMESH_GUI_SHOT=<abs path>`; see [`../../SHOWCASE.md#gui`](../../SHOWCASE.md#gui) |
| `bench_advisor_budget.png` | Advisor DOF-budget sweep: predicted DOF and chosen action across 21 `--advisor-max-dof` caps on the v6 model | [`bench/results/advisor-budget-sweep.json`](../../../bench/results/advisor-budget-sweep.json); `plot_benchmarks.py --only advisor_budget` |
| `manifest.json` | Provenance record for every image above (schema in [`../../SHOWCASE.md`](../../SHOWCASE.md)) | written by `render_showcase.py --all` |

Every stress render states its displacement warp factor, the colour range, the
percentile the range was clipped at, and the true unclipped peak nodal value
**with its node coordinates** — on the plate the peak is the hole-rim
concentration the part exists to show; on the box-selected parts it sits where
the stated BC says it should (a clamped face, or the rim where the applied
traction starts). Speed and DOF comparisons are against
PolyMesh's own frozen uniform-tet10 baseline, never against another solver
([README § Limitations](../../../README.md#limitations)).
