# Session handoff — G1 Geogram / restricted CVT (and remaining board)

**Date:** 2026-07-13  
**Audience:** next agent (or human) continuing the advisor measure-first program  
**Canonical plan:** [advisor-measure-first-program.md](advisor-measure-first-program.md)  
**ADRs:** [0023](../decisions/0023-measure-first-tet-primary-cvt-path.md) ·
[0024](../decisions/0024-advisor-measure-answers.md) ·
[0025](../decisions/0025-geogram-cvt-vendor.md)  
**Board:** [PROGRAM.yaml](../dag/PROGRAM.yaml) · **Bootstrap:** [AGENT_BOOTSTRAP.md](../dag/AGENT_BOOTSTRAP.md)

This handoff exists so the next session does **not** re-open ranked advisor
decisions or re-litigate measure-path work. Measure is closed. **Next critical
path is G1.**

---

## 1. What is complete (do not redo)

| Node / item | Status | Notes |
|-------------|--------|--------|
| M0–M4, M6–M11, M14 | **done** | probes, scorecard, sharp protect, N_pred, stress score, chordal e, lfs balls, h_min flags, wall-clock kills |
| M9 freeze | **done** | `bench/campaigns/varyhedron-baseline-m9/` — historical 75% ok (pre cylinder area fix) |
| M10 wall OCC project | **done** | `project_point_on_surface` + `wall_tangential_project` in varyhedron |
| Cylinder load normal-filter | **done** | `|n·t̂|>0.7`, tip `expected_area=πR²` |
| M12 expected_area | **done** | plate_hole, cylinder, **sphere** polar cap; icecream multi-face **deferred** to face tags |
| M13 sphere ref | **done** (cut path) | Frozen dual-mesher SE `4.60e-4` + tip `2.90e-7` — **not** full Legendre series |
| G0 | **done** | ADR-0025 + `third_party/geogram/` placeholder |
| V6e / V10c | **done** | varyhedron scorecard path; GUI supervised handoff queue |
| GUI Test Lab measure-first | **ready** | health/scorecard/answers/load_area tooltips + Results chips |

**Product accuracy claim:** tet FE. **Poly/VEM headline:** blocked until **M5** after **G4**.

---

## 2. Advisor order (frozen — do not reorder)

```
freeze (M9) → wall project (M10) → CVT (G1–G4) → VEM gate (M5)
```

- Dual-of-tet poly path: **hard-blocked until G4**
- Frame-field hex: **out of near-term horizon**
- Packing “wins”: only as **delta vs M9 freeze**, and only after health_ok scorecard
- Never score raw nodal \(\sigma_{\mathrm{vm}}^{\max}\)
- CVT density \(\rho = 1/h(x)^3\) must use the **same** \(h(x)\) as **N_pred**

---

## 3. Next workstream (G1 first)

### G1 — Vendor Geogram predicates + ConvexCell / clipped Voronoi

**Deps:** G0 done, M10 done → **unblocked now**.  
**Scope:** `third_party/geogram/`, CMake, strip unused modules, LICENSE/NOTICE.  
**Do not:** clean-room clipped Voronoi; vendor whole Geogram GUI/blob.

Normative study path (read before coding):

1. [geogram-cvt-vendoring.md](../research/geogram-cvt-vendoring.md)
2. [ADR-0025](../decisions/0025-geogram-cvt-vendor.md)
3. [third_party/geogram/README.md](../../third_party/geogram/README.md)

**Suggested G1 slices:**

1. Pin upstream tag/commit; record in `NOTICE`
2. Import minimal subset: exact predicates + ConvexCell / clip kernel
3. Namespace hygiene (`GEO::` or thin `polymesh::geo` wrappers)
4. CMake `POLYMESH_WITH_GEOGRAM` (or always-on if small enough)
5. One Catch2 smoke that builds a unit-cube clipped cell (no product mesh yet)

**Then G2 → G3 → G4** (Lloyd + density, constrained sites + OCC, poly export).  
**Then M5** VEM gate campaign on plate_hole + cylinder.

---

## 4. Deferred / side work (not ahead of G1)

| Item | Why deferred | When |
|------|--------------|------|
| Icecream `expected_area` / face-ID BCs | Multi-face box area swings 15–20% | After face-tag design implementation (~2–3 days) — [brep-face-tag-bc.md](../research/brep-face-tag-bc.md) |
| True Legendre / Hiramatsu–Oka series | M13 timebox cut | Optional upgrade of `bench/reference/sphere.json` |
| V6d order packing p>1 | Needs curved/isoparametric edges same item | After measure path; not G-critical |
| V11 packing iterate “wins” | Needs G* poly path + health_ok | After G4 + compare to M9 |
| M5 VEM gate | Needs real polys from G4 | After G4 |
| feedback-loop / campaign-1 | Process nodes | When campaign warehouse is clean and G path not starving |
| Optional re-freeze baseline | Post cylinder/sphere load fixes | Nice after G1 or before packing-win claims |

---

## 5. GUI readiness (current bar)

- **Study / Sim Setup:** STEP open, meshers including Varyhedron (ADR tooltip), solve, adapt
- **Test Lab:** campaign list, M9 baseline badge, live progress, pause/play hooks, warehouse path
- **Results:** status chips (ok / suspect / fail / budget / area_fail), health + scorecard tooltips, tip/energy columns, V10c open program nodes from `handoff.json`
- **Build:** `polymesh-gui` under `build/apps/gui/` with OCC product builds (`-DPOLYMESH_WITH_OCC=ON`)

Not claimed: full interactive face-tag BC picking (still boxes in case JSON).

---

## 6. Verify before starting G1

```bash
git pull --rebase
cmake --build build -j
ctest --test-dir build --output-on-failure
# optional measure smoke
./build/apps/testlab/polymesh_testlab run bench/campaigns/post-m10-smoke
```

Identity: **Hunter-124** commits, push `master`, no AI attribution trailers.

---

## 7. Open PROGRAM nodes after this handoff

| id | status | notes |
|----|--------|--------|
| **G1** | todo | **start here** |
| G2 | todo | after G1 |
| G3 | todo | after G2 + M10 |
| G4 | todo | after G3; unblocks dual block + M5 |
| M5 | todo | after G4 |
| V6d | todo | p>1 + curved edges |
| V11 | todo | packing iterate after G* |
| feedback-loop | todo | process |
| campaign-1 | in_progress | settings campaign hygiene |

---

## 8. Anti-confusion checklist (copy from plan)

1. Read this handoff + advisor plan + ADR-0023/24/25  
2. Claim **G1** on the board  
3. Do not optimize residual alone or wireframes  
4. Do not score raw nodal max stress  
5. Do not start dual-of-tet or frame fields as product path  
6. Packing wins only vs M9 freeze after health_ok  
7. OCC product builds: `-DPOLYMESH_WITH_OCC=ON`  
8. Commit + push master; no AI attribution  
