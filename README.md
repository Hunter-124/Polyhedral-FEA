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

P1 reference solver frozen as baseline. Campaign in progress: real mesher,
hybrid zoo, adaptivity, competitive scoreboard. Tracking:
[docs/progress.md](docs/progress.md), [docs/phases.md](docs/phases.md).

## Building

C++20, CMake ≥ 3.24, Ninja, Eigen, nlohmann-json (Catch2/GLFW/ImGui via FetchContent).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
./build/apps/gui/polymesh-gui
```

```sh
cmake -B build -DPOLYMESH_WITH_OCC=ON    # STEP/B-rep (OpenCASCADE)
cmake -B build -DPOLYMESH_WITH_CUDA=ON   # GPU backends
```

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
**How to branch, commit, and open a PR (strict, novice-safe):** [CHANGES.md](CHANGES.md)

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
