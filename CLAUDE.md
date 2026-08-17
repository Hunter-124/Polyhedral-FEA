# Polyhedral-FEA — agent notes

Human standards, layout, anti-cheat, and Eigen traps: **[CONTRIBUTING.md](CONTRIBUTING.md)**.  
Phases / work items: **[docs/ROADMAP.md](docs/ROADMAP.md)** · progress: **[docs/progress.md](docs/progress.md)**.

## Active program (do not skip)

**Learned mesh advisor — corpus and retrain program.**  
**Design:** [docs/advisor/0001-architecture.md](docs/advisor/0001-architecture.md) · **live log:** [docs/advisor/0003-training-log.md](docs/advisor/0003-training-log.md) · **latest model:** [docs/advisor/0008-v4-corpus-retrain.md](docs/advisor/0008-v4-corpus-retrain.md)  
**ADRs:** [0026](docs/decisions/0026-anisotropic-metric-adaptivity.md) · [0027](docs/decisions/0027-learned-mesh-advisor.md) · [0028](docs/decisions/0028-boundary-conformance-hardening.md) · [0029](docs/decisions/0029-independent-truth-and-honest-gates.md) · [0030](docs/decisions/0030-the-ruler-was-wrong.md)–[0033](docs/decisions/0033-a-gate-must-measure-what-ships.md)  
**Training box:** [docs/training/HANDOFF-3080ti.md](docs/training/HANDOFF-3080ti.md)

Methodology still in force from the measure-first program ([plan](docs/plans/advisor-measure-first-program.md), ADR-0023/0024): measure before claiming, no dual-first, no frame-field core, **never score raw nodal max stress**, and a gate must measure the cell that ships (ADR-0033).

[docs/dag/PROGRAM.yaml](docs/dag/PROGRAM.yaml) is a **frozen historical board** (through node G4, 2026-07-13), not the live tracker.

Open defects worth knowing before touching the meshers (ADR-0033): the graded sliver chain (`cylinder` graded h=0.005 builds a mesh CG cannot solve) and the `ellipsoid_boss` boundary tail (binding constraint is `hex8_shape_quality >= 0.02` vs required wall travel — the next thread is the size field, not the snap).

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
