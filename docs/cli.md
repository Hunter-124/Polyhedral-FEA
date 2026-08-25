# CLI and options reference

Reference for the `polymesh` subcommands and their flags, plus the build options
and resource limits that govern them. Running `polymesh` with no arguments
prints the full help.

## Commands

`check`, `mesh`, `diag` and `render` take CAD (`.step .stp .brep .brp`); `solve`
also accepts Gmsh `.msh`. Advisor features require CAD, so `--advisor` is
rejected for `.msh`. Fixture:
[`bench/geometries/public/unit_box.step`](../bench/geometries/public/unit_box.step)
(1 m axis-aligned box).

```sh
CLI=./build/apps/cli/polymesh
BOX=bench/geometries/public/unit_box.step

# Validate CAD geometry
$CLI check $BOX

# Mesh — geometry-aware (curvature/thin-wall) grading is on by default.
# Omit -h (or -h 0) for auto h0 from bbox + feature density; -h is in metres.
$CLI mesh $BOX -o /tmp/box_mesh.vtu
$CLI mesh $BOX --mesher varyhedron -h 0.1 -o /tmp/box_vary.vtu

# Geometry + simulation-setup aware: grade toward the load box (finest) and
# the fixture box. Both take 6 numbers: x0 y0 z0 x1 y1 z1.
$CLI mesh $BOX --mesher varyhedron \
  --fix-box -1 -1 -1 0.01 2 2 --load-box 0.99 -1 -1 2 2 2 \
  -o /tmp/box_bc.vtu

# Solve — default BCs fix min-x and load +Fy on max-x; the boxes override
# that selection. Writes von Mises + displacement to VTU.
$CLI solve $BOX -o /tmp/box_result.vtu
$CLI solve $BOX -h 0.08 --mesher tet -o /tmp/box_tet.vtu

# Adaptive solve: ZZ → Dörfler remesh passes until the global indicator drops.
# η is relative (dimensionless), so --eta-target is a fraction, not a stress.
$CLI solve $BOX --mesher graded --adapt 3 --eta-target 0.05 -o /tmp/box_adapt.vtu

# Same solve, but load the +x face with a 2 MPa pressure pointing -y
$CLI solve $BOX --load-box 0.99 -1 -1 2 2 2 --load-dir 0 -1 0 --traction 2e6 \
  -o /tmp/box_pressure.vtu

# JSON diagnostics: directional fidelity measured against the exact live B-rep
# (hard-bounded reverse sampling), mesh quality, per-phase timings, and η.
$CLI diag tests/fixtures/parts/pipe.step --json /tmp/pipe.json

# Headless render: rasterize the exact same boundary tessellation the Studio
# viewport paints — no GL, no window, no Xvfb. --stats adds a numeric report
# whose normal_deviation_deg is the facet-normal angle to the exact B-rep, so
# `--no-curved` visibly and measurably degrades it.
$CLI render tests/fixtures/parts/sphere.step -h 0.02 -o /tmp/sphere.png \
  --wireframe --stats /tmp/sphere.json

# Runtime stack: e.g. "cpu | OpenMP 16 threads | Eigen serial (no nest)"
$CLI backend
```

Fixtures used above: [`tests/fixtures/parts/pipe.step`](../tests/fixtures/parts/pipe.step),
[`tests/fixtures/parts/sphere.step`](../tests/fixtures/parts/sphere.step).

## GUI

![PolyMesh Studio](assets/showcase/gui_studio.png)

```sh
./build/apps/gui/polymesh-gui
./build/apps/gui/polymesh-gui bench/geometries/public/unit_box.step
```

PolyMesh Studio opens a CAD part (path field, argv, or drag-drop), sets material
and element size (mm; 0 = the same auto h0 the CLI uses), assigns fixtures and
loads on faces, then runs **Mesh only** for a preview or **Solve** for stress,
deflection and the ZZ indicator η. The status strip reports the resolved
`auto h=…`, and VTU export lives in the results panel. F12, or *File → save
screenshot*, writes a PNG of the window to the working directory as
`polymesh_shot_<UTC>.png`; setting `POLYMESH_GUI_SHOT=/abs/path.png` writes to
that exact path instead, which is how the headless capture works. The GUI needs
a display (GLFW), so CI covers the pipeline through Catch2 rather than the
window.

## Meshers

`--mesher` takes `hybrid|zoo` (default), `varyhedron|vary` (CAD packing),
`cvt_poly|cvt` (experimental packed-poly VEM), `hybridvem`, `tet`, `hex`,
`hexvem|vem`, `graded`, `hexpyr|transition`, `prism|sweep`, or
`octa|octahedral` (experimental).

Other mesh flags: `--skin n` (graded fine skin layers, default 2),
`--no-feature` (disable curvature/thin-wall grading), `--element-tendency t`
(shape dial in [-1,+1]: hex ↔ fan hybrid ↔ poly VEM ↔ tet), `--p-elevate`
(promote smooth tet4/hex8 → tet10/hex20; auto-on with `--adapt > 0`),
`--bc-grade`, `-E` (Pa), `-nu`.

## Sizing

Spectral sizing ([ADR-0034](decisions/0034-spectral-sizing-and-coarsening.md))
is on by default in the CLI, and `--no-spectral` opts out. CAD-edge curvature is
FFT-denoised before it emits chordal size sources, and the fused size field is
energy-truncated on a Cartesian grid so spectrally insignificant fine bands
merge into the coarse field. A geometry-only floor is re-imposed after
filtering, so trimming can never blur a real feature. `mesh` and `solve` print
the kept/total mode counts and the before/after density predictions, and
`diag --json` carries a `spectral` block. With `--advisor DIR`,
`--advisor-max-dof N` drops candidate actions whose predicted DOF exceeds N;
when none fit, the advisor refuses and returns clamp-box defaults.

## Loads and boundary conditions

`solve` and `diag` take `--load-dir x y z` (direction, normalised; default
`0 1 0`), `--force N` (total resultant in newtons over the loaded faces, default
1000) and `--traction Pa` (pressure instead of a total force, so the resultant
is Pa × loaded-face area). The last of `--force` / `--traction` wins. Either way
the load is applied as a consistent traction ∫Nᵀt dS over the selected boundary
faces, never as lumped point forces, and the run prints the resulting nodal-load
sum next to the requested resultant as a conservation check. `diag` accepts
`--fix-box` and `--load-box` too, so a diagnostics run can reproduce the exact
boundary conditions of a solve.

`--fix-box` and `--load-box` name a region of the **boundary surface**, and they
select the boundary nodes and faces inside it — never interior nodes. Constraining
the interior would embed a rigid inclusion: an element whose nodes all fall inside
a fixture box has identically zero strain, so its stress is identically zero, and
the union of such elements ends on a one-element staircase set by the tiling
rather than by the problem. That was the shipped behaviour before ADR-0038, and
it froze 30.7% of the showcase cylinder's elements solid
([ADR-0038](decisions/0038-a-fixture-is-applied-to-the-boundary.md)). A load
region goes one step further and is integrated at the box plane rather than over
whole faces, so the applied traction does not depend on which element edges happen
to fall inside
([ADR-0037](decisions/0037-a-box-selection-is-a-region.md)).

## Rendering

`render` flags: `--subdiv N` (tessellation subdivisions per quadratic boundary
face, default 8, the value the Studio viewport uses), `--size WxH` (default
`1200x900`), `--azimuth DEG` and `--elevation DEG` (orbit camera, defaults 35 /
25; the projection is orthographic, so a view is reproducible from those two
numbers), `--wireframe` (overlay the tessellation triangle edges), and
`--stats out.json`. The stats report carries node and element counts, the
element-type census, triangle count, covered and silhouette pixel counts, and
`normal_deviation_deg` — the mean/p99/max angle between each rendered facet
normal and the exact B-rep normal at its centroid, labelled with the
`normal_reference` actually used. On
[`tests/fixtures/parts/sphere.step`](../tests/fixtures/parts/sphere.step) at
`-h 0.02` the curved default measures p99 0.34°; the same run with `--no-curved`
measures 2.72°, which is the chordal error the curved geometry removes.

## Resource limits

All subcommands take `--max-mem <GB>` to cap the estimated solve footprint, and
`--max-elems N` / `--max-dof N` to cap mesh size (`0` = auto on all three).
These are enforced, not advisory. A solve estimates its footprint — CSR nnz from
the real connectivity, plus the LDLT factor fill-in or the CG working set — and
refuses with the estimate, the cap and the limiting term when it would exceed
`min(--max-mem, 70% of available system memory)`; under `kAuto` a solve that
fits CG but not LDLT is downgraded rather than failed. Meshing predicts its
element count first and caps it at 589,824 elements / 1,769,472 DOF by default:
with an explicit `-h` it refuses up front, while auto sizing clamps h upward
(reported in the mesh note as `auto h clamped from … (element ceiling …)`) and
coarsens-and-retries rather than failing. Adapt passes stop when the next pass
would breach the ceiling. Mesh and CG loops poll cancellation every iteration,
so **Cancel** returns in milliseconds instead of at the next phase boundary. See
[`src/fea/include/fea/resource_budget.hpp`](../src/fea/include/fea/resource_budget.hpp).

## Build options

```sh
cmake -B build -DPOLYMESH_WITH_OCC=ON      # STEP/B-rep (OpenCASCADE), default ON
cmake -B build -DPOLYMESH_WITH_CUDA=ON     # GPU backends, default OFF
cmake -B build -DPOLYMESH_WITH_OPENMP=OFF  # force serial assembly
cmake -B build -DPOLYMESH_WITH_GUI=OFF     # libs + CLI + tests only
cmake -B build -DPOLYMESH_WITH_ADVISOR=OFF # skip the ONNX inference module
cmake -B build -DPOLYMESH_WITH_GEOGRAM=OFF # no clipped-cell (restricted CVT) kernel
cmake -B build -DPOLYMESH_BUILD_TESTS=OFF  # skip Catch2 and ctest registration
```

OpenMP (default ON) parallelises element-stiffness formation, mesh inside-tests,
ZZ recovery, stress recovery and CSR SpMV, using thread-local triplets merged
outside the hot loop. Results match the serial path within patch-test
tolerances, and Eigen dense kernels stay single-threaded to avoid nested-OpenMP
hangs. Missing OpenMP falls back to serial automatically.

CUDA is OFF by default. `fea::spmv_cpu` and `csr_from_eigen` always build; the
CUDA SpMV in [`backend_cuda.cu`](../src/fea/src/backend_cuda.cu) runs only with
a device present and is parity-tested against the CPU path. `polymesh backend`
reports `cpu` or `cuda (<device>)`. Batched element-stiffness GPU kernels are
not wired yet. If host GCC outruns nvcc, add
`-DCMAKE_CUDA_FLAGS="-allow-unsupported-compiler"`. If CMake cannot find OCCT,
point it at the prefix holding `OpenCASCADEConfig.cmake` with
`-DOpenCASCADE_DIR=/path/to/cmake/OpenCASCADE`; see
[`src/geom/CMakeLists.txt`](../src/geom/CMakeLists.txt).

## Solver internals

For the linear solve, `fea::solve_elastostatics` partitions Dirichlet DOFs, then
uses `SimplicialLDLT` up to 50000 free DOFs and incomplete-Cholesky-
preconditioned `ConjugateGradient` above that (`SolveMethod::kAuto`), with a
bounded iteration cap so a non-converging system fails instead of grinding. The
choice depends only on free-DOF count, never on element type. Patch tests and
verification meshes stay on the direct path so constant-strain exactness is
preserved. See [`src/fea/include/fea/solve.hpp`](../src/fea/include/fea/solve.hpp).

## Tests and benchmarks

`ctest --test-dir build --output-on-failure --parallel 2` runs the Catch2 suite
in [`tests/`](../tests/), which covers patch tests, the Tier-1 analytical cases,
mesher fidelity and quality contracts, and the advisor's C++/Python parity.

Labeled time and accuracy snapshots live in
[`bench/results/`](../bench/results/) (schema:
[`bench/competitive/schema.json`](../bench/competitive/schema.json)); the
generated table is [the benchmark scoreboard](bench/scoreboard.md). The
harness design and its anti-cheat rules are in
[docs/benchmarks.md](benchmarks.md).

```sh
python3 bench/competitive/render_scoreboard.py   # refresh scoreboard
./bench/competitive/run_polymesh_smoke.sh        # Tier-0/1 ctest smoke
python3 bench/d6/run_tier3.py --full --render    # D6 uniform tet10 vs graded
python scripts/render_showcase.py --all          # regenerate showcase assets
```
