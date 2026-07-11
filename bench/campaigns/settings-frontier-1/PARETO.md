# Pareto analysis — `settings-frontier-1`

_Generated 2026-07-11T05:53:33Z · campaign state: **running** (PARTIAL)_

> **Partial results.** Re-run `python3 scripts/analyze_campaign.py settings-frontier-1` when the campaign finishes to refresh this report.

## Summary

| Field | Value |
|---|---|
| Result lines | 23 |
| OK / fail | 22 / 1 (ok rate 95.7%) |
| Distinct configs | 8 |
| Checkpoint | tier=0, completed_runs=23, survivors=32 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 32 configs × 3 parts = 96 tier-0 runs (3 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-0ea8730d` | 0.6415 | 0.551 | 1.18s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |
| 2 | `cfg-1b696ce7` | 0.6407 | 0.551 | 1.18s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=graded_tet, order=1 |
| 3 | `cfg-02e4c7a5` | 0.6401 | 0.551 | 1.19s | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hex, order=1 |
| 4 | `cfg-1ea46b97` | 0.6390 | 0.551 | 1.19s | 100.0% | element_tendency=0, feature_refine=False, mesher=hex, order=1 |
| 5 | `cfg-3f8ec5a6` | 0.6181 | 0.552 | 2.29s | 100.0% | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| 6 | `cfg-37584704` | 0.6167 | 0.552 | 2.39s | 100.0% | element_tendency=0.5, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 7 | `cfg-37d684d9` | 0.5303 | 0.437 | 6.61s | 66.7% | element_tendency=0, feature_refine=False, mesher=graded_tet, order=1 |
| 8 | `cfg-4ac2a08b` | 0.2755 | 0.428 | 196.7s | 100.0% | element_tendency=0.9, feature_refine=True, mesher=graded_tet, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.552 | 2.29s | 0.6181 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-0ea8730d` | 0.551 | 1.18s | 0.6415 | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |

## Pareto by part

### `cantilever`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-0ea8730d` | 0.730 | 387ms | 0.7863 | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.181 | 5.95s | 0.3030 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-1ea46b97` | 0.179 | 2.78s | 0.3392 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

### `smoke_bar`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-37d684d9` | 0.866 | 989ms | 0.7702 | element_tendency=0, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-0ea8730d` | 0.744 | 355ms | 0.7995 | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.181 | 5.95s | 0.3030 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-1ea46b97` | 0.179 | 2.78s | 0.3392 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

### `prismatic`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-4ac2a08b` | 0.740 | 355.8s | 0.4617 | element_tendency=0.9, feature_refine=True, mesher=graded_tet, order=1 |
| `cfg-0ea8730d` | 0.737 | 371ms | 0.7929 | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 95.7% over 23 runs; finished=False.

Top config overall: `cfg-0ea8730d` score=0.6415 (element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `hex` | 0.6395 | 100.0% | 6 |
| `element_tendency` | `-0.75` | 0.6408 | 100.0% | 9 |
| `feature_refine` | `False` | 0.6010 | 91.7% | 12 |
| `order` | `1` | 0.5752 | 95.7% | 23 |

### Full factor breakdown

**element_tendency**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `-0.75` | 0.6408 | 100.0% | 3 | 9 |
| `0.5` | 0.6174 | 100.0% | 2 | 6 |
| `0.0` | 0.5846 | 83.3% | 2 | 6 |
| `0.9` | 0.2755 | 100.0% | 1 | 2 |

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `False` | 0.6010 | 91.7% | 4 | 12 |
| `True` | 0.5494 | 100.0% | 4 | 11 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `hex` | 0.6395 | 100.0% | 2 | 6 |
| `hybrid_zoo` | 0.6291 | 100.0% | 2 | 6 |
| `graded_tet` | 0.5162 | 90.9% | 4 | 11 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.5752 | 95.7% | 8 | 23 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=8, ok=87.5%; best: element_tendency=`-0.75` (score 0.338), feature_refine=`False` (score 0.302), mesher=`hex` (score 0.338)
- **geom:prismatic** — n=15, ok=100.0%; best: element_tendency=`-0.75` (score 0.792), feature_refine=`False` (score 0.751), mesher=`hex` (score 0.790)
- **part:cantilever** — n=7, ok=100.0%; best: element_tendency=`-0.75` (score 0.786), feature_refine=`True` (score 0.786), mesher=`hex` (score 0.783)
- **part:plate_hole** — n=8, ok=87.5%; best: element_tendency=`-0.75` (score 0.338), feature_refine=`False` (score 0.302), mesher=`hex` (score 0.338)
- **part:smoke_bar** — n=8, ok=100.0%; best: element_tendency=`-0.75` (score 0.798), feature_refine=`False` (score 0.782), mesher=`hex` (score 0.797)

## How to re-run

```bash
python3 scripts/analyze_campaign.py settings-frontier-1
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
