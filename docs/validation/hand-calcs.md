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
engineering estimate** only (column model under-predicts by ~1.8× vs fine FE).
Max von Mises is **not** used as a campaign truth: the edge of the fixed polar
patch is a Dirichlet–Neumann transition that can produce mesh-dependent peak
stresses under refinement.

### Load-area guard (M12)

Case select uses polar plane \(z_p=0.04\,\mathrm{m}\) and
`normal_min_dot: 0.7` (same traction-alignment filter as cylinder). Continuum
cap area:

\[
A_{\mathrm{cap}} = 2\pi R(R-z_p) = \pi\times 10^{-3}
  \approx 3.141592653589793\times 10^{-3}\,\mathrm{m}^2.
\]

Written as `loads[].select.expected_area`. Hybrid meshes at campaign tiers
match within ±5%. Coarse varyhedron can under-select the cap (chordal rim) and
honestly trip `load_area_ok=false` — that is a mesh signal, not a truth bug.

### M13 frozen numerical reference (no closed-form series this pass)

Full **Hiramatsu–Oka / Legendre polar-cap series** was timeboxed per ADR-0024 Q5
and **cut** this session — not shipped as an analytic formula. Instead, freeze a
**versioned dual-mesher Richardson-class** reference from the aligned polar
cap BC (`sphere.case.json` above):

| Source | \(h_{\mathrm{scale}}\) | DOF (approx) | tip face-mean | strain energy |
|--------|------------------------|--------------|---------------|---------------|
| hybrid_zoo | 2 | ~27k | \(2.90\times 10^{-7}\,\mathrm{m}\) | \(4.59\times 10^{-4}\,\mathrm{J}\) |
| varyhedron | 2 | ~12k | \(2.89\times 10^{-7}\,\mathrm{m}\) | \(4.60\times 10^{-4}\,\mathrm{J}\) |

Agreement at the fine end: tip within ~0.1%, energy within ~0.2%. Column
estimate \(1.6\times 10^{-7}\) is retained only as a lower-bound sanity check.

**Metric `strain_energy` (primary):** value \(4.60\times 10^{-4}\,\mathrm{J}\),
relative tolerance \(20\,\%\). Probe = `strain_energy`. Best packing
discriminator among current sphere scalars (surface-resolution sensitive).

**Metric `tip_deflection` (secondary health):** value \(2.90\times 10^{-7}\,\mathrm{m}\),
relative tolerance \(25\,\%\). Probe = `tip_deflection` (face-mean |u| on
load faces — not global nodal max).

If a later session lands a true Legendre series, replace these frozen numbers
and re-freeze the M9-style baseline; do not regenerate ad hoc each campaign.

---

## icecream-cone

**Part:** `icecream_cone` · **geometry:** one watertight OpenCASCADE Boolean
solid: an upright round truncated cone fused with a deliberately overlapping
spherical scoop (`tests/fixtures/parts/icecream_cone.step`, generated by
`python3 scripts/gen_cad_parts.py --part icecream_cone`).

| Quantity | Symbol | Value |
|---|---|---|
| Lower cone radius | \(R_0\) | \(0.006\,\mathrm{m}\) at \(z=0\) |
| Upper cone radius | \(R_1\) | \(0.032\,\mathrm{m}\) at \(z=0.100\,\mathrm{m}\) |
| Cone height | \(H_c\) | \(0.100\,\mathrm{m}\) |
| Scoop radius | \(R_s\) | \(0.035\,\mathrm{m}\) |
| Scoop centre | \(z_s\) | \(0.112\,\mathrm{m}\); the sphere overlaps and encloses the cone rim |
| Fused BRep volume | \(V\) | \(2.65748\times10^{-4}\,\mathrm{m}^3\) (OpenCASCADE mass properties) |
| Envelope |  | \(x,y\in[-0.035,0.035]\,\mathrm{m}\), \(z\in[0,0.147]\,\mathrm{m}\) |
| Young's modulus | \(E\) | \(2.0 \times 10^{11}\,\mathrm{Pa}\) |
| Poisson's ratio | \(\nu\) | \(0.3\) |
| Fixed region | \(z \le 0.012\,\mathrm{m}\) | all three DOFs on the lower foot/lower-wall patch |
| Loaded region | \(z \ge 0.120\,\mathrm{m}\) | traction \((0,\,0,\,-10^{6})\,\mathrm{Pa}\) on the scoop cap |

The generator requires a successful Boolean fuse, `BRepCheck_Analyzer`
validity, one connected solid, and positive volume. The C++ geometry-fidelity
regression independently reloads the STEP and requires one solid, one closed
shell, the envelope above, and the measured BRep volume.

### No closed-form continuum solution

The fused frustum/sphere solid has no elementary elasticity solution under this
partial-surface traction. It exists to exercise a real Boolean BRep containing
plane, conical, and spherical support surfaces, then to compare mesher geometry
and relative convergence. It is not a Tier-1 analytic solver gate.

### Solver reference remains research-gated

The historical `bench/reference/icecream_cone.json` raw nodal `sigma_max`
entry was calibrated to the superseded pyramid/sphere fixture. It is retained
temporarily for warehouse schema compatibility but is **not a validated truth
for this geometry and must not support a solver-accuracy claim**. Raw nodal
maximum stress is also singularity-sensitive and is prohibited as a primary
score by the active measurement plan.

After CAD-fidelity gates pass, replace that legacy entry with a frozen
Richardson family for strain energy and face-mean displacement/stress under the
new fixture. Until then, use this part only for BRep validity, mesh fidelity,
load-resultant conservation, mesh quality, and qualitative solved renders.

---

## corpus-primitives

**Parts:** `bench/geometries/corpus/primitives/<family>_s<k>.step`, generated by
`scripts/gen_primitive_corpus.py` (six families x four size regimes = 24 solids,
three load cases each = 72 cases named `<family>_s<k>_c<j>`). Dimensions come from a
committed per-part seed table, so every number below is reproducible from the script.

Material for the whole corpus: \(E = 2.1\times10^{11}\,\mathrm{Pa}\), \(\nu = 0.3\),
\(\rho = 7850\,\mathrm{kg/m^3}\). Case archetypes per part:

| Case | Archetype | Traction | Selection filter |
|---|---|---|---|
| `c0` | axial tension along the beam axis | \(10^{6}\,\mathrm{Pa}\) | `normal_min_dot = 0.7` (traction is parallel to the end-face normal) |
| `c1` | transverse end load | \(10^{5}\,\mathrm{Pa}\) | `normal_min_dot = -1.0` (traction is perpendicular to the end-face normal, so the alignment filter would keep only side-wall slivers) |
| `c2` | oblique end load, every component \(\ge 0.26\) of the unit vector | \(2\times10^{5}\,\mathrm{Pa}\) | `normal_min_dot = -1.0` |

Fix/load select boxes are computed from each part's true bounding box: the load slab is
1 % and the fix slab 2 % of the targeted cross-section's smallest extent (the 1 % figure
reproduces the working `cantilever.case.json` slab, which selects the CAD end face and no
wall face). `select.expected_area` carries the exact CAD end-face area for the four
families whose loaded face is a planar polygon (`box_hole`, `l_bracket`, `plate_notch`,
`channel`) and is omitted where the loaded face is circular or spherical
(`stepped_shaft`, `sphere_box`) so chordal deficit cannot trip the 5 % load-area health
gate.

### corpus-primitives-kirsch

**Analytic family:** `box_hole_s<k>_c0` (uniaxial tension). Same theory as
[kirsch-plate](#kirsch-plate): an infinite plate with a circular hole under remote
uniaxial tension \(\sigma_\infty\) has hoop stress
\(\sigma_\theta(a,\theta) = \sigma_\infty(1 - 2\cos 2\theta)\) on the hole boundary,
maximal at the transverse poles, so

\[
\mathrm{SCF} = \frac{\sigma_\theta^{\max}}{\sigma_\infty} = 3.0 ,
\]

independent of \(E\), \(\nu\) and \(a\). The generator holds the corpus inside the
fixture's validated aspect band, \(a/W \in [0.088, 0.107]\) and
\(a/H \in [0.17, 0.20]\) (fixture: \(0.10\) / \(0.20\)), and keeps the plate thin,
\(t/H \in [0.16, 0.24]\), so plane stress applies.

**Metric `scf`:** value \(3.0\), tolerance \(10\,\%\), probe
`mean_vm_over_nominal` with `nominal` \(= \sigma_\infty = 10^{6}\,\mathrm{Pa}\) on the
hole patch \(|x|, |y| \le 1.5a\) through the full thickness — element-centroid
face-mean von Mises (ADR-0023), never nodal max. The \(10\,\%\) band covers the
Howland finite-width correction plus the face-mean softening, exactly as for
`plate_hole`.

### corpus-primitives-cantilever

**Analytic family:** `stepped_shaft_s<k>_c1` (transverse end load). A stepped circular
cantilever fixed at \(x=0\), of length \(L\), with radius \(R_1\) on \(x\in[0,a]\) and
\(R_2\) on \(x\in[a,L]\), carries an end traction \(t\) on the tip disk, so the
resultant transverse load is \(P = |t|\,\pi R_2^2\) acting at \(x = L\).

With \(M(x) = P(L-x)\), the unit-load method gives the exact Euler-Bernoulli term

\[
\delta_{\mathrm{bend}}
  = \frac{P}{E}\int_0^L \frac{(L-x)^2}{I(x)}\,\mathrm{d}x
  = \frac{P}{3E}\left[\frac{L^3 - (L-a)^3}{I_1} + \frac{(L-a)^3}{I_2}\right],
  \qquad I_i = \frac{\pi R_i^4}{4},
\]

which collapses to the familiar \(PL^3/(3EI)\) when \(I_1 = I_2\). The Timoshenko shear
term uses the Cowper (1966) factor for a solid circular section,
\(\kappa = 6(1+\nu)/(7+6\nu)\), with \(G = E/(2(1+\nu))\) and \(A_i = \pi R_i^2\):

\[
\delta_{\mathrm{shear}}
  = \frac{P}{\kappa G}\left[\frac{a}{A_1} + \frac{L-a}{A_2}\right],
  \qquad
\delta = \delta_{\mathrm{bend}} + \delta_{\mathrm{shear}} .
\]

Because \(P\) is the only load and \(\delta\) is its work-conjugate deflection, the
strain energy follows analytically as \(U = \tfrac{1}{2} P \delta\).

**Metrics:** `tip_deflection` (probe `tip_deflection`, the face-mean displacement
magnitude on the loaded disk) and `strain_energy` (probe `strain_energy`), both at
\(15\,\%\) tolerance. Each reference file records \(L\), \(a\), \(R_1\), \(R_2\),
\(A_i\), \(I_i\), \(\kappa\), \(G\), \(P\) and the two deflection terms, so the number
is auditable per part. The band covers: the sharp shoulder's local stiffening (outside
beam theory), the \(1\)–\(2\,\%\) chordal deficit of the meshed tip disk against
\(\pi R_2^2\) (measured on the `cylinder` fixture), the \(2\,\%\) fix slab shortening
the effective span, and discretization error. The generator holds
\(L/2R_1 \in [8.5, 9.2]\) so the shear term stays a small correction
(\(\lesssim 3\,\%\)) rather than a competing effect.

### corpus-primitives-provisional

`l_bracket`, `plate_notch`, `channel`, `sphere_box`, and the non-analytic cases of the
two families above have no closed form worth trusting (re-entrant corner, notch root,
open thin-walled section with warping, fused spherical boss). Their truth comes from an
**overkill reference solve**, not a hand calc: `bench/campaigns/advisor-truth-0`
(`order = 2`, `adapt_passes = 3`, `graded_tet`, warehoused, over an
`h_rel` ladder of \(0.060 / 0.038 / 0.024\) — a single very fine run is impossible
because testlab refuses any run predicting more than 120 000 elements, and the compiled
order-2 DOF cap puts the ceiling near 19 000 tetrahedra), promoted
into `bench/reference/corpus/*.json` exactly once by
`scripts/advisor/promote_truth.py`. Promotion picks the health-ok row with the largest
`n_dof`, rewrites `value`, sets `tol` to \(15\,\%\) (the band the analytic cantilever
references already use — the reference is itself a discrete solve, so nothing tighter is
defensible), stamps
`source = "overkill-reference"` with the campaign/cfg provenance, and never touches a
metric whose `source` is `"analytic"`.

**Read these as what they are.** A promoted value is *one solve at the finest resolution
that part could afford under the compiled campaign budget* — not a converged limit, not
a Richardson extrapolation, and not a closed form. Its own discretization error is
unquantified: the ladder tops out near 19 000 order-2 tetrahedra, which is 2.5-8x the
training grid but nowhere near mesh independence. The \(15\,\%\) tolerance stamped at
promotion is the band a *scored* coarse run is expected to hit against this reference,
not an accuracy claim about the reference itself. Use them to rank mesh actions against
each other, which is exactly what the advisor needs; do not quote them as validated
answers, do not compare them across parts, and do not treat a metric with
`source = "overkill-reference"` as interchangeable with one marked `"analytic"`. If a
part ever acquires a closed form, add it here and let `promote_truth.py` skip that metric
forever.

Until that campaign has run, the generator seeds each such metric with a **first-order
beam surrogate** marked `"source": "provisional"` at \(100\,\%\) tolerance — a value has
to exist because `load_metrics` requires a non-empty `metrics[]` before the truth
campaign can execute at all. The surrogate is

\[
\delta_{\mathrm{axial}} = \frac{t_{\parallel} L}{E},
\qquad
\delta_{\perp} = \frac{t_{\perp} A L^3}{3 E I_{\mathrm{eq}}},
\qquad
I_{\mathrm{eq}} = \frac{A\,d^2}{12},
\]

with \(A\) the loaded section area, \(L\) the fixed-to-loaded-face span, and \(d\) the
bounding-box extent along the transverse axis; the reported deflection is
\(\sqrt{\delta_{\mathrm{axial}}^2 + \delta_{\perp}^2}\) and the reported energy is
\(\tfrac12\sum_i P_i \delta_i\). It is an order-of-magnitude estimate and is labelled as
one in every file it appears in — never cite it as a validated truth.

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
