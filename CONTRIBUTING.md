# CONTRIBUTING — Codebase map & standards

**Root markdown allowed:** `README.md`, `CONTRIBUTING.md`, `CHANGES.md` only.  
External-contributor **agent** PR policy: **[CHANGES.md](CHANGES.md)** (not for owner/`master` direct work).  
Process/spec/phases/progress: under `docs/`. Do not reintroduce other root `.md` files.

This file is the onboarding map for **fresh agents and humans**. Read it before grepping the whole tree.

---

## 1. What this repo is

**PolyMesh** — adaptive hybrid polyhedral mesher co-designed with a linear-elastostatics FEA solver (C++20). Mesher and solver are optimized for each other; element zoo includes tets/hexes/prisms/pyramids/polyhedra (VEM).

| Build | Command |
|---|---|
| Configure | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build build` |
| Test | `ctest --test-dir build` (CWD for tests is **repo root**) |
| GUI | `./build/apps/gui/polymesh-gui` |
| CLI | `./build/apps/cli/polymesh` |

Options: `POLYMESH_WITH_OCC`, `POLYMESH_WITH_CUDA`, `POLYMESH_WITH_GUI`, `POLYMESH_BUILD_TESTS`.

---

## 2. Directory layout (keep it)

```text
.
├── README.md                 # product + build + bench scoreboard
├── CONTRIBUTING.md           # THIS FILE — map & standards
├── CHANGES.md                # external-agent PR workflow (clone → branch → PR)
├── LICENSE                   # BSD-3-Clause
├── CMakeLists.txt            # root build; add_subdirectory only
├── .clang-format
├── .github/workflows/        # CI
│
├── apps/                     # EXECUTABLES ONLY — no core algorithms
│   ├── cli/                  # polymesh CLI
│   └── gui/                  # GLFW+ImGui presentation (theme, widgets, viewport)
│
├── src/                      # LIBRARIES — linkable, testable, no windowing
│   ├── geom/                 # STL/surface; OCC when enabled
│   ├── mesh/                 # polyhedral mesh DS + validity + generators
│   ├── adapt/                # sizing fields, error indicators, marking
│   ├── fea/                  # elements, assembly, solve, stress (CPU/CUDA)
│   ├── bench/                # reference JSON loader only (anti-cheat boundary)
│   └── pipeline/             # headless study: import → mesh → solve job
│
├── tests/                    # Catch2; support/ helpers; no production logic
├── bench/                    # reference/*.json, geometries, reports, peer harness
├── docs/                     # all other markdown
│   ├── spec.md, phases.md, benchmarks.md, progress.md
│   ├── decisions/            # ADRs (short)
│   ├── process/              # agent notes, historical
│   └── gui/                  # theme/layout notes (when present)
│
└── graphify-out/             # committed knowledge graph for agents
```

### Dependency direction (do not invert)

```text
apps/gui ──► pipeline ──► fea ──► mesh ──► geom
apps/cli ──► pipeline / fea / mesh / geom / adapt
tests    ──► same libraries as apps (never apps/* sources)
fea may use CUDA backend; CPU path always exists
bench_harness loads bench/reference/* — ONLY module allowed to
```

**Rules:**

1. **`apps/` never implements physics or meshing.** UI calls `pipeline` / libs.
2. **`src/pipeline` has no GLFW/ImGui/OpenGL.** Headless-safe; used by GUI + tests.
3. **Libraries do not include files from `apps/`.**
4. **New code goes in the smallest library that owns the concept.** Prefer extend over new top-level folders.
5. **Public headers:** `src/<lib>/include/<lib>/...hpp`. Implementation in `src/<lib>/src/`.

---

## 3. Where to change what

| Task | Go here |
|---|---|
| Element shape / quadrature / stiffness | `src/fea/` |
| Sparse assembly / Dirichlet / solve | `src/fea/` (`assembly`, `solve`) |
| Stress recovery | `src/fea/stress.*` |
| Mesh connectivity / validity / generators | `src/mesh/` |
| STL / surface / STEP (OCC) | `src/geom/` |
| Sizing / adaptivity | `src/adapt/` |
| Import → voxel/tet mesh → background solve | `src/pipeline/` |
| Theme colors, Interwebz widgets, layout | `apps/gui/theme.*`, `widgets.*` |
| 3D view / picking | `apps/gui/viewport.*` |
| Analytical reference numbers | `bench/reference/*.json` **only** |
| Verification tests | `tests/test_*.cpp` |
| Physics/math decisions | `docs/decisions/NNNN-*.md` (ADR) |
| Phase plan / progress | `docs/phases.md`, `docs/progress.md` |
| Agent knowledge graph | `graphify-out/` + `/graphify` skill |

---

## 4. Engineering standards (non-negotiable)

### Language & build
- **C++20 only.** No Rust, no Python in the product path.
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Werror`.
- Format: repo `.clang-format` on all `*.cpp` / `*.hpp` / `*.cu` under `apps/`, `src/`, `tests/`.
- **`double` only** in solver math (CPU and GPU). No `float` shortcuts in assembly/solve.
- RAII; no raw `new`/`delete` without a `// SAFETY:` comment on hot paths.

### Eigen traps
- Any TU calling `.inverse()` must `#include <Eigen/Dense>` (not only `<Eigen/Core>`).
- Materialize inverses: `Matrix3d inv = J.inverse();` — never nest `inverse().transpose()`.
- Materialize Eigen products returned through `std::function` (expression templates → zeros).

### Anti-cheat (sacred)
1. **Never hardcode benchmark/reference answers** in `src/` or `apps/`.
2. Reference values live only in `bench/reference/*.json`, loaded via `bench_harness`.
3. **Patch test is sacred** — constant strain exact on distorted meshes.
4. **Convergence ORDER** is the metric, not a single-mesh error.
5. Every mesh must pass validity before solve (watertight, +Jacobian, conforming).
6. Determinism: randomized algorithms take an **explicit seed**.

### License
- **BSD-3-Clause.** SPDX: `// SPDX-License-Identifier: BSD-3-Clause`.
- Deps: MIT/Apache/BSD/LGPL-compatible only.

### Git identity (owner agents)
- Owner agents: commit as configured identity (**Hunter-124**), often on `master`. No AI attribution. External agents: **CHANGES.md**.

### CUDA
- Optional (`POLYMESH_WITH_CUDA`). Use GPU only where f64 parallel work wins.
- Every CUDA kernel needs a **CPU parity test**. CPU path always compiles.

---

## 5. Documentation standards (no slop)

| Put | Where |
|---|---|
| Product pitch + build + scoreboard | `README.md` |
| Map + standards (agents) | `CONTRIBUTING.md` (this file) |
| External agent PR / clone / merge | `CHANGES.md` |
| Spec / phases / benches / progress | `docs/*.md` |
| One decision = one short ADR | `docs/decisions/` |
| GUI theme tokens / layout rules | `docs/gui/` |

- Prefer **tables and short commands** over essays.
- Update `docs/progress.md` when benchmarks or phase status change.
- Do **not** duplicate the same policy in three files; link once.
- Do **not** leave TODO novels in headers — fix or file a one-line open issue in progress.

---

## 6. How to add a feature (agent checklist)

1. Read this file + relevant ADR + `docs/phases.md` for the phase you touch.
2. Put code in the correct layer (§2–3). No new root clutter.
3. Unit test in `tests/`; if physics, use `bench/reference` via harness.
4. `clang-format`, full build, full `ctest` green.
5. Grep audit: no `bench/reference` reads outside `src/bench` / tests.
6. Short ADR if you chose among real design alternatives.
7. Update `docs/progress.md` benchmark table if results move.
8. If graph-worthy structure change: refresh `graphify-out/` (or leave a note for the orchestrator).

---

## 7. GUI rules (Interwebz)

- Colors: **only** via `apps/gui/theme.hpp` tokens / palette — never raw hex in widgets.
- Layout: fixed constrained panels; prefer helpers in `widgets.*` for centering/spacing.
- Themes must be switchable from one place (`theme.cpp` apply function).
- Presentation only: if you need a new mesh/solve behavior, add it under `src/pipeline` or `src/mesh`/`src/fea`, not in `apps/gui`.

---

## 8. Graphify (for agents)

Shared knowledge graph so humans and agents navigate the codebase without full-repo greps. **Commit the portable artifacts**; regenerate machine-local views.

### What is version-controlled

| Path | Commit? | Role |
|---|---|---|
| `graphify-out/graph.json` | **yes** | Graph data (query / path / explain) |
| `graphify-out/GRAPH_REPORT.md` | **yes** | Communities, god nodes, audit |
| `graphify-out/.graphify_labels.json` | **yes** | Community names for viz / report |
| `graphify-out/manifest.json` | **yes** | File inventory for `--update` |
| `graphify-out/graph.html` | no (gitignored) | Interactive browser view — regenerate |
| `graphify-out/cache/` | no | Semantic extraction cache |
| `graphify-out/.graphify_python` | no | Local interpreter path |
| `graphify-out/.graphify_root` | no | Absolute scan root |
| `graphify-out/cost.json` | no | Local token accounting |

### Setup (once per clone)

```sh
# install CLI (pick one)
uv tool install graphifyy          # preferred
# pip install graphifyy

graphify hook install              # post-commit AST rebuild + post-checkout
git config merge.graphify.driver "graphify merge-driver %O %A %B"  # union-merge graph.json
graphify export html               # optional local browser viz
```

`.gitattributes` maps `graphify-out/graph.json` to the `graphify` merge driver so concurrent graph updates union-merge instead of conflict soup.

### Keep the graph current

| Change type | Command |
|---|---|
| Code edits (typical) | `graphify update .` — AST only, no LLM/API key |
| After commit (if hook installed) | automatic for code paths under the post-commit hook |
| Docs / ADRs / large renames | `/graphify .` or `/graphify --update` (semantic refresh) |
| Force shrink after deletions | `graphify update . --force` |

Prefer `graphify query "…"`, `graphify path "A" "B"`, `graphify explain "X"` over blind greps when `graphify-out/graph.json` exists. Skip the hook for a one-off commit with `GRAPHIFY_SKIP_HOOK=1`.

### PR hygiene

If your PR changes public structure (new libs, renames, major call graph), include an updated `graphify-out/` in the same PR (or run `graphify update .` and commit the result). Doc-only PRs do not need a graph refresh.

---

## 9. Frozen baselines

- **P1 solver baseline** (tet4/10, hex8/20 isoparametric path, Tier-0/1 cases): frozen as the comparator after GATE 1. Improve **alongside** (new elements, new mesher paths), do not silently retune tests to hide regressions.

---

## 10. Quick “I am lost” paths

| Feeling | Action |
|---|---|
| Don’t know folder | §2 layout + §3 table |
| External PR / wrong clone | `CHANGES.md` (agents) |
| Don’t know phase | `docs/phases.md` + `docs/progress.md` |
| Don’t know why a choice | `docs/decisions/` |
| Don’t know who calls what | `graphify-out/` / graphify query |
| Touching Eigen inverse | §4 Eigen traps |
| Touching benchmarks | §4 anti-cheat + `docs/benchmarks.md` |
