# Test-lab interfaces (normative)

These file formats are the contract between the test-lab harness
(`apps/testlab`), the GUI (`apps/gui`), and the analysis/feedback tooling.
Change them only by editing this file in the same commit as the code change.
All units SI (m, Pa, N, kg, s); all times in milliseconds wall clock.

**Agent strategy:** [docs/plans/advisor-measure-first-program.md](../plans/advisor-measure-first-program.md)
(ADRs [0023](../decisions/0023-measure-first-tet-primary-cvt-path.md),
[0024](../decisions/0024-advisor-measure-answers.md)). Reward = scorecard +
honest accuracy probes — never wire PNG, never raw nodal max stress.

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
  "resources": {
    "max_threads": 0,               // 0 = unlimited
    "max_mem_gb": 0,                // 0 = unlimited
    // M14 wall-clock kills (ADR-0024 Q9):
    "max_run_wall_s": 0,            // 0 → tier defaults: tier0=900, tier1=900, tier2+=2700
    "max_pack_wall_s": 0            // 0 = unlimited pack ceiling; else stop *starting* new runs
  }
}
```

Trimming rule (successive halving): at each tier every surviving config runs
every part; configs rank by weighted score aggregated over parts; the top
`keep_frac` advance. `accuracy` is the relative error against the case's
hand-calc truth (§5), mapped to a 0–1 score as `1/(1+|rel_err|/tol)`.

**Wall-clock (M14):** each run records `wall_time_s` / `max_run_wall_s`. If
elapsed exceeds the limit after mesh (or mid-solve via progress callback),
`status = over_budget` and `over_budget_cause = wall_clock` (solve skipped when
still post-mesh). Pack ceiling never aborts an in-flight run.

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
  "geom_class": {
    "curved_frac": 0.31, "thin": false, "min_feature_h": 2.4,
    "n_features_below_h_min": 0   // M11: sharp CAD edges with L < 2·h
  },
  "n_features_below_h_min": 0,    // same count on the line root (M11)
  "feature_flags": [              // M11: sharp edges with L/3 < tier h_min (≈0.25·h)
    // { "edge_id": 3, "reason": "below_h_min", "length": 0.001, "h_edge": 3.3e-4, "h_min": 0.01 }
  ],
  "mesh_ms": 412, "solve_ms": 1890,
  "wall_time_s": 12.4,            // M14: full-run wall seconds
  "max_run_wall_s": 900,          // M14: limit applied this tier
  "n_elems": 31956, "n_nodes": 11935, "n_dof": 35805,
  "quality": { "M1max": 1.0e-11, "M2max": 0.36, "M6": 0.17, "score": 0.42 },
  "answers": {
    // --- stress ---
    "sigma_max": 9.12e7,            // DIAGNOSTIC only: global nodal max VM (never campaign score)
    "sigma_face_mean": 3.06e6,      // SCORE: area-wtd face-region VM at element centroids (quality-pass)
    "sigma_p99": 4.1e6,             // DASHBOARD: p99 element-centroid VM, quality-filtered
    "n_quality_excluded": 12,       // elems dropped from p99 / face-mean by quality floor
    // --- energy / displacement ---
    "strain_energy": 0.00393,       // ½ uᵀKu (J); primary score for cylinder / no closed-form
    "tip_deflection": 1.2e-4,       // face-mean |u| on load select (BC/RBM health; secondary)
    "tip_deflection_max": 1.5e-4,   // diagnostic global max |u| only
    // --- load-face selection audit ---
    "load_face_area": 0.00785,      // selected load face area (m²)
    "load_area_rel_err": 0.01,      // |A_sel − A_expected| / A_expected when expected_area set
    "n_probe_nodes": 48,
    "n_load_faces": 12
  },
  "health": {
    "free_residual_rel": 1.2e-12,   // ||(Ku−f)_free|| / ||f||
    "reaction_sum_err": 0.01,       // |F+R| / max(|F|,eps); free force vs reactions
    "n_orphans": 0,
    "n_bc_dofs": 36,
    "load_area_ok": true,           // false if expected_area set and rel_err > 5%
    "ok": true                      // residual ∧ reaction ∧ orphans ∧ load_area_ok
  },
  "n_pred_elems": 4200,             // C·V/h³ (later ∫ dV/h³) logged before mesh (M4)
  "n_elems_over_pred": 1.1,         // actual/pred after mesh
  "over_budget_cause": null,        // null | "sizing" | "mesher" | "budget" | "wall_clock"
  // sizing: N_pred ≫ tier; mesher: N_pred OK but N_actual ≫ N_pred;
  // budget: hard-kill ≈2× DOF/elem; wall_clock: M14 per-run wall exceeded
  "scorecard": {
    "edge_hausdorff_over_h": 0.04,  // sharp CAD edges only; null without CAD
    "chordal_efficiency_max": 1.2,  // d_i/(ℓ_i² κ/8) with OCC κ; healthy ≈ [0.8, 3]
    "normal_dev_deg_max": 8.2,      // surface normal deviation (deg); null if unavailable
    "n_dof": 35805,
    "accuracy_rel_err": 0.02,       // first metric rel_err (case-specific probe)
    "min_element_quality": 0.17,    // quality.M6 when present
    "solve_residual_rel": 1.2e-12,  // free residual (gate, not score)
    "health_ok": true
  },
  "accuracy": { "metric": "scf", "value": 3.06, "truth": 3.0, "rel_err": 0.02 },
  "status": "ok"  // ok | solve_suspect | mesh_fail | solve_fail | over_budget
}
```

### Answers roles (normative)

| Field | Role |
|-------|------|
| `sigma_face_mean` | **Score** stress (area-weighted face-mean VM on tagged region; element-centroid samples of quality-passing elements) |
| `strain_energy` | **Score** when no closed-form stress (cylinder; analytic or frozen Richardson) |
| `tip_deflection` | **Secondary health** (BC/RBM; face-mean \|u\| on load select) |
| `sigma_p99` | **Dashboard** only (quality-filtered p99); log `n_quality_excluded` |
| `sigma_max` | **Diagnostic only** — raw nodal max VM; **must never** be campaign score |
| `tip_deflection_max` | Diagnostic global max \|u\| |
| `load_face_area` / `load_area_rel_err` | Load-select audit; gate via `health.load_area_ok` |

### Probe kinds (scoring)

Declared on reference metrics (`bench/reference/<part>.json` → `probe.kind`):

| `probe.kind` | Returns | Campaign role |
|--------------|---------|---------------|
| `mean_vm_over_nominal` / `scf` / `scf_mean` | `sigma_face_mean / nominal` | **Score** SCF (plate_hole hole neighborhood) |
| `mean_vm` / `mean_von_mises` / `face_mean_vm` | `sigma_face_mean` | **Score** stress magnitude |
| `strain_energy` / `energy` | `strain_energy` | **Score** energy (cylinder primary) |
| `tip_deflection` / `max_displacement` / face-mean u components | face-mean \|u\| (or signed load-dir) | **Secondary** health / tip |
| `sigma_p99` / `p99_vm` | `sigma_p99` | Dashboard only |
| `max_von_mises` / raw nodal max | `sigma_max` | **Diagnostic only — never campaign score** |

Displacement metrics use the **face-mean** of |u| (or signed dominant-load
component) over unique nodes of boundary faces whose centroids fall in the
first load select box — never global max as the primary answer. Global max is
`tip_deflection_max` only. If no faces match, fall back to nodes in the load
box; if still empty, tip = 0 (fail-closed).

### Health

After every successful mesh+solve the harness rebuilds `K`, forms `r = Ku − f`,
and reports free residual and reaction balance. Gates:

- `free_residual_rel ≤ 1e-6` (direct)
- `reaction_sum_err ≤ 0.05`
- `n_orphans == 0`
- `load_area_ok` (see load select below)

Failure → `health.ok = false`, `status = solve_suspect`, accuracy scores
zeroed (measured values still recorded for debug). Analyze should filter on
`status == "ok"` and/or `health.ok`. Residual is a **gate**, not a score axis.

### Status values

| `status` | Meaning |
|----------|---------|
| `ok` | Mesh + solve + health gates passed |
| `solve_suspect` | Solved but health failed (residual / reaction / orphans / load area) |
| `mesh_fail` | Mesher error |
| `solve_fail` | Solver error / non-convergence |
| `over_budget` | Hard-kill on DOF (~2× tier) or wall-clock; see `over_budget_cause` |

### Over-budget diagnosis

- Log `n_pred_elems` **before** meshing (`C · V / h³`, later size-field integral).
- `n_pred` ≫ tier → `over_budget_cause = "sizing"`.
- `n_pred` OK, `n_elems` ≫ `n_pred` → `"mesher"`.
- Hard DOF/elem resource kill → `"budget"`.
- Per-run wall-clock exceeded (M14) → `"wall_clock"`.

`geom_class` is computed from the part geometry (not the config) so the
feedback loop can learn per-condition presets: fraction of surface area with
per-cell turning angle > 15°, thin-wall flag (t < 2.5 h_ref), smallest
feature size in units of bulk h, plus **M11** `n_features_below_h_min` (count of
sharp CAD edges with \(L < 2h\); `feature_flags` lists edges with
\(h_{\mathrm{edge}}=L/3\) below tier \(h_{\min}\approx 0.25\,h\)). Flag only —
no OCC defeaturing.

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
    { "select": {
        "box": [[0.049,-1e9,-1e9],[1e9,1e9,1e9]],
        "expected_area": 0.01        // optional; m² of intended CAD face
      },
      "traction": [1.0e6, 0, 0] }
  ],
  "reference": "bench/reference/plate_hole.json"
}
```

Face selection is by axis-aligned box over face centroids (the only robust
selector on tessellated fixtures). Selectors must be written with slack so
they are h-independent.

**Load select `expected_area` guard (±5%).** When `loads[].select.expected_area`
is set, the harness compares selected load face area to that value. If
`|A_sel − A_expected| / A_expected > 0.05`, then `health.load_area_ok = false`,
`health.ok = false`, and `status = solve_suspect`. (Was ±2%; 5% headroom covers
mesh chordal / tip-rim snap vs CAD while still failing wall-in-box over-select
~65%.) Prefer `select.normal_min_dot` (default 0.7) so traction-aligned faces
win over lateral skins whose centroids fall in the load box. Boxes remain OK
for planar fixtures; true BRep face tags come later (curved loads / sweeps).

## 5. Reference truth — `bench/reference/<part>.json`

Hand-calculated answers ONLY (anti-cheat: nothing in src/ or apps/ may embed
these numbers; the harness loads them at runtime, and each entry cites its
derivation in `docs/validation/hand-calcs.md`).

```jsonc
{
  "part": "plate_hole",
  "metrics": [
    { "name": "scf", "value": 3.0, "tol": 0.05,
      "probe": {
        "kind": "mean_vm_over_nominal",   // SCORE: face-mean VM / σ∞ (not max_von_mises)
        "nominal": 1.0e6,
        "select": { "box": [[-0.015,-0.015,-1e9],[0.015,0.015,1e9]] }
      },
      "derivation": "docs/validation/hand-calcs.md#kirsch-plate" }
  ]
}
```

Scoring probe kinds (see §3 answers table): `mean_vm_over_nominal` / `scf`,
`strain_energy`, `tip_deflection`. **`max_von_mises` is diagnostic-only** and
must not appear as a campaign score metric.

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

## 7. Experiment warehouse — `bench/campaigns/<name>/runs/...`

Full warehouse (ADR-0022). Written by `polymesh_testlab` when
`campaign.json` has `"warehouse": true` (default for new short campaigns).

```
bench/campaigns/<name>/
  campaign.json
  checkpoint.json
  progress.json
  results.jsonl
  PARETO.md / PARETO.json          # after analyze_campaign.py
  HANDOFF.md / handoff.json        # after write_grok_handoff.py
  runs/<cfg_id>/<part>/t<tier>/
    mesh.vtu                       # volume mesh (git-LFS)
    wire.png                       # wireframe preview (git-LFS when large)
    quality.json                   # surface/edge metrics
    result.json                    # single-run mirror of the jsonl line
```

### 7a. `quality.json`

```jsonc
{
  "M1max": 1.0e-11,
  "M2max": 0.36,
  "M6": 0.17,
  "score": 0.42,
  "edge_profile_hausdorff_max": 0.012,  // metres; vs CAD edge samples
  "edge_profile_rel": 0.04,             // max / characteristic edge length
  "geom_source": "brep"                 // brep | brep_tessellate_derived | stl_compare
}
```

### 7b. Campaign extensions in `campaign.json`

```jsonc
{
  "warehouse": true,
  "on_finish": { "analyze": true, "grok_handoff": true }
}
```

Large binaries: track `*.vtu` and large `*.png` with git-LFS (see
`.gitattributes`). Pull requires `git lfs install` once per machine.

## 8. Grok handoff — `HANDOFF.md` / `handoff.json`

Written by `scripts/write_grok_handoff.py` after analyze (ADR-0022,
`docs/process/grok-loop.md`).

### 8a. `handoff.json` (machine)

```jsonc
{
  "campaign": "varyhedron-short-1",
  "git_head": "abc123…",
  "finished_utc": "2026-07-12T12:00:00Z",
  "pareto": "bench/campaigns/varyhedron-short-1/PARETO.md",
  "results": "bench/campaigns/varyhedron-short-1/results.jsonl",
  "shots": ["runs/cfg-…/plate_hole/t0/wire.png"],
  "open_program_nodes": ["M6", "M7", "M8", "M9", "M10", "M11", "M12", "M13", "M14", "M5", "G0", "G1", "G2", "G3", "G4", "V6d", "V6e", "V10c", "V11"],
  "mode": "autonomous",           // autonomous | supervised
  "max_turns": 80
}
```

### 8b. `HANDOFF.md` (human / agent prompt)

Markdown body consumed by:

```bash
grok -p --yolo --permission-mode bypassPermissions \
  --cwd <repo> --max-turns 80 \
  --prompt-file bench/campaigns/<name>/HANDOFF.md
```

Must embed bootstrap sync rules, trend tables, shot paths, and the next
experiment instruction. Schema changes to handoff fields require this section
and the writer script in the same commit.

`open_program_nodes` should list open measure / Geogram / follow-on nodes as
applicable: **M6–M14**, **G0–G4**, **V6d**, **V6e**, **V10c**, **V11** (plus
any other still-open board IDs). Do not invent packing-loop work before **M9**
baseline freeze — see agent strategy plan above.
