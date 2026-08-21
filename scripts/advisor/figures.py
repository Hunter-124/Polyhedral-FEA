#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export static advisor figures to docs/advisor/figures/.

Committed README assets, regenerated from the real training artifacts:

  training_curves.png  how far off the error prediction lands after each pass
                       through the training data — on parts the net trained on
                       and on parts it has never seen — first vs latest run
                       (runs/<NNN>/metrics.json)

What the network does with a case is no longer a still: the activation heatmap
this script used to write was replaced by `docs/assets/cinema/`, a recording of
the deployed graph's own trunk taps firing over the real candidate enumeration
beside the mesher building the mesh it chose (`scripts/render_cinema.py`,
ADR-0042). A per-row-normalised heatmap of one canonical input could show which
units were warm; it could not show which candidate a unit was warm *for*, or
that the action it argued for is the one the mesher then executed.

Missing inputs skip the affected figure with a printed "no data yet" note —
the script still exits 0 so it is safe to run before any training exists.

Run from anywhere:

    python scripts/advisor/figures.py
    python scripts/advisor/figures.py --advisor-dir bench/advisor \
        --out-dir docs/advisor/figures
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import figstyle as fs  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
FIGURES_DIR = ROOT / "docs" / "advisor" / "figures"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--advisor-dir", type=Path, default=ADVISOR_DIR,
                        help="directory holding runs/ (default: bench/advisor)")
    parser.add_argument("--runs-dir", type=Path, default=None,
                        help="override the runs directory (default: "
                             "<advisor-dir>/runs)")
    parser.add_argument("--out-dir", type=Path, default=FIGURES_DIR,
                        help="figure output directory (default: "
                             "docs/advisor/figures)")
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def run_dirs(runs_dir: Path) -> list[Path]:
    if not runs_dir.is_dir():
        return []
    return sorted(child for child in runs_dir.iterdir() if child.is_dir())


def regime_start(dirs: list[Path]) -> tuple[int, str]:
    """Index of the first run of the CURRENT training campaign, and why.

    Overlaying run 1 on run 60 is only a comparison if both were trained on
    the same labels. They were not: runs 1-30 here were fitted against the
    retired self-generated truth on a 3,456-row corpus, runs 31-60 against
    the independent Gmsh -> CalculiX references on the rebuilt 2,412-row one.
    Reporting "the latest run REGRESSED by 55%" across that boundary compares
    two different questions and calls the difference a result.

    A campaign begins where training restarted from scratch (``warm_start``
    is not "warm") or where the split size changed, so the boundary is read
    from the records instead of being pinned to a run number.
    """
    sizes = []
    for index, directory in enumerate(dirs):
        metrics = load_json(directory / "metrics.json") or {}
        sizes.append((index, metrics.get("warm_start"),
                      metrics.get("train_rows"), metrics.get("val_rows")))
    start, reason = 0, "single training campaign"
    for index, warm, train_rows, val_rows in sizes:
        if index == 0:
            continue
        previous = sizes[index - 1]
        if warm is not None and warm != "warm":
            start = index
            reason = f"training restarted from scratch at run {index + 1}"
        elif (train_rows, val_rows) != (previous[2], previous[3]):
            start = index
            changes = [f"{previous[2]:,} to {train_rows:,} training examples"]
            if val_rows != previous[3]:
                changes.append(f"{previous[3]:,} to {val_rows:,} held back")
            reason = (f"the data changed at run {index + 1} "
                      f"({' and '.join(changes)})")
    return start, reason


def epoch_series(metrics: dict[str, Any], split: str,
                 key: str = "rel_err_mae") -> tuple[list[int], list[float]]:
    xs: list[int] = []
    ys: list[float] = []
    for i, entry in enumerate(metrics.get("epochs", [])):
        value = (entry.get(split) or {}).get(key)
        if isinstance(value, (int, float)):
            xs.append(int(entry.get("epoch", i + 1)))
            ys.append(float(value))
    return xs, ys


def _pct(new: float, old: float) -> str:
    if old == 0.0:
        return "n/a"
    return f"{abs(new - old) / old * 100.0:.0f}%"


def _finding(first: dict[str, Any], latest: dict[str, Any]) -> str:
    """One honest line about the latest run, computed from the metrics.

    Never hardcoded: a rerun on corrected data flips the wording with the
    numbers. Written for someone who has not read the training code, so the
    two splits are named for what they hold — parts the net trained on, and
    parts it has never seen — rather than as "train" and "val".
    """
    first_id = str(first.get("run"))
    latest_id = str(latest.get("run"))
    _, first_val = epoch_series(first, "val")
    _, latest_val = epoch_series(latest, "val")
    _, latest_train = epoch_series(latest, "train")
    if not first_val or not latest_val:
        return "Not enough scores on unseen parts to compare the two runs."

    best_first = min(first_val)
    best_latest = min(latest_val)
    # Pass-to-pass swing on the same run is the scale below which a difference
    # between two runs says nothing. Without it a 1% move reads as a
    # regression in the same voice as a 55% one.
    swings = [abs(b - a) for a, b in zip(latest_val, latest_val[1:])]
    noise = (sum(swings) / len(swings)) if swings else 0.0
    delta = best_latest - best_first
    scores = (f"best miss {best_latest:.3f} vs {best_first:.3f}, which is "
              f"{fs.times_off(best_latest)} vs {fs.times_off(best_first)} off")
    if abs(delta) <= noise:
        verdict = (f"Run {latest_id} ties run {first_id} on parts it never "
                   f"trained on: {scores}. The {abs(delta):.3f} between them "
                   f"is smaller than the {noise:.3f} the score already drifts "
                   f"from one pass to the next.")
    elif delta < 0:
        verdict = (f"Run {latest_id} beats run {first_id} on parts it never "
                   f"trained on: {scores} — {_pct(best_latest, best_first)} "
                   f"better.")
    else:
        verdict = (f"Run {latest_id} LOST ground against run {first_id} on "
                   f"parts it never trained on: {scores} — "
                   f"{_pct(best_latest, best_first)} worse.")

    if latest_train:
        gap = latest_val[-1] - latest_train[-1]
        share = (gap / latest_val[-1] * 100.0) if latest_val[-1] else 0.0
        if gap > 0:
            verdict += (f" It ends at {latest_train[-1]:.3f} on the parts it "
                        f"trained on against {latest_val[-1]:.3f} on new ones, "
                        f"a {share:.0f}% gap: much of what it learned is those "
                        f"specific parts rather than the pattern behind them.")
        else:
            verdict += (f" It ends at {latest_train[-1]:.3f} on the parts it "
                        f"trained on and {latest_val[-1]:.3f} on new ones, so "
                        f"it is not just memorizing them.")
    return verdict


def training_curves(runs_dir: Path, out_dir: Path) -> bool:
    dirs = [d for d in run_dirs(runs_dir) if (d / "metrics.json").is_file()]
    if not dirs:
        print(f"no data yet — expected {runs_dir}/<NNN>/metrics.json; "
              "skipping training_curves.png")
        return False
    start, reason = regime_start(dirs)
    campaign = dirs[start:]
    first = load_json(campaign[0] / "metrics.json")
    latest = load_json(campaign[-1] / "metrics.json")
    assert first is not None and latest is not None  # is_file checked above

    first_id = str(first.get("run"))
    latest_id = str(latest.get("run"))
    subtitle = _finding(first, latest)
    if start:
        subtitle += (f"\nRuns before {first_id} are left out because {reason}. "
                     f"They were trained against different reference answers, "
                     f"so putting them on the same axes would compare two "
                     f"different questions rather than two runs.")
    title = (f"Advisor training curves — first run ({first_id}) vs latest "
             f"run ({latest_id}) of the current campaign")
    footer = fs.footer_source(campaign[0] / "metrics.json",
                              campaign[-1] / "metrics.json",
                              n=len(campaign),
                              note=f"runs in this campaign, of {len(dirs)} on disk")
    xlabel = "pass through the training data (epoch)"
    ylabel = f"typical miss predicting a mesh's error\n({fs.DECADES_NOTE})"
    legend_note = "runs (solid = unseen parts, dashed = training parts)"
    fs.assert_glyphs(title, subtitle, footer, first_id, latest_id,
                     xlabel, ylabel, legend_note)
    print(f"training_curves campaign: runs {first_id}-{latest_id} "
          f"({len(campaign)} of {len(dirs)}) — {reason}")
    print(f"training_curves finding: {subtitle}")

    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer, size="full")
    ax = axes[0][0]
    n_series = 0
    # End-of-run value labels collide when two runs finish at nearly the same
    # score, which is exactly what happens when a campaign converges. Collect
    # them and offset the second one instead of overprinting.
    end_labels: list[tuple[float, float, str, str]] = []
    for metrics, name in ((first, "before"), (latest, "after")):
        run = str(metrics.get("run"))
        st = fs.series(name, label=f"run {run}")
        xs, ys = epoch_series(metrics, "val")
        if xs:
            n_series += 1
            ax.plot(xs, ys, **st.line(linestyle="-", linewidth=2.0,
                                      markersize=4, markevery=max(1, len(xs) // 12),
                                      label=f"run {run} (unseen parts)"))
            end_labels.append((float(xs[-1]), float(ys[-1]),
                               f"{ys[-1]:.3f}", st.color))
        xs_t, ys_t = epoch_series(metrics, "train")
        if xs_t:
            ax.plot(xs_t, ys_t, **st.line(linestyle="--", linewidth=1.4,
                                          markersize=4, alpha=0.85,
                                          markerfacecolor="none",
                                          markevery=max(1, len(xs_t) // 12),
                                          label=f"run {run} (training parts)"))
        print(f"  run {run}: {len(xs)} val epochs, {len(xs_t)} train epochs, "
              f"final val {ys[-1]:.4f}" if xs else f"  run {run}: no val epochs")

    spread = max((y for _, y, _, _ in end_labels), default=0.0) - \
        min((y for _, y, _, _ in end_labels), default=0.0)
    for index, (x, y, text, colour) in enumerate(sorted(end_labels,
                                                        key=lambda item: -item[1])):
        crowded = len(end_labels) > 1 and spread < 0.06
        dy = (10 if index == 0 else -12) if crowded else -2
        ax.annotate(text, (x, y), textcoords="offset points",
                    xytext=(8, dy), fontsize=fs.FONT_PT["annot"], color=colour)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.legend(frameon=False, fontsize=fs.FONT_PT["legend"], ncol=2)
    fs.annotate_n(ax, n_series, what=legend_note)
    path = out_dir / "training_curves.png"
    fs.finish(fig, path)
    return True


def main() -> int:
    args = parse_args()
    fs.use("dark")
    runs_dir = args.runs_dir or args.advisor_dir / "runs"
    args.out_dir.mkdir(parents=True, exist_ok=True)
    if not training_curves(runs_dir, args.out_dir):
        print("no figures written — run scripts/advisor/train.py first "
              "(artifacts appear under bench/advisor/runs/)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
