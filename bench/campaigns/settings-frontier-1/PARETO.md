# Pareto analysis — `settings-frontier-1`

_Generated 2026-07-11T06:38:21Z · campaign state: **running** (PARTIAL)_

> **Partial results.** Re-run `python3 scripts/analyze_campaign.py settings-frontier-1` when the campaign finishes to refresh this report.

## Summary

| Field | Value |
|---|---|
| Result lines | 54 |
| OK / fail | 37 / 17 (ok rate 68.5%) |
| Distinct configs | 18 |
| Checkpoint | tier=0, completed_runs=54, survivors=32 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 32 configs × 3 parts = 96 tier-0 runs (3 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-7431bed6` | 0.6483 | 0.551 | 1.14s | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |
| 2 | `cfg-50b7a344` | 0.6445 | 0.551 | 1.14s | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 3 | `cfg-0ea8730d` | 0.6415 | 0.551 | 1.18s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |
| 4 | `cfg-1b696ce7` | 0.6407 | 0.551 | 1.18s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=graded_tet, order=1 |
| 5 | `cfg-02e4c7a5` | 0.6401 | 0.551 | 1.19s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hex, order=1 |
| 6 | `cfg-1ea46b97` | 0.6390 | 0.551 | 1.19s | 100.0% | element_tendency=0, feature_refine=False, mesher=hex, order=1 |
| 7 | `cfg-3f8ec5a6` | 0.6181 | 0.552 | 2.29s | 100.0% | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| 8 | `cfg-37584704` | 0.6167 | 0.552 | 2.39s | 100.0% | element_tendency=0.5, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 9 | `cfg-7d79e580` | 0.5328 | 0.437 | 6.63s | 66.7% | element_tendency=0.9, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 10 | `cfg-62b424e2` | 0.5322 | 0.437 | 6.47s | 66.7% | element_tendency=0.9, feature_refine=False, mesher=hex, order=1 |
| 11 | `cfg-7159ac82` | 0.5320 | 0.437 | 6.64s | 66.7% | element_tendency=0.9, feature_refine=False, mesher=graded_tet, order=1 |
| 12 | `cfg-57701654` | 0.5318 | 0.437 | 6.46s | 66.7% | element_tendency=0.9, feature_refine=False, mesher=hybrid_vem, order=1 |
| 13 | `cfg-37d684d9` | 0.5303 | 0.437 | 6.61s | 66.7% | element_tendency=0, feature_refine=False, mesher=graded_tet, order=1 |
| 14 | `cfg-704c6798` | 0.5000 | 0.000 | 0ms | 0.0% | element_tendency=0, feature_refine=True, mesher=hybrid_vem, order=1 |
| 15 | `cfg-d59de8cd` | 0.5000 | 0.000 | 0ms | 0.0% | element_tendency=0.5, feature_refine=True, mesher=hybrid_vem, order=1 |
| 16 | `cfg-e07cd50d` | 0.5000 | 0.000 | 0ms | 0.0% | element_tendency=-0.75, feature_refine=True, mesher=hybrid_vem, order=1 |
| 17 | `cfg-f329c619` | 0.5000 | 0.000 | 0ms | 0.0% | element_tendency=0.9, feature_refine=True, mesher=hybrid_vem, order=1 |
| 18 | `cfg-4ac2a08b` | 0.3331 | 0.521 | 241.5s | 100.0% | element_tendency=0.9, feature_refine=True, mesher=graded_tet, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.552 | 2.29s | 0.6181 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-7431bed6` | 0.551 | 1.14s | 0.6483 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |

## Pareto by part

### `cantilever`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-7431bed6` | 0.730 | 337ms | 0.7944 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.181 | 5.95s | 0.3030 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-50b7a344` | 0.179 | 2.72s | 0.3417 | element_tendency=-0.75, feature_refine=False, mesher=hybrid_zoo, order=1 |

### `smoke_bar`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-7d79e580` | 0.866 | 949ms | 0.7755 | element_tendency=0.9, feature_refine=False, mesher=hybrid_zoo, order=1 |
| `cfg-7431bed6` | 0.744 | 312ms | 0.8065 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.181 | 5.95s | 0.3030 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-50b7a344` | 0.179 | 2.72s | 0.3417 | element_tendency=-0.75, feature_refine=False, mesher=hybrid_zoo, order=1 |

### `prismatic`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-7431bed6` | 0.737 | 325ms | 0.8005 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 68.5% over 54 runs; finished=False.

Top config overall: `cfg-7431bed6` score=0.6483 (element_tendency=-0.75, feature_refine=False, mesher=hex, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `hex` | 0.6149 | 91.7% | 12 |
| `element_tendency` | `-0.75` | 0.6192 | 83.3% | 18 |
| `feature_refine` | `False` | 0.5826 | 83.3% | 30 |
| `order` | `1` | 0.5601 | 68.5% | 54 |

### Full factor breakdown

**element_tendency**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `-0.75` | 0.6192 | 83.3% | 6 | 18 |
| `0.5` | 0.5783 | 66.7% | 3 | 9 |
| `0.0` | 0.5564 | 55.6% | 3 | 9 |
| `0.9` | 0.4936 | 61.1% | 6 | 18 |

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `False` | 0.5826 | 83.3% | 10 | 30 |
| `True` | 0.5319 | 50.0% | 8 | 24 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `hex` | 0.6149 | 91.7% | 4 | 12 |
| `hybrid_zoo` | 0.6089 | 91.7% | 4 | 12 |
| `graded_tet` | 0.5308 | 86.7% | 5 | 15 |
| `hybrid_vem` | 0.5064 | 13.3% | 5 | 15 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.5601 | 68.5% | 18 | 54 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=14, ok=64.3%; best: element_tendency=`-0.75` (score 0.340), feature_refine=`False` (score 0.295), mesher=`hex` (score 0.321)
- **geom:prismatic** — n=40, ok=70.0%; best: element_tendency=`-0.75` (score 0.727), feature_refine=`False` (score 0.727), mesher=`hex` (score 0.762)
- **part:cantilever** — n=18, ok=77.8%; best: element_tendency=`-0.75` (score 0.740), feature_refine=`False` (score 0.670), mesher=`hex` (score 0.730)
- **part:plate_hole** — n=18, ok=50.0%; best: element_tendency=`0.5` (score 0.368), feature_refine=`True` (score 0.388), mesher=`hybrid_vem` (score 0.453)
- **part:smoke_bar** — n=18, ok=77.8%; best: element_tendency=`-0.75` (score 0.750), feature_refine=`False` (score 0.783), mesher=`hex` (score 0.794)

## How to re-run

```bash
python3 scripts/analyze_campaign.py settings-frontier-1
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
