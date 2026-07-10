# PROGRESS

## Current phase
**P0 — Decisions & scaffolding: complete, pending GATE 0 human review.**
Next: P1 — reference solver on standard elements (tet4/tet10, hex8/hex20).

## Done
- 2026-07-09: D1–D5 + GUI scope ratified with owner (ADR-0001..0006).
- 2026-07-09: Cargo workspace scaffolded (geom, mesh, adapt, fea, bench, cli);
  STL loader with welding, face-based mesh structure with structural validity
  checker, Material/D-matrix with unit tests, reference-case loader, CLI
  `check` subcommand. CI: fmt + clippy -D warnings + test.
- 2026-07-09: License AGPL-3.0-or-later applied; git identity policy recorded
  in CLAUDE.md.

## Benchmark table
| Case | Status |
|---|---|
| Tier 0 (patch/rigid-body/eigen/validity) | not yet — needs P1 elements |
| Tier 1 analytical suite | not yet |
| Tier 2 MMS | not yet |
| Tier 3 performance | not yet |

## Open issues
- GATE 0 review by owner: repo skeleton + ratified decisions.
- CLA/DCO policy before first external contribution (ADR-0002).
- `occ` feature wiring deferred until P3 consumes exact geometry (ADR-0001).
- Geometric validity checks (watertight, Jacobians, conforming interfaces)
  are P2 scope; only structural checks exist today.
