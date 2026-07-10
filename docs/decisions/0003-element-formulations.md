# ADR-0003: Element formulations — unified trait over isoparametric FEM (p=1..4) + VEM

- Status: accepted (2026-07-09, GATE 0)
- Decision: D3

## Decision
One `fea::Element` trait spans the whole zoo:
- **Tets, hexes, prisms, pyramids**: standard isoparametric FEM at adaptive
  polynomial order p = 1..=4 (tet4 → tet10 → tet20 → tet35 node counts).
  Per-region order selection is the core hp-adaptivity mechanism the owner
  described ("adaptive tet 2–16"): cheap low-order elements in benign regions,
  high-order where stress accuracy matters.
- **General polyhedra**: Virtual Element Method (VEM) k = 1, 2 — handles
  non-convex cells and hanging nodes, and has clean hp theory. Stabilization
  correctness is guarded by Tier-0 single-element eigenvalue tests.
- **GPU acceleration** (wgpu compute for assembly / iterative solves) is
  scheduled in the performance phase (P6), not baked into the formulation;
  accuracy-critical arithmetic remains f64 everywhere.

## Alternatives
- Wachspress/mean-value polyhedral FEM: convex-only, effectively first-order —
  incompatible with p-adaptivity goals. Kept as a possible cross-check element.
- VEM for everything: viable but wastes the maturity and speed of standard
  isoparametric elements on standard shapes.

## Why
Owner directive: "combine all possible methods to optimize for accuracy and
computing performance." The unified trait lets the adaptive loop pick the best
formulation-order-shape combination per region while assembly stays blind to
the choice.
