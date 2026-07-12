# Pareto analysis — `varyhedron-short-1`

_Generated 2026-07-12T23:37:22Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 24 |
| OK / fail | 22 / 2 (ok rate 91.7%) |
| Distinct configs | 2 |
| Checkpoint | tier=2, completed_runs=24, survivors=2 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 2 configs × 4 parts = 8 tier-0 runs (3 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-89f62376` | 0.4652 | 0.095 | 509ms | 91.7% | feature_refine=True, mesher=varyhedron, order=1 |
| 2 | `cfg-028399df` | 0.4159 | 0.076 | 1.67s | 91.7% | feature_refine=True, mesher=hybrid_zoo, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.095 | 509ms | 0.4652 | feature_refine=True, mesher=varyhedron, order=1 |

## Pareto by part

### `cylinder`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.041 | 710ms | 0.4077 | feature_refine=True, mesher=varyhedron, order=1 |

### `icecream_cone`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.043 | 972ms | 0.3830 | feature_refine=True, mesher=varyhedron, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.045 | 721ms | 0.4377 | feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-89f62376` | 0.043 | 159ms | 0.4866 | feature_refine=True, mesher=varyhedron, order=1 |

### `sphere`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-028399df` | 0.257 | 250ms | 0.5777 | feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-89f62376` | 0.251 | 194ms | 0.5836 | feature_refine=True, mesher=varyhedron, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-89f62376` | 0.095 | 509ms | 0.4652 | feature_refine=True, mesher=varyhedron, order=1 |

## Default-knob recommendations

**Apply code defaults?** `True` — Campaign finished with solid ok-rate; defaults may be updated.

Overall ok-rate 91.7% over 24 runs; finished=True.

Top config overall: `cfg-89f62376` score=0.4652 (feature_refine=True, mesher=varyhedron, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `varyhedron` | 0.4652 | 91.7% | 12 |
| `feature_refine` | `True` | 0.4406 | 91.7% | 24 |
| `order` | `1` | 0.4406 | 91.7% | 24 |

### Full factor breakdown

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `True` | 0.4406 | 91.7% | 2 | 24 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `varyhedron` | 0.4652 | 91.7% | 1 | 12 |
| `hybrid_zoo` | 0.4159 | 91.7% | 1 | 12 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.4406 | 91.7% | 2 | 24 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=24, ok=91.7%; best: feature_refine=`True` (score 0.441), mesher=`varyhedron` (score 0.465)
- **part:cylinder** — n=6, ok=66.7%; best: feature_refine=`True` (score 0.365), mesher=`varyhedron` (score 0.408)
- **part:icecream_cone** — n=6, ok=100.0%; best: feature_refine=`True` (score 0.355), mesher=`varyhedron` (score 0.383)
- **part:plate_hole** — n=6, ok=100.0%; best: feature_refine=`True` (score 0.462), mesher=`varyhedron` (score 0.487)
- **part:sphere** — n=6, ok=100.0%; best: feature_refine=`True` (score 0.581), mesher=`varyhedron` (score 0.584)

## How to re-run

```bash
python3 scripts/analyze_campaign.py varyhedron-short-1
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
