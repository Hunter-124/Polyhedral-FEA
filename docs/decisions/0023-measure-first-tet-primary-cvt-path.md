# ADR-0023: Measure-first, tet primary, restricted CVT path

- Status: accepted (2026-07-12)
- Decision: D23
- Related: ADR-0020 (BRep), ADR-0021 (varyhedron), ADR-0022 (warehouse/loop),
  ADR-0019 (FE+VEM)
- Supersedes in part: ADR-0021 algorithm ranking and V11 dual-first sequencing

## Context

Lane V landed a BRep substrate, edge-protect seeds, bubble packing, warehouse
campaigns, and a headless improvement loop. Campaign accuracy probes still
exploded (relative errors ~1e14 with “ok” solves), so any packing loop was
partly noise-fitting. External geometry/FEA review compressed the honest path
for a small team.

## Decision

### 1. Substrate (keep, do not replace)

Keep **protected seeds + graded tet scaffold + live BRep oracle**. The property
that makes meshing “not tessellate-and-forget” is that sizing, snapping, and
boundary conformity query the BRep throughout (project to face, closest on edge
curve, curvature at parameter) — not the algorithm family name.

**Out of core bet for ≥ one quarter:** frame-field hex-dominant (singularity
graph repair, integer-grid degeneracies). Agglomeration ideas from hex-dominant
literature may still inform a poly layer later without adopting the frame field.

### 2. Element technology claims

| Claim | Status |
|-------|--------|
| **Tet FE** | Accuracy workhorse and **default product claim** |
| **Poly VEM** | **Gated** “also ships polyhedra”; promote to primary only when it beats `hybrid_zoo` on campaign metrics (energy error per DOF and SCF error) on at least `plate_hole` and `cylinder` with frozen references |

Dual-of-tet VEM is known to be sensitive to stabilization and degraded on
stretched dual cells; it does not automatically beat well-graded tets per DOF
on stress concentrations.

### 3. Packing evolution

Ranking for this codebase:

1. **Weighted restricted CVT** (best fit) — subsumes bubble packing; Lloyd /
   L-BFGS on CVT energy; size field via weighted Voronoi; **clipped Voronoi
   cells against the BRep volume are the polyhedra** (Yan–Wang–Lévy), which can
   skip fragile boundary dualization for interior cells.
2. Lattice + clip — fast bulk fallback; needs boundary snap.
3. Advancing front — best layers, high 3D engineering cost.
4. Raw bubble packing — **keep seeds**, replace dynamics with CVT iterations.

Protected edge sites stay **fixed constrained sites** during relaxation.

### 4. CAD edge classification (normative)

Do **not** protect all BRep edges equally:

- **Sharp (G0)** — protecting balls + hard snap (e.g. hole rim).
- **Smooth / tangent-continuous** — no protect; nodes free-slide on the wall.
- **OCC seam edges** (parameterization on cylinders/spheres, not real features)
  — never protect.

Wall nodes: every smooth iteration projects displacement into the local tangent
plane and re-projects onto the true surface via OCC.

### 5. Measurement order (two-week horizon)

| Priority | Work | Why first |
|----------|------|-----------|
| **(c)** | Probe hardening + CAD-face-tagged BC/probe selection + minimum metric set | Accuracy signal is currently broken |
| **(b)** | Edge/surface residual metrics (chordal efficiency + normal deviation) | Honest mesh quality loop signal |
| **(a)** | Dual-poly / VEM headline | Only after (b) can tell if polyhedra win |
| **(d)** | Frame fields | Dropped from two-week horizon |

**Minimum metric set per run** (five numbers + one gate):

1. One-sided Hausdorff, mesh feature polylines → sampled CAD edge curves, / local h
2. Surface normal deviation on smooth faces (max angle mesh face normal vs BRep normal at projection)
3. DOF count
4. One accuracy scalar per case (SCF for plate_hole; energy or tip deflection for cylinder)
5. Worst element quality (min scaled Jacobian or dihedral)

Solve residual is a **health gate**, not a progress metric. Wireframe PNGs are
agent artifacts, never the reward.

**Probe hardening (kill 1e14 “ok” solves):**

- Reaction-sum check (Σ reactions ≈ applied load)
- Orphan-node detect/strip before assembly
- BC/probe selection by persistent BRep face tags (not mesh face index)
- Primary tip probe = **face-averaged** displacement on CAD-tagged tip face;
  global nodal max is diagnostic only

### 6. License landscape (core vs plugin)

| Component | License | Use |
|-----------|---------|-----|
| Geogram | BSD-3 | Study / vendor pieces / formulations in core |
| Shewchuk predicates | Public domain | Vendor into core |
| Protecting-ball CDT / boundary recovery | Papers (Si TOMS 2015 etc.) | Clean-room reimplement under BSD-3 |
| fTetWild | MPL-2.0 | Optional plugin only |
| Gmsh | GPL | Compare/benchmark plugin only |
| Netgen | LGPL | Compare plugin only |
| CGAL Mesh_3 | GPL | Compare plugin only |
| Verdict | BSD | Prefer over reimplementing quality measures |

### 7. Sizing field

Compose pointwise min then Lipschitz-smooth:

`h = min(h_curv, h_edge, h_thick, h_max)` with gradation `|∇h| ≤ 0.2–0.3`.

- `h_curv` from sagitta tolerance (prefer ε relative to local feature size)
- `h_edge` only in a neighborhood of that edge (do not let one fillet collapse h globally)
- Floor rule: if resolving an edge would push below tier `h_min`, flag
  **virtual-topology suppression** rather than resolve (prevents over_budget blowups)

Log **N_pred ≈ C · ∫ dV / h(x)³** before meshing. If N_pred busts the tier →
sizing bug; if N_pred OK but N_actual ≫ N_pred → mesher failure. Hard-kill runs
at ~2× tier DOF.

### 8. p-order

Uniform p per campaign first. Hierarchical **minimum rule** (Demkowicz): shared
edge/face carries min order of adjacent elements. Feature-local p-elevation only
after edge residual is stable; p>2 near curved walls needs curved boundary
handling (isoparametric / VEM curved faces) or it converges faster to the wrong
geometry.

### 9. Truth layers (verification)

1. MMS once per element technology (tet FE, VEM) — code correctness
2. Sphere: semi-analytic Legendre / Hiramatsu–Oka-class reference when ready
3. No closed form: Richardson on frozen versioned h, h/2, h/4 family (energy
   norm); never regenerate references each campaign

## Consequences

- New program nodes **M0–M5** (measure lane) block packing “wins” and VEM
  promotion until health + scorecard exist.
- ADR-0021 v1 algorithm remains the **substrate**; dual-of-tet is no longer the
  preferred path to polyhedra if clipped CVT lands.
- Agent loops optimize scorecard metrics, never residual alone or screenshots.
- Study list (order): Gao et al. SIGGRAPH 2017 agglomeration; Yan–Wang–Lévy +
  Geogram; Cheng–Dey–Shewchuk book (protecting balls); Si TetGen TOMS 2015
  (clean-room); Hitchhiker’s Guide to VEM; fTetWild robustness philosophy;
  Pietroni hex survey (what we defer).

## Alternatives rejected

- Frame-field hex-dominant as the small-team core bet.
- Promoting dual-poly VEM to the product headline before campaign gates.
- Packing improvement loops while probes still emit 1e14 garbage.
- Linking GPL meshing kernels into core.
