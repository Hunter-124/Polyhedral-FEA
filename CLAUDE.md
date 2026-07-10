# Polyhedral-FEA — agent notes

Human standards, layout, anti-cheat, and Eigen traps: **[CONTRIBUTING.md](CONTRIBUTING.md)**.  
Phases / work items: **[docs/ROADMAP.md](docs/ROADMAP.md)** · progress: **[docs/progress.md](docs/progress.md)**.

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
