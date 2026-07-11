# Test-lab interfaces (normative)

These file formats are the contract between the test-lab harness
(`apps/testlab`), the GUI (`apps/gui`), and the analysis/feedback tooling.
Change them only by editing this file in the same commit as the code change.
All units SI (m, Pa, N, kg, s); all times in milliseconds wall clock.

## 1. Campaign spec — `bench/campaigns/<name>/campaign.json`

Declares what to sweep. Written by hand or by the GUI.

```jsonc
{
  "name": "hole-plate-frontier-1",
  "parts": ["tests/fixtures/parts/plate_hole.case.json"],  // case files, §4
  "tiers": [                       // fidelity ladder for branch trimming
    { "h_scale": 2.0, "keep_frac": 0.25 },   // tier 0: cheap, keep best 25 %
    { "h_scale": 1.0, "keep_frac": 0.5 },
    { "h_scale": 0.5, "keep_frac": 1.0 }     // final tier: survivors only
  ],
  "grid": {                        // full factorial at tier 0 (no stone unturned)
    "mesher":            ["hex", "graded_tet", "hybrid_zoo"],
    "curvature_turn_deg":[10, 15, 22.5],
    "feature_refine":    [true, false],
    "snap_boundary":     [true],
    "order":             [1, 2],          // grows as p-hierarchical lands
    "element_tendency":  [-1.0, 0.0, 0.5, 1.0]  // ∈[-1,+1]; see resolve_element_tendency
  },
  "score": {                       // Pareto axes; scalar score for trimming
    "weights": { "accuracy": 0.5, "solve_ms": 0.25, "mesh_ms": 0.25 }
  },
  "resources": { "max_threads": 0, "max_mem_gb": 0 }   // 0 = unlimited
}
```

Trimming rule (successive halving): at each tier every surviving config runs
every part; configs rank by weighted score aggregated over parts; the top
`keep_frac` advance. `accuracy` is the relative error against the case's
hand-calc truth (§5), mapped to a 0–1 score as `1/(1+|rel_err|/tol)`.

## 2. Checkpoint — `bench/campaigns/<name>/checkpoint.json`

Written atomically (tmp+rename) by the runner after every completed run.
This is what makes pause/play work: SIGINT (or the GUI Pause button, which
sends SIGINT) is always safe; `resume` continues from here.

```jsonc
{
  "campaign": "hole-plate-frontier-1",
  "state": "running",              // running | paused | finished
  "tier": 1,
  "completed_runs": 137,           // count of results.jsonl lines
  "survivors": ["cfg-0007", "cfg-0012"],   // configs alive at current tier
  "started_utc": "2026-07-10T18:00:00Z",
  "updated_utc": "2026-07-10T18:22:31Z"
}
```

## 3. Results — `bench/campaigns/<name>/results.jsonl`

Append-only, one line per (config, part, tier) run. Committed to the repo —
this is the accumulated simulation data the feedback loop mines.

```jsonc
{
  "cfg_id": "cfg-0007",            // stable hash-id of the config values
  "config": { "mesher": "hybrid_zoo", "curvature_turn_deg": 15, "...": "..." },
  "part": "plate_hole", "tier": 1,
  "geom_class": { "curved_frac": 0.31, "thin": false, "min_feature_h": 2.4 },
  "mesh_ms": 412, "solve_ms": 1890,
  "n_elems": 31956, "n_nodes": 11935, "n_dof": 35805,
  "quality": { "M1max": 1.0e-11, "M2max": 0.36, "M6": 0.17, "score": 0.42 },
  "answers": { "sigma_max": 9.12e7, "tip_deflection": null },
  "accuracy": { "metric": "scf", "value": 3.06, "truth": 3.0, "rel_err": 0.02 },
  "status": "ok"                   // ok | mesh_fail | solve_fail | over_budget
}
```

`geom_class` is computed from the part geometry (not the config) so the
feedback loop can learn per-condition presets: fraction of surface area with
per-cell turning angle > 15°, thin-wall flag (t < 2.5 h_ref), smallest
feature size in units of bulk h.

## 3b. Pareto analysis — `bench/campaigns/<name>/PARETO.{md,json}`

Written by `scripts/analyze_campaign.py` (feedback-loop tooling). Safe on
**partial** `results.jsonl` while a campaign is still `running`; re-run when
`checkpoint.state` becomes `finished`.

- **PARETO.md** — human report: weighted ranking, global/per-part/per-geom
  Pareto (maximize accuracy, minimize mesh_ms+solve_ms), knob suggestions.
- **PARETO.json** — same content structured for automation (`ranking`,
  `pareto_global`, `pareto_by_part`, `pareto_by_geom_class`,
  `recommendations.apply_code_defaults`).

Does not modify checkpoint or results. Product defaults change only when
`recommendations.apply_code_defaults` is true (finished + solid ok-rate);
see `docs/process/feedback-loop.md`.

## 4. Part case — `tests/fixtures/parts/<part>.case.json`

Binds a geometry file to loads/BCs and to its reference truth.

```jsonc
{
  "part": "plate_hole",
  "geometry": "tests/fixtures/parts/plate_hole.stl",
  "material": { "E": 2.1e11, "nu": 0.3, "rho": 7850 },
  "bcs": [
    { "select": { "box": [[-1e9,-1e9,-1e9],[ -0.049,1e9,1e9]] },  // x-min face
      "fix": [true, true, true] }
  ],
  "loads": [
    { "select": { "box": [[0.049,-1e9,-1e9],[1e9,1e9,1e9]] },
      "traction": [1.0e6, 0, 0] }
  ],
  "reference": "bench/reference/plate_hole.json"
}
```

Face selection is by axis-aligned box over face centroids (the only robust
selector on tessellated fixtures). Selectors must be written with slack so
they are h-independent.

## 5. Reference truth — `bench/reference/<part>.json`

Hand-calculated answers ONLY (anti-cheat: nothing in src/ or apps/ may embed
these numbers; the harness loads them at runtime, and each entry cites its
derivation in `docs/validation/hand-calcs.md`).

```jsonc
{
  "part": "plate_hole",
  "metrics": [
    { "name": "scf", "value": 3.0, "tol": 0.05,
      "probe": { "kind": "max_vm_over_nominal", "nominal": 1.0e6 },
      "derivation": "docs/validation/hand-calcs.md#kirsch-plate" }
  ]
}
```

## 6. Live solve progress — `<run_dir>/progress.json`

For the GUI progress display. The runner rewrites it (tmp+rename) at phase
boundaries, on a ~500 ms heartbeat during long mesh/assemble/solve stretches,
and during CG every progress chunk (`cg_iter` / `cg_resid` / `phase_frac`).

```jsonc
{
  "phase": "solve",                // mesh | assemble | solve | recover | done
  "phase_frac": 0.62,              // 0–1 within phase (CG: it/max_it)
  "elapsed_ms": 8400,
  "cg_iter": 310, "cg_resid": 3.2e-7,  // null when not in iterative solve
  "n_elems": 12000, "n_nodes": 4000,  // optional; set once mesh exists
  "run": { "cfg_id": "cfg-0007", "part": "plate_hole", "tier": 1 }
}
```

### 6b. Live mesh preview — `<run_dir>/mesh_preview.pmp`

Binary boundary snapshot written after each successful mesh (for the GUI
viewport). Little-endian:

| field | type | notes |
|-------|------|--------|
| magic | 4 bytes | `PMP1` |
| n_nodes | u32 | |
| n_quads | u32 | boundary faces (tri as degenerate quad v2==v3) |
| n_elems | u64 | element count (info only; connectivity not stored) |
| nodes | n_nodes × float32×3 | xyz metres |
| quads | n_quads × u32×4 | node indices |

Writing only the boundary keeps campaign I/O cheap vs dumping the full volume.
