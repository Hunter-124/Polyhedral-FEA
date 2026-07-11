# Pareto analysis — `smoke`

_Generated 2026-07-11T05:53:33Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 2 |
| OK / fail | 2 / 0 (ok rate 100.0%) |
| Distinct configs | 2 |
| Checkpoint | tier=0, completed_runs=2, survivors=2 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 2 configs × 1 parts = 2 tier-0 runs (1 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-35f5ae24` | 0.7808 | 0.732 | 435ms | 100.0% | feature_refine=False, mesher=hex, order=1 |
| 2 | `cfg-5b36c35e` | 0.5713 | 0.403 | 735ms | 100.0% | feature_refine=False, mesher=hybrid_zoo, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-35f5ae24` | 0.732 | 435ms | 0.7808 | feature_refine=False, mesher=hex, order=1 |

## Pareto by part

### `smoke_bar`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-35f5ae24` | 0.732 | 435ms | 0.7808 | feature_refine=False, mesher=hex, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `prismatic`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-35f5ae24` | 0.732 | 435ms | 0.7808 | feature_refine=False, mesher=hex, order=1 |

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 100.0% over 2 runs; finished=True.

Top config overall: `cfg-35f5ae24` score=0.7808 (feature_refine=False, mesher=hex, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `hex` | 0.7808 | 100.0% | 1 |
| `feature_refine` | `False` | 0.6761 | 100.0% | 2 |
| `order` | `1` | 0.6761 | 100.0% | 2 |

### Full factor breakdown

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `False` | 0.6761 | 100.0% | 2 | 2 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `hex` | 0.7808 | 100.0% | 1 | 1 |
| `hybrid_zoo` | 0.5713 | 100.0% | 1 | 1 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.6761 | 100.0% | 2 | 2 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:prismatic** — n=2, ok=100.0%; best: feature_refine=`False` (score 0.676), mesher=`hex` (score 0.781)
- **part:smoke_bar** — n=2, ok=100.0%; best: feature_refine=`False` (score 0.676), mesher=`hex` (score 0.781)

## How to re-run

```bash
python3 scripts/analyze_campaign.py smoke
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
