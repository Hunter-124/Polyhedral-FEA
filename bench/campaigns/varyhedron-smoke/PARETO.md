# Pareto analysis — `varyhedron-smoke`

_Generated 2026-07-12T22:45:20Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 4 |
| OK / fail | 2 / 2 (ok rate 50.0%) |
| Distinct configs | 2 |
| Checkpoint | tier=0, completed_runs=4, survivors=2 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 2 configs × 2 parts = 4 tier-0 runs (1 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-89f62376` | 0.2836 | 0.000 | 7.80s | 0.0% | feature_refine=True, mesher=varyhedron, order=1 |
| 2 | `cfg-028399df` | 0.1510 | 0.037 | 29.6s | 100.0% | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.037 | 29.6s | 0.1510 | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Pareto by part

### `cylinder`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.000 | 55.7s | 0.0307 | feature_refine=True, mesher=hybrid_zoo, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.074 | 3.54s | 0.2713 | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.037 | 29.6s | 0.1510 | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 50.0% over 4 runs; finished=True.

Top config overall: `cfg-89f62376` score=0.2836 (feature_refine=True, mesher=varyhedron, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `varyhedron` | 0.2836 | 0.0% | 2 |
| `feature_refine` | `True` | 0.2173 | 50.0% | 4 |
| `order` | `1` | 0.2173 | 50.0% | 4 |

### Full factor breakdown

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `True` | 0.2173 | 50.0% | 2 | 4 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `varyhedron` | 0.2836 | 0.0% | 1 | 2 |
| `hybrid_zoo` | 0.1510 | 100.0% | 1 | 2 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.2173 | 50.0% | 2 | 4 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=4, ok=50.0%; best: feature_refine=`True` (score 0.217), mesher=`varyhedron` (score 0.284)
- **part:cylinder** — n=2, ok=50.0%; best: feature_refine=`True` (score 0.151), mesher=`varyhedron` (score 0.270)
- **part:plate_hole** — n=2, ok=50.0%; best: feature_refine=`True` (score 0.284), mesher=`varyhedron` (score 0.297)

## How to re-run

```bash
python3 scripts/analyze_campaign.py varyhedron-smoke
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
