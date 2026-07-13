# M9 frozen baseline — `varyhedron-baseline-m9`

**Status: FROZEN** (ADR-0024 Q2(c); plan
[`docs/plans/advisor-measure-first-program.md`](../../../docs/plans/advisor-measure-first-program.md)
§5).

This campaign is the **measure-first baseline** on the honest M1–M8 scorecard
**before** wall OCC project (**M10**) or Geogram/CVT (**G\***). Packing /
algorithm improvement loops measure **delta vs this freeze only** — never
claim a “win” against pre-M6–M8 short campaigns alone.

---

## Freeze identity

| Field | Value |
| --- | --- |
| **Frozen UTC** | 2026-07-13T00:38:59Z (checkpoint `updated_utc`; analyze 00:39:00Z) |
| **Frozen date** | 2026-07-13 |
| **polymesh git SHA** | `dcb2baa380be1bafc2b4d9ff5d585c5d31ab964c` (`dcb2baa`) |
| **Binary** | `build/apps/testlab/polymesh_testlab` (OCC product build) |
| **Campaign** | `bench/campaigns/varyhedron-baseline-m9/` |
| **Results** | [`results.jsonl`](results.jsonl) (8 lines) |
| **PARETO** | [`PARETO.md`](PARETO.md) / [`PARETO.json`](PARETO.json) |
| **HANDOFF** | [`HANDOFF.md`](HANDOFF.md) / [`handoff.json`](handoff.json) |
| **Warehouse** | `runs/<cfg>/<part>/t0/{mesh.vtu,wire.png,quality.json,result.json}` |

### Why 8 runs (not short-1’s 24)

Prefer a **fast freeze** of the post-M6–M8 metric path over a multi-tier
overnight pack:

- 4 STEP parts × 2 meshers × **1 tier** `h_scale=5.0` = **8 runs**
- Wall clock ≈ **4 s** compute (+ warehouse PNG post) on this host — no
  overnight risk
- short-1 (3 tiers) remains a coarser pre-honest-scorecard historical pack;
  **this** freeze is the packing delta baseline

---

## Metric schema version

**Label:** `scorecard-m1-m8-v1` (post M6–M8, 2026-07-12/13)

Normative defs: [`docs/research/campaign-metrics.md`](../../../docs/research/campaign-metrics.md),
[`docs/dag/interfaces.md`](../../../docs/dag/interfaces.md), ADR-0024.

### Per-run top-level fields (results.jsonl)

| Field | Role |
| --- | --- |
| `status` | `ok` \| `solve_suspect` \| `over_budget` \| … |
| `health.ok` | Gate — residual, reaction, orphans, load-area |
| `health.free_residual_rel`, `reaction_sum_err`, `n_orphans`, `load_area_*` | Gate diagnostics |
| `scorecard.edge_hausdorff_over_h` | Sharp-edge residual / local h (M2/M3) |
| `scorecard.normal_dev_deg_max` | Smooth-face normal deviation (deg) |
| `scorecard.chordal_efficiency_max` | \(e = d/(\ell^2\kappa/8)\) with OCC κ (M7) |
| `scorecard.min_element_quality` | Worst element quality proxy |
| `scorecard.n_dof`, `accuracy_rel_err`, `solve_residual_rel`, `health_ok` | Mirror scalars |
| `n_pred_elems`, `n_elems_over_pred` | Sizing budget (M4) |
| `answers.sigma_face_mean`, `sigma_p99`, `n_quality_excluded`, `strain_energy` | Stress/energy (M6) — **not** raw max as primary score when case uses SCF/energy |
| `accuracy.metric` / `rel_err` / `score` / `trusted` | Case primary accuracy |

### Case primary accuracy metrics (as wired at freeze)

| Part | `accuracy.metric` | Truth role |
| --- | --- | --- |
| `plate_hole` | `scf` (face-mean VM / σ∞) | Kirsch SCF = 3 |
| `cylinder` | `strain_energy` | Analytic energy (+ tip secondary) |
| `sphere` | `tip_deflection` | Order-of-magnitude / interim (M13 Legendre later) |
| `icecream_cone` | `sigma_max` | **Still raw nodal max** at freeze — known debt; packing deltas on this part must not treat spike SCF as progress (M6/M12 follow-on) |

**Never score** raw nodal max as the product reward (ADR-0024 Q1). plate_hole
already uses face-mean SCF; cylinder uses energy. icecream_cone residual metric
is recorded honestly as-is for freeze reproducibility.

---

## Campaign matrix

```json
parts: plate_hole, cylinder, sphere, icecream_cone  (STEP fixtures)
meshers: varyhedron, hybrid_zoo
tiers: [{ "h_scale": 5.0, "keep_frac": 1.0 }]
warehouse: true
on_finish: { analyze: true, grok_handoff: true }
```

cfg_ids: `cfg-028399df` = hybrid_zoo · `cfg-89f62376` = varyhedron
(feature_refine=true, order=1).

---

## Outcome summary

| Metric | Value |
| --- | --- |
| **ok_rate** | **75.0%** (6 / 8 `status == ok`) |
| health.ok true | 6 / 8 |
| Failures | **both** `cylinder` runs → `solve_suspect` (`health.ok=false`, load_area_ok=false, ~62–66% load-area rel err) |
| Checkpoint | `finished`, tier=0, completed_runs=8, survivors=2 |
| Rank (composite) | varyhedron score 0.5251 slightly above hybrid_zoo 0.5117 — **provisional only**; do not change product defaults from this freeze alone |

### Per-run snapshot

| part | mesher | status | health.ok | n_dof | mesh_ms | solve_ms | metric | rel_err |
| --- | --- | --- | --- | ---: | ---: | ---: | --- | ---: |
| plate_hole | hybrid_zoo | ok | true | 6192 | 55 | 63 | scf | 0.250 |
| plate_hole | varyhedron | ok | true | 2232 | 78 | 16 | scf | 0.621 |
| cylinder | hybrid_zoo | solve_suspect | false | 19635 | 95 | 1011 | strain_energy | 1.429 |
| cylinder | varyhedron | solve_suspect | false | 7383 | 187 | 95 | strain_energy | 1.460 |
| sphere | hybrid_zoo | ok | true | 2043 | 78 | 23 | tip_deflection | 0.974 |
| sphere | varyhedron | ok | true | 1197 | 70 | 15 | tip_deflection | 0.935 |
| icecream_cone | hybrid_zoo | ok | true | 8757 | 214 | 99 | sigma_max | ~9.7e13 |
| icecream_cone | varyhedron | ok | true | 4470 | 310 | 43 | sigma_max | 15.9 |

Full JSON lines: [`results.jsonl`](results.jsonl).

### Known issues frozen-in (not blockers for freeze)

1. **Cylinder load-area gate** fails both meshers at h_scale=5.0 → M12 / BC
   face selection work; still freeze the runs for delta measurement.
2. **icecream_cone** accuracy still on `sigma_max` (spike-prone); face-mean /
   energy ref follow-on.
3. **chordal_efficiency_max** still huge on some hybrid_zoo / cone runs
   (e.g. 2e3–6e3) — packing residual signal not yet trustworthy everywhere;
   varyhedron plate_hole e_max≈5.9 is closer to O(1).
4. Coarse tier only — finer tiers deferred; deltas at h_scale=5.0 are the
   packing loop default comparison until a re-freeze is explicitly approved.

---

## Rule for packing / CVT loops

1. **Baseline = this directory + SHA above.** Compare new campaigns to
   `results.jsonl` row-by-row (same part, mesher, tier/h_scale when possible).
2. Report **Δ** on scorecard fields and trusted accuracy only when
   `status==ok` and `health.ok==true` (or call out gate failures separately).
3. Do **not** restart packing “wins” against `varyhedron-short-1` alone —
   short-1 predates / is not this freeze identity.
4. **M10** wall project and **G\*** CVT land only *after* this freeze exists
   (PROGRAM deps). Re-freeze only with an explicit board decision + new
   `BASELINE.md` version note.

---

## Reproduce

```bash
# From repo root, OCC build with testlab binary present:
./build/apps/testlab/polymesh_testlab run bench/campaigns/varyhedron-baseline-m9
# on_finish writes PARETO + HANDOFF; warehouse_shots for wire.png
```

Re-runs produce new numbers; the **frozen** snapshot is the committed
`results.jsonl` / warehouse at this SHA, not a future re-execution.
