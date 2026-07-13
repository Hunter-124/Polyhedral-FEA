# Pareto analysis — `varyhedron-baseline-m9`

_Generated 2026-07-13T00:39:00Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 8 |
| OK / fail | 6 / 2 (ok rate 75.0%) |
| Distinct configs | 2 |
| Checkpoint | tier=0, completed_runs=8, survivors=2 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 2 configs × 4 parts = 8 tier-0 runs (1 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-89f62376` | 0.5251 | 0.137 | 203ms | 75.0% | feature_refine=True, mesher=varyhedron, order=1 |
| 2 | `cfg-028399df` | 0.5117 | 0.156 | 410ms | 75.0% | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.156 | 410ms | 0.5117 | feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-89f62376` | 0.137 | 203ms | 0.5251 | feature_refine=True, mesher=varyhedron, order=1 |

## Pareto by part

### `cylinder`

_No ok runs._

### `icecream_cone`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.059 | 352ms | 0.4602 | feature_refine=True, mesher=varyhedron, order=1 |
| `cfg-028399df` | 0.000 | 313ms | 0.4334 | feature_refine=True, mesher=hybrid_zoo, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.286 | 119ms | 0.6148 | feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-89f62376` | 0.139 | 95ms | 0.5472 | feature_refine=True, mesher=varyhedron, order=1 |

### `sphere`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.348 | 85ms | 0.6542 | feature_refine=True, mesher=varyhedron, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.156 | 410ms | 0.5117 | feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-89f62376` | 0.137 | 203ms | 0.5251 | feature_refine=True, mesher=varyhedron, order=1 |

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 75.0% over 8 runs; finished=True.

Top config overall: `cfg-89f62376` score=0.5251 (feature_refine=True, mesher=varyhedron, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `varyhedron` | 0.5251 | 75.0% | 4 |
| `feature_refine` | `True` | 0.5184 | 75.0% | 8 |
| `order` | `1` | 0.5184 | 75.0% | 8 |

### Full factor breakdown

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `True` | 0.5184 | 75.0% | 2 | 8 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `varyhedron` | 0.5251 | 75.0% | 1 | 4 |
| `hybrid_zoo` | 0.5117 | 75.0% | 1 | 4 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.5184 | 75.0% | 2 | 8 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=8, ok=75.0%; best: feature_refine=`True` (score 0.518), mesher=`varyhedron` (score 0.525)
- **part:cylinder** — n=2, ok=0.0%; best: feature_refine=`True` (score 0.396), mesher=`varyhedron` (score 0.439)
- **part:icecream_cone** — n=2, ok=100.0%; best: feature_refine=`True` (score 0.447), mesher=`varyhedron` (score 0.460)
- **part:plate_hole** — n=2, ok=100.0%; best: feature_refine=`True` (score 0.581), mesher=`hybrid_zoo` (score 0.615)
- **part:sphere** — n=2, ok=100.0%; best: feature_refine=`True` (score 0.650), mesher=`varyhedron` (score 0.654)

## How to re-run

```bash
python3 scripts/analyze_campaign.py varyhedron-baseline-m9
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
