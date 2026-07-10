# PolyMesh

**Adaptive hybrid polyhedral mesh generator co-designed with an FEA solver.**

Most FEA workflows treat meshing and solving as separate concerns: a uniform
tet mesh wastes enormous compute on regions a single large element would
resolve, while under-resolving the stress concentrations at edges, corners,
and fillets — exactly where results matter. PolyMesh reads the geometry,
classifies regions by criticality, and chooses element **shape, size, and
polynomial order** per region — then iterates with solver feedback until a
target accuracy is met at minimum cost.

## What it does (v1 scope)

- **Hybrid element zoo** in one conforming mesh: tets, hexes, prisms,
  pyramids, and general polyhedra (Virtual Element Method).
- **Adaptive polynomial order** p = 1–4 per element: cheap low-order elements
  in benign regions, high-order where stress accuracy matters.
- **Geometry-driven sizing**: sharp edges, corners, high curvature, and thin
  walls are detected automatically and get graded, high-fidelity treatment.
- **Solution-driven adaptivity**: solve → estimate error per element → refine
  size, raise order, or swap element type locally → re-solve.
- **Linear elastostatics** (3D), STL and STEP input, VTU output for ParaView.
- **CUDA acceleration** (optional): parallelizable kernels — batched element
  stiffness, sparse matrix-vector products, error indicators — offload to the
  GPU in full double precision, with a CPU reference path that always exists.
- **Verified, not vibes**: every element passes patch tests, every claimed
  convergence order is demonstrated on manufactured solutions, and benchmarks
  run against closed-form analytical cases (see [BENCHMARKS.md](BENCHMARKS.md)).

## Status

Early development — Phase P0 (scaffolding) complete; Phase P1 (reference
solver on standard elements) is next. See [PHASES.md](PHASES.md) for the plan
and [PROGRESS.md](PROGRESS.md) for the current state.

## Building

Requires a C++20 compiler, CMake ≥ 3.24, Eigen, and nlohmann-json
(Catch2 is fetched automatically for tests).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Optional components:

```sh
# STEP/B-rep import via OpenCASCADE
cmake -B build -DPOLYMESH_WITH_OCC=ON
# CUDA backend (requires the CUDA toolkit)
cmake -B build -DPOLYMESH_WITH_CUDA=ON
```

## Layout

| Module | Purpose |
|---|---|
| `src/geom` | Geometry input (STL; STEP via `POLYMESH_WITH_OCC`), feature analysis |
| `src/mesh` | Face-based polyhedral mesh structure, validity, generation |
| `src/adapt` | Sizing fields, error estimation, hp-refinement decisions |
| `src/fea` | Element formulations, assembly, sparse solve, CPU/CUDA backends |
| `src/bench` | Verification harness (Tier 0–3) and anti-cheat audit tooling |
| `src/cli` | `polymesh` command-line interface |

Design decisions are recorded ADR-style in [docs/decisions/](docs/decisions/).

## License

[AGPL-3.0-or-later](LICENSE).
