# ADR-0019: Mixed FE+VEM adaptive-order core (arbitrary-p hierarchical basis)

- Status: accepted (2026-07-10)
- Owner call: mixed FE+VEM core; arbitrary-p hierarchical now (not capped at
  p=2). Refines the p-mechanism of ADR-0003 and builds on VEM k=1/k=2
  (ADR-0011 / ADR-0017).

## Context

The product vision is one adaptive method where element **size** (h),
**order** (p), and **shape** (tet / hex / pyramid / arbitrary polyhedra) are
chosen together, per region, to balance accuracy against mesh and solve time.
Today the pieces exist but do not compose:

- FE zoo is tet4/tet10, hex8/hex20, pyramid5 with a *global* p1→p2 promote
  (`p_elevate`); no per-region order, nothing above p=2.
- VEM k=1/k=2 solves arbitrary `PolyCell`s but lives on a separate path; the
  hybrid zoo fan-splits every transition polyhedron into pyramids/tets before
  the solver ever sees it, inflating element counts and injecting sliver
  artifacts precisely where accuracy matters.
- ADR-0003 planned p=1..4 via nodal Lagrange (tet20/tet35). Nodal bases make
  *mixed-order meshes* painful: neighbouring elements of different p share
  faces whose node sets do not nest, so conformity needs constraint equations.

## Decision

### 1. One stiffness matrix, two formulations
Assembly accepts a heterogeneous element list: classic isoparametric FE for
the standard zoo (tet/hex — the fast paths), VEM for everything else
(pyramids at k≥2, general polyhedra, transition cells kept whole). All
elements scatter into the same global K; continuity across an FE/VEM face is
by shared vertex/edge DOFs (both formulations are H¹-conforming with
vertex + edge-midpoint DOFs at order 2). The FE/VEM interface patch test is
the acceptance gate.

### 2. Hierarchical (integrated-Legendre) basis for arbitrary p — not nodal
FE tets and hexes get a Szabó–Babuška hierarchical basis:

- **Vertex modes**: the p=1 barycentric / trilinear functions (unchanged).
- **Edge modes** (2 ≤ k ≤ p): kernel functions φ_k built from integrated
  Legendre polynomials on each edge.
- **Face modes** (k ≥ 3 tet, k ≥ 4 quad-face): products of kernels.
- **Interior modes** (k ≥ 4 tet, k ≥ 6 hex): bubble products.

Hierarchical means the order-p space is a subset of order-(p+1): raising p
*adds* DOFs without moving existing ones. Consequences we are buying:

- **Mixed order is free via the minimum rule**: every mesh *entity*
  (vertex/edge/face/cell) carries its own order = min over adjacent element
  orders. Traces on a shared entity then agree exactly — no constraint
  equations, no mortar, conformity by construction.
- **p-adaptivity is incremental**: element error indicators from the p→p+1
  hierarchical surplus are nearly free.
- Orientation: edge modes of odd k flip sign with edge direction; face modes
  permute with face orientation. Global entity orientation (min-vertex-id
  rule) is fixed at DOF-numbering time — same trick as `p_elevate` mid-edges.

### 3. Order caps by shape
- FE tet/hex: arbitrary p (implementation target p ≤ 6; quadrature tables
  sized for degree 2p).
- VEM polyhedra: k ≤ 2 for now (classical VEM k≥3 needs face/interior moment
  DOFs — deferred, see ADR-0017 discussion). The minimum rule handles the
  seam: a face shared between a p=4 hex and a k=2 poly carries order 2.
- Pyramid: p=1 as FE; at higher order it is treated as a VEM PolyCell
  (rational nodal pyramid bases are a known conditioning trap).

### 4. The (h, p, shape) driver
One adaptive loop decides per region, from three signals:

- **Geometry** (a priori): per-cell turning angle h·κ (ADR-0012 amendment)
  and thin-wall distance. Where geometry error dominates (curved/sharp
  boundary), refine **h** — polynomial order cannot fix a faceted boundary.
- **Smoothness** (a posteriori): ZZ indicator decay between hierarchical
  levels. Smooth-but-wrong regions (interior gradients) raise **p** —
  exponential payoff where the solution is analytic.
- **Cost model**: assembled-DOF and solve-time predictions, calibrated from
  accumulated test-lab campaign data (`bench/campaigns/`, DAG node
  `feedback-loop`) — the measured, not guessed, tradeoff.

Shape tendency (hex/tet/poly preference, fan-split vs native-poly
transitions) becomes a mesher knob the tuner sweeps (DAG `mesher-tendency`).

## Staging (DAG lane B)
1. `p-hierarchical`: basis + per-entity DOF numbering + MMS p=1..4 orders
   match theory (h-refinement at fixed p, and p-refinement on a fixed mesh
   showing exponential decay for a smooth solution).
2. `fe-vem-assembly`: heterogeneous assembly + FE/VEM interface patch test;
   hybrid zoo gains a native-poly mode (transition cells unsplit).
3. `hp-driver`: joint driver replacing uniform `suggest_uniform_refine`.

## Alternatives rejected
- **Nodal Lagrange p=1..4** (ADR-0003's original mechanism): no nesting →
  mixed order needs constraints; worse conditioning at p≥3; superseded here.
- **All-VEM core**: uniform math but gives up mature FE fast paths and
  quadrature-exact stiffness on the 90 %+ of cells that are standard shapes.
- **Non-conforming p-transitions (mortar/hanging)**: extra machinery and
  weaker guarantees than the minimum rule, which is free with a hierarchical
  basis.
- **p2 cap now, arbitrary-p later**: owner explicitly chose arbitrary-p now;
  hierarchical bases make the general case barely harder than the capped one.
