# Hand-calculated reference truths

Every metric in `bench/reference/<part>.json` cites a section here. Units are
SI throughout (m, Pa, N). These derivations are the **only** source of
numerical truth for the test-lab harness — nothing under `src/` or `apps/`
may embed them (anti-cheat rule #1; see `CONTRIBUTING.md` §0 and
`docs/benchmarks.md`).

Fixture geometry: `tests/fixtures/parts/`. Case bindings (loads/BCs/material):
`tests/fixtures/parts/<part>.case.json` (schema: `docs/dag/interfaces.md` §4).
Reference JSON: `bench/reference/<part>.json` (schema: §5).

Product STEP regeneration (geometry only — does **not** write truths; ADR-0020):

```bash
python3 scripts/gen_cad_parts.py
```

Legacy STL fixtures for older campaigns:

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
(`tests/fixtures/parts/plate_hole.step` from `scripts/gen_cad_parts.py`;
plate \(0.2\times 0.1\times 0.01\) with hole \(r=0.01\))

| Quantity | Symbol | Value |
|---|---|---|
| Half-width (\(x\)) | \(W\) | \(0.1\,\mathrm{m}\) (\(x\in[-W,W]\)) |
| Half-height (\(y\)) | \(H\) | \(0.05\,\mathrm{m}\) (\(y\in[-H,H]\)) |
| Thickness | \(t\) | \(0.01\,\mathrm{m}\) (\(z\in[0,t]\)) |
| Hole radius | \(a\) | \(0.01\,\mathrm{m}\) |
| Young's modulus | \(E\) | \(2.1 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Remote / applied tension | \(\sigma_\infty\) | \(1.0 \times 10^{6}\,\mathrm{Pa}\) |
| Fixed face | \(x \approx -W\) | all three DOFs (case box \(x\le -0.099\)) |
| Loaded face | \(x \approx +W\) | traction \((\sigma_\infty, 0, 0)\) (case box \(x\ge 0.099\)) |

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

Campaign probe is **face-mean** von Mises on the hole-neighborhood patch
(`mean_vm_over_nominal` with `nominal = σ_∞`), evaluated from
**element-centroid** stresses of quality-passing elements — never raw nodal
max (ADR-0023). At the hole poles the continuum stress is uniaxial hoop
tension, so \(\sigma_{\mathrm{vm}} = \sigma_\theta\) and the face-mean ratio
still targets

\[
\frac{\langle\sigma_{\mathrm{vm}}\rangle_{\mathrm{hole}}}{\sigma_\infty} = 3.0
\]

on a well-resolved rim (face-mean is slightly softer than continuum peak;
tolerance widened to \(10\,\%\)).

**Metric `scf`:** value \(3.0\), relative tolerance \(10\,\%\).
Diagnostic dashboard also records quality-filtered p99 VM and nodal max.

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

## cylinder

**Part:** `cylinder` · **geometry:** solid right circular cylinder
(`tests/fixtures/parts/cylinder.step`)

| Quantity | Symbol | Value |
|---|---|---|
| Radius | \(R\) | \(0.05\,\mathrm{m}\) |
| Height | \(H\) | \(0.2\,\mathrm{m}\) (\(z\in[0,H]\), axis \(+z\), base at origin) |
| Cross-section area | \(A=\pi R^2\) | \(\pi\times 2.5\times 10^{-3}\approx 7.854\times 10^{-3}\,\mathrm{m}^2\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Top-face traction | \(\mathbf{t}\) | \((0,\,0,\,10^{6})\,\mathrm{Pa}\) (uniaxial tension) |
| Fixed face | \(z \approx 0\) | all three DOFs (case box \(z\le 10^{-3}\)) |
| Loaded face | \(z \approx H\) | case box \(z\ge 0.199\) |

### Stress (uniaxial bar)

Flat ends make this the same continuum problem as the rectangular smoke-bar:
uniform end traction \(t_z\) on a prismatic (here circular) solid is equilibrated
by a uniform axial stress field (Saint-Venant; exact for pure end traction on
a prism):

\[
\sigma_{zz} = t_z = 1.0 \times 10^{6}\,\mathrm{Pa},
\quad
\sigma_{xx} = \sigma_{yy} = \tau_{ij} = 0.
\]

Von Mises for uniaxial tension:

\[
\sigma_{\mathrm{vm}} = |\sigma_{zz}| = 1.0 \times 10^{6}\,\mathrm{Pa}.
\]

Raw nodal \(\sigma_{\mathrm{vm}}^{\max}\) is **not** a campaign score metric
(sliver/extrapolation spikes; ADR-0023). It remains a diagnostic only.

### Strain energy (primary campaign score)

Uniform uniaxial field \(\sigma_{zz}=t_z\), \(\varepsilon_{zz}=\sigma_{zz}/E\):

\[
U = \tfrac12 \int_\Omega \boldsymbol{\sigma}:\boldsymbol{\varepsilon}\,dV
  = \tfrac12 \frac{\sigma_{zz}^2}{E}\,V,
\quad
V = \pi R^2 H = \pi\cdot 2.5\times 10^{-3}\cdot 0.2
  \approx 1.570796\times 10^{-3}\,\mathrm{m}^3.
\]

\[
U
  = \tfrac12 \frac{(10^6)^2}{2\times 10^{11}} \cdot \pi R^2 H
  = 3.926990816987241\times 10^{-3}\,\mathrm{J}.
\]

**Metric `strain_energy`:** value \(3.926990816987241\times 10^{-3}\,\mathrm{J}\),
relative tolerance \(15\,\%\). Probe = `strain_energy` (\(\tfrac12\mathbf{u}^T K\mathbf{u}\)).
Has gradient w.r.t. mesh quality (unlike tip of a uniaxial bar).

### Tip axial displacement (secondary / health)

\[
u_z(H)
  = \frac{\sigma_{zz}\,H}{E}
  = \frac{10^{6}\cdot 0.2}{2.0\times 10^{11}}
  = 1.0 \times 10^{-6}\,\mathrm{m}.
\]

(Poisson contraction of the radius does not affect \(u_z\).)

**Metric `tip_deflection`:** value \(1.0\times 10^{-6}\,\mathrm{m}\), relative
tolerance \(15\,\%\). Probe = face-mean \(|\mathbf{u}|\) on the loaded end face
(catches BC/RBM regressions; nearly mesh-insensitive for packing loops).

---

## sphere

**Part:** `sphere` · **geometry:** solid sphere centred at the origin
(`tests/fixtures/parts/sphere.step`)

| Quantity | Symbol | Value |
|---|---|---|
| Radius | \(R\) | \(0.05\,\mathrm{m}\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Polar plane (fix / load) | \(z_p\) | \(0.04\,\mathrm{m}\) |
| Cap height | \(h = R - z_p\) | \(0.01\,\mathrm{m}\) |
| Cap surface area | \(A_{\mathrm{cap}} = 2\pi R h\) | \(2\pi R(R-z_p) = \pi\times 10^{-3}\,\mathrm{m}^2\) |
| Applied traction | \(\mathbf{t}\) | \((0,\,0,\,-10^{6})\,\mathrm{Pa}\) on north polar faces |
| Fixed region | \(z \le -z_p\) | all three DOFs (south polar cap of nodes) |

### Why not a classical closed form

A solid sphere under **uniform normal pressure** has the exact hydrostatic
field \(\sigma_{ij}=-p\,\delta_{ij}\), \(\sigma_{\mathrm{vm}}=0\), and
\(u_r=-p(1-2\nu)r/E\). The test-lab case schema only supports **fixed-direction
surface traction** (not \(\mathbf{t}=-p\mathbf{n}\)), so that Lamé-style
pressure problem is not representable here. Uniaxial far-field tension on a
free sphere is not an elementary closed form either (no flat ends for clean
Saint-Venant patches).

### Engineering estimate: polar compression as a short column

Case binding (mesh-independent selectors in the continuum limit):

- Dirichlet on nodes with \(z\le -z_p\) (south polar cap);
- traction \(\mathbf{t}=(0,0,-t)\) with \(t=10^{6}\,\mathrm{Pa}\) on boundary
  faces whose centroids satisfy \(z\ge z_p\) (north polar cap).

Spherical-cap surface area is geometric (independent of \(E,\nu\)):

\[
A_{\mathrm{cap}} = 2\pi R h = 2\pi R(R-z_p)
  = 2\pi\cdot 0.05\cdot 0.01
  = \pi\times 10^{-3}
  \approx 3.1416\times 10^{-3}\,\mathrm{m}^2.
\]

Resultant polar force (continuum limit of the face traction integral):

\[
F = t\,A_{\mathrm{cap}}
  = 10^{6}\cdot \pi\times 10^{-3}
  = \pi\times 10^{3}
  \approx 3.1416\times 10^{3}\,\mathrm{N}.
\]

Approximate the body as a short column of length equal to the distance
between the polar planes, \(L_{\mathrm{eff}}=2z_p=0.08\,\mathrm{m}\), and
midspan cross-section \(A=\pi R^2\):

\[
\delta_{\mathrm{eng}}
  = \frac{F\,L_{\mathrm{eff}}}{E\,A}
  = \frac{F\cdot(2z_p)}{E\,\pi R^2}
  = \frac{4\,t\,(R-z_p)\,z_p}{E\,R}.
\]

Numerically:

\[
\delta_{\mathrm{eng}}
  = \frac{4\cdot 10^{6}\cdot 0.01\cdot 0.04}{2.0\times 10^{11}\cdot 0.05}
  = \frac{1.6\times 10^{3}}{1.0\times 10^{10}}
  = 1.6\times 10^{-7}\,\mathrm{m}.
\]

This omits local Hertz-like compliance under the caps, Poisson effects, and
the true 3-D stress paths around the free surface. It is an **order-correct
engineering estimate**, not an exact continuum solution — hence a wide
tolerance. Max von Mises is **not** used as a campaign truth: the edge of the
fixed polar patch is a Dirichlet–Neumann transition that can produce mesh-
dependent peak stresses under refinement.

**Metric `tip_deflection`:** value \(1.6\times 10^{-7}\,\mathrm{m}\), relative
tolerance \(50\,\%\). Probe = `max_displacement`.

---

## icecream-cone

**Part:** `icecream_cone` · **geometry:** regular triangular pyramid fused with
an intersecting spherical scoop (`tests/fixtures/parts/icecream_cone.step`
from `scripts/gen_cad_parts.py`)

| Quantity | Symbol | Value |
|---|---|---|
| Pyramid base (equilateral) | \(s\) | \(0.12\,\mathrm{m}\) in plane \(z=0\), centroid at origin |
| Pyramid height | \(H_p\) | \(0.15\,\mathrm{m}\) (apex at \(z=H_p\)) |
| Scoop radius | \(R_s\) | \(0.04\,\mathrm{m}\) |
| Base area | \(A_b=\sqrt{3}\,s^2/4\) | \(\approx 6.235\times 10^{-3}\,\mathrm{m}^2\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Fixed region | \(z \approx 0\) | all three DOFs on base nodes (case box \(z\le 10^{-3}\)) |
| Loaded region | \(z \ge 0.12\) | traction \((0,\,0,\,-10^{6})\,\mathrm{Pa}\) (apex + upper scoop) |

### No closed-form continuum solution

The fused pyramid∪ball has no elementary elasticity solution under this
partial-surface traction. The case exists so the campaign can exercise
**non-CAD-primitive topology** (boolean solid, sharp dihedral edges, curved
scoop) for mesh quality, mesher ranking, and relative convergence across
tiers — not as a Tier-1 analytic gate.

### Order-of-magnitude stress sanity

Upper-surface traction magnitude \(t=10^{6}\,\mathrm{Pa}\). In the continuum
limit the selected face set (\(z\ge 0.12\)) carries a resultant force
\(F=t\,A_{\mathrm{load}}\) with \(A_{\mathrm{load}}\) of the same order as a
few \(\times 10^{-3}\,\mathrm{m}^2\) (apex facets + upper scoop patch). Base
area \(A_b\sim 6\times 10^{-3}\,\mathrm{m}^2\) then implies a characteristic
compressive stress

\[
\sigma^\ast \sim \frac{F}{A_b} = O(10^{5}\text{–}10^{6})\,\mathrm{Pa}.
\]

Peak von Mises near re-entrant features (pyramid edges, fusion neck) can
exceed \(\sigma^\ast\), while a coarse mesh may under-resolve peaks. The
campaign metric is therefore an **order-of-magnitude sanity band**, not a
tight SCF-style check:

**Metric `sigma_max`:** value \(1.0\times 10^{6}\,\mathrm{Pa}\), relative
tolerance \(100\,\%\) (\(1.0\)). Probe = `max_von_mises`.

Interpretation: measured \(\sigma_{\mathrm{vm}}^{\max}\) within roughly
\([0,\,2\times 10^{6}]\) still scores nontrivially under the harness map
\(1/(1+|\mathrm{rel}|/\mathrm{tol})\); gross failures (wrong BC box, zero load,
unit mistakes) fall outside and score near zero. Tighter truths require a
separate manufactured solution or a peer-code baseline — out of scope for V2c.

---

## How to add a part

1. Author **product** geometry under `tests/fixtures/parts/<name>.step`
   (extend `scripts/gen_cad_parts.py` or drop a CAD STEP export). Legacy STL
   fixtures remain only for older campaigns (`scripts/gen_part_library.py`).
2. Write `tests/fixtures/parts/<name>.case.json` binding material, BC/load
   box selectors (with h-independent slack — `interfaces.md` §4), and the
   reference path. Geometry path must be `.step` for product campaigns.
3. Derive the closed-form (or documented engineering) answer in a new section
   of **this** file.
4. Commit `bench/reference/<name>.json` with metrics, tolerances, probe
   descriptors, and a `derivation` link to the new section.
5. Never put the truth numbers in `src/` or `apps/`.
