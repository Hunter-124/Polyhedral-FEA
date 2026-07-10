# GATE 1 — P1 baseline convergence report

**Date:** 2026-07-10  
**Status:** ready for owner review (⛔ GATE 1)  
**Suite:** 37/37 Catch2 tests green  

This freezes the P1 reference solver (tet4/tet10/hex8/hex20 isoparametric
FEM + SimplicialLDLT) as the benchmark comparator for later phases. Later
phases may add alongside it; they must not modify the frozen path.

Reference values live only in `bench/reference/*.json` (engineering rule #1).
Setup rationale: `docs/decisions/0009-tier1-verification-setups.md`.

---

## Tier 0 — Correctness gates

| Check | Result |
|---|---|
| Patch test (all 4 types, distorted meshes) | PASS, max error &lt; 1e-12 m |
| Rigid-body modes | PASS (&lt; 1e-12 relative energy) |
| Single-element eigenvalues (6 zero modes) | PASS |

---

## Tier 2 — MMS energy-norm convergence (order is the metric)

Uniform h-halving n=4 → n=8 on the unit cube. Cubic manufactured field
(seed 2026) so the solution is outside every P1 FE space.

| Element | Theory order p | Observed order | Energy error (coarse → fine) |
|---|---:|---:|---|
| tet4 | 1 | **0.997** | 178.10 → 89.25 |
| hex8 | 1 | **0.997** | 176.28 → 88.31 |
| tet10 | 2 | **2.000** | 8.920 → 2.230 |
| hex20 | 2 | **2.000** | 7.596 → 1.899 |

Tolerance: ±0.2 of theory. All pass.

### ASCII convergence (log₂ energy error vs refinement step)

```
order ≈ −Δlog₂(E) per h-halving

tet4   E: ████████████████████ 178.1
       E: ██████████           89.2     Δ → order 0.997  (theory 1)
hex8   E: ████████████████████ 176.3
       E: ██████████           88.3     Δ → order 0.997  (theory 1)
tet10  E: ████████              8.92
       E: ██                    2.23    Δ → order 2.000  (theory 2)
hex20  E: ███████               7.60
       E: ██                    1.90    Δ → order 2.000  (theory 2)
```

Exact-representation sanity (quadratic MMS field on tet10/hex20): relative
energy error &lt; 1e-9.

---

## Tier 1 — Analytical solutions

| Case | Metric | Result | Tolerance |
|---|---|---|---|
| Timoshenko cantilever (hex20, gravity) | tip deflection | **1.50%** rel err | ≤ 3% |
| Lamé cylinder (hex20 sector) | u_r at inner wall | **0.0068%** | ≤ 1% |
| Lamé cylinder | hoop stress at inner wall | **1.36%** | ≤ 4% |
| Kirsch plate (hex20 annulus, exact field BC) | SCF at hole equator | **3.056** vs 3 (**1.87%**) | ≤ 5% |
| Goodier cavity (hex20 shell octant, b/a=15) | SCF at cavity equator | **1.902** vs 2.045 (**7.04%**) | ≤ 12% |
| L-domain (hex20, uniform h) | energy-gap order | **1.265** vs 2λ=1.089 | ±0.35 |
| L-domain | corner von Mises growth | 7.52e5 → 1.07e6 → 1.24e6 | monotonic ↑ |

### Kirsch SCF bar

```
exact SCF = 3.0
fem   SCF = 3.056   ██████████████████████████████░  +1.87%
```

### Goodier SCF bar

```
exact SCF = 2.045
fem   SCF = 1.902   ████████████████████████████░░░  −7.04%
(finite domain + nodal-averaged recovery; ADR-0009)
```

### L-domain energy self-convergence

```
E(n=2) = 0.106071
E(n=4) = 0.106494   Δ = 4.23e-4
E(n=8) = 0.106671   Δ = 1.76e-4
energy-gap order = log2(Δ_coarse/Δ_fine) = 1.265   (theory 2λ ≈ 1.089)
```

---

## Infrastructure landed with GATE 1

- **Gmsh `.msh` v2.2 ASCII import** (`fea/msh.hpp`): tet4/10, hex8/20 volumes;
  tri3/6, quad4/8 surfaces with physical-group tags. Unit-tested with
  hand-written fixtures (no system `gmsh` required).
- **Reference JSONs** with citations: `kirsch-plate.json`, `goodier-cavity.json`,
  `l-domain.json`, `lame-cylinder.json`.
- **ADR-0009**: Tier-1 verification setups.

---

## What freezes at GATE 1

Once approved, the P1 solver path is the **benchmark baseline** (ADR-0005):

- Element formulations and assembly for tet4/tet10/hex8/hex20
- Dirichlet partitioning + SimplicialLDLT
- Body-force and surface-traction load assembly
- Nodal-averaged stress recovery (ZZ is P5)
- The MMS harness and Tier-0 gates

Later phases **add** hybrid meshing, VEM, adaptivity, CUDA — they do not
rewrite this baseline to chase scores.

---

## Open items deferred past GATE 1

- Exact Goodier stress-field Neumann BCs (tighter SCF without large domains)
- ZZ superconvergent recovery for peak-stress benchmarks
- System-Gmsh generated curved meshes in CI (import is ready)
- CalculiX cross-check inside `/audit` (ADR-0005)
- Geometric validity (watertight / Jacobians / conforming interfaces) — P2

---

## Owner action

Please review this report and the suite results. Reply with **GATE 1 approved**
to freeze the baseline and unlock P2 (conforming tet mesher from STL +
validity, wire into the GUI).
