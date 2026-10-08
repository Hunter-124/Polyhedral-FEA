# CONTRIBUTING — Codebase map & standards

**Root markdown allowed:** `README.md` and `CONTRIBUTING.md`. Other markdown lives under `docs/`, or as a `README.md` beside the data it describes (`bench/**`, `examples/`, `audits/`).  
Current state and next work: **[docs/STATUS.md](docs/STATUS.md)**.

This file is the onboarding map for contributors. Read it before grepping the whole tree.

---

## 0. Workflow

| Step | Rule |
|---|---|
| Sync first | `git fetch origin`, `git status`, `git pull --rebase origin master` before reading deeply or editing. Never work on a stale or dirty tree; resolve rebase conflicts (or stop and ask) first. Pull again right before pushing; never force-push `master`. |
| Honest authorship | Owner work commits as **Hunter-124**; external contributors commit as themselves. Verify with `git config user.name && git config user.email` before the first commit. |
| Find your way | This file (§2–3) before full-repo greps. |
| Pick work | [docs/STATUS.md](docs/STATUS.md) (open defects, next items). Methodology in force: [docs/plans/advisor-measure-first-program.md](docs/plans/advisor-measure-first-program.md) (ADR-0023/0024). Small fixes outside the list are fine. |
| Interfaces are contracts | Anything crossing the test-lab / GUI / campaign-analysis boundary uses the schemas in [docs/dag/interfaces.md](docs/dag/interfaces.md). Change a schema only in the same commit as both sides of the code. |
| Anti-cheat | §4. Never "fix" a failing benchmark by nudging the expected value; PRs that do this are closed. |
| Verification bar | Clean `-Werror` build; full Catch2 suite green, run from the repo root; docs/ADR updated. |
| Submit | External contributors: branch from current `master` in a real clone of this repo (not a zip or a copy of someone else's tree), one logical change per PR, never commit `build/`, secrets or binary dumps. Prefer merging `master` into your branch over rebasing unrelated history. Say in the PR body what was changed and how it was verified. |

---

## 1. What this repo is

**PolyMesh** — adaptive hybrid polyhedral mesher co-designed with a linear-elastostatics FEA solver (C++20). Mesher and solver are optimized for each other; element zoo includes tets/hexes/prisms/pyramids/polyhedra (VEM).

Presets live in [CMakePresets.json](CMakePresets.json) (CMake ≥ 3.25; `cmake --list-presets`).

| Build | Preset command | Plain CMake equivalent |
|---|---|---|
| Configure | `cmake --preset debug` (`release`, `no-gui`, `relwithdebinfo-ci`; Windows: `windows-msvc`) | `cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` |
| Build | `cmake --build --preset debug` | `cmake --build build-debug` |
| Test | `ctest --preset debug` (tests run from the **repo root**) | `ctest --test-dir build-debug --output-on-failure` |
| GUI | `./build-debug/apps/gui/polymesh-gui` | |
| CLI | `./build-debug/apps/cli/polymesh` | |

`release` and `relwithdebinfo-ci` (the CI `build-test` flags) build into `build/`;
`debug` into `build-debug/`, `no-gui` into `build-no-gui/`. `build.sh` / `build.bat`
wrap `release` / `windows-msvc` (or the debug presets with `Debug`) and copy the
binaries to the repo root. Presets only set values when used; the option defaults
in the root `CMakeLists.txt` are unchanged for `add_subdirectory` consumers.

Options: `POLYMESH_WITH_OCC`, `POLYMESH_WITH_CUDA`, `POLYMESH_WITH_GUI`, `POLYMESH_BUILD_TESTS` (full table in [README](README.md#build-ubuntu)).

**Product / campaign builds** need **`-DPOLYMESH_WITH_OCC=ON`** (STEP/B-rep;
ADR-0020). Package names: Ubuntu `libocct-*-dev` set in README; Fedora
`opencascade-devel`. Product fixtures are **STEP**
(`scripts/gen_cad_parts.py`); do not add product STL writers — gate:
`scripts/check_no_product_stl.sh` (`load_stl` remains compare/legacy only).

---

## 2. Directory layout (keep it)

```text
.
├── README.md                 # product + build + measured evidence
├── CONTRIBUTING.md           # THIS FILE — map & standards
├── LICENSE                   # BSD-3-Clause
├── CMakeLists.txt            # root build: options, dependency resolution, add_subdirectory
├── CMakePresets.json         # configure/build/test presets (release, debug, CI, Windows)
├── build.sh, build.bat       # preset wrappers: build CLI + GUI, copy to repo root
├── .clang-format
├── .github/workflows/        # CI
│
├── apps/                     # EXECUTABLES ONLY — no core algorithms
│   ├── cli/                  # polymesh CLI
│   ├── bench/                # d6 tier-3 benchmark runner
│   ├── testlab/              # campaign/probe harness (advisor corpus, gates)
│   └── gui/                  # GLFW+ImGui presentation (theme, widgets, viewport)
│
├── src/                      # LIBRARIES — linkable, testable, no windowing
│   ├── geom/                 # STL/surface; OCC when enabled
│   ├── mesh/                 # polyhedral mesh DS + validity + generators
│   ├── adapt/                # sizing fields, error indicators, marking
│   ├── fea/                  # elements, assembly, solve, stress (CPU/CUDA)
│   ├── advisor/              # learned mesh advisor inference (ONNX Runtime;
│   │                         #   POLYMESH_WITH_ADVISOR, ADR-0027)
│   ├── bench/                # reference JSON loader only (anti-cheat boundary)
│   └── pipeline/             # headless study: import → mesh → solve job
│
├── tests/                    # Catch2; support/ helpers; no production logic
├── bench/                    # reference/*.json, geometries, campaigns, reports,
│                             #   shipped advisor model (advisor/), peer harness
├── scripts/                  # Python/shell tooling (fixtures, corpus, analysis, gates);
│                             #   never in the product path
├── examples/                 # public-geometry mesh/solve example scripts
├── audits/                   # blind holdout-audit protocol (no private assets)
├── third_party/              # vendored deps (geogram; ADR-0025)
├── docs/
│   ├── STATUS.md             # single current-status page: state, defects, next
│   ├── progress.md           # short recent-change summary
│   ├── cli.md, solver-core.md, benchmarks.md, SHOWCASE.md
│   ├── decisions/            # ADRs — bodies frozen; index in decisions/README.md
│   ├── advisor/              # numbered advisor reports + model/data cards
│   ├── plans/                # methodology in force (advisor-measure-first-program.md)
│   ├── dag/interfaces.md     # test-lab / GUI / campaign schemas (contract)
│   ├── process/feedback-loop.md  # campaign analysis how-to
│   ├── training/             # advisor corpus/training guide
│   ├── validation/           # hand calcs, field verification
│   ├── bench/                # dated benchmark reports
│   ├── gui/                  # theme/layout notes
│   ├── assets/               # showcase / cinema media + provenance
│   └── archive/              # frozen history (old plans, phases, board, research)
```

### Dependency direction (do not invert)

```text
apps/gui     ──► pipeline ──► fea ──► mesh ──► geom
apps/cli     ──► pipeline / fea / mesh / geom / adapt
apps/bench   ──► fea / bench_harness
apps/testlab ──► geom / mesh / adapt / fea / pipeline / bench_harness / advisor
advisor      ──► pipeline (public) + onnxruntime (private); never the reverse
tests        ──► same libraries as apps, plus gui_testlab_data (ImGui-free) and
                 header-only apps/testlab helpers; never app entry points or UI
fea may use CUDA backend; CPU path always exists
bench_harness loads bench/reference/* — ONLY module allowed to
```

**Rules:**

1. **`apps/` never implements physics or meshing.** UI calls `pipeline` / libs.
2. **`src/pipeline` has no GLFW/ImGui/OpenGL.** Headless-safe; used by GUI + tests.
3. **Libraries do not include files from `apps/`.** Tests may link the ImGui-free app-private library `gui_testlab_data` and include the header-only testlab helpers (`apps/testlab/load_area.hpp`, `probe_util.hpp`, `run_artifacts.hpp`); never app entry points (`main.cpp`) or UI code.
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
| Advisor inference / feature vector / ONNX model load | `src/advisor/` |
| Campaign harness, probes, run artifacts | `apps/testlab/` |
| Theme colors, Interwebz widgets, layout | `apps/gui/theme.*`, `widgets.*` |
| 3D view / picking | `apps/gui/viewport.*` |
| Analytical reference numbers | `bench/reference/*.json` **only** |
| Verification tests | `tests/test_*.cpp` |
| Field-level verification of shipped solves | `scripts/verify_fields.py` + `docs/validation/field-verification.md` |
| Physics/math decisions | `docs/decisions/NNNN-*.md` (ADR) |
| Current status / next work | `docs/STATUS.md` |

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

### CUDA
- Optional (`POLYMESH_WITH_CUDA`). Use GPU only where f64 parallel work wins.
- Every CUDA kernel needs a **CPU parity test**. CPU path always compiles.

---

## 5. Documentation standards (no slop)

| Put | Where |
|---|---|
| Product pitch + build + measured evidence | `README.md` |
| Map + standards | `CONTRIBUTING.md` (this file) |
| Current state, open defects, next work | `docs/STATUS.md` (the only status page) |
| Reference (CLI, solver core, benchmarks) | `docs/*.md` |
| One decision = one short ADR (body frozen once accepted) | `docs/decisions/` |
| GUI theme tokens / layout rules | `docs/gui/` |
| Superseded plans, logs, research | `docs/archive/` (frozen) |

- Prefer **tables and short commands** over essays.
- Update `docs/STATUS.md` when benchmarks, defects, or subsystem state change; add a line to `docs/progress.md` for notable landed changes.
- Do **not** duplicate the same policy in three files; link once.
- Do **not** leave TODO novels in headers — fix, or add a one-line open item to `docs/STATUS.md`.
- Cite files and symbols, not source line numbers.

---

## 6. How to add a feature (checklist)

1. Read this file + relevant ADR + `docs/STATUS.md` for the subsystem you touch.
2. Put code in the correct layer (§2–3). No new root clutter.
3. Unit test in `tests/`; if physics, use `bench/reference` via harness.
4. `clang-format`, full build, full `ctest` green.
5. Grep audit: no `bench/reference` reads outside `src/bench` / tests.
6. Short ADR if you chose among real design alternatives.
7. Update `docs/STATUS.md` (and the evidence doc it links) if results move.

---

## 7. GUI rules (Interwebz)

- Colors: **only** via `apps/gui/theme.hpp` tokens / palette — never raw hex in widgets.
- Layout: fixed constrained panels; prefer helpers in `widgets.*` for centering/spacing.
- Themes must be switchable from one place (`theme.cpp` apply function).
- Presentation only: if you need a new mesh/solve behavior, add it under `src/pipeline` or `src/mesh`/`src/fea`, not in `apps/gui`.

---

## 8. Frozen baselines

- **P1 solver baseline** (tet4/10, hex8/20 isoparametric path, Tier-0/1 cases): frozen as the comparator after GATE 1. Improve **alongside** (new elements, new mesher paths), do not silently retune tests to hide regressions.

---

## 9. Quick “I am lost” paths

| Feeling | Action |
|---|---|
| Don’t know folder | §2 layout + §3 table |
| External PR / wrong clone | §0 Submit |
| Don’t know current state / what to work on | `docs/STATUS.md` |
| Don’t know why a choice | `docs/decisions/` |
| Touching Eigen inverse | §4 Eigen traps |
| Touching benchmarks | §4 anti-cheat + `docs/benchmarks.md` |
