# Geogram (vendored subset) — placeholder

**Status: not vendored yet.**

G0 landed the ADR and this tree stub only. Actual source import is **G1**,
which must wait until **after M10** (wall tangential smooth + OCC surface
project). Do not copy Geogram into this directory before that.

## Normative docs

- [ADR-0025](../../docs/decisions/0025-geogram-cvt-vendor.md) — vendor decision,
  dual hard-block, third_party intent
- [ADR-0024](../../docs/decisions/0024-advisor-measure-answers.md) Q3 (vendor hard
  parts) · Q8 (dual hard-block)
- [docs/research/geogram-cvt-vendoring.md](../../docs/research/geogram-cvt-vendoring.md)
  — full study path, layout, LICENSE checklist

## When G1 lands

Expect under this directory (names may adjust):

- `LICENSE` — upstream BSD-3, unmodified
- `NOTICE` — pin: version/tag/commit, date, URL
- `README.polymesh.md` — what we took / stripped / how to upgrade
- `src/` / `include/` — predicates + ConvexCell / clipped-Voronoi subset only

Lloyd loop, \(1/h^3\) density, constrained sites, and OCC bridge stay in
`src/mesh/` / `src/pipeline/` (we write those). Median dual remains hard-blocked
until G4.
