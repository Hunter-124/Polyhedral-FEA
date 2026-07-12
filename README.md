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
- **OpenMP multithreading** (default ON) for stiffness assembly, mesh inside-tests,
  ZZ recovery, stress recovery, and CSR SpMV. Eigen dense kernels stay single-threaded
  to avoid nested OpenMP hangs. Serial path always builds if OpenMP is missing.
- **Sparse direct + iterative solvers**: SimplicialLDLT for small/medium free systems;
  ConjugateGradient (diagonal preconditioner) auto-selected above 8000 free DOFs.
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
- **OpenMP** (`POLYMESH_WITH_OPENMP=ON`, default) for parallel assembly, mesh classify,
  ZZ/stress recovery, SpMV — uses libgomp with GCC; on Clang install `libomp-dev`. If
  OpenMP is missing, the build stays serial (no hard dependency).
- **CUDA** (`POLYMESH_WITH_CUDA=ON`) for GPU backends — requires a toolkit; CPU path always builds.
- **clang-format 18** for style checks: `pip install 'clang-format==18.1.8'` (or use the version on `PATH`).

C++20 compiler required (GCC 12+ or Clang 16+ recommended). CMake ≥ 3.24. Catch2, GLFW, and ImGui are fetched by CMake.

### Performance build notes

| Setting | Default | Effect |
|---------|---------|--------|
| `CMAKE_BUILD_TYPE=Release` | Release if unset | **`-O3 -DNDEBUG`** (GCC/Clang) |
| `POLYMESH_WITH_OPENMP` | ON | Multi-thread assembly / mesh / ZZ / SpMV |
| `POLYMESH_NATIVE_ARCH` | **OFF** | Optional `-march=x86-64-v3` (try only if ctest stays green) |
| `POLYMESH_ENABLE_LTO` | **OFF** | LTO can miscompile Eigen (SEGV); leave off |

We deliberately **do not** use `-ffast-math` / `-Ofast` / reduced precision — patch tests
and Tier-1 verification stay double-exact. Primary speed levers: **`-O3`** (Release) and
**OpenMP**. Host ISA flags (`-march=*`) have caused Eigen heap corruption on this
toolchain; leave them off unless you re-verify with `ctest`.

Check the runtime stack: `polymesh backend` → e.g. `cpu | OpenMP 16 threads | Eigen serial (no nest)`.

### Clone, configure, build, test

```sh
git clone <this-repo-url> polymesh && cd polymesh

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPOLYMESH_WITH_GUI=ON \
  -DPOLYMESH_WITH_OPENMP=ON \
  -DPOLYMESH_WITH_OCC=OFF \
  -DPOLYMESH_WITH_CUDA=OFF

cmake --build build -j
./build/apps/cli/polymesh backend   # confirm OpenMP threads
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

Useful flags: `--mesher tet|hex|hexvem|graded|hexpyr|prism`, `--feature`, `--adapt n`,
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
cmake -B build -DPOLYMESH_WITH_OCC=ON     # STEP/B-rep (OpenCASCADE)
cmake -B build -DPOLYMESH_WITH_CUDA=ON    # GPU backends
cmake -B build -DPOLYMESH_WITH_OPENMP=OFF # force serial assembly
cmake -B build -DPOLYMESH_WITH_GUI=OFF    # libs + CLI + tests only
```

### OpenMP assembly (`POLYMESH_WITH_OPENMP`)

When CMake finds OpenMP (default **ON**), `fea::assemble_stiffness` forms element
stiffness matrices over elements in parallel (`#pragma omp parallel for`), writing
thread-local sparse triplets and merging without a critical section in the hot loop.
Results match the serial path within patch-test / Tier-0 tolerances. Disable with
`-DPOLYMESH_WITH_OPENMP=OFF`, or leave ON when the compiler has no OpenMP — the build
then falls back to serial assembly automatically.

### Linear solve (direct / CG)

`fea::solve_elastostatics` partitions Dirichlet DOFs then solves the free system:

| Free DOFs (`nfree`) | Default (`SolveMethod::kAuto`) |
|---|---|
| ≤ 8000 | Eigen `SimplicialLDLT` (sparse Cholesky) |
| > 8000 | Eigen `ConjugateGradient` + `DiagonalPreconditioner` |

Override with `SolveOptions` (`method = kDirect | kCG`, `cg_threshold`, `cg_tol`,
`cg_max_iters`). Patch tests and small verification meshes stay on the direct path
by default so constant-strain exactness is preserved. Force CG for large-N checks
or when factorisation memory would dominate. See `fea/solve.hpp`.

### CUDA backends (`POLYMESH_WITH_CUDA`)

Default **OFF**. CI and CPU-only machines never need a toolkit. When ON:

```sh
# Toolkit on PATH (example: CUDA 12/13 under /usr/local/cuda)
export PATH=/usr/local/cuda/bin:$PATH
# or: cmake ... -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc

cmake -S . -B build -G Ninja -DPOLYMESH_WITH_CUDA=ON
# If host GCC is newer than nvcc supports (e.g. GCC 16 + CUDA 13.x):
#   cmake ... -DCMAKE_CUDA_FLAGS="-allow-unsupported-compiler"
# Architectures default to "native" (detect local GPU). Cross-build example:
#   cmake ... -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build -j
ctest --test-dir build --output-on-failure -R "SpMV|spmv"
```

- **CPU CSR SpMV** (`fea::spmv_cpu` / `fea::csr_from_eigen`) always builds — free-DOF
  helper for CG / iterative backends; unit-tested against Eigen sparse × vector.
- **CUDA SpMV** lives in `backend_cuda.cu` and runs only when a device is present;
  Catch2 parity test compares GPU vs CPU (SKIP if toolkit off or no GPU).
- `polymesh backend` reports `cpu` or `cuda (<device name>)`.
- Batched element-stiffness GPU kernels are not wired yet (same dispatch layer).

### STEP / OpenCASCADE (`POLYMESH_WITH_OCC`)

Default CI/quickstart builds are **STL-capable without OCC**. **Product
geometry, Lane V campaigns, and GUI STEP import need OCC ON**
(`-DPOLYMESH_WITH_OCC=ON`) so B-rep/STEP stays first-class (ADR-0001 / ADR-0020).
Without OCC, STEP loads and CadModel tests are unavailable / SKIP.

```sh
# Ubuntu / Debian (package names vary slightly by release; 7.6+ typical)
sudo apt install libocct-data-exchange-dev libocct-foundation-dev \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev \
  libocct-ocaf-dev libocct-visualization-dev

# Fedora
sudo dnf install opencascade-devel

cmake -S . -B build -G Ninja -DPOLYMESH_WITH_OCC=ON
cmake --build build
# STEP / CadModel tests run when OCC is linked; with OCC off they SKIP.
```

If CMake cannot find OCCT: `-DOpenCASCADE_DIR=/path/to/cmake/OpenCASCADE` (or the
prefix that contains `OpenCASCADEConfig.cmake`). See `src/geom/CMakeLists.txt`.

**Product fixtures are STEP** (`scripts/gen_cad_parts.py`). STL write is not
part of the product path (`scripts/check_no_product_stl.sh`; load_stl remains
for compare/legacy only).

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
| `examples/` | CLI mesh/solve scripts on public fixtures |
| `docs/` | Spec, phases, ADRs, progress |
| `graphify-out/` | Committed knowledge graph for agents (see CONTRIBUTING §8) |

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
python3 bench/d6/run_tier3.py --full --render    # D6 uniform tet10 vs graded
```

Peer priority (headless): **CalculiX first**, then Elmer, then Code_Aster —
see [bench/competitive/README.md](bench/competitive/README.md). Internal
verification detail: [docs/progress.md](docs/progress.md).

## License

[BSD-3-Clause](LICENSE).
