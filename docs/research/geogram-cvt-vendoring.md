# Geogram / restricted CVT — vendoring study path

- Status: G1–G4 implemented; opt-in `cvt_poly` remains experimental (2026-08-09)
- Normative: [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md),
  [ADR-0024](../decisions/0024-advisor-measure-answers.md) Q3/Q8/Q10
- Program: [docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md)
  §4.5, §5; board nodes **G0–G4**, deps **M9**, **M10**
- Related research: [varyhedron-packing.md](varyhedron-packing.md),
  [protecting-balls-lfs.md](protecting-balls-lfs.md),
  [campaign-metrics.md](campaign-metrics.md)

This note records both the historical vendoring study and the as-built weighted
restricted-CVT integration: what was taken from Geogram, what remains project
glue, the topology/admission contract, and the reproducibility/license record.

---

## 1. Why Geogram BSD-3 (not clean-room clipped Voronoi)

**Decision (ADR-0024 Q3):** vendor Geogram hard parts; **do not** reimplement
clipped Voronoi from scratch.

| Option | Verdict |
|--------|---------|
| Vendor Geogram (BSD-3) predicates + ConvexCell / clip kernel | **Chosen** — afternoon of hygiene; industry-proven clip robustness |
| Clean-room clipped Voronoi against BRep | **Rejected** — multi-week combinatorial/numerical swamp for a small team |
| Dual-of-tet → poly as primary path | **Hard-blocked** until clipped cells exist (ADR-0024 Q8) |
| GPL meshers (Gmsh, CGAL Mesh_3) in core | **Forbidden** — compare/benchmark plugins only (ADR-0023 §6) |
| TetGen / AGPL in core | **Forbidden** |

Clipped Voronoi is the hard kernel (exact predicates, cell clipping to a
polyhedral/BRep-restricted domain, numerical edge cases on nearly coplanar
faces). Lloyd iteration, density weighting, and our OCC site constraints are
thin application glue. Reimplementing the hard kernel while we still lack a
frozen baseline and wall projection is pure schedule risk.

Academic lineage for the *idea*: Yan–Wang–Lévy restricted CVT / clipped Voronoi;
Geogram is the practical BSD-3 implementation surface.

---

## 2. What to vendor vs what we write

### 2.1 Vendor from Geogram (BSD-3)

| Piece | Why |
|-------|-----|
| **Predicates** (orient/insphere-class, or Geogram’s exact-arithmetic layer) | Numerical backbone of Delaunay/Voronoi; also pairable with Shewchuk public-domain predicates if stripped |
| **ConvexCell** / clipped-Voronoi kernel | Produces the polyhedral cells we actually want as product polys |
| **Delaunay** (optional) | Useful scaffold or dual helper; pull only if G2–G4 need it — do not vendor unused modules |

Strip ruthlessly: no GUI, no unused formats, no whole “everything Geogram”
blob. Prefer a minimal subset under a clear namespace (see §5).

### 2.2 We write ourselves

| Piece | Notes |
|-------|-------|
| **Thin Lloyd loop** | Site ← centroid of weighted restricted cell; iterate; stop on energy/movement |
| **Density \(\rho = 1/h(x)^3\)** | **Must** use the **same** size field \(h(x)\) as **N_pred** (ADR-0024 Q10 trap #4). Mismatch → over/under-resolve vs budget diagnosis lies |
| **Constrained sharp sites** | Protecting-ball / sharp-edge sites stay **fixed** during relaxation (ADR-0023 §3–4) |
| **OCC bridge** | Project free wall sites to BRep surface; closest-on-edge for sharp curves; never protect OCC seams |
| **Integration** | Seed init from existing bubble/lattice pack; export clipped cells into mesh DS; quality + campaign scorecard |

### 2.3 Dual hard-block

Median / polyDualMesh-style dual-of-tet remains excluded from the product path.
G4's clipped restricted-Voronoi cells are available only through the
experimental `cvt_poly` mesher (ADR-0024 Q8); no median-dual fallback is used.

---

## 3. Dependency order (do not invent another)

Compressed path from ADR-0024:

1. Measure honest (probes, scorecard, chordal \(e\), lfs balls) — Lane **M**
2. **M9 freeze baseline** campaign on honest scorecard
3. **M10 wall** tangential smooth + OCC surface project
4. **G1** vendor Geogram → **G2** Lloyd CVT → **G3** constrained sites + OCC → **G4** poly = clipped cells
5. VEM earns headline only at **M5** gate (beats `hybrid_zoo` on frozen plate_hole + cylinder)

| Node | Title | Blocks |
|------|-------|--------|
| **M9** | Freeze baseline | All packing “wins” and M10/G work measured as deltas |
| **M10** | Wall project + OCC re-project | Shared post-pass; G1 deps include M10 |
| **G0** | Docs/ADR note + LICENSE intent | This file + dual hard-block statement |
| **G1** | Vendor predicates + ConvexCell / clip | deps: G0, **M10** |
| **G2** | Lloyd + \(1/h^3\) density | deps: G1, M4 (N_pred field) |
| **G3** | Constrained sites + OCC bridge | deps: G2, M10 |
| **G4** | Clipped poly export | deps: G3; unblocks honest poly VEM experiments |

**Rule:** never start CVT / Geogram integration before **M9** freeze and **M10**
projection land. Pre-freeze CVT hours are unmeasurable.

---

## 4. Packing context (how this sits in varyhedron)

Current substrate (keep):

- Sharp-only protecting balls + graded tet scaffold + live BRep oracle
- Bubble/lattice seeds as **sites**; dynamics replaced by CVT iterations

Target ranking (ADR-0023):

1. Weighted **restricted CVT** + clipped Voronoi ← this path  
2. Lattice + clip (bulk fallback)  
3. Advancing front (layers later)  
4. Raw bubble dynamics (seeds only)

Tet FE remains the default accuracy claim until the packed-poly promotion gate passes.

### As-built RVD assembly and admission contract

In `clip=rvd_tet` mode, `cvt_poly` treats the tetrahedral domain as an
integration scaffold rather than permanent packed-cell topology:

1. Intersections \(V_i \cap T_j\) are emitted in deterministic `(site,tet)`
   order after parallel clipping. Floating aggregates are reduced in that same
   order, and a regression verifies an actual multi-thread OpenMP team against
   one-thread output field-for-field.
2. Clipping runs in a domain-local coordinate frame. Vertices are Euclidean
   tolerance-welded (`1e-9` of the domain diagonal) through the 27 neighbouring
   quantization buckets, with range-checked coordinates and deterministic
   lowest-ID selection.
3. Exact tetra-face multiplicity classifies domain skin separately from internal
   scaffold cuts. Shared faces use canonical welded vertex-ID sets: a valid
   second claim must have the expected provenance/site pair and opposite
   winding. Third claims, unmatched cuts/bisectors, or other ownership failures
   are rejected.
4. Exact same-site internal cuts define connected components and then cancel.
   Disconnected components of one restricted site region become separate VEM
   cells instead of one invalid disconnected shell. Edge-connected fragments
   between the same two cells are unioned into polygon loops.
5. Raw clipped volume must cover the positive tetra scaffold. Empty face/vertex
   tombstones are compacted, but referenced vertices still carry three \(k=1\)
   VEM displacement DOFs, so net DOF is measured rather than inferred.
6. Original polygons are admitted before faces incident to exterior vertices
   are triangulated. Source and triangulated meshes must have unique simple
   planar faces, closed connected consistently oriented positive-volume cells,
   no intra- or cross-cell intersection/overlap/containment, and a closed
   manifold exterior.
7. Optional CAD projection is all-or-nothing. It must retain the geometry
   contract and post-VEM volume within \(10^{-4}\) relative to the authoritative
   scaffold; otherwise the already surface-snapped scaffold coordinates are
   restored and rechecked.
8. Exterior ownership is resolved on whole polygons before rendering/fidelity
   triangulation. Exact owner groups survive only when singly claimed; n-gons
   cancel against any coplanar, oppositely oriented, non-overlapping partition
   using atomized directed edges, including straight-through split-edge nodes.

This is a topology cleanup, not a DOF or accuracy claim. It removes artificial
same-site tet-cut faces and compacts only vertices that become unreferenced. The
exporter itself preserves exterior coordinates; later boundary projection may
move them only through the admission rule above. Analytical error, DOF, solve
time, and bidirectional BRep evidence remain joint promotion gates.

---

## 5. Vendored `third_party/` layout

The implemented, reproducible subset is:

```
third_party/geogram/
  CMakeLists.txt
  LICENSE
  NOTICE
  README.md
  README.polymesh.md
  delaunay/
    LICENSE
    Delaunay_psm.cpp
    Delaunay_psm.h
  predicates/
    LICENSE
    Predicates_psm.cpp
    Predicates_psm.h
```

`third_party/geogram/CMakeLists.txt` builds the internal static
`polymesh_geogram` target and exports the `polymesh::geogram` alias. The optional
`polymesh_geogram_predicates` target isolates the predicates-only PSM. Project
callers use the thin `mesh` facade; vendored headers do not spread through the
pipeline. Exact-rounding compiler flags are scoped to these third-party targets.

---

## 6. License and integration record

Project license: **BSD-3-Clause** ([ADR-0002](../decisions/0002-license-bsd3.md)).

- [x] Upstream BSD-3 `LICENSE` is present at the subset root and in both PSM
  directories.
- [x] `NOTICE` records the upstream version/source and vendoring context.
- [x] `README.polymesh.md` records included and excluded modules.
- [x] No AGPL/GPL mesh kernel is linked into the default core binary.
- [x] Project glue retains BSD-3 SPDX headers; upstream headers retain upstream
  notices.
- [x] Default OCC-on builds compile the vendored `polymesh::geogram` target.
- [x] The density path and shared \(h(x)\) / `N_pred` rule are documented.
- [ ] Any future upstream refresh must repeat the license/source audit and the
  deterministic topology regression suite.

---

## 7. Study reading (order)

1. This note + ADR-0023/0024 + program plan §4.5 / §5  
2. Yan–Wang–Lévy restricted CVT / clipped Voronoi papers  
3. Geogram source: ConvexCell, clip, predicates — skim call graph before copy  
4. Cheng–Dey–Shewchuk protecting balls (sites that stay constrained) — see
   [protecting-balls-lfs.md](protecting-balls-lfs.md)  
5. Campaign metric roles — see [campaign-metrics.md](campaign-metrics.md)  

---

## 8. Agent anti-confusion

- G1 is **not** “rewrite Voronoi.”
- G2 without shared \(h(x)\) with `N_pred` is a **bug**.
- Median dual remains excluded; G4's clipped-cell path is available only as
  experimental `cvt_poly`.
- Frame-field hex is research-only (ADR-0023).
- Measure deltas only against the **M9** frozen baseline.
- Wireframe PNGs are never the reward signal.
