# Pareto analysis — `settings-frontier-1`

_Generated 2026-07-13T03:27:05Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 150 |
| OK / fail | 150 / 0 (ok rate 100.0%) |
| Distinct configs | 32 |
| Checkpoint | tier=2, completed_runs=150, survivors=6 |
| Score weights | accuracy=0.5, mesh_ms=0.25, solve_ms=0.25 |
| Grid | Grid: 32 configs × 3 parts = 96 tier-0 runs (3 tiers planned). |

Composite score (matches `polymesh_testlab` successive-halving):  
`score = w_acc·accuracy + w_mesh/(1+mesh_ms/1000) + w_solve/(1+solve_ms/1000)`.

Pareto axes: **maximize** mean accuracy, **minimize** mean `mesh_ms + solve_ms` (ok runs contribute; configs with zero ok runs excluded).

## Config ranking (weighted mean score)

| Rank | cfg_id | score | accuracy | total time | ok | config |
|---:|---|---:|---:|---:|---:|---|
| 1 | `cfg-1ea46b97` | 0.7594 | 0.538 | 40ms | 100.0% | element_tendency=0, feature_refine=False, mesher=hex, order=1 |
| 2 | `cfg-e6a90670` | 0.7590 | 0.538 | 42ms | 100.0% | element_tendency=0.5, feature_refine=False, mesher=hybrid_vem, order=1 |
| 3 | `cfg-d832bfb3` | 0.7588 | 0.538 | 43ms | 100.0% | element_tendency=0, feature_refine=False, mesher=hybrid_vem, order=1 |
| 4 | `cfg-7431bed6` | 0.7502 | 0.559 | 146ms | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |
| 5 | `cfg-0ea8730d` | 0.7501 | 0.559 | 145ms | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |
| 6 | `cfg-c5ad7ce6` | 0.7498 | 0.559 | 146ms | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=graded_tet, order=1 |
| 7 | `cfg-3f8ec5a6` | 0.7482 | 0.559 | 152ms | 100.0% | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| 8 | `cfg-37584704` | 0.7482 | 0.559 | 152ms | 100.0% | element_tendency=0.5, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 9 | `cfg-e74a9896` | 0.7481 | 0.559 | 152ms | 100.0% | element_tendency=0.5, feature_refine=False, mesher=hex, order=1 |
| 10 | `cfg-4ac2a08b` | 0.7260 | 0.533 | 178ms | 100.0% | element_tendency=0.9, feature_refine=True, mesher=graded_tet, order=1 |
| 11 | `cfg-d2186519` | 0.7257 | 0.533 | 180ms | 100.0% | element_tendency=0.9, feature_refine=True, mesher=hex, order=1 |
| 12 | `cfg-62b424e2` | 0.7257 | 0.489 | 80ms | 100.0% | element_tendency=0.9, feature_refine=False, mesher=hex, order=1 |
| 13 | `cfg-f329c619` | 0.7250 | 0.533 | 183ms | 100.0% | element_tendency=0.9, feature_refine=True, mesher=hybrid_vem, order=1 |
| 14 | `cfg-57701654` | 0.7247 | 0.489 | 84ms | 100.0% | element_tendency=0.9, feature_refine=False, mesher=hybrid_vem, order=1 |
| 15 | `cfg-d678a231` | 0.7245 | 0.533 | 185ms | 100.0% | element_tendency=0.9, feature_refine=True, mesher=hybrid_zoo, order=1 |
| 16 | `cfg-7d79e580` | 0.7239 | 0.489 | 88ms | 100.0% | element_tendency=0.9, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 17 | `cfg-ec6a0f8e` | 0.7239 | 0.533 | 189ms | 100.0% | element_tendency=0, feature_refine=True, mesher=graded_tet, order=1 |
| 18 | `cfg-7159ac82` | 0.7236 | 0.489 | 89ms | 100.0% | element_tendency=0.9, feature_refine=False, mesher=graded_tet, order=1 |
| 19 | `cfg-37d684d9` | 0.7230 | 0.489 | 92ms | 100.0% | element_tendency=0, feature_refine=False, mesher=graded_tet, order=1 |
| 20 | `cfg-1b696ce7` | 0.6918 | 0.527 | 734ms | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=graded_tet, order=1 |
| 21 | `cfg-e07cd50d` | 0.6917 | 0.527 | 736ms | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hybrid_vem, order=1 |
| 22 | `cfg-50b7a344` | 0.6917 | 0.527 | 733ms | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=hybrid_zoo, order=1 |
| 23 | `cfg-dc413db0` | 0.6917 | 0.527 | 732ms | 100.0% | element_tendency=-0.75, feature_refine=False, mesher=hybrid_vem, order=1 |
| 24 | `cfg-02e4c7a5` | 0.6914 | 0.527 | 737ms | 100.0% | element_tendency=-0.75, feature_refine=True, mesher=hex, order=1 |
| 25 | `cfg-ff7ccfde` | 0.6909 | 0.527 | 743ms | 100.0% | element_tendency=0, feature_refine=True, mesher=hex, order=1 |

_… 7 more configs in `PARETO.json`._

## Global Pareto frontier (mean accuracy vs mean total time)

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-3f8ec5a6` | 0.559 | 152ms | 0.7482 | element_tendency=0.5, feature_refine=False, mesher=graded_tet, order=1 |
| `cfg-0ea8730d` | 0.559 | 145ms | 0.7501 | element_tendency=-0.75, feature_refine=True, mesher=hybrid_zoo, order=1 |
| `cfg-e6a90670` | 0.538 | 42ms | 0.7590 | element_tendency=0.5, feature_refine=False, mesher=hybrid_vem, order=1 |
| `cfg-1ea46b97` | 0.538 | 40ms | 0.7594 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

## Pareto by part

### `cantilever`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-704c6798` | 0.885 | 842ms | 0.8250 | element_tendency=0, feature_refine=True, mesher=hybrid_vem, order=1 |
| `cfg-1b696ce7` | 0.828 | 417ms | 0.8568 | element_tendency=-0.75, feature_refine=True, mesher=graded_tet, order=1 |
| `cfg-7431bed6` | 0.789 | 87ms | 0.8750 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |
| `cfg-1ea46b97` | 0.726 | 27ms | 0.8561 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

### `plate_hole`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-62b424e2` | 0.159 | 138ms | 0.5476 | element_tendency=0.9, feature_refine=False, mesher=hex, order=1 |
| `cfg-b287cc7b` | 0.150 | 64ms | 0.5598 | element_tendency=0, feature_refine=False, mesher=hybrid_zoo, order=1 |
| `cfg-e6a90670` | 0.145 | 63ms | 0.5575 | element_tendency=0.5, feature_refine=False, mesher=hybrid_vem, order=1 |
| `cfg-1ea46b97` | 0.145 | 50ms | 0.5604 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

### `smoke_bar`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-62b424e2` | 0.866 | 49ms | 0.9212 | element_tendency=0.9, feature_refine=False, mesher=hex, order=1 |
| `cfg-d832bfb3` | 0.744 | 26ms | 0.8656 | element_tendency=0, feature_refine=False, mesher=hybrid_vem, order=1 |

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-62b424e2` | 0.159 | 138ms | 0.5476 | element_tendency=0.9, feature_refine=False, mesher=hex, order=1 |
| `cfg-b287cc7b` | 0.150 | 64ms | 0.5598 | element_tendency=0, feature_refine=False, mesher=hybrid_zoo, order=1 |
| `cfg-e6a90670` | 0.145 | 63ms | 0.5575 | element_tendency=0.5, feature_refine=False, mesher=hybrid_vem, order=1 |
| `cfg-1ea46b97` | 0.145 | 50ms | 0.5604 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |

### `prismatic`

| cfg_id | accuracy | total time | score | config |
|---|---:|---:|---:|---|
| `cfg-7431bed6` | 0.765 | 89ms | 0.8628 | element_tendency=-0.75, feature_refine=False, mesher=hex, order=1 |
| `cfg-1ea46b97` | 0.735 | 35ms | 0.8589 | element_tendency=0, feature_refine=False, mesher=hex, order=1 |
| `cfg-d832bfb3` | 0.735 | 29ms | 0.8604 | element_tendency=0, feature_refine=False, mesher=hybrid_vem, order=1 |

## Default-knob recommendations

**Apply code defaults?** `True` — Campaign finished with solid ok-rate; defaults may be updated.

Overall ok-rate 100.0% over 150 runs; finished=True.

Top config overall: `cfg-1ea46b97` score=0.7594 (element_tendency=0, feature_refine=False, mesher=hex, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `hex` | 0.7131 | 100.0% | 42 |
| `element_tendency` | `0.9` | 0.7249 | 100.0% | 24 |
| `feature_refine` | `False` | 0.7308 | 100.0% | 75 |
| `order` | `1` | 0.7019 | 100.0% | 150 |

### Full factor breakdown

**element_tendency**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `0.9` | 0.7249 | 100.0% | 8 | 24 |
| `-0.75` | 0.7136 | 100.0% | 8 | 63 |
| `0.0` | 0.6871 | 100.0% | 8 | 30 |
| `0.5` | 0.6819 | 100.0% | 8 | 33 |

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `False` | 0.7308 | 100.0% | 16 | 75 |
| `True` | 0.6729 | 100.0% | 16 | 75 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `hex` | 0.7131 | 100.0% | 8 | 42 |
| `graded_tet` | 0.7124 | 100.0% | 8 | 36 |
| `hybrid_vem` | 0.6972 | 100.0% | 8 | 36 |
| `hybrid_zoo` | 0.6847 | 100.0% | 8 | 36 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.7019 | 100.0% | 32 | 150 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=50, ok=100.0%; best: element_tendency=`0.9` (score 0.532), feature_refine=`False` (score 0.521), mesher=`graded_tet` (score 0.502)
- **geom:prismatic** — n=100, ok=100.0%; best: element_tendency=`0.9` (score 0.821), feature_refine=`False` (score 0.832), mesher=`graded_tet` (score 0.821)
- **part:cantilever** — n=50, ok=100.0%; best: element_tendency=`-0.75` (score 0.862), feature_refine=`True` (score 0.839), mesher=`hex` (score 0.846)
- **part:plate_hole** — n=50, ok=100.0%; best: element_tendency=`0.9` (score 0.532), feature_refine=`False` (score 0.521), mesher=`graded_tet` (score 0.502)
- **part:smoke_bar** — n=50, ok=100.0%; best: element_tendency=`0.9` (score 0.879), feature_refine=`False` (score 0.836), mesher=`graded_tet` (score 0.814)

## How to re-run

```bash
python3 scripts/analyze_campaign.py settings-frontier-1
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
