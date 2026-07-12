# Pareto analysis — `varyhedron-smoke`

_Generated 2026-07-12T22:42:59Z · campaign state: **finished** (FINISHED)_

## Summary

| Field | Value |
|---|---|
| Result lines | 4 |
| OK / fail | 0 / 4 (ok rate 0.0%) |
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
| 1 | `cfg-028399df` | 0.2668 | 0.000 | 21.3s | 0.0% | feature_refine=True, mesher=hybrid_zoo, order=1 |
| 2 | `cfg-89f62376` | 0.2666 | 0.000 | 16.6s | 0.0% | feature_refine=True, mesher=varyhedron, order=1 |

## Global Pareto frontier (mean accuracy vs mean total time)

_No ok runs yet — frontier empty._

## Pareto by part

### `cylinder`

_No ok runs._

### `plate_hole`

_No ok runs._

## Pareto by geometric class

Buckets from `geom_class`: `prismatic` (curved_frac < 0.05), `mild_curve` [0.05, 0.25), `curved` ≥ 0.25, `thin_wall` if thin.

### `curved`

_No ok runs._

## Default-knob recommendations

**Apply code defaults?** `False` — Partial or insufficient data — document recommendations only; do not change product defaults yet.

Overall ok-rate 0.0% over 4 runs; finished=True.

Top config overall: `cfg-028399df` score=0.2668 (feature_refine=True, mesher=hybrid_zoo, order=1).

### Factor-level winners (mean config score)

| Knob | Best value | mean score | ok rate | n runs |
|---|---|---:|---:|---:|
| `mesher` | `hybrid_zoo` | 0.2668 | 0.0% | 2 |
| `feature_refine` | `True` | 0.2667 | 0.0% | 4 |
| `order` | `1` | 0.2667 | 0.0% | 4 |

### Full factor breakdown

**feature_refine**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `True` | 0.2667 | 0.0% | 2 | 4 |

**mesher**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `hybrid_zoo` | 0.2668 | 0.0% | 1 | 2 |
| `varyhedron` | 0.2666 | 0.0% | 1 | 2 |

**order**

| value | mean score | ok rate | n configs | n runs |
|---|---:|---:|---:|---:|
| `1` | 0.2667 | 0.0% | 2 | 4 |

### Per-condition presets (for feedback-loop)

Use these as *candidate* presets once the campaign is finished with solid ok rates. Product defaults stay unchanged on partial data.

- **geom:curved** — n=4, ok=0.0%; best: feature_refine=`True` (score 0.267), mesher=`hybrid_zoo` (score 0.267)
- **part:cylinder** — n=2, ok=0.0%; best: feature_refine=`True` (score 0.259), mesher=`varyhedron` (score 0.260)
- **part:plate_hole** — n=2, ok=0.0%; best: feature_refine=`True` (score 0.275), mesher=`hybrid_zoo` (score 0.276)

## How to re-run

```bash
python3 scripts/analyze_campaign.py varyhedron-smoke
```

Safe on live campaigns: reads `results.jsonl` append-only; does not touch the runner or checkpoint state.
