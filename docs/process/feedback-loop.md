# Feedback loop — campaign → defaults

Repeatable DAG node `feedback-loop` (`docs/dag/PROGRAM.yaml`). After each
settings campaign accumulates data, mine it, propose (or apply) default
knob deltas, re-measure.

**Measure-first constraint:** packing / algorithm loops run only after **M9
baseline freeze** on an honest scorecard. The reward for ranking configs is
`scorecard` + accuracy probes (`mean_vm_over_nominal` / `strain_energy` /
`tip_deflection` as appropriate) — **not** wire PNG, residual alone, or raw
nodal \(\sigma_{\max}\). Normative map:
[docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md);
result schema: [docs/dag/interfaces.md](../dag/interfaces.md).

## Tooling

```bash
# Partial-safe: works while a campaign is still running.
python3 scripts/analyze_campaign.py settings-frontier-1
python3 scripts/analyze_campaign.py smoke
python3 scripts/analyze_campaign.py --all
```

Reads `results.jsonl` (+ optional `campaign.json` weights, `checkpoint.json`
state). Writes:

| File | Contents |
|------|----------|
| `bench/campaigns/<name>/PARETO.md` | Human ranking, Pareto fronts, knob recs |
| `bench/campaigns/<name>/PARETO.json` | Machine-readable summary for later presets |

Scoring matches `apps/testlab` successive-halving (`scalar_score`):

```
s_mesh  = 1 / (1 + mesh_ms / 1000)
s_solve = 1 / (1 + solve_ms / 1000)
score   = w_acc·accuracy + w_mesh·s_mesh + w_solve·s_solve
```

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms+solve_ms`.
Configs are also grouped by `part` and coarse `geom_class` buckets
(`prismatic` / `mild_curve` / `curved` / `thin_wall` from the result row’s
`geom_class` tag).

## When to change product defaults

`analyze_campaign.py` sets `recommendations.apply_code_defaults = true` only
when **all** of:

1. `checkpoint.state == "finished"`
2. Overall ok-rate ≥ 85 %
3. At least 12 result lines

Otherwise: **document only** (this file + `PARETO.md`). Do not edit
`SimSetup` / `VolumeMesher` defaults or `resolve_element_tendency` thresholds
on partial frontiers.

Target knobs when applying (scope: `src/pipeline/`, docs):

| Knob | Code site | Notes |
|------|-----------|--------|
| Default mesher | `SimSetup::mesher` / CLI | Today `kHybrid` |
| `element_tendency` | `SimSetup::element_tendency` (default 0) | ∈[-1,+1]; 0 preserves base mesher |
| Feature refine | pipeline feature seeds | Campaign grid key `feature_refine` |
| Per-condition presets | future: geom→mesher map | Prefer curved vs prismatic split |

## Provisional findings (settings-frontier-1, partial)

As of the first analysis pass the campaign was still **running** (tier 0,
~23 / 96 tier-0 runs). Snapshot only — re-run the script when finished.

| Observation | Provisional takeaway |
|-------------|----------------------|
| Top composite configs bias **hex-leaning** (`element_tendency=-0.75`) with hex / hybrid_zoo / graded_tet nearly tied on prismatic bars | Keep product default mesher `kHybrid` + tendency `0` until full frontier; hex bias is a candidate preset for prismatic parts |
| `element_tendency=0.9` (tet extreme) expensive / low composite on early runs | Do not default near +1 |
| `plate_hole` (curved) accuracy still weak at coarse tier (~SCF rel_err ~0.23) | Need finer tiers + full grid before curved presets |
| One `graded_tet` solve_fail on plate_hole | Track ok-rate before promoting tet defaults |
| Smoke campaign: hex ≫ hybrid_zoo on smoke_bar at order 1 | Consistent with hex strength on simple prisms |

**No code default changes applied** this pass (`apply_code_defaults=false`).

## Procedure after campaign finishes

1. `python3 scripts/analyze_campaign.py settings-frontier-1`
2. Read `PARETO.md` survivors + per-geom factor_best.
3. If `apply_code_defaults` and the frontier is stable across parts:
   - Patch only strongly justified defaults (document delta in this file).
   - Re-run smoke + a thin campaign slice.
4. Mark `campaign-1` done in `PROGRAM.yaml` with survivor summary.
5. Set `feedback-loop` note to the campaign consumed, then flip status back
   to `todo` (repeatable node — never leave stuck at `done`).
