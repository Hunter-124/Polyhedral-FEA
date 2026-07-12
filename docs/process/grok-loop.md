# Grok improvement loop

Normative automation after a campaign finishes (ADR-0022). Complements
`docs/process/feedback-loop.md` (Pareto → defaults) and `agent-loop.md`
(session discipline).

## When it runs

After `polymesh_testlab` sets `checkpoint.state == "finished"` for a campaign
that has `"on_finish": { "analyze": true, "grok_handoff": true }` (or the
operator runs the scripts manually):

1. `python3 scripts/analyze_campaign.py <name>`
2. `python3 scripts/write_grok_handoff.py <name>`
3. Commit warehouse + PARETO + HANDOFF (agent or human)
4. `./scripts/invoke_grok_improve.sh bench/campaigns/<name>`

## Headless invoke (default)

```bash
grok -p --yolo --permission-mode bypassPermissions \
  --cwd "$(git rev-parse --show-toplevel)" \
  --max-turns "${GROK_MAX_TURNS:-80}" \
  --prompt-file "bench/campaigns/<name>/HANDOFF.md"
```

Flags:

| Flag | Purpose |
|------|---------|
| `--yolo` | Auto-approve tool executions |
| `--permission-mode bypassPermissions` | Same intent for permission layer |
| `--max-turns` | Cap agent cost / runaway loops |
| `--prompt-file` | Handoff pack (results, shots, open DAG nodes) |

## Handoff contents

`HANDOFF.md` must include:

- Sync protocol (`git pull --rebase`, no force-push, Hunter-124 identity)
- Campaign name, git HEAD, paths to `PARETO.md`, `results.jsonl`, warehouse runs
- Per-shape trend summary (mesh_ms / solve_ms / quality / accuracy vs tier)
- Paths to mesh wire PNGs for visual review (`read_file` on images)
- Open `PROGRAM.yaml` nodes relevant to packing/meshing
- Instruction: propose one packing/sizing improvement, implement + test,
  re-run short campaign or a thin slice, commit+push, rewrite handoff if looping

`handoff.json` is the machine-readable twin for GUI/scripts.

## Autonomous vs supervised answers

| Mode | Question handling |
|------|-------------------|
| **Autonomous** | Handoff rules: pick the recommended option; never block on ask |
| **Supervised** | GUI queues questions; user answers; next `grok -c` or `-p` continues |

## Safety

- Scripts should `--deny` force-push patterns where supported.
- Never `rm -rf` outside the repo; never rewrite published history.
- Prefer small commits scoped to one DAG node.

## Manual interactive (optional)

Human may open interactive Grok, enter plan mode, Ctrl+O once for bypass, send
`/new` then paste HANDOFF content. This is **not** the overnight default.
