#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export static advisor figures to docs/advisor/figures/.

Committed README assets, regenerated from the real training artifacts:

  training_curves.png  per-epoch train and validation MAE of log10(rel_err),
                       first vs latest training run overlaid
                       (runs/<NNN>/metrics.json)
  activation_map.png   per-layer activation heatmap of the latest run for
                       its canonical input case (runs/<NNN>/activations.json)

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

import numpy as np  # noqa: E402
from matplotlib.colors import TwoSlopeNorm  # noqa: E402


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
            reason = (f"training restarted from scratch at run "
                      f"{index + 1} (warm_start={warm!r})")
        elif (train_rows, val_rows) != (previous[2], previous[3]):
            start = index
            reason = (f"the corpus changed at run {index + 1}: "
                      f"{previous[2]}/{previous[3]} -> {train_rows}/{val_rows} "
                      f"train/val rows")
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
    numbers.
    """
    _, first_val = epoch_series(first, "val")
    _, latest_val = epoch_series(latest, "val")
    _, latest_train = epoch_series(latest, "train")
    if not first_val or not latest_val:
        return "not enough validation epochs to compare the two runs"

    best_first = min(first_val)
    best_latest = min(latest_val)
    # Epoch-to-epoch swing on the same run is the scale below which a
    # difference between two runs says nothing. Without it a 1% move reads as
    # a regression in the same voice as a 55% one.
    swings = [abs(b - a) for a, b in zip(latest_val, latest_val[1:])]
    noise = (sum(swings) / len(swings)) if swings else 0.0
    delta = best_latest - best_first
    if abs(delta) <= noise:
        verdict = (f"latest run is level with the first on val: best MAE of "
                   f"log10(rel_err) {best_latest:.3f} vs {best_first:.3f}, a "
                   f"{abs(delta):.3f} difference inside the {noise:.3f} "
                   f"epoch-to-epoch swing of the run itself")
    elif delta < 0:
        verdict = (f"latest run improved on val: best MAE of log10(rel_err) "
                   f"{best_latest:.3f} vs {best_first:.3f} "
                   f"(−{_pct(best_latest, best_first)})")
    else:
        verdict = (f"latest run REGRESSED on val: best MAE of log10(rel_err) "
                   f"{best_latest:.3f} vs {best_first:.3f} "
                   f"(+{_pct(best_latest, best_first)})")

    if latest_train:
        gap = latest_val[-1] - latest_train[-1]
        share = (gap / latest_val[-1] * 100.0) if latest_val[-1] else 0.0
        if gap > 0:
            verdict += (f"; final train/val {latest_train[-1]:.3f}/"
                        f"{latest_val[-1]:.3f} — {share:.0f}% gap, overfits")
        else:
            verdict += (f"; final train/val {latest_train[-1]:.3f}/"
                        f"{latest_val[-1]:.3f} — no train/val gap")
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
        subtitle += (f"\nRuns before {first_id} are excluded: {reason}. They "
                     f"were fitted against different labels, so overlaying "
                     f"them would compare two questions, not two runs.")
    title = (f"Advisor training curves — first run ({first_id}) vs latest "
             f"run ({latest_id}) of the current campaign")
    footer = fs.footer_source(campaign[0] / "metrics.json",
                              campaign[-1] / "metrics.json",
                              n=len(campaign),
                              note=f"runs in this campaign, of {len(dirs)} on disk")
    fs.assert_glyphs(title, subtitle, footer, first_id, latest_id)
    print(f"training_curves campaign: runs {first_id}-{latest_id} "
          f"({len(campaign)} of {len(dirs)}) — {reason}")
    print(f"training_curves finding: {subtitle}")

    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer, size="full")
    ax = axes[0][0]
    n_series = 0
    # End-of-run value labels collide when two runs finish at nearly the same
    # MAE, which is exactly what happens when a campaign converges. Collect
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
                                      label=f"run {run} (val)"))
            end_labels.append((float(xs[-1]), float(ys[-1]),
                               f"{ys[-1]:.3f}", st.color))
        xs_t, ys_t = epoch_series(metrics, "train")
        if xs_t:
            ax.plot(xs_t, ys_t, **st.line(linestyle="--", linewidth=1.4,
                                          markersize=4, alpha=0.85,
                                          markerfacecolor="none",
                                          markevery=max(1, len(xs_t) // 12),
                                          label=f"run {run} (train)"))
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
    ax.set_xlabel("epoch")
    ax.set_ylabel("MAE of log10(rel_err)  (unitless)")
    ax.legend(frameon=False, fontsize=fs.FONT_PT["legend"], ncol=2)
    fs.annotate_n(ax, n_series, what="runs (solid = val, dashed = train)")
    path = out_dir / "training_curves.png"
    fs.finish(fig, path)
    return True


def _tick_step(n: int) -> int:
    for step in (1, 2, 5, 10, 20, 25, 50, 100):
        if n / step <= 14:
            return step
    return max(1, n // 14)


def activation_map(runs_dir: Path, out_dir: Path) -> bool:
    dirs = [d for d in run_dirs(runs_dir) if (d / "activations.json").is_file()]
    if not dirs:
        print(f"no data yet — expected {runs_dir}/<NNN>/activations.json; "
              "skipping activation_map.png")
        return False
    act = load_json(dirs[-1] / "activations.json")
    assert act is not None
    layers = act.get("layers", [])
    if not layers:
        print(f"{dirs[-1] / 'activations.json'} has no layers; "
              "skipping activation_map.png")
        return False

    case = act.get("input_case") or {}
    run_id = str(act.get("run"))
    part = str(case.get("part", "?"))
    cfg_id = str(case.get("cfg_id", "?"))
    amax = max((abs(float(v)) for layer in layers
                for v in layer.get("values", [])), default=0.0)
    if amax <= 0.0:
        amax = 1.0

    title = f"Advisor activations — run {run_id}"
    if case:
        title += f" on {part} · {cfg_id}"
    subtitle = ("all units, no subsampling; each row scaled to its own "
                "max |a|, given in the row label")
    # The input row is WIDER than the documented input contract and saying
    # "unit index (0-48)" beside a 43-column contract invites the reader to
    # think one of the two is wrong. Name the composition from the data:
    # the two categorical columns are replaced by their embeddings.
    encoder = next((l for l in layers if str(l.get("name")) == "input"), None)
    if encoder:
        labels = [str(x) for x in (encoder.get("labels") or [])]
        embedded = [x for x in labels if "_emb_" in x]
        if embedded:
            groups = sorted({x.split("_emb_")[0] for x in embedded})
            subtitle += (f"\nthe encoder row is {len(labels)} units, not the "
                         f"{len(labels) - len(embedded) + len(groups)}-column "
                         f"input contract: {', '.join(groups)} enter as "
                         f"{len(embedded)} embedding units rather than as "
                         f"{len(groups)} index columns")
    footer = fs.footer_source(
        dirs[-1] / "activations.json",
        n=sum(len(l.get("values", [])) for l in layers),
        note="per-row scaling: trunk and head magnitudes differ ~10×")
    fs.assert_glyphs(title, subtitle, footer, run_id, part, cfg_id)

    n_layers = len(layers)
    # The bottom row draws its unit names rotated, and those names are long
    # (`policy_mesher_logit_graded_tet`). A fixed spacer let them run into the
    # colourbar, so the gutter is sized from the longest label actually drawn.
    longest = max((len(str(x)) for layer in layers
                   for x in (layer.get("labels") or [])
                   if len(layer.get("labels") or []) <= 24), default=0)
    spacer_ratio = max(0.6, 0.075 * longest)
    fig, axes = fs.figure(title, subtitle=subtitle, footer=footer,
                          size="tall", nrows=n_layers + 2, ncols=1,
                          gridspec_kw={"height_ratios":
                                       [1.0] * n_layers + [spacer_ratio, 0.16],
                                       "hspace": 1.0})
    spacer = axes[-2][0]   # keeps the long head labels clear of the colourbar
    spacer.set_axis_off()
    cax = axes[-1][0]
    norm = TwoSlopeNorm(vmin=-1.0, vcenter=0.0, vmax=1.0)
    cmap = fs.field_cmap("signed")
    im = None
    for ax, layer in zip([a[0] for a in axes[:-2]], layers):
        values = [float(v) for v in layer.get("values", [])]
        labels = [str(x) for x in (layer.get("labels") or [])]
        name = str(layer.get("name"))
        fs.assert_glyphs(name, *labels)
        row_max = max((abs(v) for v in values), default=0.0) or 1.0
        row = np.asarray(values, dtype=float).reshape(1, -1) / row_max
        im = ax.imshow(row, cmap=cmap, norm=norm, aspect="auto",
                       interpolation="nearest",
                       extent=(-0.5, len(values) - 0.5, -0.5, 0.5))
        ax.set_yticks([])
        ax.set_ylabel(f"{name}\n±{row_max:.2f}", fontsize=fs.FONT_PT["label"],
                      rotation=0, ha="right", va="center", labelpad=8)
        for side in ax.spines.values():
            side.set_visible(False)
        if labels and len(labels) == len(values) and len(values) <= 24:
            ax.set_xticks(range(len(values)))
            ax.set_xticklabels(labels, rotation=90, ha="center",
                               va="top",
                               fontsize=fs.FONT_PT["tick"] - 1.5)
        else:
            step = _tick_step(len(values))
            ticks = list(range(0, len(values), step))
            ax.set_xticks(ticks)
            ax.set_xticklabels([str(t) for t in ticks],
                               fontsize=fs.FONT_PT["tick"] - 1)
            ax.set_xlabel(f"unit index (0–{len(values) - 1})",
                          fontsize=fs.FONT_PT["tick"])
        print(f"  layer {name}: {len(values)} units, "
              f"row scale ±{row_max:.3f}")

    print(f"  max |a| over all layers = {amax:.2f}")
    if im is not None:
        fs.colorbar(fig, im, label="activation, normalised", unit="row max",
                    cax=cax, orientation="horizontal")
    else:
        cax.set_visible(False)
    path = out_dir / "activation_map.png"
    fs.finish(fig, path)
    return True


def main() -> int:
    args = parse_args()
    fs.use("dark")
    runs_dir = args.runs_dir or args.advisor_dir / "runs"
    args.out_dir.mkdir(parents=True, exist_ok=True)
    wrote = [training_curves(runs_dir, args.out_dir),
             activation_map(runs_dir, args.out_dir)]
    if not any(wrote):
        print("no figures written — run scripts/advisor/train.py first "
              "(artifacts appear under bench/advisor/runs/)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
