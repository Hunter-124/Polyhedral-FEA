# Hand-calculated reference truths

Every metric in `bench/reference/<part>.json` cites a section here. Units are
SI throughout (m, Pa, N). These derivations are the **only** source of
numerical truth for the test-lab harness — nothing under `src/` or `apps/`
may embed them (anti-cheat rule #1; see `CONTRIBUTING.md` §0 and
`docs/benchmarks.md`).

Fixture geometry: `tests/fixtures/parts/`. Case bindings (loads/BCs/material):
`tests/fixtures/parts/<part>.case.json` (schema: `docs/dag/interfaces.md` §4).
Reference JSON: `bench/reference/<part>.json` (schema: §5).

STL regeneration (geometry only — does **not** write truths):

```bash
python3 scripts/gen_part_library.py
```

---

## smoke-bar

**Part:** `smoke_bar` · **geometry:** rectangular prism along \(+x\)

| Quantity | Symbol | Value |
|---|---|---|
| Length | \(L\) | \(0.1\,\mathrm{m}\) |
| Cross-section | \(w \times h\) | \(0.01 \times 0.01\,\mathrm{m}\) |
| Area | \(A = wh\) | \(1.0 \times 10^{-4}\,\mathrm{m}^2\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| End traction (on \(x = L\)) | \(\mathbf{t}\) | \((10^6,\,0,\,0)\,\mathrm{Pa}\) |
| Fixed face | \(x \approx 0\) | all three DOFs |

### Stress (uniaxial tension)

The applied surface traction is uniform on the free end. Equilibrium of the
bar requires a uniform axial stress field (Saint-Venant, away from the
loaded/fixed ends the stress is exact; for a prismatic bar under pure end
traction the continuum solution is uniform everywhere):

\[
\sigma_{xx} = t_x = 1.0 \times 10^{6}\,\mathrm{Pa},
\quad
\sigma_{yy} = \sigma_{zz} = \tau_{xy} = \tau_{yz} = \tau_{zx} = 0.
\]

Von Mises stress for uniaxial tension:

\[
\sigma_{\mathrm{vm}}
  = \sqrt{\tfrac{1}{2}\bigl[
      (\sigma_{xx}-\sigma_{yy})^2
    + (\sigma_{yy}-\sigma_{zz})^2
    + (\sigma_{zz}-\sigma_{xx})^2
    \bigr] + 3(\tau_{xy}^2+\tau_{yz}^2+\tau_{zx}^2)}
  = |\sigma_{xx}|
  = 1.0 \times 10^{6}\,\mathrm{Pa}.
\]

**Metric `sigma_vm`:** value \(1.0\times 10^{6}\), relative tolerance \(2\,\%\).

### Tip axial displacement

Hooke's law under uniaxial stress: \(\varepsilon_{xx} = \sigma_{xx}/E\).
Integrating from the fixed end:

\[
u_x(L)
  = \frac{\sigma_{xx}\,L}{E}
  = \frac{10^{6}\cdot 0.1}{2.0\times 10^{11}}
  = 5.0 \times 10^{-7}\,\mathrm{m}.
\]

(Poisson contraction of the cross-section does not affect \(u_x\).)

**Metric `tip_ux`:** value \(5.0\times 10^{-7}\,\mathrm{m}\), relative
tolerance \(2\,\%\). Probe = mean \(u_x\) on the \(x=L\) face nodes.

---

## kirsch-plate

**Part:** `plate_hole` · **geometry:** plate with centred circular hole

| Quantity | Symbol | Value |
|---|---|---|
| Half-width (\(x\)) | \(W\) | \(0.1\,\mathrm{m}\) (\(x\in[-W,W]\)) |
| Half-height (\(y\)) | \(H\) | \(0.05\,\mathrm{m}\) (\(y\in[-H,H]\)) |
| Thickness | \(t\) | \(0.01\,\mathrm{m}\) (\(z\in[0,t]\)) |
| Hole radius | \(a\) | \(0.01\,\mathrm{m}\) |
| Young's modulus | \(E\) | \(2.1 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Remote / applied tension | \(\sigma_\infty\) | \(1.0 \times 10^{6}\,\mathrm{Pa}\) |
| Fixed face | \(x \approx -W\) | all three DOFs |
| Loaded face | \(x \approx +W\) | traction \((\sigma_\infty, 0, 0)\) |

### Infinite-plate Kirsch solution

Kirsch (1898); Timoshenko & Goodier, *Theory of Elasticity*, Art. 35.
For an infinite plate with a circular hole of radius \(a\) under remote
uniaxial tension \(\sigma_\infty\) along \(x\), the polar stress field is

\begin{align}
\sigma_r
  &= \frac{\sigma_\infty}{2}\Bigl(1-\frac{a^2}{r^2}\Bigr)
   + \frac{\sigma_\infty}{2}\Bigl(1-\frac{4a^2}{r^2}+\frac{3a^4}{r^4}\Bigr)\cos 2\theta, \\
\sigma_\theta
  &= \frac{\sigma_\infty}{2}\Bigl(1+\frac{a^2}{r^2}\Bigr)
   - \frac{\sigma_\infty}{2}\Bigl(1+\frac{3a^4}{r^4}\Bigr)\cos 2\theta, \\
\tau_{r\theta}
  &= -\frac{\sigma_\infty}{2}\Bigl(1+\frac{2a^2}{r^2}-\frac{3a^4}{r^4}\Bigr)\sin 2\theta.
\end{align}

On the hole boundary \(r=a\), \(\sigma_r = \tau_{r\theta} = 0\) and

\[
\sigma_\theta(a,\theta) = \sigma_\infty\bigl(1 - 2\cos 2\theta\bigr).
\]

The hoop stress is maximised at \(\theta = \pm\pi/2\) (transverse poles):

\[
\sigma_\theta^{\max} = 3\,\sigma_\infty.
\]

Hence the **stress concentration factor** (SCF), independent of \(E\), \(\nu\),
and \(a\):

\[
\mathrm{SCF}
  \equiv \frac{\sigma_\theta^{\max}}{\sigma_\infty}
  = 3.0.
\]

For the probe used by the harness (`max_vm_over_nominal` with
`nominal = σ_∞`), note that at the hole poles the stress state is uniaxial
hoop tension, so \(\sigma_{\mathrm{vm}}^{\max} = \sigma_\theta^{\max}\) and

\[
\frac{\sigma_{\mathrm{vm}}^{\max}}{\sigma_\infty} = 3.0.
\]

**Metric `scf`:** value \(3.0\), relative tolerance \(5\,\%\).

### Finite-domain note

The fixture is a finite plate (\(a/W = 0.1\), \(a/H = 0.2\)). The infinite-
plate SCF is the reference; Howland-type finite-width corrections raise the
true continuum SCF by a few percent at this aspect ratio. The \(5\,\%\)
tolerance covers that domain-truncation bias plus discretization error. A
tighter check with exact Kirsch traction on an annular sector lives in the
Tier-1 Catch2 suite (`tests/test_kirsch_plate.cpp`, reference
`bench/reference/kirsch-plate.json`) and is independent of this part.

---

## cantilever

**Part:** `cantilever` · **geometry:** prismatic beam along \(+x\)

| Quantity | Symbol | Value |
|---|---|---|
| Length | \(L\) | \(1.0\,\mathrm{m}\) |
| Cross-section | \(b \times h\) | \(0.1 \times 0.1\,\mathrm{m}\) |
| Area | \(A = bh\) | \(1.0 \times 10^{-2}\,\mathrm{m}^2\) |
| Second moment | \(I = bh^3/12\) | \(8.\overline{3}\times 10^{-6}\,\mathrm{m}^4\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Shear modulus | \(G = E/(2(1+\nu))\) | \(7.\overline{692}\times 10^{10}\,\mathrm{Pa}\) |
| Tip-face traction | \(\mathbf{t}\) | \((0,\,0,\,-10^{5})\,\mathrm{Pa}\) |
| Resultant tip load | \(P = \|t_z\|\,A\) | \(1.0 \times 10^{3}\,\mathrm{N}\) (down, \(-z\)) |
| Fixed face | \(x \approx 0\) | all three DOFs |

### Timoshenko tip deflection under end load

For a cantilever fixed at \(x=0\) with transverse end load \(P\) at \(x=L\),
the tip deflection is the sum of Euler–Bernoulli bending and shear:

\[
\delta
  = \underbrace{\frac{P L^3}{3 E I}}_{\text{bending}}
  + \underbrace{\frac{P L}{\kappa G A}}_{\text{shear}},
\]

where the shear correction for a rectangular cross-section is
(Cowper, 1966)

\[
\kappa = \frac{10(1+\nu)}{12 + 11\nu}
       = \frac{10\cdot 1.3}{12 + 3.3}
       = \frac{13}{15.3}
       \approx 0.8496732026.
\]

Substituting the table values:

\begin{align}
\delta_{\mathrm{bend}}
  &= \frac{1000 \cdot 1^3}{3 \cdot 2\times 10^{11} \cdot (0.1)^4/12}
   = \frac{1000}{5\times 10^{6}}
   = 2.0 \times 10^{-4}\,\mathrm{m}, \\
\delta_{\mathrm{shear}}
  &= \frac{1000 \cdot 1}{\kappa\, G\, A}
   \approx 1.53 \times 10^{-6}\,\mathrm{m}, \\
\delta
  &= \delta_{\mathrm{bend}} + \delta_{\mathrm{shear}}
   = 2.0153 \times 10^{-4}\,\mathrm{m}.
\end{align}

(Sign: load is \(-z\), so the tip \(u_z\) is negative; the metric records the
signed mean tip \(u_z\), and the harness compares with absolute/relative
error against the signed truth \(-2.0153\times 10^{-4}\). Implementations that
score on magnitude should use \(|u_z|\). The reference value below is the
**magnitude** of the tip deflection, matching the probe name
`tip_deflection`.)

**Metric `tip_deflection`:** value \(2.0153\times 10^{-4}\,\mathrm{m}\),
relative tolerance \(5\,\%\). Probe = mean \(|u_z|\) on the \(x=L\) face
nodes (or signed \(u_z\) with matching sign convention in the harness).

Shear is only \(\sim 0.76\,\%\) of bending here (\(L/h = 10\)); an
Euler-only truth of \(2.0\times 10^{-4}\) would still pass the \(5\,\%\)
band, but the reference keeps the Timoshenko term so a locking-free
solution is not systematically biased low.

---

## How to add a part

1. Author geometry under `tests/fixtures/parts/<name>.stl` (extend
   `scripts/gen_part_library.py` or drop a CAD export).
2. Write `tests/fixtures/parts/<name>.case.json` binding material, BC/load
   box selectors (with h-independent slack — `interfaces.md` §4), and the
   reference path.
3. Derive the closed-form answer in a new section of **this** file.
4. Commit `bench/reference/<name>.json` with metrics, tolerances, probe
   descriptors, and a `derivation` link to the new section.
5. Never put the truth numbers in `src/` or `apps/`.
