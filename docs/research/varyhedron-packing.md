# Varyhedron packing — algorithm survey (V5)

- Status: accepted for v1 path (2026-07-12); **ranking pivoted** by
  [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md)
- Related: [ADR-0021](../decisions/0021-varyhedron-packing.md), [ADR-0020](../decisions/0020-brep-volume-meshing.md),
  [ADR-0019](../decisions/0019-mixed-fe-vem-adaptive-order-core.md), [ADR-0002](../decisions/0002-license-bsd3.md)
- Microbench: [`bench/packing/run_packing_microbench.py`](../../bench/packing/run_packing_microbench.py)

This note surveys volume-meshing *packing* families relevant to **varyhedron**
(variable polyhedral cells driven by CAD edge/face constraints) and records the
**v1 algorithm choice**. It is intentionally implementation-oriented: what we
can reimplement under BSD-3 vs wrap as optional plugins.

---

## 0. Normative ranking (ADR-0023 — do not ignore)

**Substrate to keep:** protected seeds + graded tet scaffold + live BRep oracle
(sizing/snapping/conformity query OCC throughout). Algorithm family is
secondary to the oracle property.

| Rank | Family | Role for us |
| --- | --- | --- |
| 1 | **Weighted restricted CVT** + clipped Voronoi (Yan–Wang–Lévy; Geogram BSD-3) | Target seed relaxation + **polyhedra = clipped cells** (skip fragile dual for interior) |
| 2 | Lattice + clip | Fast bulk fallback |
| 3 | Advancing front | Boundary layers only if we invest later |
| 4 | Raw bubble packing | **Keep seeds**; replace dynamics with CVT iterations |
| deferred | Frame-field hex-dominant | Multi-year robustness swamp — research, not core bet |
| gated | Dual-of-tet → VEM | Ship as poly export only; **headline only if M5 gate** beats hybrid_zoo |

**Protect only sharp CAD edges** (G0). OCC seams and smooth dihedrals must not
get protecting balls. Walls = free-sliding nodes + tangential smooth + surface
project.

**Measure before packing loops:** probes, CAD-tagged BC/probe, chordal
efficiency + normal deviation (program nodes M1–M2). Wireframes are artifacts,
not rewards.

---

## 1. Goals (from ADR-0021)

| Goal | Implication for packing |
| --- | --- |
| Clean CAD edge *profiles* within element budget | Protecting balls on **sharp** edges only; residual metrics M2 |
| Variable polyhedra, not layered N-hedra | Prefer clipped CVT cells; dual-of-tet is fallback/legacy idea |
| Tet FE default claim; VEM gated | Poly export optional until M5 |
| BSD-3 core | Geogram/Shewchuk/Verdict OK; Gmsh/CGAL/TetGen code stay plugins |

---

## 2. Bubble / sphere packing → Delaunay

### Idea

Place *bubbles* (spheres) with target radii from a size field \(h(\mathbf{x})\).
Neighbor forces (repulsion when overlapping, mild attraction when gaps are
large) relax centers to a stable packing. The Delaunay tetrahedralization of
bubble centers (optionally constrained by the boundary PLC) yields a quality
tet mesh; further dualization yields polyhedra.

Canonical reference line: **Shimada & Gossard** “bubble mesh” / sphere packing
for automatic mesh generation; many later CAD-to-mesh pipelines reuse the same
seed + Delaunay skeleton.

### Pros

- Size field maps cleanly to bubble radius → graded meshes without Cartesian
  layers.
- Good dihedral statistics when packing equilibrates.
- Natural place to inject **boundary-edge seeds** (protect CAD profiles).

### Cons / costs

- Force relaxation needs careful damping and collision neighborhoods (spatial
  hash / Verlet lists).
- Pure packing does not by itself enforce CAD faces; must couple to a
  surface/PLC constraint loop.
- 3D packing quality is sensitive to initial seeds and \(h\) contrast.

### Fit for varyhedron

**Primary seed engine for v1 boundary + interior centers.** We do *not* need a
full published bubble-mesh reimplementation on day one: a simpler constrained
seed pack (grid + jitter + edge samples + optional few Lloyd/bubble iterations)
is enough to drive a tet scaffold.

---

## 3. Dual-of-tet polyhedra (cfMesh / polyDualMesh lineage)

### Idea

1. Build a (constrained) tetrahedralization of the volume.
2. Form the **dual**: one polyhedral cell per tet vertex (or per dual site),
   faces dual to tet edges/faces. Variants:
   - **Voronoi dual** of tet vertices (true Voronoi if sites are Delaunay).
   - **polyDualMesh-style** dual used in OpenFOAM: agglomerate dual cells,
     optionally clip to domain.
   - **cfMesh / cartesian dual hybrids**: start from tet or hex background,
     dualize or agglomerate into general polys for FV/VEM.

### Pros

- Always fills the volume if the tet mesh does (no void-carving drama).
- Arbitrary polyhedra are first-class → matches **`kPolyVem`** (ADR-0019).
- CAD constraints reduce to **constrained Delaunay / PLC tet** quality, a well
  studied problem.
- Incremental path: improve tet scaffold → dual quality improves.

### Cons

- Dual of a bad tet is a bad poly (skinny dual faces, high face count).
- Boundary faces of the dual must still align with CAD surfaces/edges
  (protecting balls + surface recovery).
- Not field-aligned hex in prismatic bulk (that is a later upgrade path).

### Fit for varyhedron

**v1 interior representation:** dual / cluster of a refined **constrained tet
scaffold**, with CAD edge protection so dual boundary edges track BRep edges.
Regular zoo cells (hex/prism/tet) may still appear where the dual collapses to
them; VEM covers the rest.

---

## 4. Field-aligned hex-dominant (PGP3D-class)

### Idea

Estimate a **frame field** (or cross field) from geometry (sharp features,
curvature, medial directions). Integrate a **periodic global parameterization**
(PGP / CubeCover / Instant Meshes 3D lineage) so integer iso-surfaces of the
param define hex-dominant cells. Transitions use pyramids/prisms/tets or
polyhedra.

### Pros

- Best-in-class hex alignment in prismatic / swept regions.
- Element budgets often beat isotropic tet for the same accuracy on
  structural parts.

### Cons

- Frame-field + PGP is a large R&D surface (robustness on dirty CAD, singularities,
  feature curves).
- Harder license story if vendoring research codes.
- ADR-0021 **rejects** “full frame-field hex-dominant as the *only* v1 path.”

### Fit for varyhedron

**Post-v1 upgrade**, not blocking. Keep interfaces (size field, edge protect,
quality metrics) compatible so a PGP-class packer can later replace or
supplement the dual-poly interior in selected regions.

---

## 5. CAD edge protecting balls / PLC constraints

### Idea

A **piecewise linear complex (PLC)** encodes vertices, edges, and faces that
must appear in the mesh. **Protecting balls** (or protecting segments) around
CAD edges guarantee that no Steiner point falls too close to a constrained
edge, so the edge survives Delaunay refinement (CGAL Mesh_3, TetGen-style CDT
refinement literature).

For varyhedron specifically:

- Sample CAD edges at spacing ~ \(h(s)\) along arc length.
- Place protecting radii \(r \propto h\) so dual/tet edges lock to the profile.
- Faces get surface seeds + curvature-aware \(h\) (node V4 size field).
- **Boundary residual** (Hausdorff of mesh edge polylines vs BRep edges) is the
  owner-facing metric (V6b); packing microbench only stubs a geometric proxy.

### Fit for varyhedron

**Non-negotiable for v1.** Without edge protect, dual polys will chord-cut
fillets and hole rims the same way graded lattice layers do.

---

## 6. Licensing notes (core vs plugin)

Project license: **BSD-3-Clause** ([ADR-0002](../decisions/0002-license-bsd3.md)).

| Source / idea | Typical license | Guidance for PolyMesh |
| --- | --- | --- |
| Bubble packing *ideas* (Shimada-class papers) | Academic / implement yourself | **Reimplement** under BSD-3 in `src/mesh/` |
| Dual-of-tet / Voronoi poly ideas (polyDualMesh lineage) | OpenFOAM is GPL; algorithms are classical | **Reimplement** dual/cluster; do not vendor OF |
| cfMesh | GPL | Ideas only; no core vendor |
| TetGen | **AGPL** | **Do not link into core.** Optional external process plugin only if owner accepts AGPL boundary and docs it |
| Gmsh | **GPL** | Already used as *import format* / external tool historically; do not embed GPL kernel in `src/` |
| CGAL Mesh_3 | GPL / commercial dual | Prefer OCC BRep + our CDT/scaffold; no CGAL in core without license review |
| NetGen | LGPL | Possible optional plugin; still prefer in-tree BSD path for varyhedron |
| PGP / frame-field research codes | Mixed (often research / GPL) | Reimplement or plugin; not v1 |

**Policy (normative for Lane V):**

1. Core library packing/meshing code is **BSD-3 reimplementation**.
2. Optional plugins may wrap third-party meshers; license must be documented in
   the PR and never silently pull AGPL into the default binary.
3. Papers and classical computational geometry (Delaunay, Voronoi, PLC
   refinement) are fair game; copy-paste from AGPL/GPL trees is not.

---

## 7. Decision: v1 algorithm

**Chosen v1 path** (confirms and slightly sharpens ADR-0021 §3):

1. **Map CAD edges/faces** (ADR-0020 / CadModel) and build a size field
   (node V4).
2. **Boundary-edge constrained seed packing** — sample and protect CAD edge
   profiles with protecting balls; pack surface and volume seeds to \(h\).
3. **Refined constrained tet scaffold** from those seeds (in-tree or
   BSD-licensed CDT path; no TetGen-in-core).
4. **Dual / cluster → variable polyhedra** for interior cells; keep FE zoo
   cells where the dual is regular enough.
5. **Solve with VEM** on general polys (`kPolyVem`); FE on tet/hex/prism/pyramid.
6. **Order packing (p)** later (V6d): hierarchical min-rule / same-order faces.

Explicitly **deferred** from v1:

- Full PGP3D / frame-field hex-dominant as the sole interior engine.
- Vendoring TetGen, Gmsh kernel, or cfMesh into `src/`.
- Pure sphere packing without a tet scaffold (quality too brittle early).

### Pipeline sketch

```
BRep (OCC CadModel)
  → edge/face samples + protecting balls
  → volume seeds (packing / jittered lattice + few relax steps)
  → constrained tet scaffold
  → dual / cluster poly mesh
  → quality.json (incl. boundary edge residual, V6b+)
  → FE+VEM assemble
```

### Microbench (this node)

[`bench/packing/run_packing_microbench.py`](../../bench/packing/run_packing_microbench.py)
is a **pure-Python** seed-packing demo (no FEA, no OCC). It reports wall time,
a fill-fraction proxy (packed ball volume / domain volume), and a **boundary
residual placeholder** so later V6b can replace the stub with true CAD
Hausdorff. Output: `bench/packing/out/summary.json`.

---

## 8. Open questions for agent loops (V6 / V11)

- How many bubble-relax iterations buy dual quality vs tet refine alone?
- Prefer vertex-Voronoi dual vs agglomerated polyDualMesh-style cells for VEM?
- When to insert FE hex blocks inside dual regions (hybrid_zoo interplay)?
- Edge residual vs element budget trade curve on plate_hole / icecream_cone.

---

## 9. References (anchors, not exhaustive)

- Shimada & Gossard — bubble/sphere packing mesh generation.
- OpenFOAM `polyDualMesh` — dual polyhedral mesh from tet/hex.
- cfMesh — Cartesian / polyhedral meshing (GPL; ideas only).
- CGAL Mesh_3 — protecting balls, PLC, Delaunay refinement.
- PGP / CubeCover / Instant Meshes 3D — field-aligned hex-dominant.
- ADR-0021 research anchors (same four families as §§2–5 above).
