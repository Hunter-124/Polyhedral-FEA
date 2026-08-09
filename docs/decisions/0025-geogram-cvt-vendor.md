# ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)

- Status: accepted; G1–G4 implemented, `cvt_poly` remains experimental (2026-08-09)
- Decision: D25
- Related: ADR-0023 (strategy), ADR-0024 Q3/Q8 (normative answers), ADR-0002 (BSD-3)
- Full study path: [docs/research/geogram-cvt-vendoring.md](../research/geogram-cvt-vendoring.md)
- Program: [docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md) §4.4–§4.5; board **G0–G4**

## Context

Weighted restricted CVT with **clipped Voronoi cells** is the ranked packing
path (ADR-0023). Clipped Voronoi is a hard numerical/combinatorial kernel;
clean-room reimplementation is multi-week risk for a small team. Advisor
answers (ADR-0024) freeze vendoring Geogram BSD-3 hard parts and hard-blocking
median dual until clipped cells exist.

The original dependency gate was M9 baseline freeze → M10 owner-aware wall
projection → G1–G4. Those nodes have landed. The clipped-RVD path is exposed as
the opt-in `cvt_poly` mesher while its accuracy/efficiency gate remains active.

## Decision

### 1. Vendor Geogram (BSD-3) for hard parts (ADR-0024 Q3)

| Vendor | Write ourselves |
|--------|-----------------|
| Predicates / exact-arithmetic layer | Thin Lloyd loop |
| ConvexCell / clipped-Voronoi kernel | Density \(\rho = 1/h(x)^3\) from **same** size field as N_pred |
| Delaunay only if G2–G4 need it | Constrained sharp sites + OCC bridge |

Do **not** reimplement clipped Voronoi from scratch. Do **not** vendor the whole
Geogram tree (strip GUI / unused formats). Afternoon-scale hygiene (namespace,
subset, LICENSE/NOTICE) is the expected cost.

### 2. Dual hard-block (ADR-0024 Q8)

**Hard-block** median dual / dual-of-tet as the product poly path until **G4**
exports clipped restricted-Voronoi cells. Optional ~2-day experimental dual
export only if external/marketing demand, flagged experimental; otherwise skip
the dualizer entirely. Clipped cells *are* the polyhedra.

### 3. `third_party/` plan

Layout and checklist live in the research note (normative for G1 PR):

- **Study + vendor path:** [docs/research/geogram-cvt-vendoring.md](../research/geogram-cvt-vendoring.md)
- **Vendored subset:** `third_party/geogram/` contains the BSD-3 license,
  pinned NOTICE, integration README, predicates, ConvexCell support, and the
  Delaunay subset used by the experimental path.
- G1 PR must copy upstream **LICENSE** (BSD-3), **NOTICE** (pin tag/commit/URL),
  and `README.polymesh.md` (included vs stripped modules). Project remains
  BSD-3-Clause (ADR-0002); no AGPL/GPL mesh kernels in the default core binary.

### 4. Order (do not invent another)

1. **M9** freeze baseline (done)  
2. **M10** wall tangential smooth + OCC surface project  
3. **G1** vendor predicates + ConvexCell / clip  
4. **G2** Lloyd + \(1/h^3\) density  
5. **G3** constrained sites + OCC bridge  
6. **G4** clipped poly export → unblocks honest poly VEM experiments (**M5** gate)

Frame-field hex remains research-only (ADR-0023). Measure packing deltas only
against the M9 freeze.

## Consequences

- Geogram hard parts are isolated behind the `mesh` facade; project code does
  not spread vendor headers through the pipeline.
- RVD clipping and welding use a domain-local frame. Canonical welded vertex-ID
  topology pairs faces, while explicit tetra-face provenance distinguishes
  domain skin from internal scaffold cuts. Invalid multiplicity, site
  ownership, winding, unmatched bisectors/cuts, incomplete scaffold-volume
  coverage, and disconnected single-cell shells are hard failures;
  disconnected restricted regions become separate cells.
- Simple planar source faces, consistently oriented closed shells, cross-cell
  intersections/containment, positive volume, and post-projection domain-volume
  preservation are checked fail-closed. Failed or implausibly sparse RVD
  assembly never replaces the requested solid with its axis-aligned bounding
  box.
- VEM headline promotion remains gated on beating `hybrid_zoo` on frozen
  plate-hole and cylinder evidence. `cvt_poly` is therefore still experimental.

## Alternatives rejected

| Option | Why rejected |
|--------|----------------|
| Clean-room clipped Voronoi | Schedule swamp; Geogram already BSD-3 |
| Dual-first poly now | ADR-0024 Q8 hard-block until G4 |
| Full Geogram blob / system package only | Non-reproducible; unused modules bloat |
| GPL meshers (Gmsh, CGAL Mesh_3) in core | Forbidden — compare plugins only |

## Implementation status

G0–G4 are implemented. Promotion still requires reporting topology invariants,
exact directional BRep/mesh fidelity metrics, load-area health, analytical
error, DOF, and wall time together; a lower polyhedron count alone is not a win.
