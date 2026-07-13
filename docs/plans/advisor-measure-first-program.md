# Advisor measure-first program (canonical agent plan)

**Status:** active (2026-07-12)  
**Normative decisions:** [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md),
[ADR-0024](../decisions/0024-advisor-measure-answers.md)  
**Board:** [docs/dag/PROGRAM.yaml](../dag/PROGRAM.yaml) Lane **M** (+ CVT/G nodes)  
**Bootstrap:** [docs/dag/AGENT_BOOTSTRAP.md](../dag/AGENT_BOOTSTRAP.md)

This file is the **single map** of the external geometry/FEA advisor guidance.
If a future agent is unsure what to build next, read this first, then the ADRs,
then claim a `todo` node on the program board. Do **not** re-open ranked
decisions without new evidence.

---

## 0. One-sentence strategy

Harden measurement first; keep **tet scaffold + live BRep oracle** as substrate;
evolve packing toward **constrained restricted CVT / clipped Voronoi (Geogram)**;
**tet FE** is the product accuracy claim until **poly VEM** beats `hybrid_zoo` on
frozen campaign metrics; **never** dualize-for-VEM or frame-field-hex as the
near-term core bet.

---

## 1. Substrate (keep forever until proven wrong)

| Keep | Why |
|------|-----|
| Protected seeds on **sharp** CAD edges only | Real G0 features; not OCC seams |
| Graded tet scaffold as FE workhorse | Solves today; accuracy default claim |
| Live BRep oracle (OCC project / closest / κ) | “Not tessellate-and-forget” property |
| Bubble seeds as **sites** | Dynamics → CVT later; seeds stay |

| Do not | Why |
|--------|-----|
| Frame-field hex-dominant as core | Multi-year singularity/grid swamp |
| Dual-of-tet poly as primary poly path | Boundary dual on curved walls reimports staircasing; VEM gated |
| Packing “improvements” without health_ok scorecard | Noise-fitting (was 1e14 tip) |
| GPL meshing in core (Gmsh, CGAL Mesh_3) | Compare plugins only |
| Clean-room clipped Voronoi | Vendor Geogram BSD-3 instead |

---

## 2. Claims (product honesty)

| Claim | Rule |
|-------|------|
| **Default accuracy** | Tet FE |
| **Poly / VEM** | “Also ships polyhedra” until **M5**: beats `hybrid_zoo` on energy/DOF **and** SCF on frozen `plate_hole` + `cylinder` |
| **Varyhedron** | Packing path name; still exports tet scaffold until CVT cells land |
| **Reward signal** | Scorecard + accuracy metrics — **never** wire PNG, never residual alone |

---

## 3. Measurement (what every campaign run must produce)

### 3.1 Five-number scorecard + residual gate

1. One-sided Hausdorff mesh feature polylines → **sharp** CAD edges, / local h  
2. Surface **normal deviation** (deg) on smooth faces  
3. **DOF** count  
4. One **accuracy** scalar (case-specific — see §3.2)  
5. Worst **element quality** (min aspect / scaled Jacobian proxy)  
6. **Gate (not score):** free residual / reaction sum / orphans / load-area guard  

Wireframes = agent artifacts only.

### 3.2 What to score vs dashboard (stress)

| Role | Metric |
|------|--------|
| **Score stress** | Area-weighted **face-mean** VM on tagged region; stress from **element-centroid** samples of **quality-passing** elements |
| **Score no-closed-form / cylinder** | **Strain energy** (analytic uniaxial or frozen Richardson energy) |
| **Secondary health** | Face-mean **tip deflection** (catches BC/RBM; nearly mesh-insensitive for packing) |
| **Dashboard** | Quality-filtered **p99** VM; log `n_quality_excluded` |
| **Never score** | Raw nodal \(\sigma_{\mathrm{vm}}^{\max}\) (1e20 = sliver/extrapolation) |

Plate_hole SCF: face-mean VM / \(\sigma_\infty\) on hole neighborhood, not global max.

### 3.3 Chordal efficiency (edge residual)

Per **mesh feature segment** \(i\):

\[
e_i = \frac{d_i}{\ell_i^2 \kappa / 8}
\]

- \(\ell_i\) = actual segment length (not global h)  
- \(\kappa\) from **OCC BRep curve** at projected parameter (not discrete Menger on coarse polylines)  
- Healthy: \(e \in [0.8, 3]\); \(e > 5\) sustained = packer placing badly; \(e < 0.7\) = over-resolving  
- \(e \sim 100\) at coarse h was a **metric bug**, not “coarse-h behavior”

### 3.4 Over-budget diagnosis

- Log **N_pred ≈ C · V / h³** (later: \(\int dV/h^3\) from size field) **before** meshing  
- N_pred ≫ tier → **sizing** bug  
- N_pred OK, N_actual ≫ N_pred → **mesher** bug  
- Hard-kill ~**2×** tier DOF + **wall-clock** per run (15 / 15 / 45 min tiers) + pack ceiling  

### 3.5 BC / probe selection

- Boxes OK while faces planar & axis-aligned (current four STEP cases)  
- **Guard:** selected load face area ≈ `expected_area` ± **2%** → loud fail  
- True **BRep face tags** before: curved loads, geometry sweeps, BC-adjacent stress metrics  

---

## 4. Geometry / packing rules

### 4.1 Edge classification

| Class | Action |
|-------|--------|
| **Sharp (G0)** | Protecting balls + hard snap |
| **Smooth** | No protect; free-slide wall |
| **OCC seam** | Never protect |

Wall nodes: tangential smooth displacement + OCC surface re-project every iteration.

### 4.2 Protecting balls (CDS)

\[
r = \min(\alpha h,\, \beta \cdot \mathrm{lfs}),\quad
\alpha \approx 0.45,\; \beta \approx 1/3
\]

- lfs ≈ distance to nearest **other** sharp geometry (sampled OK)  
- Shrink near CAD vertices/corners  
- Overlapping balls intentional (min_sep ~0.55 r-scale)  

### 4.3 Sizing field

\[
h = \min(h_{\mathrm{curv}}, h_{\mathrm{edge}}, h_{\mathrm{thick}}, h_{\max}),
\quad |\nabla h| \le 0.2\text{–}0.3
\]

- ε relative to local feature size  
- **Floor:** if resolving a feature would push below tier \(h_{\min}\), **flag** virtual-topology (do not resolve) — detector now, OCC defeaturing later  

### 4.4 Packing evolution ranking

1. Weighted **restricted CVT** + clipped Voronoi (poly cells = clips)  
2. Lattice + clip (bulk fallback)  
3. Advancing front (layers later)  
4. Raw bubble dynamics (keep seeds only)  

**Hard-block median dual** until CVT cells exist (optional experimental dual export only if external demand).

### 4.5 Geogram vendoring (when CVT work starts)

| Vendor (BSD-3) | Write ourselves |
|----------------|-----------------|
| Predicates | Thin Lloyd loop |
| ConvexCell / clipped Voronoi | Size-field weight \(1/h^3\) (**same h as N_pred**) |
| Delaunay if useful | Constrained sites (sharp fixed) + OCC bridge |

Shewchuk predicates public domain OK. Verdict BSD for quality. fTetWild MPL plugin only.

### 4.6 p-order

- Uniform p per campaign first  
- Hierarchical **min-rule** (already in hp_assembly)  
- Feature-local p↑ only after edge residual stable  
- **Isoparametric / curved boundary before any p>1** near curved walls (or loop signal inverts)  

### 4.7 VEM (when un-gated)

- Stabilization: implement **D-recipe / trace-scaled** alongside classic; MMS both before campaigns  
- Do not promote to headline until M5  

---

## 5. Execution order (do not skip)

| Priority | Work | Program nodes | Horizon |
|----------|------|---------------|---------|
| **0** | Docs/ADRs/board honest | M0, this plan | done |
| **1** | Probe health + face-mean tip | M1 | done |
| **2** | Scorecard metrics | M2 | done |
| **3** | Sharp-only protect + (follow-on wall project) | M3, M3b | M3 done; M3b open |
| **4** | N_pred + over_budget cause | M4 | done (∫h³ / h_min flag follow-on) |
| **5** | Stress score fix (centroid face-mean, drop raw max) + energy cylinder | M6 | in flight |
| **6** | Chordal e fix (ℓ, OCC κ, true segments) | M7 | in flight |
| **7** | Protecting-ball lfs clamp | M8 | in flight |
| **8** | **Freeze baseline campaign** on honest scorecard | M9 | **next land** |
| **9** | Wall tangential + OCC project | M3b / M10 | after freeze |
| **10** | Vendor Geogram + Lloyd CVT + constrained sites | G1–G3 | after projection |
| **11** | Clipped Voronoi poly export | G4 | after G1–G3 |
| **12** | VEM gate campaign | M5 | after metrics + poly path |
| **later** | BRep face tags; sphere Legendre ref (~1 day); p>1 + curved edges; frame fields research | … | not two-week core |

**Near-term 3–5 day order (advisor Q2):**  
**(c) freeze baseline → (a) wall project → (b) Lloyd CVT.**  
Never start CVT before freeze + projection.

---

## 6. Campaign tiers (overnight)

| Tier | Target DOF (default) | Wall-clock kill (suggested) |
|------|----------------------|------------------------------|
| 0 | ~10k | 15 min |
| 1 | ~50k | 15 min |
| 2 | ~250k (or **500k** tet-FE-only packs) | 45 min |

Hard-kill run at **2×** DOF. Cap whole pack wall-clock so 24 runs finish overnight.

Parts: `plate_hole`, `cylinder`, `sphere`, `icecream_cone`.  
Meshers: `varyhedron`, `hybrid_zoo` only for short packing campaigns.

---

## 7. Truth layers (verification)

1. **MMS** once per element technology (tet FE, VEM)  
2. **Sphere:** Hiramatsu–Oka / Legendre polar-cap (~1 day, after freeze; cut if slips)  
3. **No closed form:** Richardson on **frozen versioned** h, h/2, h/4 family — never regen refs each campaign  
4. Cylinder analytic **energy** + tip as secondary  

Anti-cheat: truths only in `bench/reference/` + `docs/validation/hand-calcs.md`.

---

## 8. Study list (reading order)

1. Gao et al., SIGGRAPH 2017 — poly agglomeration ideas (without adopting frame field as core)  
2. Yan–Wang–Lévy + **Geogram** codebase — CVT / clipped Voronoi  
3. Cheng–Dey–Shewchuk, *Delaunay Mesh Generation* — protecting balls / PSC  
4. Si, TetGen TOMS 2015 — boundary recovery (clean-room algorithms, not AGPL code)  
5. Hitchhiker’s Guide to the VEM — stabilization  
6. Hu et al., fTetWild — robustness philosophy  
7. Pietroni et al. 2022 hex survey — what we defer  

---

## 9. Agent anti-confusion checklist

Before claiming work:

1. [ ] Read this plan + ADR-0023 + ADR-0024  
2. [ ] `docs/dag/PROGRAM.yaml` — claim a `todo` whose deps are `done`  
3. [ ] Do not optimize residual alone or wireframes  
4. [ ] Do not score raw nodal max stress  
5. [ ] Do not start dual-of-tet or frame fields as product path  
6. [ ] Do not run packing “wins” until **M9 baseline** exists and health_ok  
7. [ ] CVT density uses **same** \(h(x)\) as N_pred  
8. [ ] OCC product builds: `-DPOLYMESH_WITH_OCC=ON`  
9. [ ] Commit + push master; no AI attribution trailers  

---

## 10. Related files

| File | Role |
|------|------|
| `docs/decisions/0023-…` | Strategy ADR |
| `docs/decisions/0024-…` | Concrete advisor Q&A rules |
| `docs/research/varyhedron-packing.md` | Algorithm ranking detail |
| `docs/dag/PROGRAM.yaml` | Executable board |
| `docs/dag/interfaces.md` | results.jsonl / scorecard schema |
| `docs/process/grok-loop.md` | Headless improve loop |
| `bench/campaigns/*` | Frozen baselines + short packs |
