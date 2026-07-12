# ADR-0021: Varyhedron — variable polyhedral packing mesher

- Status: accepted (2026-07-12)
- Decision: D21
- Related: ADR-0012 (hybrid + graded legacy), ADR-0019 (FE+VEM), ADR-0020 (BRep)

## Context

Graded tet uses multi-level LEB layers on a Cartesian lattice. Owner wants a
**variable polyhedron** path (“varyhedron”): not layered N-hedra, but packing
driven by CAD edge/face constraints for clean boundary profiles and surface
finish within element budgets.

## Decision

1. Ship **`VolumeMesher::kVaryhedron`** as a **new** mesher from day one
   (not a rename-only of graded). CLI: `varyhedron`. GUI label: **Varyhedron**.
2. **Tooltip (normative UI copy):**

   > **Varyhedron** — variable polyhedral meshing: cells are mixed polyhedra
   > whose sizes and face counts adapt to the CAD geometry. Boundary edges are
   > packed to match the CAD edge profile within the element budget (not
   > uniform layered N-hedra). Interior uses 3D packing chosen for the part’s
   > edge/face constraints. Higher order uses entity packing so corners and
   > faces stay conforming.

3. **v1 algorithm** (subject to refinement in `docs/research/varyhedron-packing.md`
   and agent loops):
   - Map CAD edges/faces (ADR-0020).
   - Boundary-edge constrained seed packing (protect CAD edge profiles).
   - Interior: dual / cluster of a refined tet scaffold into polyhedra when
     needed; pure packing preferred as quality improves.
   - Solve arbitrary polys with **VEM** (`kPolyVem`); FE for regular zoo cells.
   - Order packing: hierarchical min-rule / same-order faces for clean p↑.

4. **Legacy `kGradedTet`** remains labeled “graded tet (legacy)” until
   varyhedron wins the curved scorecard; short campaigns **exclude** graded.

5. Prefer **BSD-3-compatible reimplementation** of packing ideas over vendoring
   AGPL meshing codes into the core library. Optional plugins may wrap third
   parties; license must be documented before merge.

## Research anchors

- Bubble/sphere packing → quality seeds (Shimada et al.).
- Dual-of-tet polyhedral cells (cfMesh / polyDualMesh lineage).
- Field-aligned hex-dominant packing for prismatic bulk (PGP3D-class).
- CAD feature protecting balls / edge constraints (CGAL Mesh_3, PLC ideas).

## Consequences

- Campaigns: `varyhedron` + `hybrid_zoo` only for `varyhedron-short-1`.
- Quality.json includes boundary edge Hausdorff residual vs CAD.
- Agent loops iterate packing parameters and algorithms; git tracks results.

## Alternatives rejected

- Rename graded only without a new packing path.
- Require full frame-field hex-dominant as the only v1 path (ship dual-poly
  scaffold first, then improve).
