# D6 Tier-3 — L-domain uniform tet10 vs graded tet10

Instrument for ROADMAP **D6** / SPEC Tier-3 (≥5× DOF, ≥3× wall time vs uniform tet10 baseline, ADR-0005). **Same assembly and linear solver**; only the mesh density strategy changes (geometric layers toward the re-entrant corner vs uniform structured tet10).

_Label: `d6-tier3` · timestamp `2026-07-10T10:20:00Z`_

## Headline (equal strain-energy match, 0.01% tol)

| Path | Tag | Free DOFs | Energy (J) | Total s |
|---|---|---:|---:|---:|
| uniform tet10 | `n6` | **6384** | 0.106614 | **2.762** |
| graded tet10 | `h0=w/8_rho2` | **1248** | 0.106611 | **0.2267** |

## Ratios (uniform / graded)

| Metric | Value | Target | Met? |
|---|---:|---:|---|
| **DOF ratio** | **5.115×** | ≥5.0× | yes |
| **Wall-time ratio** | **12.18×** | ≥3.0× | yes |

- **Claim mode:** `graded_energy_ge_uniform_n6_with_fewer_dofs`
- **Energy ref (max over suite):** 0.106705 J
- Galerkin energy from below; match requires \(E_{\mathrm{graded}} \ge E_{\mathrm{uniform}}(1-10^{-4})\).

**Status:** Tier-3 targets **met** on this L-domain instrument. Product-mesh (STL graded + ZZ adapt) on the full public geometry suite remains future work.

## Reproduce

```sh
cmake -S . -B build -G Ninja -DPOLYMESH_WITH_GUI=OFF
cmake --build build --target polymesh-d6-tier3 -j
python3 bench/d6/run_tier3.py --full --render
```

Artifacts: `bench/d6/out/polymesh-d6-l-domain-raw.json` (full suite), `bench/results/polymesh-d6-l-domain.json` (scoreboard rows).

