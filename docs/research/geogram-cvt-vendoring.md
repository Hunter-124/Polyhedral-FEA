# Geogram / restricted CVT — vendoring study path

- Status: study / pre-implementation (do not start coding G1 before freeze + wall project)
- Normative: [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md),
  [ADR-0024](../decisions/0024-advisor-measure-answers.md) Q3/Q8/Q10
- Program: [docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md)
  §4.5, §5; board nodes **G0–G4**, deps **M9**, **M10**
- Related research: [varyhedron-packing.md](varyhedron-packing.md),
  [protecting-balls-lfs.md](protecting-balls-lfs.md),
  [campaign-metrics.md](campaign-metrics.md)

This note is the agent-facing **full study + vendor path** for weighted restricted
CVT with clipped Voronoi cells. It freezes *what* we take from Geogram, *what*
we write, *when* we start, and the license/PR checklist. It is **not** a CMake
implementation PR.

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

**Do not** land median / polyDualMesh-style dual-of-tet as the product poly path
until **G4** clipped cells exist. Optional ~2-day experimental dual export only
if external/marketing demand, flagged experimental; otherwise skip the dualizer
entirely (ADR-0024 Q8). Clipped restricted-Voronoi cells *are* the polyhedra.

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

Tet FE remains the default accuracy claim until M5.

---

## 5. Suggested `third_party/` layout (not full CMake yet)

Illustrative tree for the G1 PR — adjust names when vendoring, keep the
invariants: **LICENSE visible**, **namespace isolated**, **subset only**.

```
third_party/
  geogram/                    # or geogram_clip/ for a stripped subset
    LICENSE                   # upstream BSD-3 text, unmodified
    NOTICE                    # our pin: version/tag/commit, date, URL
    README.polymesh.md        # what we took, what we stripped, how to upgrade
    src/                      # vendored sources (predicates, ConvexCell, …)
    include/                  # public headers we consume
  shewchuk_predicates/        # optional; public domain — if used instead of/with Geogram
    README.polymesh.md
    predicates.*
```

**CMake note (when G1 lands — not this doc’s job to implement):**

- Add an optional or hard internal target, e.g. `polymesh_geogram_clip`, built
  only from the vendored subset.
- Prefer `add_subdirectory(third_party/geogram …)` or explicit file lists over
  system Geogram (reproducible builds).
- Do **not** link GPL/LGPL mesh kernels into the default core binary.
- Expose a thin C++ facade in `src/mesh/` (e.g. clipped cell API) so callers
  never include Geogram headers across the whole tree.
- Namespace: wrap or rename if needed to avoid ODR clashes; document in
  `README.polymesh.md`.

---

## 6. License checklist for the vendoring PR

Project license: **BSD-3-Clause** ([ADR-0002](../decisions/0002-license-bsd3.md)).

Before merge of G1 (or any Geogram-touching PR):

- [ ] Upstream **LICENSE** (BSD-3) copied into `third_party/geogram/LICENSE`
- [ ] **NOTICE** with exact upstream tag/commit SHA, fetch URL, and date
- [ ] `README.polymesh.md` lists included vs excluded modules
- [ ] No AGPL/GPL sources mixed into the vendored tree
- [ ] SPDX on *our* glue files remains BSD-3; do not relicense upstream files
- [ ] Root or `THIRD_PARTY.md` (if present) mentions Geogram + Shewchuk if used
- [ ] CI builds with the vendored subset on the default OCC-on configuration
- [ ] Dual/export path still **hard-blocked** or explicitly experimental flag
- [ ] Density path documented: same \(h(x)\) as N_pred
- [ ] No force-push; no AI attribution trailers on the commit

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
- G2 without shared \(h(x)\) with N_pred is a **bug**.  
- Dual-first poly is **blocked** until G4.  
- Frame-field hex is research-only (ADR-0023).  
- Measure deltas only against **M9** frozen baseline.  
- Wireframe PNGs are never the reward signal.  
