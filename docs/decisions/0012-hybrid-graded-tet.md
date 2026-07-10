# ADR-0012: Hybrid meshing — graded all-tet until pyramids exist

- Status: accepted (2026-07-10)

## Context
True hex-core + tet-skin needs **conforming transitions**. A hex face is a
quad; a tet face is a triangle. Without pyramids (or mortar), a mixed
hex/tet mesh is non-conforming and breaks the nodal assembly path.

## Decision
Ship **`graded_tet_fill`** as the hybrid-capable mesher:

- Coarse Kuhn tets deep in the interior (spacing `h`)
- Fine Kuhn tets (`h/2`) in a boundary skin of `skin_layers` cells
- Fully conforming, deterministic, seed-free

GUI/CLI expose this as **“graded tet”**.

## Deferred
Hex core + pyramid5 transition + tet skin (true hybrid zoo). Documented as
follow-on once pyramid shape functions pass patch tests.

## Alternatives rejected for now
- Non-conforming hex/tet with hanging nodes — needs mortar; out of P2 scope.
- Flood-fill “convert interface hex → tets” — converges to all-tet on connected
  domains, so no hex core remains.
