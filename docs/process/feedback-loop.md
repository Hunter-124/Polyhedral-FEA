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

## Final findings (settings-frontier-1, finished)

**Finished 2026-07-13**: `checkpoint.json` state `finished`, **150 completed
runs**, tier 2, ok-rate 100 %. Full record:
[`bench/campaigns/settings-frontier-1/SURVIVORS.md`](../../bench/campaigns/settings-frontier-1/SURVIVORS.md)
(+ `PARETO.md` / `PARETO.json`).

**Survivors (tier-2 keep, 6 configs):**

| cfg_id | config (approx) |
|--------|-----------------|
| cfg-e07cd50d | hybrid_vem, tendency=-0.75, feature_refine=true |
| cfg-1b696ce7 | graded_tet, tendency=-0.75, feature_refine=true |
| cfg-dc413db0 | hybrid_vem, tendency=-0.75, feature_refine=false |
| cfg-ff7ccfde | hex, tendency=0, feature_refine=true |
| cfg-02e4c7a5 | hex, tendency=-0.75, feature_refine=true |
| cfg-50b7a344 | hybrid_zoo, tendency=-0.75, feature_refine=false |

Tooling top score was `cfg-1ea46b97` (hex, tendency=0, feature_refine=false)
and `analyze_campaign.py` set `apply_code_defaults=true` (finished + 100 %
ok-rate). **Product defaults were NOT flipped** — the survivor table is
dominated by hex-leaning configs, but tier-2 rows show *identical*
`rel_err` / DOF across different meshers on the same part (suspicious
collapse), and ADR-0023 keeps tet FE / hybrid_zoo as the accuracy claim
until M9-frozen curved-STEP campaigns say otherwise. All four candidate knob
changes (mesher→hex, tendency→0.9, feature_refine→false, order) were
**documented and rejected**; `order=1` was already default. Any future
default change must re-validate on `varyhedron-baseline-m9` +
`vem-gate-m5` first.

## Procedure after campaign finishes

1. `python3 scripts/analyze_campaign.py settings-frontier-1`
2. Read `PARETO.md` survivors + per-geom factor_best.
3. If `apply_code_defaults` and the frontier is stable across parts:
   - Patch only strongly justified defaults (document delta in this file).
   - Re-run smoke + a thin campaign slice.
4. Mark `campaign-1` done in `PROGRAM.yaml` with survivor summary.
5. Set `feedback-loop` note to the campaign consumed, then flip status back
   to `todo` (repeatable node — never leave stuck at `done`).
