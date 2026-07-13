# Campaign metrics — normative definitions for agents

- Status: normative for Lane M campaigns and packing loops
- Normative: [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md) §5,
  [ADR-0024](../decisions/0024-advisor-measure-answers.md) Q1/Q4/Q5,
  [advisor-measure-first-program.md](../plans/advisor-measure-first-program.md) §3
- Schema sketch: [docs/dag/interfaces.md](../dag/interfaces.md) scorecard / health
- Related: [varyhedron-packing.md](varyhedron-packing.md),
  [geogram-cvt-vendoring.md](geogram-cvt-vendoring.md),
  [protecting-balls-lfs.md](protecting-balls-lfs.md)

Agents running warehouse campaigns, freeze baselines (**M9**), or packing
improvement loops use **only** these metric roles. Screenshots and residual-alone
are never the reward.

---

## 1. Score vs dashboard vs gate

| Role | Purpose | Examples |
|------|---------|----------|
| **Score** | Scalar the loop optimizes / ranks configs on (per part, then weighted) | Face-mean SCF (plate_hole); **strain energy** error (cylinder); energy/DOF vs frozen Richardson |
| **Dashboard** | Diagnostic distributions — log and plot, **do not** primary-rank on spikes | Quality-filtered **p99** VM; `n_quality_excluded`; tip face-mean as BC/RBM health |
| **Gate** | Pass/fail health — bad gate → `health.ok = false`, accuracy scores zeroed for ranking | Free residual, reaction sum, orphans, load-area ±2%, over_budget / wall-clock kill |

**Rules:**

- **Never score** raw nodal \(\sigma_{\mathrm{vm}}^{\max}\) (or any global nodal max stress). 1e20 spikes are sliver/extrapolation, not real SCF.
- **Never** treat wireframe PNGs as reward.
- **Never** optimize residual alone; residual is a **gate**, not progress.
- Analyze / Pareto: filter `status == "ok"` and `health.ok` before ranking.

### Stress scoring policy (ADR-0024 Q1)

| Role | Metric |
|------|--------|
| **Score** | Area-weighted **face-mean** VM on tagged CAD face / hole neighborhood; samples at **element-centroid / interior** of **quality-passing** elements; plus energy vs frozen truth where used |
| **Dashboard** | **p99** over quality-filtered elements; log exclusion count |
| **Never score** | Raw nodal \(\sigma_{\max}\) |

Real SCF is supported by a patch of elements and survives percentile filters;
1e20 spikes are a handful of slivers.

---

## 2. Minimum scorecard (five numbers + residual gate)

Every campaign run should produce:

1. **Edge residual** — one-sided Hausdorff, mesh feature polylines → **sharp** CAD edges, normalized by local \(h\)
2. **Surface normal deviation** (deg) on smooth faces (mesh face normal vs BRep normal at projection)
3. **DOF** count
4. **One accuracy scalar** (case-specific — §3)
5. **Worst element quality** (min scaled Jacobian / aspect proxy)
6. **Gate (not score):** free residual / reaction sum / orphans / load-area guard

Plus packing/sizing diagnostics: `n_pred`, `n_elems_over_pred`, `over_budget_cause`.

---

## 3. Case-specific accuracy scores

| Part | Primary **score** | Secondary / health |
|------|-------------------|--------------------|
| **`plate_hole`** | **SCF** = face-mean VM / \(\sigma_\infty\) on hole neighborhood (quality-passing, centroid samples) — **not** global max | Dashboard p99 VM; residual/reaction gates |
| **`cylinder`** | **Strain energy** vs analytic uniaxial or **frozen Richardson** energy | Tip **face-mean** deflection = BC/RBM health (nearly mesh-insensitive for packing gradient) — keep, do not promote over energy |
| **`sphere`** | When ready: Legendre / Hiramatsu–Oka-class; else frozen Richardson energy | After M9 freeze; cut if schedule slips |
| **`icecream_cone`** | Frozen Richardson / agreed energy or displacement face-mean on tagged region | Watch thin-wall + LFS balls ([protecting-balls-lfs.md](protecting-balls-lfs.md)) |

**Truth layers:** MMS once per element tech; no closed form → Richardson on
**frozen versioned** h, h/2, h/4 family — never regenerate references every
campaign. Truths live under `bench/reference/` + `docs/validation/hand-calcs.md`.

---

## 4. Chordal efficiency \(e\)

Per **mesh feature segment** \(i\) on sharp CAD edges:

\[
e_i = \frac{d_i}{\ell_i^{2}\,\kappa / 8}
\]

| Symbol | Correct definition | Common bugs |
|--------|--------------------|-------------|
| \(d_i\) | Chordal / geometric distance of segment to **true** BRep curve | Distance to a coarse tessellation of the same curve |
| \(\ell_i\) | **Actual segment length** | Global or tier \(h\) |
| \(\kappa\) | Curvature from **OCC BRep curve** at projected parameter | Discrete Menger curvature on the polyline |

**Interpretation:**

| Band | Meaning |
|------|---------|
| **Healthy** \(e \in [0.8, 3]\) | Locally circular geometry + correct formula → O(1) |
| \(e > 5\) sustained | Packer placing badly / under-resolving curvature |
| \(e < 0.7\) | Over-resolving (budget waste) |
| \(e \sim 100\) at coarse h | **Metric bug**, not “coarse-h behavior” (ADR-0024 Q4) |

**Sanity:** synthetic circle + hand chord must give \(e \approx 1\) before
trusting campaigns. Report e.g. `chordal_efficiency_max` (and optionally mean)
on the scorecard; fix M7 before packing loops trust edge residual.

Sagitta scale: \(d \approx \ell^{2}\kappa/8\) for a circular arc — so correctly
normalized \(e\) stays O(1) when geometry is locally circular.

---

## 5. Gates and kills (not scores)

Typical health gates (see interfaces + harness):

- Free residual relative ≤ threshold (e.g. \(10^{-6}\) direct)
- Reaction sum error ≤ threshold (e.g. 5%)
- Zero orphan nodes
- Load/select face area ≈ expected CAD face area **±5%** in harness (interfaces;
  plan text often quotes ±2% as the wrong-face ideal). Fixture values today:
  **cylinder** top \(\pi R^2 \approx 7.854\times 10^{-3}\,\mathrm{m}^2\);
  **plate_hole** x-max end \(H\times t = 0.1\times 0.01 = 0.001\,\mathrm{m}^2\);
  **sphere** polar cap \(A_{\mathrm{cap}}=2\pi R(R-z_p)=\pi\times 10^{-3}\) with
  \(z_p=0.04\), `normal_min_dot: 0.7`.
  **icecream_cone** (fused multi-face load) still omits `expected_area` until
  BRep face tags ([brep-face-tag-bc.md](brep-face-tag-bc.md)) — partial-face box
  area swings ~15–20% across meshers/tiers and would false-fail the gate.
- Hard-kill ~**2×** tier DOF; wall-clock kills (e.g. 15 / 15 / 45 min tiers) + pack ceiling

**Over-budget diagnosis:**

- Log **N_pred ≈ C · ∫ dV / h³** (or C·V/h³) **before** mesh
- N_pred ≫ tier → **sizing** bug
- N_pred OK, N_actual ≫ N_pred → **mesher** bug

CVT density must use the **same** \(h(x)\) as N_pred when Lane G lands.

---

## 6. Displacement probes

- Primary tip / face displacement metrics: **face-mean** |u| (or signed load
  component) on CAD-tagged / load-box faces — not global nodal max
- Global max may be logged as diagnostic only (`tip_deflection_max`)
- Stress: face-mean / centroid path above — **never raw max as score**

---

## 7. Agent checklist before claiming a campaign “win”

- [ ] `health.ok` and status ok; residual is gate only
- [ ] Accuracy score is face-mean / energy — **not** raw nodal max stress
- [ ] Chordal \(e\) uses \(\ell_i\), OCC \(\kappa\), true curve distance
- [ ] plate_hole SCF = hole-neighborhood face-mean / \(\sigma_\infty\)
- [ ] cylinder primary = **energy**; tip face-mean secondary
- [ ] Deltas measured against **M9** frozen baseline once it exists
- [ ] No dual-first or frame-field “win” stories without G4 / research scope
