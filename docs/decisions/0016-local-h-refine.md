# ADR-0016: True local h-refine — longest-edge bisection (no hanging nodes)

- Status: accepted (2026-07-10)
- Track: D4

## Context
ADR-0014 ships adaptive **global remesh** with Dörfler centroids as graded-fill
seeds. That concentrates DOFs but is not true local h-refinement of an existing
tet mesh. D4 requires a documented + tested local path with either hanging-node
constraints or remesh conformity.

## Decision
**Option B (conforming local refine), Rivara longest-edge bisection (LEB):**

1. Mark tet4 elements (e.g. Dörfler indices).
2. For each remaining mark, walk the **longest-edge propagation path (LEPP)**
   until a terminal edge (longest for every tet that currently shares it).
3. Insert one midpoint on that edge; **bisect every tet that shares the edge**
   into two children.
4. Repeat until all original marks have been bisected at least once (conformity
   neighbors may be bisected without becoming new marks).

**Output is always conforming tet4** — no hanging nodes, no multipoint
constraints in assembly. Seed remesh (ADR-0014) remains the default when local
refine is inapplicable (non-tet meshers, empty marks, or failure).

## API
- `mesh::local_refine_tets(nodes, tets, marked_element_indices)` → refined
  nodes + tets (`TetFillOutput`-compatible).
- Pipeline (optional): on adapt passes with tet/graded-tet mesher, try LEB on
  the current `NodalMesh` before falling back to full `volume_mesh` remesh.

## Alternatives considered
| Option | Why not now |
|--------|-------------|
| A — remesh-only (ADR-0014 seeds) | Interim product path; not “true local” |
| Local 8-subdivision (red) + green neighbors | Face/edge midpoint cascades can refine far from the mark; more code |
| Hanging-node MPCs in assembly | Touches frozen GATE-1 assembly; deferred |
| Full red-green tet | Correct but larger than D4 minimum |

## Guarantees / limits
- **Tet4 only.** Mixed hex/pyramid/VEM meshes are not refined locally.
- **Positive volume:** children are orientation-fixed; parent volume splits ~½+½.
- **Deterministic:** LE ties broken by ordered endpoint indices; mark pick is
  lowest index.
- **Not geometry-aware:** midpoints are Euclidean; boundary midpoints are not
  projected to CAD/STL (surface snap remains a mesher concern).
- **1-level marks:** each marked tet is refined once via LEB; deep multi-level
  h-adapt is repeated adapt passes / re-marking.

## Related
- ADR-0014 (seed remesh interim)
- ADR-0004 (face-based PolyMesh — future local splice target)
- `docs/ROADMAP.md` D4
