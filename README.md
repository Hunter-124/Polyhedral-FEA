# PolyMesh

**Adaptive hybrid polyhedral mesh generator co-designed with an FEA solver.**

Most FEA workflows treat meshing and solving as separate concerns. PolyMesh
classifies geometry by criticality and chooses element **shape, size, and
polynomial order** per region, then iterates with solver feedback toward a
target accuracy at minimum cost. The mesher and solver are optimized for each
other — not glued together after the fact.

## What it does

- **Hybrid element zoo** in one conforming mesh: tets, hexes, prisms, pyramids,
  general polyhedra (VEM).
- **Geometry- and solution-driven adaptivity** (sizing, hp, local remesh).
- **Linear elastostatics** (3D), STL (STEP via OpenCASCADE option), VTU export.
- **Optional CUDA** for kernels where f64 parallelism wins; CPU path always exists.
- **Verified**: patch tests, manufactured-solution convergence orders, analytical
  Tier-1 cases — see [docs/benchmarks.md](docs/benchmarks.md).

## Status

P1 solver baseline frozen. Working product path: tet/hex/hex-VEM/graded-tet mesh from STL/STEP, GUI
study (fixtures/loads/solve/export), CLI mesh/solve → VTU, ZZ indicators,
feature-edge sizing hooks. Hybrid VEM/adapt loop still expanding. Tracking:
[docs/progress.md](docs/progress.md), [docs/phases.md](docs/phases.md),
[docs/bench/scoreboard.md](docs/bench/scoreboard.md).

## Quickstart (Ubuntu)

About ten minutes from clone to a VTU on the public unit box.

### Dependencies

Match CI (`.github/workflows/ci.yml`). On Ubuntu / Debian:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  ninja-build \
  cmake \
  g++ \
  libeigen3-dev \
  nlohmann-json3-dev \
  libgl1-mesa-dev \
  libx11-dev \
  libxrandr-dev \
  libxinerama-dev \
  libxcursor-dev \
  libxi-dev \
  libxext-dev
```

Optional:

- **OpenCASCADE** (`POLYMESH_WITH_OCC=ON`) for STEP/B-rep — see [STEP / OpenCASCADE](#step--opencascade-polymesh_with_occ) below.
- **CUDA** (`POLYMESH_WITH_CUDA=ON`) for GPU backends — requires a toolkit; CPU path always builds.
- **clang-format 18** for style checks: `pip install 'clang-format==18.1.8'` (or use the version on `PATH`).

C++20 compiler required (GCC 12+ or Clang 16+ recommended). CMake ≥ 3.24. Catch2, GLFW, and ImGui are fetched by CMake.

### Clone, configure, build, test

```sh
git clone <this-repo-url> polymesh && cd polymesh

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OCC=OFF \
  -DPOLYMESH_WITH_CUDA=OFF

cmake --build build -j
ctest --test-dir build --output-on-failure --parallel 2
```

Debug CI-style configure:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPOLYMESH_WITH_GUI=ON
```

### CLI examples (public unit box)

Fixture: [`bench/geometries/public/unit_box.stl`](bench/geometries/public/unit_box.stl)
(1 m axis-aligned box).

```sh
# Validate surface
./build/apps/cli/polymesh check bench/geometries/public/unit_box.stl

# Mesh — omit -h for auto h0 (bbox + sharp-edge density); or pass -h in metres
./build/apps/cli/polymesh mesh bench/geometries/public/unit_box.stl \
  -o /tmp/unit_box_mesh.vtu
./build/apps/cli/polymesh mesh bench/geometries/public/unit_box.stl \
  -h 0.1 -o /tmp/unit_box_mesh.vtu

# Solve — fix min-x face, +Fy on max-x; writes VTU with von Mises + displacement
./build/apps/cli/polymesh solve bench/geometries/public/unit_box.stl \
  -o /tmp/unit_box_result.vtu
./build/apps/cli/polymesh solve bench/geometries/public/unit_box.stl \
  -h 0.08 -o /tmp/unit_box_result.vtu --mesher tet
```

Useful flags: `--mesher tet|hex|hexvem|graded|hexpyr`, `--feature`, `--adapt n`,
`--eta-target η`, `-E`, `-nu`. Run `./build/apps/cli/polymesh` with no args for full help.

### GUI

```sh
./build/apps/gui/polymesh-gui
./build/apps/gui/polymesh-gui bench/geometries/public/unit_box.stl
```

Open an STL/STEP (path field, argv, or drag-drop). Set material, **element size
(mm, 0=auto)** — zero uses the same auto h0 as the CLI. Assign fixtures/loads on
faces, **Mesh only** for a preview, **Solve** for stress/deflection/ZZ η.
Mesh note / status shows resolved `auto h=…` when size is automatic. Export VTU
from the results panel. Needs a display (GLFW); headless CI covers the pipeline
via Catch2, not the window.

## Building (options)

```sh
cmake -B build -DPOLYMESH_WITH_OCC=ON    # STEP/B-rep (OpenCASCADE)
cmake -B build -DPOLYMESH_WITH_CUDA=ON   # GPU backends
cmake -B build -DPOLYMESH_WITH_GUI=OFF   # libs + CLI + tests only
```

### STEP / OpenCASCADE (`POLYMESH_WITH_OCC`)

Default builds are **STL-only**. STEP/B-rep import is optional (ADR-0001):

```sh
# Ubuntu / Debian (package names vary slightly by release; 7.6+ typical)
sudo apt install libocct-data-exchange-dev libocct-foundation-dev \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev \
  libocct-ocaf-dev libocct-visualization-dev

cmake -S . -B build -G Ninja -DPOLYMESH_WITH_OCC=ON
cmake --build build
# STEP tests run when OCC is linked; with OCC off they SKIP.
```

If CMake cannot find OCCT: `-DOpenCASCADE_DIR=/path/to/cmake/OpenCASCADE` (or the
prefix that contains `OpenCASCADEConfig.cmake`). See `src/geom/CMakeLists.txt`.

### Mesh path caveat

Product volume fills are **Cartesian grid** tet/hex/graded/hex+pyramid over the
bbox (stair-cased boundary, limited surface snap) — **not** constrained Delaunay
or advancing-front. Validity and determinism are guaranteed; analytical Tier-1
accuracy on product meshes is not claimed yet ([ADR-0015](docs/decisions/0015-grid-fill-limits.md)).

## Layout (short)

| Path | Role |
|---|---|
| `apps/cli`, `apps/gui` | Executables only (GUI is presentation) |
| `src/geom` `mesh` `adapt` `fea` | Core libraries |
| `src/pipeline` | Headless import → mesh → solve (no OpenGL) |
| `src/bench` | Reference JSON loader (anti-cheat boundary) |
| `tests/` | Catch2 suite |
| `bench/` | Reference cases, reports, peer harness |
| `docs/` | Spec, phases, ADRs, progress |
| `graphify-out/` | Committed knowledge graph for agents |

**Full map and coding standards:** [CONTRIBUTING.md](CONTRIBUTING.md)  
**External contributors (for their AI agents — clone/branch/PR):** [CHANGES.md](CHANGES.md)

## Benchmark scoreboard

Labeled time/accuracy snapshots live in [`bench/results/`](bench/results/)
(schema: [`bench/competitive/schema.json`](bench/competitive/schema.json)).
Generated table + sparklines:

**[docs/bench/scoreboard.md](docs/bench/scoreboard.md)**

```sh
python3 bench/competitive/render_scoreboard.py   # refresh scoreboard
./bench/competitive/run_polymesh_smoke.sh        # Tier-0/1 ctest smoke
```

Peer priority (headless): **CalculiX first**, then Elmer, then Code_Aster —
see [bench/competitive/README.md](bench/competitive/README.md). Internal
verification detail: [docs/progress.md](docs/progress.md).

## License

[BSD-3-Clause](LICENSE).
