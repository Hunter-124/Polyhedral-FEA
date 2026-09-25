# Campaign analysis — `results.jsonl` → `PARETO.md`

How to rank a test-lab campaign and decide whether its results justify a
product-default change. File schemas: [docs/dag/interfaces.md](../dag/interfaces.md)
§1–§3b. Ranking reward follows the measure-first rules in
[docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md).

## Run

```bash
# Partial-safe: works while a campaign is still running.
python3 scripts/analyze_campaign.py settings-frontier-1
python3 scripts/analyze_campaign.py bench/campaigns/settings-frontier-1
python3 scripts/analyze_campaign.py --all
```

A campaign with `"on_finish": { "analyze": true }` runs this automatically when
it finishes (interfaces §7b). Re-run by hand after a partial analysis.

Reads `results.jsonl` (+ optional `campaign.json` weights, `checkpoint.json`
state). Never modifies them. Writes:

| File | Contents |
|------|----------|
| `bench/campaigns/<name>/PARETO.md` | Weighted ranking, Pareto fronts, knob suggestions |
| `bench/campaigns/<name>/PARETO.json` | Same, machine-readable (`ranking`, `pareto_*`, `recommendations`) |

## Scoring

Same composite as `polymesh_testlab` successive halving (`scalar_score`):

```
s_mesh  = 1 / (1 + mesh_ms / 1000)
s_solve = 1 / (1 + solve_ms / 1000)
score   = w_acc·accuracy + w_mesh·s_mesh + w_solve·s_solve
```

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms`.
Fronts are also computed per `part` and per `geom_class` bucket:

| Bucket | Rule (from row `geom_class`) |
|--------|------------------------------|
| `thin_wall` | `thin` is true |
| `curved` | `curved_frac ≥ 0.25` |
| `mild_curve` | `0.05 ≤ curved_frac < 0.25` |
| `prismatic` | `curved_frac < 0.05` |

## Changing product defaults

`recommendations.apply_code_defaults` is `true` only when **all** hold:

1. `checkpoint.state == "finished"`
2. Overall ok-rate ≥ 85 %
3. At least 12 result lines

It is necessary, not sufficient. Before changing a default:

- Check the frontier is stable across parts and that tied rows are not a
  measurement collapse (identical `rel_err` / DOF across different meshers).
- Re-validate on `bench/campaigns/varyhedron-baseline-m9` and
  `bench/campaigns/vem-gate-m5`.
- Patch only the justified knob and cite the campaign in the commit.

| Knob | Code site |
|------|-----------|
| Default mesher | `SimSetup::mesher` (default `kGradedTet`) / CLI |
| `element_tendency` | `SimSetup::element_tendency` (default 0; ∈ [-1, +1]) |
| `feature_refine` | pipeline feature seeds (campaign grid key) |

Precedent: `settings-frontier-1` (finished 2026-07-13, 150 runs, 100 % ok-rate)
set `apply_code_defaults = true`, but tier-2 rows showed identical `rel_err` /
DOF across meshers, so every proposed default change was rejected. Record:
[`bench/campaigns/settings-frontier-1/SURVIVORS.md`](../../bench/campaigns/settings-frontier-1/SURVIVORS.md).
