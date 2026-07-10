# Benchmark scoreboard

_Generated 2026-07-10T10:23:46Z from `bench/results/*.json` via `bench/competitive/render_scoreboard.py`. Schema: `bench/competitive/schema.json`._

Primary DOF-reduction baseline is PolyMesh's frozen P1 uniform path (ADR-0005). Peer solvers are audit cross-checks.

## All runs

| Solver | Version | Case | DOFs | mesh s | solve s | total s | Accuracy | Value | Label | Timestamp |
|---|---|---|---:|---:|---:|---:|---|---:|---|---|
| PolyMesh | 0.1.0 | kirsch-plate | 1509 | — | — | 0.39 | scf_rel_err_pct | 1.87 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | 1257 | — | — | 0.32 | u_r_rel_err_pct | 0.0068 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | lame-cylinder | 1257 | — | — | 0.32 | hoop_rel_err_pct | 1.36 | `gate1-p1` | 2026-07-10T00:00:00Z |
| PolyMesh | 0.1.0 | timoshenko-cantilever | 1440 | — | — | 0.45 | tip_rel_err_pct | 1.5 | `gate1-p1` | 2026-07-10T00:00:00Z |
| calculix | 2.23 | cantilever_smoke | 48 | — | 0.03055 | 0.03055 | smoke_ran | 1 | `calculix-cantilever-smoke` | 2026-07-10T08:54:04.391282+00:00 |
| PolyMesh | 0.1.0 | l-domain-d6-baseline | 6384 | 0.01322 | 2.749 | 2.762 | energy_deficit_pct | 0.08537 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-graded | 1248 | 0.001028 | 0.2257 | 0.2267 | energy_deficit_pct | 0.08881 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-ratio | 1248 | — | — | 0.2267 | dof_ratio_uniform_over_graded | 5.115 | `d6-tier3` | 2026-07-10T10:20:00Z |
| PolyMesh | 0.1.0 | l-domain-d6-ratio | 1248 | — | — | 0.2267 | time_ratio_uniform_over_graded | 12.18 | `d6-tier3` | 2026-07-10T10:20:00Z |

## Accuracy vs labeled commits

ASCII sparkline scales within each case/metric series (height ∝ value). SVG polyline when ≥2 numeric points. Lower is better for `*_err_*` metrics.

### `cantilever_smoke` — `smoke_ran`

- labels: `calculix-cantilever-smoke`
- solvers: calculix
- values: 1
- sparkline: `▁`

### `kirsch-plate` — `scf_rel_err_pct`

- labels: `gate1-p1`
- solvers: PolyMesh
- values: 1.87
- sparkline: `▁`

### `l-domain-d6-baseline` — `energy_deficit_pct`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 0.08537
- sparkline: `▁`

### `l-domain-d6-graded` — `energy_deficit_pct`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 0.08881
- sparkline: `▁`

### `l-domain-d6-ratio` — `dof_ratio_uniform_over_graded`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 5.115
- sparkline: `▁`

### `l-domain-d6-ratio` — `time_ratio_uniform_over_graded`

- labels: `d6-tier3`
- solvers: PolyMesh
- values: 12.18
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
