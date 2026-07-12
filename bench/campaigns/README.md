# Campaign warehouse

Layout for experiment campaigns under `bench/campaigns/<name>/`.
Normative schema: [docs/dag/interfaces.md §7](../../docs/dag/interfaces.md) ·
[ADR-0022](../../docs/decisions/0022-experiment-warehouse-grok-loop.md).

## Directory layout

```
bench/campaigns/<name>/
  campaign.json                  # grid, parts, tiers, warehouse flags
  checkpoint.json                # resume state (runner)
  progress.json                  # live phase heartbeat
  results.jsonl                  # one JSON object per finished run
  PARETO.md / PARETO.json        # after scripts/analyze_campaign.py
  HANDOFF.md / handoff.json      # after scripts/write_grok_handoff.py
  runs/<cfg_id>/<part>/t<tier>/
    mesh.vtu                     # volume mesh (git-LFS)
    wire.png                     # wireframe preview (git-LFS)
    quality.json                 # surface/edge metrics
    result.json                  # single-run mirror of the jsonl line
```

- **`cfg_id`**: stable hash-id of config values (e.g. `cfg-0007`).
- **`part`**: case stem (`plate_hole`, `cylinder`, …).
- **`t<tier>`**: tier index as `t0`, `t1`, `t2` (matches successive-halving
  order in `campaign.json` `tiers[]`).

Warehouse files are written by `polymesh_testlab` when `campaign.json` has
`"warehouse": true` (node **V3b**): `mesh.vtu`, `quality.json`, `result.json`.

### Wireframe PNGs (`wire.png`)

After mesh VTUs exist, render exterior wireframes with:

```sh
# Campaign name or path under bench/campaigns/
python3 scripts/warehouse_shots.py varyhedron-short-1
python3 scripts/warehouse_shots.py bench/campaigns/varyhedron-smoke --force
python3 scripts/warehouse_shots.py varyhedron-short-1 --hole-zoom   # plate_hole ROI
```

`warehouse_shots.py` walks `runs/**/mesh.vtu` → sibling `wire.png` via
[`scripts/vtu_wire_png.py`](../../scripts/vtu_wire_png.py) (pure Python, no
meshio). Skips existing PNGs unless `--force`. Testlab also runs this
post-hook when `"warehouse": true` so HANDOFF packs can list shots (V9b).

## Short-campaign defaults (Lane V)

| Field | Value |
| --- | --- |
| Meshers | `varyhedron`, `hybrid_zoo` |
| Shapes | `plate_hole`, `cylinder`, `sphere`, `icecream_cone` (STEP; V2) |
| Tiers | ~3, `keep_frac: 1.0` (no aggressive trim) |
| Finish | `"on_finish": { "analyze": true, "grok_handoff": true }` |

Skeleton: [`varyhedron-short-1/`](varyhedron-short-1/) (executed under **V8**
once parts + mesher land).

## git-LFS

Root [`.gitattributes`](../../.gitattributes) tracks:

- `*.vtu` (all volume meshes)
- `bench/campaigns/**/runs/**/*.png` (warehouse wire previews)

**Once per machine / clone:**

```bash
git lfs install
```

Without LFS installed, clones store pointer files instead of binaries.
Large JSON/MD (results, PARETO, HANDOFF) stay normal git objects.
