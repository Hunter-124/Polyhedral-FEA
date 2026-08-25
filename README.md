# PolyMesh

An adaptive hybrid polyhedral mesher and the linear-elastostatics solver it was
co-designed with. C++20, OpenCASCADE for CAD, Eigen for the algebra.

<p align="center">
  <a href="docs/assets/cinema/advisor_cinema.mp4"><img
    src="docs/assets/cinema/advisor_cinema.gif"
    alt="Curvature read off a wishbone B-rep, advisor activations, cells landing, and the solved stress field"
    width="100%"></a>
</p>

Sixty seconds on a suspension wishbone: curvature read off the exact B-rep,
the advisor scoring its candidate meshes, cells landing in emission order, then
stress, recovered gradient and the ZZ error field from the solve that mesh
produced. The inline GIF and the [1080p60 MP4](docs/assets/cinema/advisor_cinema.mp4)
carry the same frames.

None of it is mocked. The network is the deployed ONNX graph, the cells are
captured `pipeline::MeshStage` snapshots, and the fields are that run's
`SolveStage` result: 40,170 tet4 cells, 29,388 unknowns, a 4.717 kN distributed
load, 202.49 MPa peak stress, 1.198 mm peak displacement, and a 29.77% global ZZ
indicator that the film states instead of laundering into a confidence claim.
Framing, pacing and the deformation scale are the only cosmetics, itemised in
[the cinema notes](docs/assets/cinema/NOTES.md).

## The idea

Most FEA toolchains split meshing from solving. The mesher emits elements, the
solver takes what it gets, and neither one gets to tell the other what it needs.
PolyMesh closes that loop: classify the geometry by criticality, pick element
shape, size and polynomial order per region, solve, estimate the error, refine,
and iterate toward a target accuracy at minimum cost. Because the solver
consumes general polyhedra, the mesher never has to shatter an awkward cell into
slivers just to keep the solver happy.

```mermaid
flowchart TD
    CAD["STEP / B-rep import<br/>(OpenCASCADE)"] --> FEAT["feature analysis<br/>curvature, thin wall, FFT edge denoise"]
    FEAT --> SIZE["spectral-trimmed sizing field<br/>h(x) from geometry + BC boxes"]
    FEAT --> ADV["learned mesh advisor<br/>mesher / h / adapt / order, DOF-budget gated"]
    SIZE --> MESH["hybrid meshers<br/>tet · hex · prism · pyramid · polyhedron"]
    MESH --> ASM["FE + VEM assembly<br/>one global K, minimum rule"]
    ASM --> SOLVE["linear solve<br/>SimplicialLDLT / equilibrated CG"]
    SOLVE --> ZZ["Zienkiewicz–Zhu<br/>recovery + error estimate"]
    ZZ --> ADAPT{"η ≤ target?"}
    ADAPT -- "no" --> HP["hp-adapt driver<br/>refine / coarsen / p-elevate per element"]
    HP --> SIZE
    ADAPT -- "yes" --> OUT["VTU export<br/>von Mises + displacement"]
```

## Highlights

**One system for FE and VEM.** Standard isoparametric elements
(tet4/tet10/hex8/hex20, prism, pyramid) and the Virtual Element Method for
arbitrary polyhedra scatter into the same `assemble_stiffness`. There is no
second solve and no mortar coupling, and the constant-strain patch test `u = Gx`
is exact to 1e-9 m across FE/VEM interfaces
([solver-core](docs/solver-core.md#3-shape-fe-fast-paths--vem-for-everything-else),
[test](tests/test_fe_vem_assembly.cpp)).

**Order is hierarchical, p = 1..4, with a minimum rule on conformity.** A shared
face or edge carries the lowest order of the elements touching it, so a p=1 cell
sits next to a p=3 cell without transition machinery. Manufactured-solution
energy-norm convergence measures 1.02 / 1.99 / 2.98 / 3.98 against theory
1/2/3/4 ([report](bench/reports/p1-gate1-convergence.md)).

**Adaptivity is joint in (h, p, shape).** The driver scores a geometry utility,
an error utility and a cost utility per element — benefit per relative DOF — and
takes the winner, breaking ties h > p > shape. Curved and singular regions get
smaller cells, smooth regions get higher order, awkward regions get a different
element shape ([hp_driver.hpp](src/adapt/include/adapt/hp_driver.hpp),
[test](tests/test_hp_driver.cpp)).

**Sizing fields are FFT-filtered before the mesher sees them.** CAD-edge
curvature is denoised by energy-truncated inverse FFT before it emits chordal
sources, spectrally insignificant fine bands merge into the coarse field, and a
geometry-only floor is re-imposed afterwards so a real feature is never blurred.
The same work lets the loop run backwards: an anti-Dörfler insignificant tail
plus a size-versus-demand gate coarsens a-posteriori over-refinement instead of
only ever refining
([ADR-0034](docs/decisions/0034-spectral-sizing-and-coarsening.md),
[spectral_sizing.hpp](src/adapt/include/adapt/spectral_sizing.hpp),
[test](tests/test_spectral_sizing.cpp)).

**A 2:1 coarse/fine interface is one polyhedral VEM cell** whose faces match its
neighbours exactly: a single quad against a bulk hex, four child quads against a
2×2×2 refinement, n-gons with hanging mid-nodes. No centroid apex, no fan of
near-degenerate tets, no element-count blow-up
([ADR-0012](docs/decisions/0012-hybrid-graded-tet.md),
[ADR-0019](docs/decisions/0019-mixed-fe-vem-adaptive-order-core.md)).

**Boundary nodes sit on the exact B-rep rather than near it.** A
mesher-independent gate moves a node only as far as keeps every incident cell
integrable under `fea::element_jacobians_positive`
([ADR-0035](docs/decisions/0035-boundary-conformity.md)). A symmetric part also
gets a symmetric tiling — the Kuhn diagonal rotates per cell and geometry
queries are answered in one octant and mirrored back, so
[test_graded_fill.cpp](tests/test_graded_fill.cpp) asserts a mirrored-tet
fraction of exactly 1.0 rather than a floor
([ADR-0036](docs/decisions/0036-a-symmetric-part-gets-a-symmetric-tiling.md)).

## Gallery

![PolyMesh stress render](docs/assets/showcase/hero.png)

**hero** — `plate_hole` on the feature-graded mesher at h = 6 mm: 52,080 curved
cells, 233,820 DOF, min-x face fixed and a conserved +x resultant on max-x.

| | |
|---|---|
| ![Plate with hole](docs/assets/showcase/gallery_plate_hole.png) <br> **plate_hole** — graded mesher at h = 6 mm; von Mises around the stress riser. | ![Cylinder](docs/assets/showcase/gallery_cylinder.png) <br> **cylinder** — graded mesher at h = 12 mm on the curved STEP wall. |
| ![Sphere](docs/assets/showcase/gallery_sphere.png) <br> **sphere** — graded mesher at h = 8 mm on a closed curved B-rep. | ![Ice-cream cone](docs/assets/showcase/gallery_icecream_cone.png) <br> **icecream_cone** — graded mesher at h = 10 mm on the fused cone and spherical scoop. |
| ![Mesher comparison](docs/assets/showcase/compare_meshers.png) <br> **compare_meshers** — h = 6 mm: `tet`, `graded`, and `hybrid` (hex bulk + transition cells). | ![DOF/time benchmark](docs/assets/showcase/bench_dof_time.png) <br> **bench_dof_time** — the D6 L-domain result: 6384 → 1248 DOF, 2.762 s → 0.227 s. |

Every render is real solver VTU output. Displacement is warped for visibility
and drawn against a grey outline of the undeformed shape, because most of these
cases deform along their own long axis and the warp is unreadable without the
reference. Colour is clipped at a stated percentile: a clamped face is a
boundary-condition singularity, and its peak nodal value is not a physical
stress. Element size, DOF count, warp factor, colour range, clipping percentile
and true peak are recorded per image in
[manifest.json](docs/assets/showcase/manifest.json).

Full gallery and reproduce commands: [docs/SHOWCASE.md](docs/SHOWCASE.md).

## Measured results

Every number below comes from a committed artifact in this repository.

### Analytical verification

| Case | Metric | Result | Tolerance |
|---|---|---|---|
| Lamé thick cylinder (hex20 sector) | radial displacement, inner wall | 0.0068% error | ≤ 1% |
| Lamé thick cylinder | hoop stress, inner wall | 1.36% error | ≤ 4% |
| Kirsch plate with hole (exact-field BC) | stress concentration factor | 3.056 vs 3.0 (1.87%) | ≤ 5% |
| Timoshenko cantilever (hex20, gravity) | tip deflection | 1.50% error | ≤ 3% |
| Goodier spherical cavity (b/a = 15) | SCF at cavity equator | 1.902 vs 2.045 (7.04%) | ≤ 12% |
| L-domain re-entrant corner | energy-gap convergence order | 1.265 vs theory 2λ = 1.089 | ±0.35 |

Sources: [convergence report](bench/reports/p1-gate1-convergence.md),
[docs/ROADMAP.md](docs/ROADMAP.md). Setup rationale:
[ADR-0009](docs/decisions/0009-tier1-verification-setups.md).

### What adaptivity buys

| Experiment | Baseline | PolyMesh | Delta |
|---|---|---|---|
| L-domain singularity, geometry-graded vs uniform tet10 (the graded mesh's energy deficit is 1.04× the baseline's: 0.0888% against 0.0854%) | 6384 DOF, 2.762 s | 1248 DOF, 0.227 s | 5.12× fewer DOF, 12.2× lower wall time |
| Kirsch SCF error at identical 648 free DOF, feature-aware logarithmic radial grading vs linear | 3.06% error | 0.70% error | 4.4× tighter at zero DOF cost |
| Hybrid meshing wall time on a 28,656-element mesh, after replacing brute-force closest-point search with a uniform spatial index | 25.5 s | 5.1 s | ~5× faster, results unchanged |

Sources: [D6 L-domain](bench/results/polymesh-d6-l-domain.json),
[docs/progress.md](docs/progress.md).

### Against other tools

![External mesh-source comparison](docs/advisor/figures/external_comparison.png)

Two scoped comparisons, and they isolate different components. Swapping only the
mesh source, our meshers win all four Gmsh case families on order-1 median
accuracy — and lose two of them once you charge for the degrees of freedom that
bought it. Swapping only the solver, CalculiX 2.23 and PolyMesh agree in tip
deflection to better than 2e-5% at every rung on identical hex8 cantilever
meshes. Neither is an end-to-end matched-CAD comparison of both mesher and
solver, and Elmer and Code_Aster remain unmeasured.

The full matrix, the refusal and failure counts, the DOF-charged flip, Gmsh's
own run-to-run noise, and the stress-recovery defect this comparison exposed on
our side are in [docs/comparisons.md](docs/comparisons.md).

## The learned mesh advisor

A compact multi-head MLP maps geometry and BC features plus a candidate mesh
action to accuracy, B-rep fidelity, cost and failure risk
([ADR-0027](docs/decisions/0027-learned-mesh-advisor.md)). The shipped decision
rule is gated enumeration: score all 38 measured candidates, drop the ones the
feasibility head expects to fail, take the argmin of predicted per-case
accuracy. Hard runtime vetoes still run afterwards. The gate improves a choice;
the veto refuses one. `--advisor-max-dof N` adds a budget to that gate, and a
cap that empties the candidate set returns clamp-box defaults with every
prediction suppressed rather than an action the caller cannot afford.

![Advisor mesh choices before and after](docs/advisor/figures/mesh_before_after.png)

Evaluation is leave-one-family-out over 8 geometry families with 5 seeds, under
a DOF-primary budget, with failing actions offered and charged. Regret is log10
distance from the best feasible measured action. At q0.5 the shipped chooser
scores 0.3338 regret at a 27.5% pick-failure rate, ahead of the shipped default
(0.3796), a random feasible action (0.4076) and "just mesh finer" (0.4409); the
operating point was picked to avoid doomed choices rather than to win a median
that sits inside the ±0.24 fold spread. The deployed model is 16,177 parameters,
exports at ONNX opset 17 with 2.158e-06 relative C++ parity, and costs about
1.0 ms per recommendation.

The advisor also refuses parts it does not recognise. A Mahalanobis distance
over 31 part-geometry columns is tested against the shipped operating point of
5.034, the training 99th percentile. Swept live over all 44 corpus primitives,
it refuses all 20 parts from the five families absent from training at distances
of 11.36 to 80.19, and advises all 12 trained geometries. The refusal is
enforced in C++, not merely measured: it falls back to defaults and suppresses
every `predicted_*` value to NaN. The case that motivated the gate had been
reporting a predicted mesh time of about 5,300 years beside a failure
probability of 1e-65.

The known weak point is the feasibility head the gate is built on: its AUC on
the shipped checkpoint's own fold is 0.5248, near chance against a cross-fold
mean of 0.806. Full metrics, per-head accuracy, calibration and the leakage
correction that made the older numbers look better are in the
[model card](docs/advisor/0004-model-card.md) and
[data card](docs/advisor/0005-data-card.md).

## Limits

- Speedups are self-relative. "5.12× fewer DOF, 12.2× lower wall time" is
  against PolyMesh's own frozen uniform-tet10 baseline
  ([ADR-0005](docs/decisions/0005-benchmark-baseline.md)), not against another
  solver.
- The default coarse product mesher can miss stress concentrations. At matched
  order 1 on `box_hole_s0_c0` it recorded 0.664 relative error against 0.190 for
  PolyMesh graded tet ([details](docs/comparisons.md)).
- Product volume fills are Cartesian grid-fill, not constrained Delaunay. The
  committed boundary guard rail is `dist_max ≤ 0.25 h` / `dist_p99 ≤ 0.10 h`
  (measured maximum 0.059 h) over seven fixtures, which bounds known behaviour
  without making grid-fill constrained Delaunay
  ([ADR-0015](docs/decisions/0015-grid-fill-limits.md),
  [ADR-0028](docs/decisions/0028-boundary-conformance-hardening.md)).
- The iterative solver has a demonstrated order-2 scaling limit. Two ~200k-DOF
  truth runs hit CG's 20,000-iteration cap at tolerance 1e-8 with relative
  residual ~5e-4; six more finished between 1e-6 and 2e-5 and were flagged
  rather than promoted.
- Analytical accuracy was measured on structured parametric meshes built to
  [ADR-0009](docs/decisions/0009-tier1-verification-setups.md), not on the
  product grid-fill meshes. Tier-1 accuracy on product meshes is not claimed.
- The poly-VEM product path is research-gated. The mixed FE+VEM assembler and
  native-poly transitions are implemented and tested, but VEM is not promoted to
  the default path until it beats `hybrid_zoo` on the frozen references. Tet FE
  remains the default accuracy claim.

## Build

Ubuntu or Debian, matching CI (`.github/workflows/ci.yml`):

```sh
sudo apt-get install -y --no-install-recommends \
  ninja-build cmake g++ libeigen3-dev nlohmann-json3-dev \
  libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libxext-dev

# STEP / B-rep input (POLYMESH_WITH_OCC, ON by default)
sudo apt-get install -y libocct-data-exchange-dev libocct-foundation-dev \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev \
  libocct-ocaf-dev libocct-visualization-dev
```

Fedora: `sudo dnf install opencascade-devel`. You need a C++20 compiler
(GCC 12+ or Clang 16+) and CMake ≥ 3.24; Catch2, GLFW, ImGui and the advisor's
prebuilt ONNX Runtime are fetched by CMake. CUDA is optional and OFF by default.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPOLYMESH_WITH_GUI=ON
cmake --build build -j
./build/apps/cli/polymesh backend   # confirm the runtime stack
ctest --test-dir build --output-on-failure --parallel 2
```

There is deliberately no `-ffast-math`, no `-Ofast` and no reduced precision:
patch tests and Tier-1 verification stay double-exact. The speed levers are
`-O3` and OpenMP. Host ISA flags (`POLYMESH_NATIVE_ARCH`) have caused Eigen heap
corruption on this toolchain and LTO has hit Eigen ODR problems, so both default
OFF — leave them off unless you re-verify with `ctest`.

## Use

`check`, `mesh`, `diag` and `render` take CAD (`.step .stp .brep .brp`); `solve`
also accepts Gmsh `.msh`.

```sh
CLI=./build/apps/cli/polymesh
BOX=bench/geometries/public/unit_box.step

$CLI check $BOX                                    # validate the CAD
$CLI mesh  $BOX -o /tmp/box.vtu                    # auto h0 from bbox + features
$CLI solve $BOX -o /tmp/box_result.vtu             # fix min-x, load +Fy on max-x
$CLI solve $BOX --mesher graded --adapt 3 --eta-target 0.05 -o /tmp/adapt.vtu
$CLI diag  tests/fixtures/parts/pipe.step --json /tmp/pipe.json
$CLI render tests/fixtures/parts/sphere.step -h 0.02 -o /tmp/sphere.png --wireframe
```

Run `$CLI` with no arguments for the full help. Meshers, sizing, boundary
conditions, resource limits and build options are documented in
[docs/cli.md](docs/cli.md).

![PolyMesh Studio](docs/assets/showcase/gui_studio.png)

`./build/apps/gui/polymesh-gui [part.step]` opens Studio: pick a part, set
material and element size, assign fixtures and loads on faces, then **Mesh only**
for a preview or **Solve** for stress, deflection and the ZZ indicator η. The
GUI needs a display, so CI covers the pipeline through Catch2 rather than the
window.

## Layout

| Path | Role |
|---|---|
| `apps/cli`, `apps/gui` | Executables only (the GUI is presentation) |
| `apps/bench`, `apps/testlab` | Benchmark driver and the `polymesh_testlab` harness |
| `src/geom` `mesh` `adapt` `fea` | Core libraries |
| `src/advisor` | Learned mesh advisor inference (ONNX Runtime) |
| `src/pipeline` | Headless import → mesh → solve (no OpenGL) |
| `src/bench` | Reference JSON loader (anti-cheat boundary) |
| `tests/` | Catch2 suite |
| `bench/` | Reference cases, reports, peer harness |
| `scripts/` | Fixture generation, diagnostics, showcase rendering |
| `examples/` | Runnable mesh/solve scripts on the public fixtures |
| `docs/` | Spec, ADRs, progress, showcase |

The benchmark harness is adversarial on purpose: holdout geometries are
git-ignored so the implementation loop never sees them, random rigid transforms
catch coordinate hacks, a grep audit flags numeric literals near reference
values, and ZZ effectivity is bounded to [0.5, 2] so the loop cannot win by
making the estimator lie ([docs/benchmarks.md](docs/benchmarks.md)).

Every non-obvious decision has an ADR under
[docs/decisions/](docs/decisions/), written after the measurement rather than
before it. [docs/solver-core.md](docs/solver-core.md) is the design narrative and
[docs/progress.md](docs/progress.md) is the running log. Coding standards and the
contribution flow are in [CONTRIBUTING.md](CONTRIBUTING.md) and
[CHANGES.md](CHANGES.md).

## License

[BSD-3-Clause](LICENSE).
