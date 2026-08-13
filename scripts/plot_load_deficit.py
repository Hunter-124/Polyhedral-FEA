#!/usr/bin/env python3
"""Load-deficit mechanism figure.

Every number is parsed from the ``load_deficit_mechanism`` block of
bench/reference/external/external-truth-findings.json. Nothing is typed in.

The honest reading, which the figure must carry:
  * the F^2 load-deficit argument explains the LARGE positive movers only;
  * most cases have essentially no area deficit (median ~0%) and still moved,
    which is ordinary discretisation / geometry-fidelity error;
  * a few channel cases have a NEGATIVE deficit (old selection over-covered);
  * the F^2 prediction is an UPPER BOUND -- it systematically over-predicts,
    because the old mesh was also missing material and hence more flexible.

Usage:
    python scripts/plot_load_deficit.py [--out-dir docs/validation/figures]
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402
import numpy as np  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
FINDINGS = REPO / "bench/reference/external/external-truth-findings.json"
DEFAULT_OUT = REPO / "docs/validation/figures"

BIG_DEFICIT_PCT = 5.0
NEAR_ZERO_PCT = 0.5


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO).as_posix()
    except (ValueError, OSError):
        return path.as_posix()


def load_block(path: Path) -> dict | None:
    if not path.is_file():
        print(f"no data yet — {rel(path)} missing, skipping")
        return None
    block = json.loads(path.read_text(encoding="utf-8")).get("load_deficit_mechanism")
    if not block or not block.get("per_case"):
        print(f"no data yet — {rel(path)} has no load_deficit_mechanism/per_case, skipping")
        return None
    return block


def scatter_panel(ax, cases, pred_key, obs_key, title, big_flags):
    """Predicted vs observed change, with the 1:1 line."""
    t = fs.theme()
    fams = sorted({c["family"] for c in cases})
    pred = np.array([c[pred_key] for c in cases], dtype=float)
    obs = np.array([c[obs_key] for c in cases], dtype=float)
    lo = float(min(pred.min(), obs.min()))
    hi = float(max(pred.max(), obs.max()))
    pad = 0.08 * (hi - lo)
    span = [lo - pad, hi + pad]

    ax.plot(span, span, color=t.rule, lw=1.2, ls=(0, (5, 3)), zorder=1)
    mid = span[0] + 0.42 * (span[1] - span[0])
    ax.annotate("1:1  prediction = observation", xy=(mid, mid),
                xytext=(6, -4), textcoords="offset points", ha="left", va="top",
                fontsize=7.5, color=t.muted, rotation=45,
                rotation_mode="anchor")
    ax.axhline(0.0, color=t.grid, lw=0.8, zorder=0)
    ax.axvline(0.0, color=t.grid, lw=0.8, zorder=0)

    for fam in fams:
        st = fs.series(fam)
        idx = [i for i, c in enumerate(cases) if c["family"] == fam]
        small = [i for i in idx if not big_flags[i]]
        large = [i for i in idx if big_flags[i]]
        if small:
            ax.scatter(pred[small], obs[small], s=26, marker=st.marker,
                       facecolors="none", edgecolors=st.color, linewidths=1.2,
                       zorder=3, label=st.label)
        if large:
            ax.scatter(pred[large], obs[large], s=110, marker=st.marker,
                       color=st.color, edgecolors=t.ink, linewidths=1.1,
                       zorder=4, label=None if small else st.label)

    ax.set_xlim(*span)
    ax.set_ylim(*span)
    ax.set_xlabel("predicted change from F$^2$ load deficit (%)")
    ax.set_ylabel("observed change, old -> external (%)")
    fs.panel_title(ax, title)
    ax.grid(True, color=t.grid, lw=0.5, alpha=0.7)
    ax.set_axisbelow(True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    block = load_block(FINDINGS)
    if block is None:
        return 0

    cases = list(block["per_case"])
    n = len(cases)
    for slot, fam in enumerate(sorted({c["family"] for c in cases})):
        fs.register_series(fam, slot, label=fam.replace("_", " "))
    fs.use("light")

    deficits = np.array([c["area_deficit_pct"] for c in cases], dtype=float)
    big_flags = [d > BIG_DEFICIT_PCT for d in deficits]
    n_big = int(sum(big_flags))
    near = [c for c, d in zip(cases, deficits) if abs(d) <= NEAR_ZERO_PCT]
    n_near = len(near)
    med = statistics.median(deficits)
    neg = [c for c, d in zip(cases, deficits) if d < 0.0]
    worst_neg = min(deficits)
    big_lo = min(d for d in deficits if d > BIG_DEFICIT_PCT)
    big_hi = max(deficits)
    near_e_lo = min(c["observed_strain_energy_change_pct"] for c in near)
    near_e_hi = max(c["observed_strain_energy_change_pct"] for c in near)

    over_e = [(c["case"],
               c["predicted_strain_energy_change_pct"] - c["observed_strain_energy_change_pct"])
              for c, f in zip(cases, big_flags) if f]
    over_d = [(c["case"],
               c["predicted_tip_deflection_change_pct"] - c["observed_tip_deflection_change_pct"])
              for c, f in zip(cases, big_flags) if f]
    over_e_lo = min(v for _, v in over_e)
    over_e_hi = max(v for _, v in over_e)
    over_d_lo = min(v for _, v in over_d)
    over_d_hi = max(v for _, v in over_d)
    big_obs_d_lo = min(c["observed_tip_deflection_change_pct"] for c, f in zip(cases, big_flags) if f)
    big_obs_d_hi = max(c["observed_tip_deflection_change_pct"] for c, f in zip(cases, big_flags) if f)

    print(f"cases resolved: {n} (file says {block.get('cases_resolved')})")
    print(f"deficit > {BIG_DEFICIT_PCT:g}%: {n_big} "
          f"(file says {block.get('cases_with_deficit_above_5pct')}), "
          f"range {big_lo:.3f}% to {big_hi:.3f}%")
    print(f"median deficit: {med:.2f}%   min {deficits.min():.3f}%  max {deficits.max():.3f}%")
    print(f"|deficit| <= {NEAR_ZERO_PCT:g}%: {n_near} cases, observed strain-energy change "
          f"{near_e_lo:.2f}% to {near_e_hi:.2f}%")
    print(f"negative deficit (old over-covered): {len(neg)} cases, worst {worst_neg:.3f}%")
    print("F^2 over-prediction on large-deficit cases "
          "(predicted - observed, percentage points):")
    for (name, ve), (_, vd) in zip(over_e, over_d):
        print(f"  {name:<24} energy {ve:+6.2f} pp   deflection {vd:+6.2f} pp")
    print(f"  energy over-prediction range: {over_e_lo:.2f} to {over_e_hi:.2f} pp")
    print(f"  deflection over-prediction range: {over_d_lo:.2f} to {over_d_hi:.2f} pp")
    print("all plotted points (case, deficit%, pred E%, obs E%, pred d%, obs d%):")
    for c in cases:
        print(f"  {c['case']:<24} {c['area_deficit_pct']:+8.3f} "
              f"{c['predicted_strain_energy_change_pct']:+9.2f} "
              f"{c['observed_strain_energy_change_pct']:+9.2f} "
              f"{c['predicted_tip_deflection_change_pct']:+9.2f} "
              f"{c['observed_tip_deflection_change_pct']:+9.2f}")

    title = "Load deficit: the old chain solved a smaller-force problem"
    subtitle = (
        f"F$^2$ upper bound explains the {n_big} cases with an area deficit above "
        f"{BIG_DEFICIT_PCT:g}% ({big_lo:.1f}-{big_hi:.1f}%), over-predicting strain energy "
        f"by {over_e_lo:.1f}-{over_e_hi:.1f} pp and deflection by "
        f"{over_d_lo:.1f}-{over_d_hi:.1f} pp.\n"
        f"Median deficit over all {n} cases is {med:.2f}%: {n_near} cases within "
        f"+/-{NEAR_ZERO_PCT:g}% deficit still moved {near_e_lo:.2f}% to {near_e_hi:+.2f}% "
        f"in strain energy, from discretisation error with no load component."
    )
    footer = fs.footer_source(FINDINGS, n=n,
                              note="deficit = 1 - old-mesh selected load-face area / external area")
    fs.assert_glyphs(title, subtitle, footer)

    fig, axes = fs.figure(
        title, subtitle=subtitle, footer=footer, size=(13.0, 6.0), nrows=1, ncols=3,
        share_y_axis="each panel is a different quantity: strain energy %, deflection %, area deficit %",
    )
    t = fs.theme()
    ax_e, ax_d, ax_h = axes[0]

    scatter_panel(ax_e, cases, "predicted_strain_energy_change_pct",
                  "observed_strain_energy_change_pct",
                  "a. strain energy  (F$^2$)", big_flags)
    scatter_panel(ax_d, cases, "predicted_tip_deflection_change_pct",
                  "observed_tip_deflection_change_pct",
                  "b. tip deflection  (F$^1$)", big_flags)
    ax_e.annotate("points below 1:1 = expected physical residual, not noise:\n"
                  "the old mesh also missed material, so it was more flexible\n"
                  "and stored more energy per unit force",
                  xy=(0.035, 0.975), xycoords="axes fraction", ha="left", va="top",
                  fontsize=7.5, color=t.muted)

    # Panel c: the deficit distribution itself, one dot per case.
    fams = sorted({c["family"] for c in cases})
    ax_h.axvspan(-NEAR_ZERO_PCT, NEAR_ZERO_PCT, color=t.band, alpha=0.55, zorder=0)
    ax_h.axvline(0.0, color=t.grid, lw=0.8, zorder=1)
    ax_h.axvline(BIG_DEFICIT_PCT, color=t.rule, lw=1.1, ls=(0, (4, 3)), zorder=2)
    ax_h.axvline(med, color=t.ink, lw=1.1, ls=(0, (1, 2)), zorder=2)
    rng = np.random.default_rng(0)
    for row, fam in enumerate(fams):
        st = fs.series(fam)
        vals = [c["area_deficit_pct"] for c in cases if c["family"] == fam]
        flags = [d > BIG_DEFICIT_PCT for d in vals]
        ys = row + rng.uniform(-0.22, 0.22, size=len(vals))
        for v, y, f in zip(vals, ys, flags):
            if f:
                ax_h.scatter([v], [y], s=95, marker=st.marker, color=st.color,
                             edgecolors=t.ink, linewidths=1.1, zorder=4)
            else:
                ax_h.scatter([v], [y], s=26, marker=st.marker, facecolors="none",
                             edgecolors=st.color, linewidths=1.2, zorder=3)
    ax_h.set_yticks(range(len(fams)))
    ax_h.set_yticklabels([f.replace("_", " ") for f in fams], fontsize=8)
    ax_h.set_ylim(-0.7, len(fams) - 0.3)
    ax_h.set_xlabel("load-face area deficit of the old mesh (%)")
    fs.panel_title(ax_h, f"c. only {n_big} of {n} meshes lost real load area")
    ax_h.grid(True, axis="x", color=t.grid, lw=0.5, alpha=0.7)
    ax_h.set_axisbelow(True)
    note_c = (f"median {med:.2f}% (dotted) · +/-{NEAR_ZERO_PCT:g}% band shaded · "
              f"{BIG_DEFICIT_PCT:g}% threshold (dashed): {n_big} cases to its right\n"
              f"{len(neg)} cases have a NEGATIVE deficit — the old selection\n"
              f"over-covered the face by up to {abs(worst_neg):.2f}%")
    fs.assert_glyphs(note_c)
    ax_h.annotate(note_c, xy=(0.0, -0.145), xycoords="axes fraction", ha="left", va="top",
                  fontsize=7.5, color=t.muted)

    handles = fs.series_handles(fams)
    ax_e.legend(handles=handles, loc="lower right", fontsize=7.5, frameon=False,
                title="family", title_fontsize=7.5)
    fs.annotate_n(ax_d, n, what="cases", loc="lower right",
                  extra=f"filled + outlined = deficit > {BIG_DEFICIT_PCT:g}% ({n_big})")

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    path = fs.finish(fig, out_dir / "load_deficit.png")
    print(f"subtitle: {subtitle}")
    print(f"wrote {rel(Path(path))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
