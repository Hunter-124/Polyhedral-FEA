# ADR-0018: Graded tet conformity via LEB (not 2:1 hanging Kuhn)

- Status: accepted (2026-07-10)
- Track: mesher overhaul G0/G1 (`docs/plans/mesher-solver-overhaul.md`)

## Context
`graded_tet_fill_surface` previously emitted bulk Kuhn cubes at step 2 next to
skin/feature `2×2×2` step-1 Kuhn cubes. Shared faces then had mid-edge nodes
only on the fine side → **hanging-node nonconformity**. Unit tests only checked
positive volumes, so the bug shipped.

FE results on graded meshes were not trustworthy; hex grid looked better
because it stayed conforming and used isoparametric hex8.

## Decision
1. Emit a **uniform coarse Kuhn lattice** at target `h` for every interior cell.
2. Mark free-surface / feature / seed cells as refine targets (existing stamps).
3. Apply **Rivara LEB + LEPP** (`local_refine_tets`, ADR-0016) for
   `subdivision` passes (default 2) on tets whose centroid lies in a marked
   cell. Propagation keeps the mesh face-conforming.
4. Rebuild boundary nodes from unpaired faces; Jacobian-safe snap as before.
5. Ship `tet4_face_conformity` (topology + AABB hanging-face detector) and
   Catch2 coverage so regressions fail loudly.

## Consequences
- Graded meshes are conforming tet4 (no multipoint constraints).
- Element count / shapes differ from pure 2×2×2 Kuhn (LEB, not red templates).
- Optional follow-on: red-green transition templates for fewer buffer tets (G2).
- Mesher note string: `graded tet v4 (LEB-conforming geo)`.

## Alternatives rejected
- Keep 2:1 Kuhn + hanging MPCs — solver complexity, easy to get wrong.
- Flood-refine until no coarse–fine face remains — fills the volume with fine.
- True Delaunay graded mesh — still ADR-0015 follow-on.
