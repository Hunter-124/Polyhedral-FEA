# ADR-0012: Hybrid meshing — graded tet + true mixed zoo

- Status: accepted (2026-07-10); amended with `mixed_fill` / `kHybrid` (multi-type)

## Context
True hex-core + tet-skin needs **conforming** shared faces. A hex face is a
quad; a tet face is a triangle. Naive mixed isoparametric-hex + tet is
nonconforming. Pyramid transitions (ADR-0013) need matching face spaces too.

## Decision (v1 — graded all-tet)
Ship **`graded_tet_fill`** as the size-graded all-tet path (coarse bulk /
fine skin + feature/seed bands). GUI/CLI: **“graded tet”**.

## Decision (v2 — SPEC hybrid zoo)
Ship **`mixed_fill_surface`** / `VolumeMesher::kHybrid` as the multi-type path:

| Zone | Elements |
|------|----------|
| Bulk (deep interior) | hex8 |
| Free-surface / feature / curvature skin | Kuhn 6× tet4 per cell |

**FE conformity:** when a mesh contains both hex and tet (or pyramid), hex8
stiffness is assembled as the **same Kuhn 6-tet split** used by the skin
(`fea/assembly.cpp`). Shared lattice faces then share the same face diagonal,
so the constant-strain patch test is exact with **true multi-type** elements
(no hex→pyramid expand required for the hybrid zoo).

Pure-hex meshes (no tets) keep GATE-1 isoparametric trilinear hex.

Surface snap (≤0.9 h, multi-pass, greedy unsnap) on free-boundary lattice
nodes softens curved walls within ADR-0015 grid limits.

Default product mesher (GUI/CLI) is **`kHybrid`**.

## Related mono paths
| Mesher | Role |
|--------|------|
| tet / graded tet | All-tet (uniform or size-graded) |
| hex / hex VEM | All-hex or poly-VEM hex |
| hex+pyramid | Hex core + pyramid skin (ADR-0013 expand for tet-split product FE) |
| prism | All-prism6 Cartesian sweep |

## Deferred
- Prism wedges auto-inserted in sweepable bulk of the hybrid zoo
- Pyramid transition ring between hex and tet (optional; not required for Kuhn PL)
- General n-hedron VEM cells for irregular leftover regions
- True CAD-fitted / Delaunay boundary (ADR-0015)

## Alternatives rejected for now
- Non-conforming hex/tet with hanging nodes — needs mortar
- Always expand hex→pyramid for hybrid — hides multi-type in the product mesh
