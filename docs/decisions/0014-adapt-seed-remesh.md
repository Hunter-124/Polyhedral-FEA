# ADR-0014: A posteriori Dörfler seed remesh

- Status: accepted (2026-07-10)

## Decision
Adaptive remesh after ZZ recovery:
1. Dörfler-mark high-η elements (θ = 0.3).
2. Collect element centroids as `refine_seeds`.
3. Shrink global h (uniform factor 0.75, floor 0.35 · h₀).
4. On the next mesh, graded Cartesian fill marks coarse blocks whose center
   lies within `seed_band ≈ 1.5 h` of any seed as fine (h/2 Kuhn), in addition
   to free-surface skin and optional sharp-edge feature bands.

## Why
Full local h-refinement with hanging nodes is deferred. Seed balls reuse the
existing conforming graded tet path and concentrate DOFs where the indicator
is large without remeshing the whole domain to h_min.

## CLI / GUI
- CLI: `polymesh solve … --adapt n`
- GUI: existing “adapt passes” slider; seeds flow through `SolveJob`.
- If the user mesher is uniform tet fill and seeds are non-empty, remesh uses
  graded tet so local balls take effect.
