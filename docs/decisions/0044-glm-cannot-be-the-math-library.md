# ADR-0044: GLM cannot be the math library, and is not a useful second one

- Status: accepted (2026-08-21)
- Decision: keep Eigen as the single math library. GLM is rejected both as a
  replacement and as a parallel "point of comparison".
- New: `bench/mathlib/mathlib_probe.cpp`, `bench/mathlib/run_probe.sh`,
  `bench/mathlib/README.md`, `docs/bench/mathlib-probe.txt`
- Touches: nothing in `src/` or `apps/`. GLM is not added as a dependency.

## Context

The question was whether GLM could serve as the project's math library, either
instead of Eigen or alongside it as a second implementation to compare against.
It was worth asking: GLM is header-only, has no ABI, and is the idiomatic choice
for exactly the kind of 3-vector and 4×4 camera math that `apps/gui/viewport.cpp`
does.

The answer is no, on two independent grounds — scope and numerics — and the
investigation turned up one genuinely actionable finding that has nothing to do
with GLM. Numbers below come from `bench/mathlib/run_probe.sh`; the committed run
is `docs/bench/mathlib-probe.txt` (Eigen 3.5.0, GLM 1.0.3, GCC 16.1.1, `-O3`, no
`-march`).

## 1. Scope: GLM cannot express most of what this codebase does

GLM's vector and matrix templates are declared only for sizes 1–4
(`glm/detail/qualifier.hpp:34-37`); anything larger is an incomplete type. There
is no dynamic-size type, no sparse type, and no factorization beyond fixed-size
`inverse`/`determinant` (`glm/matrix.hpp:92-158`). GLM's own authors record the
gap in a TODO: "Implement other types of matrix factorisation, such as: QL and
LQ, L(D)U, eigendecompositions, etc..." (`glm/gtx/matrix_factorisation.hpp:24-28`).

What this codebase actually requires of a math library:

| Requirement | Where | GLM |
|---|---|---|
| Runtime-sized element matrices, 12–60 DOF and unbounded for VEM | `fea/src/assembly.cpp:30-45,122`, `hierarchical.cpp`, `hp_assembly.cpp`, `vem.cpp` | none |
| `SparseMatrix<double>` + `Triplet` global assembly | `fea/include/fea/assembly.hpp:27`, `constraints.hpp:46`, `hp_assembly.hpp:57` | none |
| Sparse direct + iterative solve (`SimplicialLDLT`, `IncompleteCholesky`, CG) | `fea/src/solve.cpp:376,453`, `hp_assembly.cpp:788` | none |
| Dense `SelfAdjointEigenSolver`, `EigenSolver`, `GeneralizedSelfAdjointEigenSolver`, `FullPivLU` | `adapt/src/metric_field.cpp:33,283,289,304` | symmetric only, experimental |
| `LDLT`, `ColPivHouseholderQR`, `JacobiSVD` | `fea/src/vem.cpp:313,386`, `stress.cpp:250`, `zz.cpp:221` | none |
| `Eigen::Index`-keyed DOF maps, `.segment<3>()` scatter/gather | `fea/include/fea/solve.hpp:21`, ~40 sites | n/a |

This is not a long tail. It is the solver.

Nor is the geometry layer separable. Public headers under `src/*/include` carry
355 `Eigen::` occurrences across 53 headers, and `geom`, `mesh`, `adapt`, `fea`,
`pipeline` — five of the six libraries — all link `Eigen3::Eigen` **PUBLIC**
(`src/geom/CMakeLists.txt:15`, `src/mesh/CMakeLists.txt:29`,
`src/adapt/CMakeLists.txt:13`, `src/fea/CMakeLists.txt:29`,
`src/pipeline/CMakeLists.txt:12`). `NodalMesh::nodes` is
`std::vector<Eigen::Vector3d>` and `element_stiffness` consumes it, so "port only
the geometry to GLM" means introducing a conversion boundary at every mesh/FEA
interface plus a permanent second math dependency. Only `src/bench` and
`src/advisor` are Eigen-free, deliberately.

Quantitatively, the part of the FE hot loop GLM has no type for is nearly all of
it. Timing `element_stiffness`'s quadrature body with and without its `B^T D B`
product:

```
full element_stiffness inner loop       627.904 ns/op
same loop minus the B^T D B product      53.473 ns/op
B^T D B (6x24) alone                    574.430 ns/op  = 91.5% of the loop
```

So even a perfect GLM port of the 3×3 Jacobian work would be optimising 8.5% of
the kernel.

## 2. Performance: where GLM can compete, it is a wash

Six kernels lifted from real call sites, best-of-7, `GLM/Eigen < 1` means GLM
won:

| kernel | Eigen | GLM | GLM/Eigen |
|---|---|---|---|
| K1 hex8 Jacobian: `J=dNᵀX`, det, inv, `dN/dx` | 23.621 | 28.946 | 1.23× |
| K2 tet orientation predicate | 2.093 | 1.545 | 0.74× |
| K3 bbox min/max stream | 0.525 | 0.728 | 1.39× |
| K4 face normal (Newell, materialized) | 25.650 | 5.864 | 0.23× |
| K5 GUI mat4 vertex transform (float) | 1.068 | 0.654 | 0.61× |
| K6 symmetric 3×3 eigendecomposition | 289.485 | 255.386 | 0.88× |

Ignoring K4 (see §3), there is no winner: GLM takes the float camera transform,
Eigen takes the Jacobian and the bbox stream, and the rest is inside noise.
`GLM_FORCE_INTRINSICS` changed nothing measurable, which is expected — GLM's
matrix SIMD kernels are entirely `_mm_*_ps`, i.e. float-only
(`glm/simd/matrix.h`), and this codebase's solver math is double by mandate.

GLM also has no expression templates: every operator materialises its result.
For 3- and 4-element types the compiler generally keeps those temporaries in
registers, so this is not the disaster it would be for `MatrixXd` — but it also
means `.noalias()`, which this codebase relies on at every stiffness
accumulation (`assembly.cpp:139`, `vem.cpp:269,290,394,476,568`,
`solve.cpp:192,210,258`), has no counterpart to translate into.

## 3. The one large margin is an Eigen finding, not a GLM one

K4 shows GLM 4.4× ahead, which does not fit the rest of the table. A
hand-written scalar control settles it:

```
K4 control, hand-written scalar cross: 4.654 ns/op (Eigen 5.51x, GLM 1.26x of control)
```

Eigen is 5.5× slower than plain scalar code; GLM is merely 1.26×. Isolating a
triple cross-accumulate on this toolchain:

| build | ns/op |
|---|---|
| as written | 12.9 |
| `-DEIGEN_DONT_VECTORIZE` | 1.59 |
| `-march=x86-64-v3` | 15.1 |

Eigen 3.5.0's vectorized path for `Vector3d` is the problem: three doubles is 24
bytes, so the cross is synthesised from partial SSE loads, shuffles and a scalar
tail, and the shuffle chain costs more than the arithmetic it replaces. `-march`
makes it worse. GLM wins here only because it has no double-precision SIMD path
for `vec3` and therefore emits the scalar code that happens to be fastest.

This matters to us independently of GLM. The affected pattern is a **materialized**
3-vector cross, and it is in real per-triangle and per-face loops:
`fea/src/traction.cpp:632` (`n += p.cross(q)` in `surface_face_normal`),
`fea/src/boundary_faces.cpp:170`, `mesh/src/poly_mesh.cpp:97`,
`mesh/src/cvt_export.cpp:739`, `mesh/include/mesh/cell_validity.hpp:103-106`,
`geom/src/features.cpp:21`, `geom/src/indicators.cpp:24,46,57`,
`mesh/src/brep_fidelity.cpp:263,468`. The many `a.dot(b.cross(c))` triple
products are *not* affected — only one scalar escapes, so the compiler deletes
most of the work (K2 measures that case at 2.09 ns).

Note this is also a fresh argument for keeping `POLYMESH_NATIVE_ARCH` off, which
until now rested on the heap-corruption history recorded in
`CMakeLists.txt:69-71`.

Deliberately not fixed here: this ADR is the GLM decision, and a global
`EIGEN_DONT_VECTORIZE` is the wrong remedy anyway — it would also disable the
dense kernels where Eigen's vectorization pays for itself. Left as a scoped
follow-up with a measured starting point.

## 4. Numerics: GLM's only real offer is disqualified

GLM does ship one non-trivial routine: `glm::findEigenvaluesSymReal`
(`glm/gtx/pca.hpp:78-107`), a symmetric eigensolver that could in principle
replace `Eigen::SelfAdjointEigenSolver<Matrix3d>` at its six call sites in
`adapt/src/metric_field.cpp` (lines 33, 43, 80, 93, 105, 456) and the one in
`fea/src/stress.cpp:412`. K6 shows it
is even slightly faster.

It cannot be used. Its convergence and zero tests hardcode
`static_cast<T>(0.0000001)` — 1e-7 — for **every** scalar type including
`double` (`glm/gtx/pca.inl:78,116,183,225,244,263`). Scored against a known
spectrum:

| conditioning class | Eigen worst rel | GLM worst rel |
|---|---|---|
| well-conditioned | 1.89e-15 | 9.33e-15 |
| aspect 1e3 (spread 1e6) | 3.87e-10 | 4.70e-10 |
| aspect 1e6 (spread 1e12) | 4.07e-04 | 5.20e-04 |
| near-degenerate pair (1e-8) | 1.78e-15 | **5.00e-09** |
| **tiny magnitude (1e-8 scale)** | 1.82e-15 | **9.97e-01** |
| large magnitude (1e8 scale) | 1.79e-15 | 2.38e-15 |

On tensors whose overall magnitude falls below its epsilon, GLM returns
eigenvalues that are essentially 100% wrong — for all 6000 of them — **and
reports success**. A silent wrong answer is the worst possible failure mode, and
that input class is one this codebase manufactures on purpose:
`metric_field.cpp:92` forms `delta = ½((finer-coarser) + (finer-coarser)ᵀ)` and
eigensolves the difference of two nearly equal metrics, and `matrix_log`
(`metric_field.cpp:69-73`) maps `h ≈ 1` to log-eigenvalues near zero. On
near-degenerate pairs GLM is six orders of magnitude worse than Eigen, against a
pipeline that gates on `rcond() > 1e-12` (`metric_field.cpp:290`).

Two further disqualifiers: the routine returns its eigenvalues **unordered**, so
`glm::sortEigenvalues` is a second required call (Eigen guarantees ascending);
and it lives in `gtx`, which requires `GLM_ENABLE_EXPERIMENTAL` and carries GLM's
own warning that such extensions "might change from version to version without
any restriction". `metric_field.cpp` also needs a *generalized* eigensolver
(`GeneralizedSelfAdjointEigenSolver`, line 283) and a non-symmetric one
(`EigenSolver`, line 304). GLM has neither at any quality.

For the record, GLM's fixed-size arithmetic is fine: `det(J)` agrees with Eigen
to 8.1e-16 relative, `J⁻¹` to 1.3e-15 absolute, and the tet orientation
predicate is bitwise identical on all 4096 samples. The problem is never the
arithmetic; it is everything GLM does not have.

## 5. Why not keep GLM around as a second opinion

A second math library only pays for itself if it can cross-check something.
GLM's overlap with our usage is the 8.5% of the FE inner loop that is 3×3 work,
plus geometry primitives that already agree to 1e-15, and its one non-trivial
routine is silently wrong on inputs we generate. Against that: a new dependency,
a conversion boundary at every mesh/FEA interface, `GLM_ENABLE_EXPERIMENTAL` in
the build, and two divergent idioms for the same operations.

The existing benchmark machinery also gives it nowhere to live. `bench/results/`
is scored against `bench/competitive/schema.json`, whose `case_id` and `accuracy`
fields mean physics accuracy on a named case; a math-library throughput number
cannot honestly populate them. That is why the probe writes plain text to
`docs/bench/` and stays off the scoreboard.

## Rejected

- **GLM as a replacement.** Cannot express dynamic-size or sparse matrices, has
  no solvers, no SVD/QR/LU, and no general or generalized eigensolver. §1.
- **GLM for the geometry layer only, Eigen for the solver.** `Eigen::` appears
  355 times in the public headers of five PUBLIC-linked libraries; the split
  lands a conversion boundary on every mesh/FEA call and buys a wash on
  throughput. §1, §2.
- **GLM's `findEigenvaluesSymReal` for the metric/stress eigensolves.** Silently
  returns ~100%-wrong eigenvalues below its hardcoded 1e-7 epsilon, on an input
  class `metric_field.cpp` constructs deliberately. §4.
- **A global `EIGEN_DONT_VECTORIZE`** to capture the §3 cross-product win. It
  would also de-vectorize the dense kernels that are 91.5% of the element loop.

## Consequences

- Eigen remains the single math library. No dependency is added; `src/` and
  `apps/` are unchanged by this ADR.
- `bench/mathlib/` holds a standalone, checksum-pinned probe so the numbers above
  can be re-measured rather than trusted. It fetches GLM into a scratch directory
  and is never part of the CMake build.
- `docs/bench/mathlib-probe.txt` is the committed run behind every number quoted
  here.
- Open, with a measured starting point: Eigen 3.5.0's vectorized
  `Vector3d::cross` costs 5.5× hand-written scalar code, in real per-face and
  per-triangle loops listed in §3. The remedy is site-local, not a global flag.
- `POLYMESH_NATIVE_ARCH` gains a second reason to stay off: `-march=x86-64-v3`
  made the same cross-product kernel slower (15.1 vs 12.9 ns/op).
