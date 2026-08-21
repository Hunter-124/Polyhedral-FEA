# ADR-0032: The mesh may not depend on which standard library built it

Status: accepted (2026-08-14); **amended 2026-08-21** (an unstable sort is the
same defect)
Date: 2026-08-14

## Context

The labelled advisor corpus was produced on a Windows laptop (MSVC). Replaying 24
of those `(part, cfg)` pairs on the 3080 Ti box (gcc 15, Release, same commit)
reproduced 19 exactly and disagreed on 5:

| part | mesher | MSVC | gcc |
| --- | --- | --- | --- |
| `sphere_box_s0_c0` | graded_tet | `solve_fail`, no mesh | `ok`, 21,256 elems |
| `stepped_shaft_s2_c0` | hybrid_zoo | 264 elems, err 0.0574 | 200 elems, err 0.0620 |
| `stepped_shaft_s0_c0` | graded_tet | 12,671 elems | 12,662 elems |
| `stepped_shaft_s2_c0` | graded_tet | 11,171 elems | 11,173 elems |
| `plate_notch_s0_c0` | graded_tet | 6,426 elems | 6,424 elems |

Two hypotheses were tested and rejected before the real one was found.

**Not run-to-run noise, and not threading.** `bench/campaigns/xcheck-omp1` and
`xcheck-omp12` re-ran the divergent parts at `OMP_NUM_THREADS` 1 and 12 and are
byte-identical to each other and to `xcheck-gcc-1`. Two consecutive CLI solves of
the smoke-gate command return the same `rel_err` to every digit.

**Not floating point.** gcc contracts `a*b+c` into an FMA by default and MSVC
`/fp:precise` does not, which is the obvious suspect. A full `-ffp-contract=off`
rebuild (`bench/campaigns/xcheck-fpoff`) reproduced the gcc numbers exactly — all
24 pairs, zero change. No FP flag explains it.

**The cause was iteration order over hash containers.** Several passes iterate a
`std::unordered_map` / `std::unordered_set` and feed that order into a *mutation*:

- `hybrid_fill.cpp` built the `free_faces` list handed to `smooth_boundary_nodes`
  by walking a face map. The smoother relaxes and re-projects nodes in the order
  it receives them and reverts moves that invert a tet, so an earlier node's
  accepted move decides whether a later one is legal.
- `hybrid_fill.cpp:tet_boundary_nodes` returned a node list in set order — the
  snap round's per-node accept/reject order.
- `surface_project.cpp` classified and re-projected boundary nodes in `nbr` map
  order, mutating shared per-node provenance as it went.
- `wall_project.cpp` built each node's neighbour list, and its relaxation sweep
  and residual sum, in face-map order.
- `varyhedron_fill.cpp:boundary_nodes` returned a set-ordered node list.
- `hp_assembly.cpp` assigned global mode index blocks by walking the edge, quad
  and tri order maps, so the DOF numbering — and therefore the iterative solve's
  rounding — was a function of the STL.

The bucket layout of a hash container is an implementation detail. Making mesh
geometry a function of it means the same source, the same inputs and the same
flags legitimately produce different answers on different platforms, and no
label produced on one machine is comparable to a label produced on another.

## Decision

**A mesher pass may use unordered containers for lookup, and may not let their
iteration order reach an output.** Where iteration drives a mutation, a
selection, or an index assignment, iterate a deterministically ordered sequence
instead — collect the keys, sort by id, walk that. Order-independent uses
(counting, membership, `min`/`max` reductions, building a set that is sorted
later) are unaffected and were left alone; each one is recorded as verified in
the commit that landed this.

Ordering keys are node ids, packed edge keys, or face-key tuples, ascending.
Never a floating-point score: that reintroduces the problem one rounding mode
later.

## Consequences

- `scripts/check_cross_stdlib_mesh.sh` builds the CLI with libstdc++ and with
  libc++ and requires byte-identical meshes on four part/mesher pairs. It runs in
  CI as the `cross-stdlib-determinism` job. libc++ hashes and buckets differently
  from libstdc++, so it detects the reintroduction directly, with no Windows
  runner. Measured on the fixed tree: all four pairs identical.
- `POLYMESH_WERROR` is now an option (default ON). libc++ raises a different
  `-Wconversion` set, unrelated to the property under test; the gcc job keeps
  warnings as errors.
- Two portability defects surfaced while getting libc++ to build and were fixed:
  lambdas in `prism_fill.cpp` and `mixed_fill.cpp` deduced a return type from
  both `bool` and a `std::vector<bool>` proxy reference, and `p_elevate.cpp`
  called `determinant()` on a dynamic-size product without including `Eigen/LU`.
- **Existing labels are stale where the fix changed the answer.** On the 24-pair
  probe the fix moved 2 pairs, one of them onto the MSVC value
  (`plate_notch_s0_c0` graded_tet, 6,424 -> 6,426), which is corroboration rather
  than coincidence. 20 of 24 pairs now agree with the MSVC labels, up from 19.
- Four pairs still differ from MSVC and cannot be diagnosed further here: the
  two Linux toolchains now agree bit for bit, so whatever remains is MSVC-side
  (libm, or an OCC version difference on the laptop) and needs that machine to
  investigate. Labelling standardises on gcc for this reason —
  `docs/training/ACCESS-hunter-pc.md` §4.1.
- 409/409 ctest pass on the fixed tree.

## Amendment 2026-08-21: an unstable sort is the same defect

`cross-stdlib-determinism` failed on `plate_hole` `hybrid` while the other three
pairs stayed byte-identical. No hash container was involved. `feature_pin.cpp`
ordered each sharp-edge chain on its arclength parameter alone:

```cpp
std::sort(chain.begin(), chain.end(),
          [](const Pinned& a, const Pinned& b) { return a.t < b.t; });
```

`std::sort` is not stable and that comparator is not total, so which of two nodes
with equal `t` comes first is an implementation detail — the same class of detail
as a bucket layout. The ties are not rare: on this part the box rims carry 3–88
tied pairs per chain and the two bore rims 2–6. For a *closed* chain the Fourier
re-spacing immediately below assigns each node the target at its **index** in
this order, so transposing two tied nodes moves both of them.

Measured at h = 4 mm: g++/libstdc++ pinned 1868 edge nodes and refused 16,
clang/libc++ pinned 1867 and refused 17, and the two meshes differed in 732 node
positions, worst 0.44 mm. A third build — **clang with libstdc++** — came out
byte-identical to g++ (same md5), which isolates the standard library from the
compiler and rules out floating point a second time.

The fix is `std::stable_sort`. `chain` is built by walking `candidates`, which
`sort_mirror_canonical` has already put in mirror-canonical order, so stability
removes the platform dependence *and* keeps a node adjacent to its mirror image
(ADR-0036). A node-id tie-break would have done only the first: ids do not
mirror.

The decision therefore extends: **any ordering an output depends on must be
total.** A comparator that leaves ties, sorted unstably, is exactly as
platform-dependent as iterating a hash map. Where the sequence is already
canonical, `std::stable_sort` is the cheaper statement of the same requirement.

Consequences: the gcc answer moved by one pin (1868 -> 1867, onto the libc++
value), the BRep fill error improved (`rel_err` 4.842e-05 -> 4.82e-05), all four
gate pairs are byte-identical, and 451/451 ctest pass.
