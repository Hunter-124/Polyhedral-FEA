#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Export static advisor figures to docs/advisor/figures/.

Committed README assets, regenerated from the real training artifacts:

  training_curves.png  per-epoch validation rel_err MAE, first vs latest
                       training run overlaid (runs/<NNN>/metrics.json)
  activation_map.png   per-layer activation profile of the latest run for
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

ROOT = Path(__file__).resolve().parents[2]
ADVISOR_DIR = ROOT / "bench" / "advisor"
FIGURES_DIR = ROOT / "docs" / "advisor" / "figures"

DPI = 110  # committed PNGs stay small; both figures land well under 300 KB
MAX_INPUT_BARS = 32

BLUE = "#2166ac"
ORANGE = "#b35806"
GREY = "#4d4d4d"
LIGHT_BLUE = "#92c5de"
LIGHT_ORANGE = "#f4a582"

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    raise SystemExit(
        "matplotlib is required for figures.py — install it with:\n"
        "  pip install matplotlib"
    )


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


def style_axes(ax: Any) -> None:
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.grid(axis="y", color="#e4e1da", linewidth=0.8)
    ax.set_axisbelow(True)


def epoch_series(metrics: dict[str, Any], key: str) -> tuple[list[int], list[float]]:
    xs: list[int] = []
    ys: list[float] = []
    for entry in metrics.get("epochs", []):
        value = (entry.get("val") or {}).get(key)
        if isinstance(value, (int, float)):
            xs.append(int(entry.get("epoch", len(xs) + 1)))
            ys.append(float(value))
    return xs, ys


def training_curves(runs_dir: Path, out_dir: Path) -> bool:
    dirs = [d for d in run_dirs(runs_dir) if (d / "metrics.json").is_file()]
    if not dirs:
        print(f"no data yet — expected {runs_dir}/<NNN>/metrics.json; "
              "skipping training_curves.png")
        return False
    first = load_json(dirs[0] / "metrics.json")
    latest = load_json(dirs[-1] / "metrics.json")
    assert first is not None and latest is not None  # is_file checked above

    fig, ax = plt.subplots(figsize=(8.4, 4.6))
    for metrics, color, light in ((first, LIGHT_BLUE, BLUE),
                                  (latest, ORANGE, LIGHT_ORANGE)):
        run = metrics.get("run")
        xs, ys = epoch_series(metrics, "rel_err_mae")
        if not xs:
            continue
        ax.plot(xs, ys, color=color, linewidth=2.2,
                label=f"run {run} (val)")
        ax.annotate(f"{ys[-1]:.3f}", (xs[-1], ys[-1]),
                    textcoords="offset points", xytext=(8, -2),
                    fontsize=9, color=color)
        xs_t = [int(e.get("epoch", i + 1)) for i, e in
                enumerate(metrics.get("epochs", []))
                if isinstance((e.get("train") or {}).get("rel_err_mae"),
                              (int, float))]
        ys_t = [float((e.get("train") or {})["rel_err_mae"]) for e in
                metrics.get("epochs", [])
                if isinstance((e.get("train") or {}).get("rel_err_mae"),
                              (int, float))]
        if xs_t:
            ax.plot(xs_t, ys_t, color=light, linewidth=1.2, linestyle="--",
                    alpha=0.75, label=f"run {run} (train)")
    ax.set_xlabel("epoch")
    ax.set_ylabel("rel_err MAE (log10 space)")
    ax.set_title(f"Training curves — first run ({first.get('run')}) vs latest "
                 f"run ({latest.get('run')}) overlaid")
    ax.legend(frameon=False, fontsize=9)
    style_axes(ax)
    fig.tight_layout()
    path = out_dir / "training_curves.png"
    fig.savefig(path, dpi=DPI)
    plt.close(fig)
    print(f"wrote {path} ({path.stat().st_size / 1024:.0f} KB)")
    return True


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
    fig, axes = plt.subplots(len(layers), 1, figsize=(8.4, 2.1 * len(layers)),
                             sharey=False)
    if len(layers) == 1:
        axes = [axes]
    amax = max((abs(float(v)) for layer in layers
                for v in layer.get("values", [])), default=0.0)
    for ax, layer in zip(axes, layers):
        values = [float(v) for v in layer.get("values", [])]
        labels = layer.get("labels") or []
        indices = list(range(len(values)))
        note = ""
        if layer.get("name") == "input" and len(values) > MAX_INPUT_BARS:
            step = len(values) / MAX_INPUT_BARS
            indices = sorted({int(i * step) for i in range(MAX_INPUT_BARS)})
            note = f" (showing {len(indices)} of {len(values)}, evenly spaced)"
        shown = [values[i] for i in indices]
        colors = [BLUE if v >= 0 else ORANGE for v in shown]
        ax.bar(range(len(shown)), shown, color=colors, width=0.82)
        ax.axhline(0, color="#9a9ea6", linewidth=0.8)
        name = layer.get("name")
        ax.set_ylabel(str(name), fontsize=9)
        if note:
            ax.set_title(note, fontsize=8, color=GREY, loc="left")
        if layer is layers[-1] and labels:
            ax.set_xticks(range(len(indices)))
            ax.set_xticklabels([str(labels[i]) for i in indices],
                               rotation=65, ha="right", fontsize=7)
        else:
            ax.set_xticks([])
        style_axes(ax)
    title = f"Activations — run {act.get('run')}"
    if case:
        title += (f" on {case.get('part', '?')} · {case.get('cfg_id', '?')}")
    title += f"  (max |a| = {amax:.2f})"
    fig.suptitle(title, fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    path = out_dir / "activation_map.png"
    fig.savefig(path, dpi=DPI)
    plt.close(fig)
    print(f"wrote {path} ({path.stat().st_size / 1024:.0f} KB)")
    return True


def main() -> int:
    args = parse_args()
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
