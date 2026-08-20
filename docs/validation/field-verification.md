# Field verification — stress *and* deformation, pointwise

`bench/reference/*.json` gates one scalar per part (tip deflection, peak SCF)
against [hand-calcs.md](hand-calcs.md) at a 5–15% band. That catches a broken
solve. It does not catch a solve that gets the peak right and the field wrong,
and it says almost nothing about the **deformation** beyond a single maximum.

This is the field-level check. Harness:

```sh
python3 scripts/verify_fields.py            # 33 checks on the cached showcase VTUs
python3 scripts/verify_fields.py --h-study   # + cantilever h-refinement
```

It reads the solve VTUs that `scripts/render_showcase.py` caches in
`build/showcase/`, so it verifies **exactly the fields the shipped figures
draw** — not a separate toy problem. Nonzero exit if any check leaves its band.
Measured 2026-08-20 at `33d60bf`: **33/33 inside band.**

---

## 1. Deformation

| part | quantity | measured | truth | error |
|---|---|---|---:|---:|
| cantilever | tip deflection | 2.001388e-4 m | Timoshenko 2.0153e-4 | **−0.690%** |
| cantilever | tip section rotation | 2.986804e-4 rad | PL²/2EI = 3.0e-4 | **−0.440%** |
| cantilever | plane sections stay plane, 1 − R² | 8.93e-7 | 0 | — |
| cantilever | interior curvature coefficient | 2.998513e-4 1/m | P/(2EI) = 3.0e-4 | **−0.050%** |
| cantilever | shear angle | 1.553818e-6 rad | P/(κGA) = 1.53e-6 | **+1.56%** |
| cantilever | rotation offset / shear angle | 0.8311 | ≈1 | −16.9% |
| cylinder | interior du_z/dz | 4.993850e-6 | σ/E = 5.0e-6 | **−0.123%** |
| cylinder | interior du_r/dr | −1.496819e-6 | −νσ/E = −1.5e-6 | **+0.212%** |
| plate_hole | du_y/dy out of the clamp layer | 0.99779 × (−νσ/E) | 1 | **−0.221%** |
| sphere | max \|u_hoop\| / max \|u\| | 5.96e-3 | 0 | — |
| icecream_cone | max \|u_hoop\| / max \|u\| | 7.97e-4 | 0 | — |
| all five | constrained-node displacement | exactly 0 | 0 | exact |

The **cylinder** is the cleanest deformation statement in the set: uniform axial
tension has no shear correction, no section theory and no fitting, so with both
Saint-Venant end layers excluded (`R < z < H − R`) the axial strain and the
Poisson contraction are exact truths. Both land inside 0.25% on 46,161 nodes.

The **sphere and cone** have no closed form, so the truth used is an invariant:
an axisymmetric body under an axisymmetric load has *identically zero* hoop
displacement. A tetrahedral mesh cannot be axisymmetric, so what this measures is
how much asymmetry the discretisation injects — 0.6% of peak displacement on the
sphere, 0.08% on the cone.

## 2. Stress

| part | quantity | measured | truth | error |
|---|---|---|---:|---:|
| cantilever | von Mises vs √(σ_xx² + 3τ²), 34,952 bending-dominated nodes | ratio 0.998629 | 1 | **−0.137%** (rms 3.20%) |
| cantilever | neutral-plane von Mises | 2.639881e5 Pa | √3·3P/2A = 2.598076e5 | **+1.61%** |
| cantilever | recovered M(x) / P(L−x), 60 stations | 0.995755 | 1 | **−0.425%** (rms 0.55%) |
| cantilever | shear force −dM/dx | 994.85 N | applied 1000 N | **−0.515%** |
| cylinder | interior von Mises | 9.977892e5 Pa | F/A = 1.0e6 | **−0.221%** |
| cylinder | section force ∫vm dA at z = 0.08 / 0.10 / 0.12 m | 7876.3 / 7856.8 / 7826.9 N | 7854.0 N | **+0.28 / +0.04 / −0.34%** |
| plate_hole | section force 8.5a from the hole | 999.78 N | 1000 N | **−0.022%** |
| plate_hole | far-field von Mises (6a…9a) | 9.995324e5 Pa | F/A = 1.0e6 | **−0.047%** |
| plate_hole | stress concentration factor | 3.139853 | Kirsch 3.0, Howland 3.12 | **+0.64%** |

The moment/shear recovery is the strongest of these: `M(x)` comes out of the
exported von Mises by inverting the beam stress state, and `−dM/dx` must return
the applied resultant *by statics alone*. It does, to 0.5%, and it converges —
see §4.

## 3. The 0.7% the cantilever misses, and where it comes from

The tip deflection is 0.690% below Timoshenko, and that number wants an account
rather than a tolerance band. Three measurements pin it down.

**It is not discretisation error.** Over a 14× element-count range the tip
deflection moves by 0.077%:

| h | nodes | elements | tip deflection | vs Timoshenko | θ_tip | vs PL²/2EI |
|---|---:|---:|---:|---:|---:|---:|
| 60 mm | 10,521 | 6,912 | 2.000169e-4 | −0.751% | 2.986014e-4 | −0.466% |
| 45 mm | 135,265 | 94,848 | 2.001249e-4 | −0.697% | 2.986660e-4 | −0.445% |
| 30 mm | 64,657 | 44,832 | 2.001388e-4 | −0.690% | 2.986804e-4 | −0.440% |
| 20 mm | 133,809 | 92,560 | 2.001702e-4 | −0.675% | 2.986940e-4 | −0.435% |

**It is not distributed along the beam.** The interior curvature coefficient is
`P/(2EI)` to −0.050%, and statics fixes `M(x) = P(L−x)` exactly, so the interior
`EI` is right to five parts in ten thousand. The transverse shear compliance is
there too, confirmed twice independently: the neutral-plane shear stress is
+1.6% of `√3·3P/2A`, and the rotation profile carries a constant offset of
0.83 × `P/(κGA)` (§5).

**It is the root.** A fully clamped end face forbids the lateral Poisson
contraction and the shear warping that beam theory's ideal clamp allows, so the
root layer turns through less angle than beam theory says. The harness asserts
that the lost rotation and the lost deflection are the same fact:

> root rotation deficit × L / tip deflection gap = **0.9486**
> (lost rotation 1.320e-6 rad, deflection gap 1.391e-6 m)

95% of the gap is the lost root rotation riding out to the tip. Across the four
meshes above the ratio is 0.92 / 0.95 / 0.95 / 0.96 — stable, so this is a
property of the boundary condition, not of the mesh. If that ratio ever drifts
off 1, the 0.7% has become something else and wants investigating; the check
fails at 15%.

The same effect is directly visible on the **plate**, where the clamped face is
perpendicular to the load and the layer is resolvable station by station. Lateral
strain `du_y/dy` as a fraction of the free-bar value `−νσ/E`, against distance
from the clamped face in plate widths W:

| (x − x_min)/W | 0.02 | 0.10 | 0.18 | 0.26 | 0.35 | 0.43 | 0.51 | 0.59 | 0.67 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| du_y/dy ÷ (−νσ/E) | 0.121 | 0.409 | 0.630 | 0.753 | 0.834 | 0.891 | 0.936 | 0.988 | 1.066 |

Zero contraction at the clamp, full contraction by ~0.6 W. This is why a
whole-plate average of `du_x/dx` reads 5% high and `du_y/dy` 17% low: the plate
is 2 W long, so *every* point is inside one boundary layer or the other. Those
averages are not a solver error and the harness does not gate them; it gates the
recovery, which is what the physics predicts.

## 4. Convergence of the stress recovery

Nodal stress from a quadratic displacement field is element-wise linear and
averaged at nodes, so it converges more slowly than the displacement. Measured on
the same four meshes:

| h | mean M(x) error | shear force −dM/dx |
|---|---:|---:|
| 60 mm | −1.91% | 979.56 N |
| 45 mm | −0.63% | 993.57 N |
| 30 mm | −0.42% | 994.85 N |
| 20 mm | −0.20% | 997.21 N |

Monotone toward the exact values in both columns, while the displacement columns
in §3 are already flat. That ordering is the expected one.

## 5. Two measurements that were wrong before they were right

Recorded because both are easy traps and both produced a convincing false
positive on the first pass.

**A three-term fit of the deflection curve cannot see the shear term.** Fitting
`w(x) = a·x²(3L−x) + c·x + d` on the interior looks like the obvious way to
separate bending from shear. It is ill-conditioned — `cond ≈ 45…390` depending on
the window — and `c/c_theory` came out as 0.25, 0.10, 1.57, −3.26 and −0.68 on
five reasonable windows of the same data. The shear term is worth 0.76% of the
tip value and the two basis functions are 98% correlated, so the fit trades it
against `a` freely. Reported as "shear compliance MISSING", it was a fitting
artifact.

The measurement that does work differences two *independently measured* fields:
`w(x) − w(x₁) − ∫θ dx` must equal `γ·(x − x₁)`, because the section rotation
carries no shear. That lands on `γ = 1.5538e-6` against `P/(κGA) = 1.53e-6`
(+1.6%) — and it still only resolves γ to about ±20% across meshes, which is why
its band is 25%.

**A one-term fit of the rotation profile reads the curvature 0.58% low.** The
exact Saint-Venant `u_x` carries a shear-warping term that is constant along the
beam (V is constant) and odd in z, so a linear-in-z regression picks it up as a
*rotation offset*. Fitting `θ = a·x(2L−x)` with no constant absorbs it into `a`:

| h | a/a_theory, one term | a/a_theory, two terms | offset c | c/γ |
|---|---:|---:|---:|---:|
| 60 mm | 0.99369 | **0.99984** | −1.480e-6 | −0.967 |
| 45 mm | 0.99396 | **0.99984** | −1.414e-6 | −0.924 |
| 30 mm | 0.99422 | **0.99950** | −1.272e-6 | −0.831 |
| 20 mm | 0.99432 | **1.00008** | −1.386e-6 | −0.906 |

The "0.58% low curvature at every h" was a missing basis function. With the
offset admitted the curvature is exact to 0.05%, and the offset itself is
0.83–0.97 γ — the second independent confirmation of the shear compliance.

Two smaller traps, for the same reason:

- **Binned von Mises means read high near the neutral axis.** Comparing
  `mean(vm)` in a ±3.5 mm z-band to `vm(mean z)` gave +25…+34% at
  `z − z_c = ±5 mm`. von Mises is convex in σ_xx and the band spans a factor of 5
  in |σ_xx| there; it is Jensen's inequality, not the solver. The harness
  compares per node.
- **Trapezoid quadrature over bin centres truncates the outer fibres.** The first
  moment integral over 20 bin *centres* covers `[−0.475h, +0.475h]`, and the
  integrand goes as z², so it reads 14.3% low — a bias so flat across stations it
  looked like a real defect. The harness fits the slope of |σ_xx| against |z−z_c|
  and uses `M = k·I`, which needs no quadrature.

## 6. What this does not verify

- **The stress tensor is not exported.** The VTU carries `displacement` and
  `von_Mises` only, so every stress check here goes through von Mises. That is
  exact where the state is uniaxial (cantilever fibres, cylinder wall, plate away
  from the hole) and it is why the section-force test is applied only there. On
  the sphere and cone the state is triaxial — barrelling puts tensile hoop stress
  against compressive σ_zz, so `∫vm dA` runs 8–24% above the applied resultant
  and is *not* an equilibrium statement. A real equilibrium check on those parts
  needs `σ_zz`, i.e. tensor export.
- **Reactions are not exported either**, so equilibrium is checked through the
  interior stress field rather than at the fixture.
- **The load side is already gated elsewhere**: the CLI prints the assembled
  resultant and its conservation error (`Σf = (0,0,−1000) N, conservation
  err = 6.8e-13 N` on the cantilever), and `tests/test_traction_selection.cpp`
  pins the selection rules.
- **Solver convergence rates** are gated separately by
  `tests/test_mms_convergence.cpp` against a manufactured solution; this document
  is about the shipped parts.
