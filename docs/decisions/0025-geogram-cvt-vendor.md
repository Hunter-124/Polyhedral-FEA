# ADR-0025: Vendor Geogram hard parts for restricted CVT (dual hard-block)

- Status: accepted (2026-07-13)
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

G0 is **docs + LICENSE intent only**. Code vendoring is **G1** and still waits
on **M10** (wall tangential project) after the **M9** baseline freeze.

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
- **Placeholder:** `third_party/geogram/README.md` — **not vendored yet**; G1
  after M10 only.
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

- G0 (this ADR + placeholder tree) may land early; **G1 code still deps M10**.
- Product poly path waits for G4; dual remains blocked until then.
- VEM headline claim stays gated on M5 (beat `hybrid_zoo` on frozen plate_hole +
  cylinder).

## Alternatives rejected

| Option | Why rejected |
|--------|----------------|
| Clean-room clipped Voronoi | Schedule swamp; Geogram already BSD-3 |
| Dual-first poly now | ADR-0024 Q8 hard-block until G4 |
| Full Geogram blob / system package only | Non-reproducible; unused modules bloat |
| GPL meshers (Gmsh, CGAL Mesh_3) in core | Forbidden — compare plugins only |

## Status of G0

**Done** when this ADR is committed and `third_party/geogram/README.md` states
the not-yet-vendored placeholder. Implementation = G1+.
