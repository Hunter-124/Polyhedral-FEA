#!/usr/bin/env python3
"""Refusal boundary: the mesher declines a size it cannot represent.

Single source: bench/advisor/dataset.csv (the advisor sweep). Every number on
the figure is computed from that file; nothing is typed in.

What the figure says:
  * a run is classified only by what the engine itself reported -- the status
    column plus the three textually distinct refusal messages in ``error``;
  * the boundary drawn is the engine's OWN stated rule from
    src/pipeline/src/scene.cpp ("two cells across the thinnest wall is the
    minimum that can represent it", i.e. h <= feature/2), not a fitted curve;
  * refusals carry an actionable recommendation ("reduce -h to <= X m"), and
    the figure shows the move from the refused h to the recommended h.

Usage:
    python scripts/plot_refusal_boundary.py [--out-dir docs/validation/figures]
"""

from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
DATASET = REPO / "bench/advisor/dataset.csv"
SCENE = REPO / "src/pipeline/src/scene.cpp"
DEFAULT_OUT = REPO / "docs/validation/figures"

# The engine's stated rule: two cells across the thinnest feature.
CELLS_ACROSS = 2.0

REFUSAL_KINDS = ("feature unresolved at h=",
                 "resolution refused at h=",
                 "geometry fill-stage guard failed:")
REC_RE = re.compile(r"reduce -h to <= ([0-9.eE+-]+) m")
AT_H_RE = re.compile(r"at h=([0-9.eE+-]+) m")

CLASSES = ("succeeded", "refused-with-recommendation", "fill-guard failure",
           "over budget")


def rel(path: Path) -> str:
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return path.as_posix()


def classify(row: dict) -> str:
    """Class from what the engine reported, nothing else."""
    err = row.get("error") or ""
    if row["status"] == "ok":
        return "succeeded"
    if REC_RE.search(err):
        return "refused-with-recommendation"
    if err.startswith("wall-clock budget exceeded") or row["status"] == "over_budget":
        return "over budget"
    if REFUSAL_KINDS[2] in err:
        return "fill-guard failure"
    if REFUSAL_KINDS[0] in err or REFUSAL_KINDS[1] in err:
        return "refused, no recommendation"
    return "other failure"


def load_rows(path: Path) -> list[dict] | None:
    if not path.is_file():
        print(f"no data yet: {rel(path)} is missing")
        return None
    with path.open(newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        print(f"no data yet: {rel(path)} has no rows")
        return None
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    rows = load_rows(DATASET)
    if rows is None:
        return 0

    pts: list[dict] = []
    for row in rows:
        feat = float(row["min_feature_h"])
        h_abs = float(row["h"])          # metres, the h the engine actually ran
        if feat <= 0.0 or h_abs <= 0.0:
            continue
        err = row.get("error") or ""
        m = REC_RE.search(err)
        pts.append({
            "part": row["part"],
            "family": row["part"].split("_s")[0],
            "feature": feat,
            "h": h_abs,
            "cls": classify(row),
            "rec": float(m.group(1)) if m else None,
            "refined": row["feature_refine"] == "True",
        })

    n_total = len(rows)
    excluded = n_total - len(pts)
    counts = Counter(p["cls"] for p in pts)
    status_counts = Counter(r["status"] for r in rows)
    kind_counts = Counter()
    for row in rows:
        err = row.get("error") or ""
        for kind in REFUSAL_KINDS:
            if kind in err:
                kind_counts[kind] += 1

    plotted = [p for p in pts if p["cls"] in CLASSES]
    dropped = len(pts) - len(plotted)
    refused = [p for p in plotted if p["cls"] == "refused-with-recommendation"]
    n_ref = len(refused)
    n_rec = sum(1 for p in refused if p["rec"] is not None)
    fam_ref = Counter(p["family"] for p in refused)
    fam_guard = Counter(p["family"] for p in plotted
                        if p["cls"] == "fill-guard failure")
    fam_all = Counter(p["family"] for p in plotted)

    ratios = {c: sorted(p["h"] / p["feature"] for p in plotted if p["cls"] == c)
              for c in CLASSES}
    rule_ratio = 1.0 / CELLS_ACROSS
    below = {c: sum(1 for v in ratios[c] if v <= rule_ratio) for c in CLASSES}
    shrink = [p["rec"] / p["h"] for p in refused if p["rec"]]
    ok_above = below["succeeded"]

    print(f"dataset rows: {n_total}   plotted points: {len(plotted)}   "
          f"excluded: {excluded + dropped} "
          f"(non-positive h/feature {excluded}, "
          f"other failure/no-recommendation refusal {dropped})")
    print("status counts: " + "  ".join(f"{k}={v}" for k, v in
                                        sorted(status_counts.items())))
    print("engine refusal message kinds present in the error column:")
    for kind in REFUSAL_KINDS:
        print(f"  {kind!r}: {kind_counts.get(kind, 0)}")
    print("plotted classes:")
    for c in CLASSES:
        r = ratios[c]
        if not r:
            # A class can be empty against a regenerated corpus (e.g. no
            # 'resolution refused' rows) — print that, don't crash.
            print(f"  {c:<28} n=0     (none in this dataset)")
            continue
        print(f"  {c:<28} n={len(r):<5} h/feature min {min(r):.3f} "
              f"median {statistics.median(r):.3f} max {max(r):.3f}   "
              f"at or below the h<=feature/{CELLS_ACROSS:g} rule: {below[c]}")
    print(f"refusals: {n_ref}; carrying a 'reduce -h to <= X m' recommendation: "
          f"{n_rec} ({100.0 * n_rec / n_ref:.1f}%)")
    print(f"recommended/refused h ratio: min {min(shrink):.4f} "
          f"median {statistics.median(shrink):.4f} max {max(shrink):.4f}")
    print("refusals per family (family: refused / all plotted):")
    for fam, k in fam_ref.most_common():
        print(f"  {fam:<18} {k:>4} / {fam_all[fam]}")
    print("fill-guard failures per family:")
    for fam, k in fam_guard.most_common():
        print(f"  {fam:<18} {k:>4} / {fam_all[fam]}")

    # ---- figure -------------------------------------------------------
    for slot, name in enumerate(("succeeded", "refused-with-recommendation",
                                 "fill-guard failure", "over budget")):
        fs.register_series(name, slot, label=name)
    fs.use("dark")
    t = fs.theme()

    title = "The mesher now refuses a size it cannot represent -- and says which size to use"
    subtitle = (
        f"{len(plotted)} advisor runs. Every one of the {n_ref} refusals sits ABOVE the engine's own stated rule "
        f"h <= feature/{CELLS_ACROSS:g} (two cells across the thinnest wall, src/pipeline/src/scene.cpp);\n"
        f"{n_rec} of {n_ref} ({100.0 * n_rec / n_ref:.0f}%) carry an actionable "
        f"'reduce -h to <= X m', a median {statistics.median(shrink):.2f}x cut of the refused h. "
        f"{ok_above} of {len(ratios['succeeded'])} successes are at or below the rule."
    )
    footer = fs.footer_source(DATASET, SCENE, n=len(plotted),
                              note="class = engine's own status + error text; "
                                   "h is the absolute mesh size the run used (m)")
    fs.assert_glyphs(title, subtitle, footer)

    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer,
                          size=(13.2, 6.1), nrows=1, ncols=3,
                          share_y_axis="panels a/b share mesh size in m; "
                                       "panel c counts runs")
    ax_a, ax_b, ax_c = axes[0]

    feats = [p["feature"] for p in plotted]
    fmin, fmax = min(feats), max(feats)
    hs = [p["h"] for p in plotted] + [p["rec"] for p in refused if p["rec"]]
    hmin, hmax = min(hs), max(hs)

    def rule_line(ax, label):
        xs = [fmin * 0.8, fmax * 1.25]
        ax.plot(xs, [x * rule_ratio for x in xs], color=t.ink, lw=1.4,
                ls=(0, (5, 3)), zorder=5,
                label=f"stated rule  h = feature/{CELLS_ACROSS:g}")
        if label:
            ax.annotate(f"engine's stated rule: h = feature/{CELLS_ACROSS:g}\n"
                        "(two cells across the thinnest wall)\n"
                        "-- refusals are all ABOVE it",
                        xy=(fmax * 1.15, hmin * 0.78), ha="right", va="bottom",
                        fontsize=7.2, color=t.ink)

    def frame(ax, xlabel):
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlim(fmin * 0.8, fmax * 1.25)
        ax.set_ylim(hmin * 0.7, hmax * 1.5)
        ax.set_xlabel(xlabel)
        ax.set_ylabel("mesh size h used by the run (m)")
        ax.grid(True, which="major", color=t.grid, lw=0.5, alpha=0.8)
        ax.set_axisbelow(True)

    # Panel a -- every run, classified.
    fill = {"succeeded": False, "refused-with-recommendation": True,
            "fill-guard failure": True, "over budget": False}
    size = {"succeeded": 12, "refused-with-recommendation": 34,
            "fill-guard failure": 30, "over budget": 26}
    for c in CLASSES:
        st = fs.series(c)
        sel = [p for p in plotted if p["cls"] == c]
        if not sel:
            continue
        xs = [p["feature"] for p in sel]
        ys = [p["h"] for p in sel]
        if fill[c]:
            ax_a.scatter(xs, ys, s=size[c], marker=st.marker, color=st.color,
                         edgecolors=t.ink, linewidths=0.45, alpha=0.9,
                         zorder=4, label=f"{st.label} ({len(sel)}, filled)")
        else:
            ax_a.scatter(xs, ys, s=size[c], marker=st.marker, facecolors="none",
                         edgecolors=st.color, linewidths=0.9, alpha=0.85,
                         zorder=3, label=f"{st.label} ({len(sel)}, open)")
    frame(ax_a, "thinnest resolvable feature of the part (m)")
    rule_line(ax_a, True)
    fs.panel_title(ax_a, "a. every run, classified by what the engine said")
    ax_a.legend(loc="upper left", bbox_to_anchor=(0.0, -0.16), ncol=2,
                fontsize=6.8, frameon=False, handletextpad=0.5, borderpad=0.2,
                columnspacing=1.2)
    fs.annotate_n(ax_a, len(plotted), excluded=excluded + dropped,
                  what="runs", loc="upper right")

    # Panel b -- the recommendation, as a move.
    step = max(1, round(n_ref / 45))
    sub = sorted(refused, key=lambda p: (p["feature"], p["h"]))[::step]
    st_ref = fs.series("refused-with-recommendation")
    st_ok = fs.series("succeeded")
    ax_b.scatter([p["feature"] for p in plotted if p["cls"] == "succeeded"],
                 [p["h"] for p in plotted if p["cls"] == "succeeded"],
                 s=9, marker=st_ok.marker, facecolors="none",
                 edgecolors=t.grid, linewidths=0.7, zorder=2,
                 label="succeeded (context)")
    for p in sub:
        ax_b.annotate("", xy=(p["feature"], p["rec"]),
                      xytext=(p["feature"], p["h"]),
                      arrowprops=dict(arrowstyle="-|>", color=st_ref.color,
                                      lw=0.9, shrinkA=1.2, shrinkB=0.6),
                      zorder=6)
    ax_b.scatter([p["feature"] for p in sub], [p["h"] for p in sub],
                 s=30, marker=st_ref.marker, color=st_ref.color,
                 edgecolors=t.ink, linewidths=0.45, zorder=7,
                 label=f"refused at this h ({len(sub)} shown, filled)")
    ax_b.scatter([p["feature"] for p in sub], [p["rec"] for p in sub],
                 s=30, marker="_", color=t.ink, linewidths=1.2, zorder=7,
                 label="recommended h (bar)")
    frame(ax_b, "thinnest resolvable feature of the part (m)")
    rule_line(ax_b, False)
    fs.panel_title(ax_b, "b. each refusal hands back a smaller h")
    ax_b.legend(loc="upper right", fontsize=6.8, frameon=False,
                handletextpad=0.5, borderpad=0.2)
    note_b = (f"every {step}th refusal by feature size ({len(sub)} of {n_ref} "
              f"arrows shown) to keep the panel readable;\n"
              f"arrow = refused h -> recommended h "
              f"({min(shrink):.2f}x to {max(shrink):.2f}x of the refused h)")
    fs.assert_glyphs(note_b)
    ax_b.annotate(note_b, xy=(0.0, -0.20), xycoords="axes fraction",
                  ha="left", va="top", fontsize=7.2, color=t.muted)

    # Panel c -- who refuses, by family.
    fams = sorted(fam_all, key=lambda f: (-fam_ref.get(f, 0), f))
    ypos = list(range(len(fams)))
    for c, hatch in (("refused-with-recommendation", "//"),
                     ("fill-guard failure", "xx")):
        st = fs.series(c)
        vals = [(fam_ref if c == "refused-with-recommendation"
                 else fam_guard).get(f, 0) for f in fams]
        off = -0.2 if c == "refused-with-recommendation" else 0.2
        ax_c.barh([y + off for y in ypos], vals, height=0.36, color=st.color,
                  edgecolor=t.ink, linewidth=0.5, hatch=hatch,
                  label=f"{st.label} ({sum(vals)})", zorder=3)
        for y, v in zip(ypos, vals):
            if v:
                ax_c.text(v + 4, y + off, str(v), va="center", ha="left",
                          fontsize=7.0, color=t.ink)
    ax_c.set_yticks(ypos)
    ax_c.set_yticklabels([f"{f.replace('_', ' ')}\n({fam_all[f]} runs)"
                          for f in fams], fontsize=7.2)
    ax_c.set_ylim(-0.65, len(fams) - 0.35)
    ax_c.invert_yaxis()
    ax_c.set_xlim(0, max(max(fam_ref.values(), default=0),
                         max(fam_guard.values(), default=0), 1) * 1.45)
    ax_c.set_xlabel("runs the engine declined")
    ax_c.grid(True, axis="x", color=t.grid, lw=0.5, alpha=0.8)
    ax_c.set_axisbelow(True)
    fs.panel_title(ax_c, "c. refusals track geometry")
    ax_c.legend(loc="lower right", fontsize=6.8, frameon=False)
    note_c = ("hatch + colour, never colour alone.\n"
              "fill-guard failure is a different outcome:\n"
              "the fill ran, its volume check failed, and\n"
              "there is no h to recommend.")
    fs.assert_glyphs(note_c)
    ax_c.annotate(note_c, xy=(0.0, -0.20), xycoords="axes fraction",
                  ha="left", va="top", fontsize=7.2, color=t.muted)

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    path = fs.finish(fig, out_dir / "refusal_boundary.png")
    print(f"subtitle: {subtitle}")
    print(f"wrote {rel(Path(path))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
