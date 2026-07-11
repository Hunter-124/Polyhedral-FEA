# ADR-0012: Hybrid meshing — graded tet + true mixed zoo

- Status: accepted (2026-07-10); amended with `mixed_fill` / `kHybrid` (multi-type);
  amended v4 (conforming fan transitions) + graded S4/S5 quality repair (2026-07-10)

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

## Amendment (v4): conforming 2:1 fan transitions

The v3 pyramid transition (subdivided faces only toward fine *face*-neighbors)
was **non-conforming**: a hanging mid-node on a coarse lattice edge propagates
to *every* cell sharing that edge, so v3 meshes cracked along cell edges —
unpaired `(c0,c1,apex)` vs `(c0,m,apex)+(m,c1,apex)` side triangles, exposed
interior apex nodes, and boundary extraction reporting faces deep inside the
solid (curved scorecard: hybrid 0.46 vs hex 0.85 on the sphere).

v4 rule: a mid-node exists on a coarse edge **iff some cell incident to that
edge is fine**. Every non-fine cell touching such an edge becomes a *fan* cell:
apex at the cell centre, each face emitted as its polygon (4 corners + the
hanging mid of every split edge) with a canonical min-node-id fan
triangulation. Both cells sharing a face build the same polygon and the same
fan, so every facet pairs — no cracks, no hanging nodes; the mesh gains a few
`tet4` next to the pyramids. Scorecard after fix: hybrid ≥ hex on sphere /
cylinder / hole plate.

## Amendment: graded tet S4 cap collapse + S5 void carve

Snapping all four corners of a skin tet onto a curved (or flat) wall leaves a
near-zero-volume cap (measured min boundary aspect ~1e-18 on the cylinder);
hole-void stair chords leave jut nodes ~0.25 h off-surface that no projection
can fix without inverting a tet. After snap the graded fill now runs:

- **S4 sliver-cap collapse** — conforming shortest-edge collapse of boundary
  caps (aspect < 0.05), accepting a collapse when incident tets stay valid and
  do not get worse (clusters heal stepwise);
- **S5 void carve** — peel the tets of pre-identified jut nodes (residual
  > 0.15 h) as they gain free faces (bounded to the juts' stars);
- a **second snap + repair round** so faces exposed by the carve reach the
  surface too.

Result: graded M1max ≈ 0 on all scorecard fixtures, min boundary aspect
≥ ~0.04, composite within 0.9× of hex (all-tet pays the M6 aspect term that a
pure-hex mesh never does).

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
