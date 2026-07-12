# ADR-0020: True BRep volume meshing (product path)

- Status: accepted (2026-07-12)
- Decision: D20
- Supersedes in part: ADR-0001 default product input (STL-first)

## Context

`load_step()` historically used OpenCASCADE only to tessellate into
`TriSurface`, so STEP and STL shared the same discrete pipeline. Owner
requirement: raw CAD imports (`.brep` / `.step` / `.stp`) for honest meshing
and feature/edge alignment — not hand-authored STL as the product geometry.

## Decision

1. Introduce first-class **`CadModel`** holding the live BRep (`TopoDS_Shape`
   behind a pimpl when `POLYMESH_WITH_OCC` is on), plus classified vertices,
   edges, and faces.
2. Product mesh/solve and the short campaign suite require a `CadModel`.
3. Tessellation is **derived only** for:
   - viewport display,
   - legacy hybrid_zoo fill bridge (`boundary_surface_for_legacy_fill`),
   - optional STL **compare** export.
4. Varyhedron (ADR-0021) must not treat tessellation as primary geometry.
5. Product/campaign builds document **OCC ON** (`-DPOLYMESH_WITH_OCC=ON`).
   Non-OCC builds may still compile unit tests that never open CAD.

## Consequences

- Amend ADR-0001: OCC is required for product geometry path; STL remains
  compare/legacy only.
- Part library generators emit STEP; STL only under an explicit compare flag.
- Edge-profile metrics and auto-h use BRep curves/surfaces.

## Alternatives rejected

- STEP fixtures with tessellate-first forever (weak anti-cheat story).
- Full industrial CAD-constrained hex mesher as v1 (too large; iterate via
  packing loop instead).
