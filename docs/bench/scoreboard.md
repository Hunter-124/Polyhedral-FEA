# Benchmark scoreboard

_Generated 2026-07-10T06:19:25Z from `bench/results/*.json` via `bench/competitive/render_scoreboard.py`. Schema: `bench/competitive/schema.json`._

Primary DOF-reduction baseline is PolyMesh's frozen P1 uniform path (ADR-0005). Peer solvers are audit cross-checks.

## All runs

| Solver | Version | Case | DOFs | mesh s | solve s | total s | Accuracy | Value | Label | Timestamp |
|---|---|---|---:|---:|---:|---:|---|---:|---|---|
| PolyMesh | 0.1.0 | kirsch-plate | — | — | — | 0.39 | scf_rel_err_pct | 1.87 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | — | — | — | 0.32 | u_r_rel_err_pct | 0.0068 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | — | — | — | 0.32 | hoop_rel_err_pct | 1.36 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | timoshenko-cantilever | — | — | — | 0.45 | tip_rel_err_pct | 1.5 | `gate1-p1` | 2026-07-10T00:00:00Z |

## Accuracy vs labeled commits

ASCII sparkline scales within each case/metric series (height ∝ value). SVG polyline when ≥2 numeric points. Lower is better for `*_err_*` metrics.

### `kirsch-plate` — `scf_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.87
- sparkline: `▁`

### `lame-cylinder` — `hoop_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.36
- sparkline: `▁`

### `lame-cylinder` — `u_r_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 0.0068
- sparkline: `▁`

### `timoshenko-cantilever` — `tip_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.5
- sparkline: `▁`

## How to refresh

```sh
python3 bench/competitive/render_scoreboard.py
```

See [bench/competitive/README.md](../../bench/competitive/README.md).
