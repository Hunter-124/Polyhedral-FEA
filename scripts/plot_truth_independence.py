#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""How the independent Gmsh -> CalculiX chain earned the right to define truth.

Four panels, all computed from the external-truth artefacts:

  A  box_hole stress concentration against the citable finite-width closed form
     (Howland 1930 via Roark ch.6 / Peterson chart 4.1): the old 3.0
     infinite-plate reference sits below it on every case, the independent 3D
     solve sits above it on every case.
  B  stepped_shaft tip deflection and strain energy against the Timoshenko
     closed form, with the U = P*delta/2 self-consistency residual beside them.
  C  convergence quality: rung-to-rung change per quantity, plus the two
     independent mesh sizing policies. This is where the honesty about peak
     stress lives -- the nodal peak is the slow, policy-sensitive quantity.
  D  the identity checks (Clapeyron, equilibrium, loaded area, FRD reader) and
     the CalculiX-vs-our-solver control, on a log axis against the tightest
     tolerance any reference in the corpus carries.

Missing inputs print "no data yet" and exit 0, so this is safe to run while a
campaign is regenerating the artefacts. Every plotted number is printed.

    python scripts/plot_truth_independence.py
    python scripts/plot_truth_independence.py --out-dir docs/validation/figures
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
VALIDATE = REPO / "bench/reference/external/external-truth-validate.json"
FINDINGS = REPO / "bench/reference/external/external-truth-findings.json"
EXTERNAL_TRUTH_PY = REPO / "bench/reference/external_truth.py"

fs.register_series("old_reference_3p0", 1, label="old reference: 3.0 (infinite plate)")
fs.register_series("external_3d", 5, label="independent 3D: Gmsh + CalculiX")
fs.register_series("tip_deflection", 0, label="tip deflection")
fs.register_series("strain_energy", 2, label="stored energy")
fs.register_series("self_consistency", 3, label="energy vs work check")
fs.register_series("peak_von_mises", 1, label="peak stress (von Mises)")


def load(path: Path):
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"no data yet \u2014 {path} is not valid JSON ({exc}); skipping")
        return None


def tolerance_floor() -> float:
    """The tightest tolerance any external-truth reference carries."""
    if EXTERNAL_TRUTH_PY.is_file():
        m = re.search(r"^TOL_FLOOR\s*=\s*([0-9.eE+-]+)", 
                      EXTERNAL_TRUTH_PY.read_text(encoding="utf-8"), re.M)
        if m:
            return float(m.group(1))
    return math.nan


def finding(findings: dict, fid: str) -> dict | None:
    for f in findings.get("findings", []):
        if f.get("id") == fid:
            return f.get("evidence", {})
    return None


def short(case: str) -> str:
    """box_hole_s0_c0 -> s0."""
    m = re.search(r"_(s\d+)_c\d+$", case)
    return m.group(1) if m else case


# ---------------------------------------------------------------------------
# Panels
# ---------------------------------------------------------------------------
def panel_box_hole(ax, cases: list[dict]) -> dict:
    """Signed deviation of each candidate 'truth' from the finite-width form."""
    old_st = fs.series("old_reference_3p0")
    ext_st = fs.series("external_3d")
    labels, old_dev, ext_dev = [], [], []
    print("\npanel A \u2014 box_hole vs Howland/Roark finite-width Ktg")
    for c in cases:
        ktg = float(c["Ktg"])
        old = (float(c["old_reference"]) / ktg - 1.0) * 100.0
        ext = (float(c["external_converged_3d"]) / ktg - 1.0) * 100.0
        labels.append(f"{short(c['case'])}\nhole/width\n{float(c['d_over_W']):.3f}")
        old_dev.append(old)
        ext_dev.append(ext)
        print(f"  {c['case']:<16} d/W={float(c['d_over_W']):.4f} "
              f"Ktn={float(c['Ktn']):.4f} Ktg={ktg:.4f}  "
              f"old 3.0 -> {old:+.2f}% (file {float(c['old_reference_error_vs_Ktg_pct']):+.2f}%)  "
              f"3D {float(c['external_converged_3d']):.5f} -> {ext:+.2f}% "
              f"(file {float(c['gap_3d_vs_2d_pct']):+.2f}%)")

    x = np.arange(len(labels), dtype=float)
    t = fs.theme()
    ax.axhline(0.0, color=t.ink, linewidth=1.4, zorder=4)
    ax.text(0.01, 0.0, " handbook value = 0%",
            transform=ax.get_yaxis_transform(), ha="left", va="bottom",
            fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink, zorder=5)
    for xi, v in zip(x - 0.13, old_dev):
        ax.vlines(xi, 0.0, v, color=old_st.color, linewidth=1.4, zorder=2)
    for xi, v in zip(x + 0.13, ext_dev):
        ax.vlines(xi, 0.0, v, color=ext_st.color, linewidth=1.4, zorder=2)
    ax.scatter(x - 0.13, old_dev, s=70, zorder=5, **old_st.scatter())
    ax.scatter(x + 0.13, ext_dev, s=70, zorder=5, **ext_st.scatter())
    for xi, v in zip(x - 0.13, old_dev):
        ax.annotate(f"{v:+.2f}%", (xi, v), textcoords="offset points",
                    xytext=(0, -14), ha="center",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink)
    for xi, v in zip(x + 0.13, ext_dev):
        ax.annotate(f"{v:+.2f}%", (xi, v), textcoords="offset points",
                    xytext=(0, 8), ha="center",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlim(-0.55, len(labels) - 0.45)
    ax.set_ylim(min(old_dev) - 1.2, max(ext_dev) + 2.6)
    ax.set_ylabel("difference from the handbook stress concentration (%)")
    ax.legend(handles=fs.series_handles(["old_reference_3p0", "external_3d"]),
              loc="lower right", frameon=False,
              fontsize=fs.FONT_PT["legend"] - 0.5)
    fs.annotate_n(ax, len(labels), what="plate-with-hole cases", loc="upper left",
                  extra="all 4 old references sit low, all 4 independent solves sit high\n"
                        "same direction every time — that is real thickness, not scatter")
    fs.panel_title(ax, "A \u00b7 the old 3.0 reference came from an infinitely "
                       "wide plate \u2014 3\u20134% too low")
    return {"old": old_dev, "ext": ext_dev}


def panel_stepped(ax, cases: list[dict]) -> dict:
    """Timoshenko bias and the Clapeyron self-consistency residual."""
    print("\npanel B \u2014 stepped_shaft vs Timoshenko closed form")
    names = ["tip_deflection", "strain_energy", "self_consistency"]
    keys = ["tip_deflection_err_pct", "strain_energy_err_pct",
            "self_consistency_pct"]
    labels, vals = [], {n: [] for n in names}
    for c in cases:
        labels.append(f"{short(c['case'])}\n{int(c['n_dof']):,}\nunknowns")
        for n, k in zip(names, keys):
            vals[n].append(float(c[k]))
        print(f"  {c['case']:<20} tip {float(c['tip_deflection_err_pct']):+.2f}%  "
              f"energy {float(c['strain_energy_err_pct']):+.2f}%  "
              f"U vs P\u00b7d/2 {float(c['self_consistency_pct']):.2f}%  "
              f"rung delta {float(c['rung_delta_pct']):.2f}%  "
              f"{int(c['n_dof']):,} DOF")

    x = np.arange(len(labels), dtype=float)
    t = fs.theme()
    ax.axhline(0.0, color=t.ink, linewidth=1.4, zorder=4)
    ax.text(0.01, 0.0, " textbook formula = 0%",
            transform=ax.get_yaxis_transform(), ha="left", va="top",
            fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink, zorder=5)
    for i, n in enumerate(names):
        st = fs.series(n)
        xs = x + (i - 1) * 0.16
        ax.vlines(xs, 0.0, vals[n], color=st.color, linewidth=1.4, zorder=2)
        ax.scatter(xs, vals[n], s=70, zorder=5, **st.scatter())
    for xi, v in zip(x - 0.16, vals["tip_deflection"]):
        ax.annotate(f"{v:+.2f}%", (xi, v), textcoords="offset points",
                    xytext=(0, 9), ha="center",
                    fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlim(-0.55, len(labels) - 0.45)
    ax.set_ylim(-0.35, max(vals["tip_deflection"]) * 2.2)
    ax.set_ylabel("difference from the textbook beam formula (%)")
    ax.legend(handles=fs.series_handles(names), loc="upper right",
              frameon=False, fontsize=fs.FONT_PT["legend"] - 0.5)
    worst_self = max(vals["self_consistency"])
    fs.annotate_n(ax, len(labels), what="stepped-shaft cases", loc="upper left",
                  extra=f"all 8 gaps positive — the real\n"
                        f"3D part bends more than the\n"
                        f"beam formula predicts\n"
                        f"energy check agrees to {worst_self:.2f}% or better")
    fs.panel_title(ax, "B \u00b7 every case sits +1 to +2.4% above the beam")
    return vals


def panel_convergence(ax, val_cases: list[dict], sizing: dict) -> dict:
    """Rung-to-rung change and sizing-policy sensitivity, peak stress apart."""
    print("\npanel C \u2014 convergence quality and sizing-policy sensitivity")
    groups: dict[str, list[tuple[str, float]]] = {
        "peak_von_mises": [], "tip_deflection": [], "strain_energy": []}
    key_map = {"scf": "peak_von_mises", "tip_deflection": "tip_deflection",
               "strain_energy": "strain_energy"}
    for c in val_cases:
        for k, v in (c.get("convergence_delta") or {}).items():
            name = key_map.get(k)
            if name is None:
                continue
            groups[name].append((short(c["case_id"]), float(v) * 100.0))
            print(f"  rung\u2192rung {c['case_id']:<20} {k:<15} "
                  f"{float(v) * 100.0:.4f}%")

    policy = {"strain_energy": float(sizing["strain_energy_rel"]) * 100.0,
              "tip_deflection": float(sizing["tip_deflection_rel"]) * 100.0,
              "peak_von_mises": float(sizing["peak_von_mises_rel"]) * 100.0}
    global_policy = max(policy["strain_energy"], policy["tip_deflection"])
    ratio = policy["peak_von_mises"] / global_policy
    for k, v in policy.items():
        print(f"  sizing policy  {k:<16} {v:.5f}%")
    print(f"  sizing-policy sensitivity ratio: peak / global = {ratio:.0f}\u00d7")

    columns = [("peak\nstress\nrefining\nthe mesh", "peak_von_mises",
                [v for _, v in groups["peak_von_mises"]]),
               ("tip\ndeflection\nrefining\nthe mesh", "tip_deflection",
                [v for _, v in groups["tip_deflection"]]),
               ("stored\nenergy\nrefining\nthe mesh", "strain_energy",
                [v for _, v in groups["strain_energy"]]),
               ("peak\nstress\nnew sizing\nrule", "peak_von_mises",
                [policy["peak_von_mises"]]),
               ("whole\npart\nnew sizing\nrule", "tip_deflection",
                [policy["tip_deflection"], policy["strain_energy"]])]

    every = [v for _, _, vs in columns for v in vs]
    info = fs.loglim(ax, every, axis="y")
    t = fs.theme()
    for i, (label, name, vs) in enumerate(columns):
        st = fs.series(name)
        ys = fs.clamp_to_floor(vs, info.floor)
        xs = np.full(len(ys), float(i)) + np.linspace(-0.08, 0.08, len(ys))
        ax.scatter(xs, ys, s=70, zorder=5, **st.scatter())
        ax.text(i, max(ys) * 1.35, f"{max(vs):.3g}%", ha="center", va="bottom",
                fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink)
    ax.axvline(2.5, color=t.rule, linewidth=1.0, linestyle=(0, (2, 2)))
    ax.set_xticks(range(len(columns)))
    ax.set_xticklabels([c[0] for c in columns])
    ax.set_xlim(-0.6, len(columns) - 0.4)
    ax.set_ylabel("change in the answer (%)")
    ax.annotate(
        f"change the mesh sizing rule and the peak stress\n"
        f"moves {ratio:.0f}\u00d7 more than the whole-part numbers do",
        xy=(3.0, policy["peak_von_mises"]), xytext=(0.03, 0.10),
        textcoords="axes fraction", ha="left", va="bottom",
        fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink,
        arrowprops=dict(arrowstyle="->", color=t.muted, linewidth=1.0))
    fs.annotate_n(ax, len(every), what="measurements", loc="upper right",
                  extra=info.note("numerical noise"))
    ax.set_ylim(top=ax.get_ylim()[1] * 3.0)
    fs.panel_title(ax, "C \u00b7 the honest one: peak stress settles slowest "
                       "and shifts with the sizing rule")
    return {"ratio": ratio, "policy": policy, "groups": groups}


def panel_identities(ax, sup: dict, cross: list[dict], tol: float) -> dict:
    """Conservation identities and the solver-vs-solver control, log scale."""
    print("\npanel D \u2014 identity checks and the solver control")
    area = sup["loaded_area_vs_exact_CAD_area_rel_err"]
    rows = [
        ("CalculiX energy vs work done (Clapeyron)  (all 72)",
         float(sup["calculix_internal_energy_vs_half_f_dot_u_max_rel_gap"])),
        ("reactions balance the applied load  (all 72)",
         float(sup["reaction_vs_applied_resultant_max_rel_err"])),
        ("loaded area vs exact CAD area \u2014 rectangular end",
         float(area["box_hole_rectangular_end"])),
        ("loaded area vs exact CAD area \u2014 circular end",
         float(area["stepped_shaft_circular_end"])),
        ("our results reader vs CalculiX's own printout",
         float(sup["frd_reader_validated_against_dat_print_rel_err"])),
    ]
    for c in cross:
        rows.append(("CalculiX vs our solver, one identical mesh: deflection",
                     float(c["tip_deflection_rel_diff"])))
        rows.append(("CalculiX vs our solver, one identical mesh: energy",
                     float(c["strain_energy_rel_diff"])))
    for label, v in rows:
        print(f"  {label:<62} {v:.2e}   {tol / v:,.0f}\u00d7 inside the "
              f"{tol:.0%} tolerance floor")

    labels = [r[0] for r in rows]
    vals = [r[1] for r in rows]
    y = np.arange(len(rows))[::-1]
    t = fs.theme()
    info = fs.loglim(ax, vals + [tol], axis="x")
    xs = fs.clamp_to_floor(vals, info.floor)
    ax.hlines(y, info.lo, xs, color=t.grid, linewidth=1.2, zorder=1)
    st = fs.series("external_3d")
    ctl = fs.series("calculix")
    for yi, xi, label in zip(y, xs, labels):
        s = ctl if "our solver" in label else st
        ax.scatter([xi], [yi], s=70, zorder=5, **s.scatter(label=None))
        ax.text(xi * 1.35, yi, f"{xi:.1e}", va="center", ha="left",
                fontsize=fs.FONT_PT["annot"] - 0.5, color=t.ink, zorder=6)
    ax.axvline(tol, color=t.band, linewidth=1.4, linestyle=(0, (4, 2)), zorder=3)
    ax.text(tol, 1.0, f" strictest tolerance we allow: {tol:.0%}",
            transform=ax.get_xaxis_transform(), rotation=90, va="top",
            ha="right", fontsize=fs.FONT_PT["annot"] - 0.5, color=t.band)
    ax.set_yticks(y)
    ax.set_yticklabels(labels, fontsize=fs.FONT_PT["tick"] - 0.5)
    ax.set_ylim(-0.8, len(rows) - 0.2)
    ax.set_xlabel("size of the gap, as a fraction of the value (log scale)")
    ax.grid(axis="y", visible=False)
    worst = max(vals)
    fs.annotate_n(ax, len(rows), what="checks", loc="lower left",
                  extra=f"worst gap {worst:.1e} \u2014 "
                        f"{tol / worst:,.0f}\u00d7 tighter than the strictest tolerance")
    fs.panel_title(ax, "D \u00b7 the checks close; the two solvers agree")
    return {"rows": rows, "worst": worst}


# ---------------------------------------------------------------------------
def build(out_dir: Path) -> int:
    validate = load(VALIDATE)
    findings = load(FINDINGS)
    if validate is None or findings is None:
        missing = [Path(os.path.relpath(p, REPO)).as_posix()
                   for p, d in ((VALIDATE, validate), (FINDINGS, findings))
                   if d is None]
        print(f"no data yet \u2014 {', '.join(missing)} missing; skipping "
              "truth_independence.png")
        return 0

    ev_a = finding(findings, "a")
    ev_c = finding(findings, "c")
    sup = findings.get("supporting_verification") or {}
    sizing = sup.get("mesh_sizing_policy_sensitivity") or {}
    cross = validate.get("crosscheck") or []
    val_cases = [c for c in validate.get("cases", [])
                 if c.get("convergence_delta")]
    tol = tolerance_floor()
    if not (ev_a and ev_c and sup and sizing and cross and val_cases
            and math.isfinite(tol)):
        print("no data yet \u2014 external-truth artefacts are incomplete "
              "(findings a/c, supporting_verification, crosscheck, "
              "convergence deltas or TOL_FLOOR); skipping "
              "truth_independence.png")
        return 0

    fs.use("light")
    box = ev_a["per_case"]
    shaft = ev_c["per_case"]

    # --- everything the title bar claims, computed here -------------------
    old_dev = [(float(c["old_reference"]) / float(c["Ktg"]) - 1.0) * 100.0
               for c in box]
    ext_dev = [(float(c["external_converged_3d"]) / float(c["Ktg"]) - 1.0) * 100.0
               for c in box]
    beam_dev = ([float(c["tip_deflection_err_pct"]) for c in shaft]
                + [float(c["strain_energy_err_pct"]) for c in shaft])
    n_signed = len(old_dev) + len(ext_dev) + len(beam_dev)
    uniform = (all(v < 0 for v in old_dev) and all(v > 0 for v in ext_dev)
               and all(v > 0 for v in beam_dev))
    ratio = (float(sizing["peak_von_mises_rel"])
             / max(float(sizing["strain_energy_rel"]),
                   float(sizing["tip_deflection_rel"])))
    n_refs = int((findings.get("corrections_delivered") or {})
                 .get("references_rewritten", 0))

    sign_txt = (f"All {n_signed} of the differences land on the side they should"
                if uniform else
                f"WARNING: the differences do NOT all land on the side they "
                f"should, across the {n_signed} comparisons")
    subtitle = (
        f"{n_refs} reference answers were rebuilt by outside tools: shapes read "
        f"from STEP files, meshed by Gmsh {validate['gmsh_version']},\n"
        f"solved by CalculiX {validate['calculix_version']}. Our own mesher and "
        "our own solver take no part in any reference answer.\n"
        f"{sign_txt}.\n"
        f"The peak stress at a single point reacts {ratio:.0f}\u00d7 more "
        "strongly to the mesh sizing rule than any whole-part number.")
    footer = fs.footer_source(
        VALIDATE, FINDINGS, n=n_refs,
        note="closed forms: Howland (1930) Phil. Trans. R. Soc. A 229:49\u201386\n"
             "via Roark ch.6 / Peterson chart 4.1; Timoshenko + Cowper shear "
             "(docs/validation/hand-calcs.md)")
    fs.assert_glyphs(subtitle, footer)

    title = ("The reference answers come from outside tools \u2014 and those "
             "tools earned the job")
    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size=(13.0, 10.4),
        nrows=2, ncols=2,
        share_y_axis="the four panels measure different quantities: signed "
                     "percent deviation, relative change on a log axis, and "
                     "dimensionless residuals on a log axis")

    a = panel_box_hole(axes[0][0], box)
    b = panel_stepped(axes[0][1], shaft)
    c = panel_convergence(axes[1][0], val_cases, sizing)
    d = panel_identities(axes[1][1], sup, cross, tol)

    print("\ncomputed subtitle:\n  " + subtitle)
    if not uniform:
        print("  !! the data no longer supports the uniform-sign claim")
    worst_old = min(a["old"])
    print(f"  worst old-reference deviation: {worst_old:+.2f}%   "
          f"worst 3D-vs-2D gap: {max(a['ext']):+.2f}%   "
          f"worst beam gap: {max(b['strain_energy'] + b['tip_deflection']):+.2f}%   "
          f"peak/global sizing ratio: {c['ratio']:.0f}\u00d7   "
          f"worst identity residual: {d['worst']:.1e}")

    out_dir.mkdir(parents=True, exist_ok=True)
    fs.finish(fig, out_dir / "truth_independence.png")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Render the truth-independence validation figure.")
    ap.add_argument("--out-dir", type=Path,
                    default=REPO / "docs/validation/figures",
                    help="output directory for the PNG")
    args = ap.parse_args(argv)
    written = build(args.out_dir)
    print(f"\n{written}/1 figures written to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
