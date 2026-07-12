# ADR-0022: Full experiment warehouse + headless Grok improvement loop

- Status: accepted (2026-07-12)
- Decision: D22

## Context

Campaigns already write `results.jsonl` / checkpoints. Owner wants:

1. Full experiment warehouse (every mesh VTU, wire PNG, quality, result) in
   git (LFS for large binaries) so agents never re-run blind.
2. After testing, call **Grok CLI headless** to continue improvements from
   results and screenshots.
3. Autonomous mode auto-accepts tools; supervised mode surfaces agent
   questions in the GUI.

## Decision

### Warehouse layout

Under `bench/campaigns/<name>/`:

```
runs/<cfg_id>/<part>/t<tier>/
  mesh.vtu
  wire.png          # optional until shot harness lands
  quality.json
  result.json       # single-run mirror of the jsonl line
results.jsonl
checkpoint.json
progress.json
PARETO.md / PARETO.json
HANDOFF.md / handoff.json
```

- Track `*.vtu` and large `*.png` via **git-LFS** (`.gitattributes`).
- Commit after each campaign batch (or logical agent improvement unit).

### Short campaign shape set

Product campaign geometries (STEP only):

- `plate_hole`
- `cylinder`
- `sphere`
- `icecream_cone` (triangular pyramid with ball intersecting a face)

Meshers: **`varyhedron`**, **`hybrid_zoo`**. Approximately **3 tiers/runs per
shape** for solvetime/quality trends (`keep_frac: 1.0`, no aggressive trim).

### Grok invocation (normative automation)

```bash
grok -p --yolo --permission-mode bypassPermissions \
  --cwd <repo> --max-turns <N> \
  --prompt-file bench/campaigns/<name>/HANDOFF.md
```

- **Autonomous:** handoff rules answer agent questions with the recommended
  default; no interactive plan-mode requirement.
- **Supervised (GUI):** present questions in a queue/modal; user answers are
  relayed on the next resume/`-c` or follow-up `-p`.
- Interactive TUI (plan mode, Ctrl+O) remains optional for humans; **not** the
  overnight default.

### Safety

- Deny force-push and destructive commands outside the repo in invoke scripts.
- Always `git pull --rebase` before push (AGENT_BOOTSTRAP).
- Cap `--max-turns` and document cost controls.

## Consequences

- Interfaces.md §7–§8 define warehouse and handoff schemas.
- `scripts/write_grok_handoff.py` + `scripts/invoke_grok_improve.sh`.
- GUI Test Lab shows git HEAD + campaign sync state.

## Alternatives rejected

- Results-only git (no VTU/PNG) — owner chose full warehouse.
- Interactive TUI as the only automation path — too brittle headless.
