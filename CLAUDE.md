# Polyhedral-FEA — agent notes

Harness entrypoint only; policy lives in the linked files.

| Need | Read |
|---|---|
| Map, layering, standards, workflow, anti-cheat, Eigen traps | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Current state, open defects, next work | [docs/STATUS.md](docs/STATUS.md) |
| Current advisor cycle | [docs/advisor/0012-portable-cost-retrain.md](docs/advisor/0012-portable-cost-retrain.md) |
| External-contributor PR flow | [CHANGES.md](CHANGES.md) |

## Methodology in force

- Measure before claiming; no dual-first, no frame-field core ([ADR-0023](docs/decisions/0023-measure-first-tet-primary-cvt-path.md), [ADR-0024](docs/decisions/0024-advisor-measure-answers.md), [plan](docs/plans/advisor-measure-first-program.md)).
- Never score raw nodal max stress ([ADR-0027](docs/decisions/0027-learned-mesh-advisor.md)).
- Truth is independent of the thing measured; a gate measures the cell that ships ([ADR-0029](docs/decisions/0029-independent-truth-and-honest-gates.md), [ADR-0033](docs/decisions/0033-a-gate-must-measure-what-ships.md)).

Open mesher defects (ADR-0033): the graded sliver chain (`cylinder` graded h=0.005 builds a mesh CG cannot solve) and the `ellipsoid_boss` boundary tail (next thread is the size field, not the snap). Status: [docs/STATUS.md](docs/STATUS.md).

The completed DAG board is frozen history: [docs/archive/dag/PROGRAM.yaml](docs/archive/dag/PROGRAM.yaml).

## graphify

This project has a committed knowledge graph under `graphify-out/` so agents share
the same map of the codebase.

Rules:

- For codebase questions, first run `graphify query "<question>"` when
  `graphify-out/graph.json` exists. Use `graphify path "<A>" "<B>"` for
  relationships and `graphify explain "<concept>"` for a focused concept.
  Prefer these over full-repo greps when the graph has an answer.
- Read `graphify-out/GRAPH_REPORT.md` for broad architecture (god nodes,
  communities) or when query/path/explain are not enough.
- After modifying **code**, run `graphify update .` (AST-only, no API key) and
  commit the updated `graphify-out/` artifacts when the change is structural.
  With hooks installed (`graphify hook install`), post-commit does the code
  rebuild automatically — still commit the resulting graph files if they dirty
  the tree after your feature commit.
- After large **doc/ADR** moves, run a full `/graphify .` (or `--update`) so
  semantic edges stay honest.
- Do **not** commit machine-local files: `.graphify_python`, `.graphify_root`,
  `cache/`, `cost.json`, `graph.html` (regenerate HTML with
  `graphify export html`).

Setup once per clone: see **CONTRIBUTING.md §8**.
