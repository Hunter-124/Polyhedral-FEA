# ADR-0024: Advisor measure-first answers (normative Q&A)

- Status: accepted (2026-07-12)
- Decision: D24
- Related: ADR-0023 (strategy), ADR-0020/21/22 (Lane V substrate)
- Full plan map: [docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md)

This ADR freezes the **blunt answers** from the post-M1–M4 geometry/FEA review.
Future agents treat these as law unless new measurements overturn them.

---

## Q1 — 1e20 von Mises with 1e-13 residual

**Cause ranking:** (i) near-degenerate element inverse-Jacobian in stress recovery
(slivers — hybrid_zoo boundary layer prime suspect), (ii) nodal extrapolation from
Gauss points on bad elements, (iii) true singularity at re-entrant BC (growth as
h→0, not 1e20). Check element quality at argmax node first.

**Scoring policy:**

| Role | Metric |
|------|--------|
| **Score** | **(c)** area-weighted face-mean VM on tagged CAD face region at **element-centroid / interior** samples of quality-passing elements + **(d)** energy vs frozen Richardson (or analytic energy) |
| **Dashboard** | **(b)** p99 over quality-filtered elements; log exclusion count |
| **Never score** | Raw nodal \(\sigma_{\max}\) |

Real SCF is supported by a patch of elements and survives percentile; 1e20 spikes
are a handful of slivers.

**Cylinder:** primary score = strain energy; tip face-mean stays as BC/RBM health
(mesh-insensitive for packing gradient).  
**Plate_hole:** SCF = face-mean VM / \(\sigma_\infty\) on hole neighborhood, not global max.

---

## Q2 — Next 3–5 days order

**(c) freeze baseline → (a) wall tangential smooth + OCC project → (b) Lloyd CVT.**

- Freeze first: half-day; every hour of projection/CVT before baseline is unmeasurable.  
- Projection: 2–3 days; attacks normal deviation / staircasing; shared post-pass for CVT.  
- Lloyd CVT: 1–2 weeks with clipping robustness — do not start in a 3–5 day window
  if it risks landing nothing.

---

## Q3 — Geogram

**Vendor now for hard parts; reimplement never (clipped Voronoi).**

- Vendor: predicates, ConvexCell / clipped-Voronoi kernel, Delaunay if useful.  
- Write: thin Lloyd loop, \(1/h^3\) size-field weighting, constrained sites, OCC bridge.  
- Cost: afternoon of vendoring hygiene (namespace, strip modules, LICENSE). Worth it.

---

## Q4 — Chordal efficiency e ~ 100 at h_scale=5

**Metric bug, not coarse-h behavior.** Sagitta bound \(d \approx h^2\kappa/8\) scales with
segment size; e should stay O(1) when geometry is locally circular.

Causes ranked: (i) normalize by global h not **segment length \(\ell\)**,
(ii) κ from discrete polyline not **OCC curve**, (iii) distance to tessellation not
true curve, (iv) diameter/radius slip (~2× not 100×).

**Rule:** \(e_i = d_i / (\ell_i^2 \kappa/8)\) with OCC κ. Synthetic circle + hand chord
must give e ≈ 1 before trusting campaigns. Healthy band **[0.8, 3]**; e>5 = bad
placement; e<0.7 = over-resolved.

---

## Q5 — Cylinder truth

- **Keep** tip deflection (cheap health).  
- **Promote energy** (analytic uniaxial or frozen Richardson) to **primary** campaign score.  
- Sphere Legendre / Hiramatsu–Oka: ~1 day **after** freeze; cut if overruns.

---

## Q6 — Protecting balls

**Must-change #1:** \(r = \min(\alpha h, \beta\cdot\mathrm{lfs})\), \(\alpha\approx 0.45\),
\(\beta\approx 1/3\). lfs from distance to non-adjacent sharp geometry. Prevents balls
swallowing thin walls / adjacent edges (plate_hole, icecream).

**Must-change #2:** shrink near CAD vertices/corners (corner ball first spirit).

**Check:** overlapping balls with min_sep ~0.55·r are intentional for edge coverage.

---

## Q7 — Box BC vs face tags

Boxes fine for current planar axis-aligned cases (~2 more weeks) **if** per-run
assert: selected face area ≈ intended CAD face area **±2%**. Face tags (~2–3 days)
before curved loads, parameter sweeps, or BC-adjacent stress metrics.

---

## Q8 — Dualization

**Hard-block** median dual until clipped-CVT cells exist.  
Exception: ~2-day experimental poly export only if external/marketing demand,
flagged experimental. Otherwise skip dualizer entirely.

---

## Q9 — Tiers

Confirm **10k / 50k / 250k** DOF default; tet-FE-only overnight may use **500k** top
tier for Richardson asymptotics. Hard-kill **2× DOF** + **wall-clock** (15 / 15 / 45 min)
+ pack-level time ceiling.

---

## Q10 — High-dimensional traps

1. **Isoparametric/curved boundary before any p>1** — or loop signal inverts.  
2. **VEM stabilization:** D-recipe/trace-scaled + MMS from day one when VEM un-gates.  
3. **Virtual topology:** detector/flag under \(h_{\min}\) now; do **not** start OCC
   defeaturing “while touching fillets.”  
4. **CVT density = \(1/h(x)^3\) from the same h field as N_pred** — wire on day one of CVT.

---

## Compressed path (do not invent another)

1. Measure honest (probes, scorecard, chordal e, lfs balls).  
2. Freeze baseline campaign.  
3. Wall project.  
4. Vendor Geogram → constrained restricted CVT → poly = clipped cells.  
5. VEM earns headline only at M5 gate.  
