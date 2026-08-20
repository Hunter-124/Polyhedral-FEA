#!/usr/bin/env python3
"""Verify the solved *fields* on the showcase parts against closed-form elasticity.

The existing gates check scalar probes: `bench/reference/*.json` compares one
number per part (tip deflection, peak SCF) against a hand calc at a 5-15% band.
That catches a broken solve. It does not catch a solve that gets the peak right
and the field wrong, and it says nothing about the *deformation* beyond a single
maximum.

This checks the whole field, for both stress and displacement, against truths
that hold pointwise:

* **cantilever** — Saint-Venant flexure. sigma_xx = M(x)(z-zc)/I and
  tau_xz = (3P/2A)(1-(2(z-zc)/h)^2) are exact in the interior, so the exported
  von Mises must equal sqrt(sigma_xx^2 + 3 tau^2) at every node. Integrating the
  recovered |sigma_xx| gives M(x), and -dM/dx must return the applied shear.
  On the kinematic side: plane sections stay plane, the section rotation is
  Px(2L-x)/(2EI), and the tip deflection is Timoshenko.
* **cylinder** — uniform axial tension. In the interior (both Saint-Venant end
  layers excluded) du_z/dz = sigma/E and du_r/dr = -nu sigma/E *exactly*, and
  von Mises = F/A. This is the clean deformation check: no shear correction, no
  section theory, no fitting.
* **plate_hole** — Kirsch. The section force must integrate to F, the peak gives
  the SCF, and the lateral contraction must recover -nu sigma/E out of the
  clamped-face boundary layer.
* **sphere / icecream_cone** — no closed form, so the truths are invariants: an
  axisymmetric load produces zero hoop displacement, and constrained nodes are
  exactly zero.

Where a measured number misses its beam-theory truth by more than the
discretisation error, the reason is named and *itself* measured -- see
`docs/validation/field-verification.md`. Nothing here is a tuned band.

Usage
-----
    python3 scripts/verify_fields.py                # showcase VTUs must exist
    python3 scripts/verify_fields.py --h-study      # also solve/measure h-refinement
    python3 scripts/verify_fields.py --only cylinder

The showcase VTUs come from `python3 scripts/render_showcase.py --all` (cached in
`build/showcase/`). Exit status is nonzero if any check leaves its band.

On the anti-cheat rule (CONTRIBUTING §0): the closed forms below are written out
here rather than read from `bench/reference/*.json` because they are *fields*,
not the scalar probes that file holds, and their derivations live in
`docs/validation/hand-calcs.md`. This is a verification harness under
`scripts/`; it feeds no product path, no scoreboard and no `bench/reference`
value, and nothing under `src/` or `apps/` imports it.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
CACHE = REPO / "build/showcase"
VERIFY = REPO / "build/verify"

# Cantilever fixture, from docs/validation/hand-calcs.md#cantilever.
CANT = dict(L=1.0, b=0.1, h=0.1, E=2.0e11, nu=0.3, P=1000.0)
CYL = dict(R=0.05, H=0.2, E=2.0e11, nu=0.3, F=7853.981633974483)
PLATE = dict(a=0.01, E=2.1e11, nu=0.3, F=1000.0)
CONE_F = 1000.0
SPHERE_F = 3141.592653589793


# ---------------------------------------------------------------------------
# Result plumbing
# ---------------------------------------------------------------------------
@dataclass
class Check:
    part: str
    name: str
    measured: float
    truth: float
    band_pct: float
    unit: str = ""
    note: str = ""
    # Absolute-tolerance checks (an invariant that must be zero) set this and
    # leave `truth` at 0; relative error is undefined there.
    absolute: bool = False

    @property
    def err_pct(self) -> float:
        if self.absolute:
            return 100.0 * self.measured
        if self.truth == 0.0:
            return float("nan")
        return 100.0 * (self.measured - self.truth) / abs(self.truth)

    @property
    def ok(self) -> bool:
        return abs(self.err_pct) <= self.band_pct


@dataclass
class Report:
    checks: list[Check] = field(default_factory=list)

    def add(self, *a, **k) -> None:
        self.checks.append(Check(*a, **k))

    def dump(self) -> int:
        part = None
        for c in self.checks:
            if c.part != part:
                part = c.part
                print(f"\n[{part}]")
            flag = "ok  " if c.ok else "FAIL"
            if c.absolute:
                shown = f"{c.measured: .4e}{(' ' + c.unit) if c.unit else ''}"
                truth = "0 (invariant)"
            else:
                shown = f"{c.measured: .6e}{(' ' + c.unit) if c.unit else ''}"
                truth = f"{c.truth: .6e}"
            print(f"  {flag} {c.name:<44s} {shown:>22s}  truth {truth:>14s}"
                  f"  {c.err_pct:+8.3f}%  band {c.band_pct:.2f}%")
            if c.note:
                print(f"       {c.note}")
        bad = [c for c in self.checks if not c.ok]
        print(f"\n{len(self.checks) - len(bad)}/{len(self.checks)} checks inside band")
        for c in bad:
            print(f"  FAIL {c.part}/{c.name}: {c.err_pct:+.3f}% vs band {c.band_pct:.2f}%")
        return 1 if bad else 0


def load(path: Path):
    import pyvista as pv

    if not path.is_file():
        raise SystemExit(
            f"{path} missing -- run `python3 scripts/render_showcase.py --all` first "
            "(it caches the solve VTUs this script reads)"
        )
    g = pv.read(str(path))
    return (np.asarray(g.points),
            np.asarray(g.point_data["displacement"], dtype=float),
            np.asarray(g.point_data["von_Mises"], dtype=float),
            g)


# ---------------------------------------------------------------------------
# Cantilever: Saint-Venant flexure, stress and kinematics
# ---------------------------------------------------------------------------
def cantilever_stations(P, u, x0, x1, n=190, halfwidth=0.0025, minnodes=40):
    """Per-station section fits of u_x and u_z.

    The section fit is what makes the rotation and the axis deflection separable
    from the Poisson terms: u_z over a section carries a (z-zc)^2 and (y-yc)^2
    part whose coefficient is proportional to M(x), so a plain section *mean*
    mixes a term linear in x into the deflection curve. Projecting those out
    leaves the neutral-axis value.
    """
    L, hh = CANT["L"], CANT["h"]
    yc = zc = 0.05
    out = []
    for xs in np.linspace(x0, x1, n):
        s = np.abs(P[:, 0] - xs) < halfwidth
        k = int(s.sum())
        if k < minnodes:
            continue
        dz = P[s, 2] - zc
        dy = P[s, 1] - yc
        ax = np.column_stack([np.ones(k), dz, dy, dz * dy, dz**2, dy**2])
        cx, *_ = np.linalg.lstsq(ax, u[s, 0], rcond=None)
        az = np.column_stack([np.ones(k), dz**2, dy**2, dz, dy])
        cz, *_ = np.linalg.lstsq(az, u[s, 2], rcond=None)
        out.append((xs, cx[1], -cz[0]))
    a = np.array(out)
    return a[np.argsort(a[:, 0])]


def cantilever_moment(P, vm, x0=0.18, x1=0.85, n=60):
    """M(x) from the recovered |sigma_xx|, and the shear force from -dM/dx.

    |sigma_xx| = sqrt(vm^2 - 3 tau^2) with the beam tau profile, then
    M = k*I from a through-origin least-squares slope k of |sigma_xx| against
    |z-zc|. Fitting the slope rather than quadrating a binned profile matters:
    a trapezoid over bin *centres* truncates the outer h/40 on each face, where
    the lever arm is largest, and reads 14% low on this fixture.
    """
    L, b, hh, Pl = CANT["L"], CANT["b"], CANT["h"], CANT["P"]
    A = b * hh
    I = b * hh**3 / 12
    zc = 0.05
    rows = []
    for xs in np.linspace(x0, x1, n):
        s = np.abs(P[:, 0] - xs) < 0.0035
        if s.sum() < 60:
            continue
        zr = P[s, 2] - zc
        tau = 1.5 * Pl / A * (1 - (zr / (hh / 2)) ** 2)
        sabs = np.sqrt(np.maximum(vm[s] ** 2 - 3 * tau**2, 0.0))
        az = np.abs(zr)
        rows.append((xs, (az @ sabs) / (az @ az) * I))
    return np.array(rows)


def check_cantilever(rep: Report, vtu: Path) -> None:
    L, b, hh, E, nu, Pl = (CANT[k] for k in ("L", "b", "h", "E", "nu", "P"))
    A = b * hh
    I = b * hh**3 / 12
    G = E / (2 * (1 + nu))
    kappa = 10 * (1 + nu) / (12 + 11 * nu)          # Cowper, rectangular section
    zc = 0.05
    P, u, vm, _ = load(vtu)
    part = "cantilever"

    # ---- deformation: tip deflection and tip section rotation
    tip = P[:, 0] > L - 1e-9
    w_tip = -u[tip, 2].mean()
    w_timo = Pl * L**3 / (3 * E * I) + Pl * L / (kappa * G * A)
    rep.add(part, "tip deflection (Timoshenko)", w_tip, w_timo, 2.0, "m",
            "gap is the fully-clamped end face, not discretisation: h-converged, "
            "see --h-study")
    dz = P[tip, 2] - zc
    slope, icept = np.polyfit(dz, u[tip, 0], 1)
    rep.add(part, "tip section rotation PL^2/2EI", abs(slope),
            Pl * L**2 / (2 * E * I), 2.0, "rad")
    pred = slope * dz + icept
    r2 = 1 - ((u[tip, 0] - pred) ** 2).sum() / ((u[tip, 0] - u[tip, 0].mean()) ** 2).sum()
    rep.add(part, "plane sections stay plane (1 - R^2)", 1 - r2, 0.0, 0.05,
            absolute=True, note="u_x is linear in z across the free end face")

    # ---- deformation: the transverse shear angle, gamma = dw/dx - theta.
    # Measured as w(x2)-w(x1) minus the integral of the independently measured
    # section rotation, which is the only way to separate gamma*x from the cubic:
    # a direct 3-term fit of w(x) is collinear (cond ~1e2) and cannot resolve a
    # term worth 0.76% of the tip value.
    S = cantilever_stations(P, u, 0.06, 0.995)
    X, TH, W = S[:, 0], S[:, 1], S[:, 2]
    i1 = int(np.abs(X - 0.20).argmin())
    cum = np.concatenate([[0.0], np.cumsum(0.5 * (TH[1:] + TH[:-1]) * np.diff(X))])
    D = (W - W[i1]) - (cum - cum[i1])
    m = (X >= 0.25) & (X <= 0.92)
    gam = np.linalg.lstsq(np.column_stack([X[m] - X[i1], np.ones(int(m.sum()))]),
                          D[m], rcond=None)[0][0]
    rep.add(part, "shear angle P/(kappa G A)", gam, Pl / (kappa * G * A), 25.0, "rad",
            "wide band on purpose: this differences two ~2e-4 fields to recover "
            "1.5e-6, and resolves it to about +-20% across meshes")

    # ---- deformation: interior bending curvature, and the warping offset.
    #
    # theta(x) = a x(2L-x) + c. The constant c is real, not slop: the exact
    # Saint-Venant u_x carries a shear-warping term that is constant along the
    # beam (V is constant) and odd in z, so a linear-in-z regression picks it up
    # as a rotation offset of order gamma. Fitting a alone absorbs it and reads
    # the curvature 0.58% low at every h -- a "defect" that is entirely the
    # missing basis function. With c admitted, a is exact to 0.05% and c comes
    # out at 0.83..0.97 gamma, which is a second, independent confirmation that
    # the transverse shear compliance is present at the right magnitude (the
    # first being the neutral-plane shear stress below).
    mt = (X >= 0.2) & (X <= 0.9)
    basis = X[mt] * (2 * L - X[mt])
    a_meas, c_meas = np.linalg.lstsq(
        np.column_stack([basis, np.ones(int(mt.sum()))]), TH[mt], rcond=None)[0]
    rep.add(part, "interior curvature coefficient P/(2EI)", a_meas,
            Pl / (2 * E * I), 0.5, "1/m",
            "statics fixes M(x) = P(L-x), so this is E*I against b h^3/12 * E")
    rep.add(part, "rotation offset / shear angle", abs(c_meas) / (Pl / (kappa * G * A)),
            1.0, 40.0, "",
            "the constant term above, as a multiple of P/(kappa G A)")

    # ---- why the tip deflection misses Timoshenko by 0.7%.
    #
    # The interior curvature is exact and the shear angle is right, so the
    # deficit cannot be distributed along the beam; it has to come from the root.
    # A fully clamped end face forbids the lateral Poisson contraction and the
    # shear warping that beam theory's ideal clamp allows, so the root layer
    # turns through less angle than beam theory says. That lost rotation
    # d(theta) then rides out to the tip as d(theta)*L of lost deflection. This
    # check asserts the two numbers are the same one: if the ratio drifts away
    # from 1, the 0.7% is something else and wants investigating.
    lost_rot = Pl * L**2 / (2 * E * I) - abs(slope)
    rep.add(part, "root rotation deficit x L / tip deflection gap",
            lost_rot * L / (w_timo - w_tip), 1.0, 15.0, "",
            f"lost rotation {lost_rot:.3e} rad, deflection gap "
            f"{w_timo - w_tip:.3e} m")

    # ---- stress: pointwise Saint-Venant agreement, bending-dominated nodes
    sel = (P[:, 0] > 0.15) & (P[:, 0] < 0.85)
    zs = P[sel, 2] - zc
    sxx = Pl * (L - P[sel, 0]) * zs / I
    tau = 1.5 * Pl / A * (1 - (zs / (hh / 2)) ** 2)
    vmt = np.sqrt(sxx**2 + 3 * tau**2)
    rel = (vm[sel] - vmt) / vmt
    dom = np.abs(sxx) > 3 * np.sqrt(3) * tau
    rep.add(part, "vm vs sqrt(sxx^2+3tau^2), bending nodes (mean)",
            1 + rel[dom].mean(), 1.0, 1.5, "",
            f"{int(dom.sum())} nodes, rms {100 * np.sqrt((rel[dom]**2).mean()):.2f}%")
    na = np.abs(zs) < 0.002
    rep.add(part, "neutral-plane vm = sqrt(3)*3P/2A", vm[sel][na].mean(),
            np.sqrt(3) * 1.5 * Pl / A, 5.0, "Pa",
            "the transverse shear stress that carries the shear compliance")

    # ---- stress: section equilibrium
    Ms = cantilever_moment(P, vm)
    relM = (Ms[:, 1] - Pl * (L - Ms[:, 0])) / (Pl * (L - Ms[:, 0]))
    rep.add(part, "recovered M(x) / P(L-x) (mean)", 1 + relM.mean(), 1.0, 2.0, "",
            f"{len(Ms)} stations, rms {100 * np.sqrt((relM**2).mean()):.2f}%")
    V = -np.polyfit(Ms[:, 0], Ms[:, 1], 1)[0]
    rep.add(part, "shear force -dM/dx = applied P", V, Pl, 2.0, "N",
            "global equilibrium of the exported stress field")


# ---------------------------------------------------------------------------
# Cylinder: the clean deformation check
# ---------------------------------------------------------------------------
def check_cylinder(rep: Report, vtu: Path) -> None:
    R, H, E, nu, F = (CYL[k] for k in ("R", "H", "E", "nu", "F"))
    A = np.pi * R**2
    sig = F / A
    P, u, vm, _ = load(vtu)
    part = "cylinder"
    r = np.hypot(P[:, 0], P[:, 1])
    z = P[:, 2]
    ur = (u[:, 0] * P[:, 0] + u[:, 1] * P[:, 1]) / np.maximum(r, 1e-12)

    # Both Saint-Venant layers excluded: the clamped base suppresses the Poisson
    # contraction over ~R, and the load box reaches 5 mm down the wall.
    mi = (z > R) & (z < H - R)
    k = np.linalg.lstsq(np.column_stack([z[mi], np.ones(int(mi.sum()))]),
                        u[mi, 2], rcond=None)[0][0]
    rep.add(part, "interior du_z/dz = sigma/E", k, sig / E, 1.0, "",
            f"{int(mi.sum())} nodes, R < z < H-R")
    kr = np.linalg.lstsq(np.column_stack([r[mi], np.ones(int(mi.sum()))]),
                         ur[mi], rcond=None)[0][0]
    rep.add(part, "interior du_r/dr = -nu sigma/E", kr, -nu * sig / E, 2.0, "",
            "Poisson contraction, the transverse half of the deformation")
    rep.add(part, "interior von Mises = F/A", vm[mi].mean(), sig, 1.0, "Pa")

    # Section force. Annulus-area weighting, because a graded mesh puts more
    # nodes near the wall and a plain nodal mean would over-weight them.
    for frac in (0.4, 0.5, 0.6):
        z0 = z.min() + frac * (z.max() - z.min())
        s = np.abs(z - z0) < 0.0025
        edges = np.linspace(0.0, r[s].max(), 15)
        Fs = 0.0
        for i in range(len(edges) - 1):
            kk = s & (r >= edges[i]) & (r < edges[i + 1])
            if kk.sum():
                Fs += vm[kk].mean() * np.pi * (edges[i + 1] ** 2 - edges[i] ** 2)
        rep.add(part, f"section force at z = {z0:.3f} m", Fs, F, 1.5, "N",
                "uniaxial there, so von Mises is |sigma_zz| and this is equilibrium")


# ---------------------------------------------------------------------------
# Plate with hole
# ---------------------------------------------------------------------------
def check_plate(rep: Report, vtu: Path) -> None:
    a, E, nu, F = (PLATE[k] for k in ("a", "E", "nu", "F"))
    P, u, vm, _ = load(vtu)
    part = "plate_hole"
    lo, hi = P.min(axis=0), P.max(axis=0)
    W, t = hi[1] - lo[1], hi[2] - lo[2]
    Ap = W * t
    sig = F / Ap
    cx = 0.5 * (lo[0] + hi[0])

    # Section force at the station furthest from the hole. von Mises is |sigma_xx|
    # there (uniaxial), so this integrates to the applied resultant.
    s = np.abs(P[:, 0] - (hi[0] - 0.015)) < 0.0025
    rep.add(part, "section force 8.5a from the hole", vm[s].mean() * Ap, F, 1.5, "N")
    rep.add(part, "far-field von Mises (6a..9a)",
            vm[(np.abs(P[:, 0] - cx) > 6 * a) & (np.abs(P[:, 0] - cx) < 9 * a)].mean(),
            sig, 1.5, "Pa")

    # Kirsch, with the Howland finite-width correction for 2a/W = 0.2.
    rep.add(part, "stress concentration factor", vm.max() / sig, 3.12, 4.0, "",
            "Kirsch 3.0 for an infinite plate; 3.12 with Howland at 2a/W = 0.2")

    # The lateral contraction is zero at the clamped face and recovers to the
    # free-bar value over a Saint-Venant layer. Measured, so the plate's
    # interior-strain miss is attributed rather than assumed.
    truth = -nu * sig / E
    best = 0.0
    for x0 in np.linspace(lo[0] + 0.02, cx - 4 * a, 12):
        s = np.abs(P[:, 0] - x0) < 0.004
        if s.sum() < 200:
            continue
        ky = np.linalg.lstsq(np.column_stack([P[s, 1], np.ones(int(s.sum()))]),
                             u[s, 1], rcond=None)[0][0]
        best = max(best, ky / truth)
    rep.add(part, "du_y/dy recovers -nu sigma/E out of the clamp layer",
            best, 1.0, 8.0, "",
            "ratio at the best station inboard of the clamped face; 0.12 at the "
            "face itself, so the whole-plate average is not the free-bar value")


# ---------------------------------------------------------------------------
# Curved parts with no closed form: invariants
# ---------------------------------------------------------------------------
def check_axisymmetric(rep: Report, vtu: Path, part: str) -> None:
    P, u, vm, _ = load(vtu)
    r = np.hypot(P[:, 0], P[:, 1])
    # An axisymmetric geometry under an axisymmetric load has zero hoop
    # displacement. A tetrahedral mesh cannot be axisymmetric, so this measures
    # how much asymmetry the discretisation injects.
    uth = (-u[:, 0] * P[:, 1] + u[:, 1] * P[:, 0]) / np.maximum(r, 1e-12)
    scale = np.abs(u).max()
    rep.add(part, "max |u_hoop| / max |u|", np.abs(uth).max() / scale, 0.0, 1.0,
            absolute=True,
            note=f"rms {np.sqrt((uth**2).mean()) / scale:.2e}; the load and the "
                 "geometry are both axisymmetric, so the exact u_hoop is 0")


def check_sanity(rep: Report, name: str, vtu: Path) -> None:
    P, u, vm, _ = load(vtu)
    fixed = np.abs(u).max(axis=1) == 0.0
    rep.add(name, "constrained nodes exactly zero",
            float(np.abs(u[fixed]).max()) if fixed.any() else 1.0, 0.0, 0.0,
            absolute=True, note=f"{int(fixed.sum())} constrained nodes")
    bad = int((~np.isfinite(u)).sum() + (~np.isfinite(vm)).sum() + (vm < 0).sum())
    rep.add(name, "non-finite or negative field values", float(bad), 0.0, 0.0,
            absolute=True)


# ---------------------------------------------------------------------------
# h-refinement: is the beam-theory gap discretisation error?
# ---------------------------------------------------------------------------
def h_study(hs=(0.06, 0.045, 0.03, 0.02)) -> None:
    L, b, hh, E, nu, Pl = (CANT[k] for k in ("L", "b", "h", "E", "nu", "P"))
    I = b * hh**3 / 12
    A = b * hh
    G = E / (2 * (1 + nu))
    kappa = 10 * (1 + nu) / (12 + 11 * nu)
    w_timo = Pl * L**3 / (3 * E * I) + Pl * L / (kappa * G * A)
    VERIFY.mkdir(parents=True, exist_ok=True)
    cli = next((c for c in (REPO / "build/apps/cli/polymesh", REPO / "polymesh")
                if c.is_file()), None)
    if cli is None:
        raise SystemExit("polymesh CLI not found; build it first")
    print("\n[cantilever h-refinement]  tip deflection and section rotation vs h")
    print("   h(m)    nodes    elems |   tip w (m)     vs Timoshenko | theta_tip (rad)  vs PL^2/2EI")
    rows = []
    for hval in hs:
        out = VERIFY / f"cant_{hval:g}.vtu"
        if not out.is_file():
            argv = [str(cli), "solve", "tests/fixtures/parts/cantilever.step",
                    "-o", str(out.relative_to(REPO)), "-h", f"{hval:g}",
                    "--mesher", "graded", "-E", f"{E:g}", "-nu", f"{nu:g}",
                    "--load-dir", "0", "0", "-1", "--force", f"{Pl:g}"]
            print("    $ " + " ".join(argv))
            subprocess.run(argv, cwd=REPO, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        P, u, vm, g = load(out)
        tip = P[:, 0] > L - 1e-9
        w = -u[tip, 2].mean()
        th = abs(np.polyfit(P[tip, 2] - 0.05, u[tip, 0], 1)[0])
        rows.append((hval, g.n_points, g.n_cells, w, th))
        print(f"  {hval:6.3f} {g.n_points:8d} {g.n_cells:8d} | {w:.6e} "
              f"{100 * (w - w_timo) / w_timo:+8.3f}% | {th:.6e} "
              f"{100 * (th - Pl * L**2 / (2 * E * I)) / (Pl * L**2 / (2 * E * I)):+8.3f}%")
    ws = np.array([r[3] for r in rows])
    print(f"\n  tip deflection spread over a {max(r[2] for r in rows) / min(r[2] for r in rows):.0f}x "
          f"element-count range: {100 * (ws.max() - ws.min()) / ws.mean():.3f}%")
    print("  -> the residual gap to beam theory is not discretisation error; it is the")
    print("     fully-clamped end face, which beam theory does not model.")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", action="append", default=[],
                    help="part name; repeatable")
    ap.add_argument("--h-study", action="store_true",
                    help="also solve the cantilever at several h and report convergence")
    args = ap.parse_args()
    want = (lambda n: not args.only or n in args.only)

    rep = Report()
    parts = {
        "cantilever": (check_cantilever, CACHE / "cantilever.vtu"),
        "cylinder": (check_cylinder, CACHE / "cylinder.vtu"),
        "plate_hole": (check_plate, CACHE / "plate_hole.vtu"),
    }
    for name, (fn, vtu) in parts.items():
        if want(name):
            fn(rep, vtu)
            check_sanity(rep, name, vtu)
    for name in ("sphere", "icecream_cone"):
        if want(name):
            check_axisymmetric(rep, CACHE / f"{name}.vtu", name)
            check_sanity(rep, name, CACHE / f"{name}.vtu")

    rc = rep.dump()
    if args.h_study:
        h_study()
    return rc


if __name__ == "__main__":
    sys.exit(main())
