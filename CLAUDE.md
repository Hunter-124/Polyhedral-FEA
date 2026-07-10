# CLAUDE.md — PolyMesh (working name)

Adaptive/hybrid polyhedral mesh generator co-designed with an FEA solver.
The mesher and solver are optimized *together*: the mesh adapts element type,
size, and polynomial order to the geometry and the physics so critical regions
(edges, corners, fillets, contact zones) get high-fidelity treatment while
bulk regions (flat faces, uniform-stress volumes) use large cheap elements.

## Language & tooling
- **C++20**, CMake ≥ 3.24 + Ninja (ADR-0007). Builds with `-Wall -Wextra
  -Wpedantic -Wconversion -Werror`; `clang-format` (repo `.clang-format`) must
  be clean before any commit. Tests: Catch2 via `ctest --test-dir build`.
- Modules under `src/`: `geom` (geometry kernel interface), `mesh` (data
  structures + generation), `adapt` (sizing fields, error estimation,
  refinement), `fea` (solver), `bench` (verification harness), `cli`.
- Memory safety: RAII everywhere, no raw `new`/`delete` outside clearly
  justified hot paths (each needs a `// SAFETY:` comment). Prefer
  `std::vector`/`std::span`/smart pointers.
- `double` everywhere in the solver — CPU and GPU. No `float` shortcuts in
  assembly or solves.
- Dependencies: Eigen (dense + sparse; SimplicialLDLT first, iterative CG+AMG
  later), nlohmann-json, Catch2, OpenCASCADE behind `POLYMESH_WITH_OCC`.
- **CUDA** (ADR-0008): optional backend behind `POLYMESH_WITH_CUDA` for
  parallelizable kernels (batched element stiffness, SpMV, error indicators).
  The CPU path is the reference implementation and always exists; every CUDA
  kernel needs a parity test against it. CI builds CPU-only.

## Non-negotiable engineering rules
1. **Never hardcode expected benchmark values anywhere in `mesh/`, `adapt/`, or `fea/`.** Reference values live only in `bench/reference/` and are loaded by the harness. Any constant in solver code that matches a benchmark target is treated as cheating.
2. **The patch test is sacred.** Every element type/order must reproduce a constant-strain field exactly (to solver tolerance) on an arbitrary distorted mesh. A change that breaks the patch test cannot merge, no matter what it does for benchmarks.
3. **Convergence rate, not just error, is the metric.** An element claiming order p must demonstrate O(h^p) energy-norm convergence on a manufactured solution. Hitting a target error at one mesh size proves nothing.
4. **Every mesh must pass validity checks** before it reaches the solver: watertight, no inverted/negative-Jacobian elements, conforming interfaces between element types (or explicitly handled hanging nodes/mortar constraints).
5. **Determinism**: same input + same seed = same mesh, bit-for-bit where feasible. Randomized algorithms take an explicit seed.
6. All physics/math decisions get a short note in `docs/decisions/` (ADR style): what was chosen, alternatives, why.

## Definitions of done
- Code + unit tests + (if physics-touching) a verification case in `bench/`.
- `cargo test --workspace` green, benchmarks not regressed beyond stated tolerance.
- Public APIs documented with rustdoc including units (SI throughout: m, Pa, N).

## Licensing
**Decided: AGPL-3.0-or-later** (ADR-0002). All source files carry
`// SPDX-License-Identifier: AGPL-3.0-or-later`. Dependency policy:
MIT/Apache-2.0 preferred, LGPL acceptable (e.g. OpenCASCADE bindings), any
GPL-family dep must be AGPL-compatible. Dual commercial licensing stays open,
so no external contributions without a CLA/assignment policy in place.

## Git & attribution
- Commits are authored solely as **Hunter-124** (`git config user.name
  "Hunter-124"`, already set locally). Never add `Co-Authored-By`,
  "Generated with", or any other AI-agent attribution to commits, PRs, issues,
  or release notes. This is the repository owner's standing policy.
- Remote: https://github.com/Hunter-124/Polyhedral-FEA (push to `main` during
  P0; feature branches once CI exists).

## Workflow
- Guided phases are defined in `PHASES.md`. Do not start a phase whose upstream
  DAG dependencies aren't marked complete. Stop at every ⛔ GATE for human review.
- Inside a phase, the autonomous loop is `/loop` (see `.claude/commands/loop.md`).
- After benchmarks pass, always run `/audit` before declaring a phase complete.
- Keep a running `PROGRESS.md`: what's done, current benchmark table, open issues.
