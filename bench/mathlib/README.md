# mathlib probe — GLM vs Eigen

Supporting measurement for
[ADR-0044](../../docs/decisions/0044-glm-cannot-be-the-math-library.md).

GLM is **not** a dependency of this project and this directory does not make it
one. `mathlib_probe.cpp` is compiled only by `run_probe.sh`, which fetches a
pinned, checksum-verified GLM into a scratch directory. Nothing under `src/` or
`apps/` includes it, and the CMake build never sees it.

```sh
bench/mathlib/run_probe.sh                       # human-readable report
bench/mathlib/run_probe.sh --simd                # also build -DGLM_FORCE_INTRINSICS
bench/mathlib/run_probe.sh --out docs/bench/mathlib-probe.txt
```

The last committed run is `docs/bench/mathlib-probe.txt`.

## What it measures

**Throughput (K1–K6).** Six kernels lifted from real call sites, each written
twice — once per library — and timed best-of-7 with the flags a Release build of
this project uses (`-O3 -DNDEBUG`, no `-march`, no fast-math; see the
`POLYMESH_NATIVE_ARCH` / `POLYMESH_ENABLE_LTO` notes in the root
`CMakeLists.txt`). Only kernels GLM can actually express are paired.

K4 carries a hand-written scalar control. It exists because K4 is the one row
with a large margin, and the control shows that margin is Eigen 3.5.0's
vectorized `Vector3d::cross` being 5.5× slower than plain scalar code rather
than GLM being fast — see ADR-0044 §3.

**Share of the inner loop GLM cannot reach.** The `element_stiffness`
quadrature body is timed with and without its `B^T D B` product (6×24 for hex8).
GLM has no type of that shape at any size, so the difference is the fraction of
the hot loop that is structurally out of GLM's reach.

**Eigensolver accuracy.** GLM's one non-trivial offer is
`glm::findEigenvaluesSymReal`, which could in principle stand in for
`Eigen::SelfAdjointEigenSolver<Matrix3d>` at the call sites in
`src/adapt/src/metric_field.cpp` and `src/fea/src/stress.cpp:412`. The probe
builds symmetric tensors from a **known** spectrum (`M = Q diag(λ) Qᵀ`) across
ten conditioning classes and scores both libraries against λ, so neither library
is treated as the reference.

## Anti-cheat boundary

Per `CONTRIBUTING.md` §4 and `docs/benchmarks.md`, this probe:

- reads nothing from `bench/reference/` — that directory stays readable only
  from `src/bench`;
- computes no physics accuracy metric and emits no row in the competitive
  scoreboard schema (`bench/competitive/schema.json`), because library
  throughput is not a solver result and must not appear on that scoreboard;
- draws every random input from an explicit fixed seed (`kThroughputSeed`,
  `kSpectrumSeed`);
- accumulates every kernel into a printed sink, so no timed loop can be
  dead-code eliminated;
- scores the eigensolver comparison against an independently constructed
  spectrum rather than against Eigen's output.

## Reading the numbers

`GLM/Eigen < 1` means GLM was faster. Timings are per-op nanoseconds on one
pinned core; they are codegen-sensitive, and the K4 story is a live example —
the same kernel moved by 8× under `EIGEN_DONT_VECTORIZE` and got *worse* under
`-march=x86-64-v3`. Treat a ratio inside roughly ±40% as "no difference", and
re-run the probe rather than quoting these numbers on another toolchain.
