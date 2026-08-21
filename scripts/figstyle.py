#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""One visual identity for every generated figure in the repository.

Every figure generator imports this module and nothing else defines a colour,
a font size or a dpi. Two themes share one grammar:

  ``light``  analysis charts (advisor report, benchmark charts). They are read
             in a README and printed, so they sit on white.
  ``dark``   3D renders and product screenshots, where a dark stage genuinely
             helps a field read.

Beyond styling, the module carries the *honesty primitives*. The defects these
exist to prevent were real, and they are cheap to reintroduce by hand:

  ``loglim``          a log axis whose limits are set by a handful of values at
                      machine precision shows nothing. This floors the axis at
                      a stated precision limit, keeps the floored points
                      visible on the floor line, and returns the count so the
                      caller must say so on the figure.
  ``share_y``         multi-panel grids share a y-axis unless the caller passes
                      an explicit reason for not doing so.
  ``annotate_n``      sample size and exclusion count, same corner every time.
  ``tolerance_band``  one visual treatment for a tolerance region.
  ``convergence``     fits and prints the measured slope instead of drawing a
                      two-point line that invites the eye to extrapolate.

Series are keyed by *name*, not by call order, so ``graded_tet`` is the same
colour in every figure forever -- and every categorical series carries a marker
and a dash pattern in lockstep with its colour, so no chart ever distinguishes
series by colour alone.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, ListedColormap  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402
from matplotlib.font_manager import FontProperties  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

__all__ = [
    "use", "theme", "figure", "finish", "panel_title", "axes_off",
    "series", "series_handles", "SERIES_ORDER", "register_series",
    "field_cmap", "field_lut", "colorbar",
    "loglim", "share_y", "annotate_n", "tolerance_band",
    "convergence", "si", "unit_formatter", "footer_source",
    "provenance", "git_revision", "digest",
    "font_path", "assert_glyphs", "GLYPHS_REQUIRED",
    "QUANTITY_LABELS", "quantity_label", "metric_label",
    "times_off", "DECADES_NOTE",
]


# ---------------------------------------------------------------------------
# Fonts and glyph coverage
# ---------------------------------------------------------------------------
#: Glyphs that appear in captions across the repo. A font that cannot draw one
#: of these produces a tofu box or a bare "?" in a portfolio image, which has
#: happened before -- so it is asserted at import rather than discovered later.
GLYPHS_REQUIRED = "σ⌀≈≥≤×→·—±½∞°µΩ"

#: Preference order. Every entry is checked for full coverage of the glyph set
#: before it is accepted, so a partial font is skipped rather than silently
#: producing boxes for the characters it lacks.
_FONT_CANDIDATES = {
    "regular": ["DejaVu Sans", "Segoe UI", "Liberation Sans", "Arial"],
    "bold": ["DejaVu Sans", "Segoe UI", "Liberation Sans", "Arial"],
    "mono": ["DejaVu Sans Mono", "Consolas", "Liberation Mono", "Courier New"],
}

_font_cache: dict[tuple[str, bool], Path] = {}
_coverage_cache: dict[Path, set[int]] = {}


def _charmap(path: Path) -> set[int]:
    cached = _coverage_cache.get(path)
    if cached is None:
        from matplotlib.ft2font import FT2Font

        cached = set(FT2Font(str(path)).get_charmap())
        _coverage_cache[path] = cached
    return cached


def _covers(path: Path, glyphs: str) -> list[str]:
    chars = _charmap(path)
    return [c for c in glyphs if ord(c) not in chars]


def font_path(kind: str = "regular", bold: bool = False) -> Path:
    """Resolve a concrete TTF path with verified glyph coverage.

    PIL-based generators need a file path, matplotlib needs a family name, and
    both must agree -- so both go through here.
    """
    key = (kind, bold)
    hit = _font_cache.get(key)
    if hit is not None:
        return hit

    from matplotlib import font_manager as fm

    problems: list[str] = []
    for name in _FONT_CANDIDATES[kind]:
        try:
            found = fm.findfont(
                fm.FontProperties(family=name,
                                  weight="bold" if bold else "normal"),
                fallback_to_default=False,
            )
        except Exception:
            continue
        path = Path(found)
        missing = _covers(path, GLYPHS_REQUIRED)
        if missing:
            problems.append(f"{name}: missing {''.join(missing)}")
            continue
        _font_cache[key] = path
        return path

    raise SystemExit(
        "figstyle: no installed font covers the required glyph set "
        f"({GLYPHS_REQUIRED}).\n  tried: "
        + "; ".join(problems or _FONT_CANDIDATES[kind])
        + "\n  install DejaVu Sans (pip install matplotlib ships it) or add a "
          "font that covers those characters."
    )


def assert_glyphs(*texts: str) -> None:
    """Fail loudly if any caption contains a glyph the chosen font lacks.

    Generators that compose captions from data (part names, metric names,
    units) call this before drawing, so a surprising character from a data file
    is a build failure rather than a box in a committed PNG.
    """
    joined = "".join(texts)
    interesting = {c for c in joined if ord(c) > 0x7E}
    if not interesting:
        return
    for bold in (False, True):
        path = font_path("regular", bold=bold)
        missing = sorted({c for c in interesting if ord(c) not in _charmap(path)})
        if missing:
            raise SystemExit(
                f"figstyle: {path.name} cannot draw {''.join(missing)} "
                f"(U+{' U+'.join(f'{ord(c):04X}' for c in missing)}) — "
                "these would render as boxes. Replace the character or add a "
                "font that covers it."
            )


# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------
#: Okabe-Ito: eight hues that stay distinguishable under deuteranopia,
#: protanopia and tritanopia, and survive greyscale printing.
OKABE_ITO = {
    "blue": "#0072B2",
    "vermillion": "#D55E00",
    "bluish_green": "#009E73",
    "orange": "#E69F00",
    "sky": "#56B4E9",
    "purple": "#CC79A7",
    "yellow": "#F0E442",
    "black": "#000000",
}
SERIES_ORDER = ["blue", "vermillion", "bluish_green", "orange",
                "sky", "purple", "yellow", "black"]

MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]
DASHES: list[Any] = ["-", (0, (4, 1.5)), (0, (1, 1.2)), (0, (6, 1.5, 1, 1.5)),
                     (0, (3, 1, 1, 1, 1, 1)), (0, (8, 2)), (0, (2, 2)),
                     (0, (5, 1, 2, 1))]


@dataclass(frozen=True)
class Style:
    """Everything a categorical series needs, resolved together."""
    name: str
    label: str
    color: str
    marker: str
    dash: Any
    slot: int

    def line(self, **kwargs: Any) -> dict[str, Any]:
        out = dict(color=self.color, linestyle=self.dash, marker=self.marker,
                   label=self.label)
        out.update(kwargs)
        return out

    def scatter(self, **kwargs: Any) -> dict[str, Any]:
        out = dict(color=self.color, marker=self.marker, label=self.label)
        out.update(kwargs)
        return out


#: Stable name -> slot map. Adding a figure never renumbers an existing series,
#: so the same mesher is the same colour and the same marker across the repo.
_SERIES_SLOTS: dict[str, int] = {}
_SERIES_LABELS: dict[str, str] = {}
_NEUTRAL_SERIES: set[str] = set()


def register_series(name: str, slot: int | None = None, *,
                    label: str | None = None, neutral: bool = False) -> None:
    """Pin a series name to a palette slot (or to the neutral grey)."""
    if neutral:
        _NEUTRAL_SERIES.add(name)
    elif slot is not None:
        _SERIES_SLOTS[name] = slot % len(SERIES_ORDER)
    if label is not None:
        _SERIES_LABELS[name] = label


def _preregister() -> None:
    # meshers -- the most widely shared vocabulary in the repo. These labels are
    # the compact form, for an in-plot legend or a step label; QUANTITY_LABELS
    # carries the longer wording for axes and colourbars, where there is room
    # for it. The two differ deliberately and are not a drift to be "fixed".
    for slot, (name, label) in enumerate([
        ("hybrid_zoo", "hybrid, hex + pyramid skin"),
        ("graded_tet", "graded tets"),
        ("hex", "hex bricks"),
        ("hybrid_vem", "hybrid VEM"),
        ("varyhedron", "varyhedron"),
        ("cvt_poly", "Voronoi polyhedra"),
    ]):
        register_series(name, slot, label=label)
    # external comparison sources
    register_series("gmsh-mesh+polymesh-solver", 4, label="Gmsh mesh")
    register_series("calculix", 5, label="CalculiX")
    register_series("polymesh-native-graded", 1, label="native graded")
    register_series("polymesh-native", neutral=True, label="native default")
    # element orders
    for order in (1, 2, 3, 4):
        register_series(f"order {order}", order - 1)
    # benchmark comparisons
    register_series("baseline", neutral=True)
    register_series("measured", 0)
    register_series("theory", neutral=True)
    register_series("oracle", neutral=True, label="oracle")
    register_series("advisor", 0, label="advisor")
    register_series("before", neutral=True, label="before")
    register_series("after", 0, label="after")


_preregister()


def series(name: str, label: str | None = None) -> Style:
    """Style for a named series. Unregistered names get a stable hashed slot."""
    display = label or _SERIES_LABELS.get(name, name)
    if name in _NEUTRAL_SERIES:
        return Style(name, display, theme().muted, MARKERS[-1], DASHES[1], -1)
    slot = _SERIES_SLOTS.get(name)
    if slot is None:
        # Deterministic across runs and machines: hash the name, not the order
        # of appearance, so a figure that gains a series does not recolour the
        # ones it already had.
        slot = int(re.sub(r"\W", "", name).encode().hex() or "0", 16) % len(SERIES_ORDER)
        _SERIES_SLOTS[name] = slot
    return Style(name, display, OKABE_ITO[SERIES_ORDER[slot]],
                 MARKERS[slot % len(MARKERS)], DASHES[slot % len(DASHES)], slot)


def series_handles(names: Sequence[str], **kwargs: Any) -> list[Line2D]:
    """Legend handles that show colour *and* marker *and* dash together."""
    out = []
    for name in names:
        st = series(name)
        out.append(Line2D([0], [0], color=st.color, linestyle=st.dash,
                          marker=st.marker, markersize=6, linewidth=2.0,
                          label=st.label, **kwargs))
    return out


# ---------------------------------------------------------------------------
# Quantity names
# ---------------------------------------------------------------------------
#: Dataset column / model head -> the words a reader sees on an axis, in a
#: legend, on a colourbar or in a generated table cell.
#:
#: It lives here once because the same six quantities are drawn by six
#: generators (``advisor/report.py``, ``advisor/plot_evaluation.py``,
#: ``advisor/figures.py``, ``advisor/dashboard.py``, ``plot_benchmarks.py``
#: and the showcase composites). Six hand-typed copies of "cell size, as a
#: fraction of the part" drift, and a figure whose axis disagrees with the doc
#: beside it is worse than either one being wrong on its own: the reader
#: cannot tell which of the two is the mistake, so both stop being evidence.
#:
#: The keys stay verbatim. They are provenance -- CSV column names, ONNX
#: output names, ``history.jsonl`` metric keys -- and every footer, manifest
#: and record still spells them exactly. Only the value is prose.
QUANTITY_LABELS: dict[str, str] = {
    # accuracy heads
    "rel_err": "predicted relative error",
    "rel_err_rel": "relative error, against the case's own median",
    # the campaign CSV's *measured* column, which is not a prediction: keeping
    # the two labels distinct is the difference between an honest axis and a
    # claim the figure cannot support.
    "accuracy_rel_err": "measured relative error",
    "geo_chamfer": "mesh-to-CAD distance",
    "geo_fidelity_chamfer_mean": "mesh-to-CAD distance",
    "geo_p99": "mesh-to-CAD worst 1%",
    "geo_fidelity_dist_p99": "mesh-to-CAD worst 1%",
    # cost heads
    "dof": "degrees of freedom",
    "n_dof": "degrees of freedom",
    "max_dof": "degrees-of-freedom budget",
    "mesh_ms": "meshing time",
    "solve_ms": "solve time",
    "efficiency": "relative error x degrees of freedom",
    # feasibility. The head emits a logit; a figure that plots the squashed
    # probability must say "probability" itself -- this label is the quantity,
    # not the scale.
    "failure": "failure risk",
    "failure_logit": "failure risk",
    "ood_distance": "distance from the training distribution",
    # action dimensions
    "h_rel": "cell size, as a fraction of the part",
    "eta_target": "error target",
    "adapt_passes": "refinement passes",
    "p_elevate": "quadratic elements",
    "global_eta": "global error estimate",
    "policy": "recommended action",
    # The policy head's own regression twins of the three action dimensions
    # above. They need their own entries rather than a prefix-stripping rule:
    # `h_rel` is the size a run actually used, `policy_h_rel` is the size the
    # network would advise, and a figure that lets those two share one label
    # cannot say which of them it drew.
    "policy_h_rel": "recommended cell size, as a fraction of the part",
    "policy_adapt_passes": "recommended refinement passes",
    "policy_eta_target": "recommended error target",
    "policy_order_logit_1": "order 1 (linear)",
    "policy_order_logit_2": "order 2 (quadratic)",
    "policy_mesher_logit_graded_tet": "mesher: graded tets",
    "policy_mesher_logit_hex": "mesher: hex bricks",
    "policy_mesher_logit_hybrid_vem": "mesher: hybrid VEM",
    "policy_mesher_logit_hybrid_zoo": "mesher: hybrid, hex bulk + pyramid skin",
    "total_loss": "total loss",
    # Run bookkeeping the dashboard plots. `prune.py` drops the worst-fitting
    # rows per head into a persistent ledger and every later run trains without
    # them, so these count dropped *training rows* -- not anything wrong with a
    # part, which is what "pruned" reads as to someone meeting it cold.
    "pruned_rows": "worst-fitting rows dropped this run",
    "pruned_total": "worst-fitting rows dropped so far",
    # Summary-JSON keys rather than heads, but they reach an axis in
    # `plot_evaluation.py` and a table in `report.py`, so they belong to the
    # same vocabulary. "regret" is the gap to the best action that case could
    # have had, in powers of 10 -- pair either of these with `DECADES_NOTE`.
    "macro_mean_regret": "how much worse than the best choice",
    "pick_failure_rate": "share of picks that fail",
}

#: ``history.jsonl`` keys are ``<head>_<statistic>``. One table over heads and
#: one over statistics covers every legend entry the dashboard draws, so a new
#: head needs no new legend string.
#:
#: The statistics are spelled out rather than abbreviated. The abbreviations
#: used to stand here on the grounds that "MAE and AUC are the words a reader
#: of an error plot expects" -- true of a reader who already trains models, and
#: the people these figures get shown to are not that reader. "MAE" on an axis
#: explains nothing to them, while "average miss" costs the trained reader
#: nothing: the identifier is still in the footer, the manifest and every
#: record, so nothing became less traceable by the axis becoming readable.
_METRIC_STATS: dict[str, str] = {
    "mae": "average miss",
    "rmse": "average miss, RMS",
    "mse": "mean squared miss",
    "bce": "cross-entropy",
    "acc": "accuracy",
    "auc": "ranking quality, ROC AUC",
}


def quantity_label(name: str) -> str:
    """Reader-facing words for a column/head name.

    Unmapped names come back verbatim rather than de-snaked mechanically: an
    identifier showing through on an axis is a visible prompt to add a real
    label here, whereas ``"geo p99"`` reads like prose and hides.
    """
    return QUANTITY_LABELS.get(name, name)


def metric_label(name: str) -> str:
    """``rel_err_mae`` -> ``predicted relative error (average miss)``."""
    head, _, stat = name.rpartition("_")
    if head and stat in _METRIC_STATS:
        return f"{quantity_label(head)} ({_METRIC_STATS[stat]})"
    return quantity_label(name)


#: Every accuracy and cost head is trained on a log10 target, so a miss on one
#: of those axes is a distance in powers of ten and not a percentage: 0.30 is
#: not "30% off", it is "off by a factor of two". An axis carrying one of those
#: distances says so, because the bare number reads as a fraction and misleads
#: in the direction of sounding better than it is.
DECADES_NOTE = "powers of 10 — 0.30 means off by about 2x"


def times_off(decades: float) -> str:
    """A log10 distance as the plain factor it stands for: ``0.30`` -> ``2.0x``.

    The display half of :func:`scripts.advisor.regret.decades_to_factor`, which
    stays numeric because JSON records and console tables consume the float.
    Both are kept on purpose: a record wants the number, an axis wants the four
    characters a reader can picture. Do not collapse either into the other.
    """
    if not math.isfinite(decades):
        return "n/a"
    return f"{10.0 ** decades:.1f}x"


# ---------------------------------------------------------------------------
# Themes
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Theme:
    name: str
    bg: str
    panel: str
    ink: str
    muted: str
    grid: str
    rule: str
    accent: str
    ok: str
    warn: str
    bad: str
    band: str


LIGHT = Theme(
    name="light", bg="#FFFFFF", panel="#FFFFFF", ink="#1A1D21",
    muted="#5B6470", grid="#DFE3E8", rule="#B7BEC7", accent="#0072B2",
    ok="#009E73", warn="#E69F00", bad="#D55E00", band="#0072B2",
)
DARK = Theme(
    name="dark", bg="#0E1116", panel="#161B22", ink="#E6EAF0",
    muted="#9AA4B2", grid="#2A3240", rule="#3A4454", accent="#56B4E9",
    ok="#009E73", warn="#E69F00", bad="#D55E00", band="#56B4E9",
)
THEMES = {"light": LIGHT, "dark": DARK}

_active: Theme = LIGHT


def theme() -> Theme:
    return _active


# Type sizes are declared in points at the *final display width*, and dpi is
# derived from a target display width, so a 900 px README chart and a 1920 px
# hero end up with the same apparent text size.
FONT_PT = {
    "title": 13.0,
    "subtitle": 10.5,
    "panel": 11.0,
    "label": 10.0,
    "tick": 9.0,
    "legend": 9.0,
    "annot": 9.0,
    "footer": 8.5,
}

#: figure presets: (width_in, height_in). Nothing wider than 2.2:1 -- the 4:1
#: letterboxes in the old showcase were unreadable at README width.
SIZES = {
    "half": (6.4, 4.4),
    "full": (10.0, 6.0),
    "wide": (11.0, 5.0),
    "tall": (8.0, 9.0),
    "square": (7.0, 7.0),
    "hero": (12.0, 6.75),
}

#: Rendered so the long edge lands near this many pixels; dpi follows.
TARGET_PX = 1800
MAX_ASPECT = 2.2

#: Share of the canvas width a title, subtitle or footer may occupy before it
#: wraps. Captions are drawn from x=0.012, so this leaves a right margin to
#: match the left inset rather than letting text run to the very edge.
CAPTION_WIDTH = 0.976


def save_dpi(width_in: float) -> int:
    """The dpi a figure of this width will be written at.

    Shared by `figure` and `finish` so a caption is measured at exactly the
    resolution it is rasterised at. They must not derive this separately.
    """
    return max(120, round(TARGET_PX / width_in))


def use(name: str = "light") -> Theme:
    """Activate a theme and push it into matplotlib's rcParams."""
    global _active
    if name not in THEMES:
        raise ValueError(f"unknown theme {name!r}; expected one of {sorted(THEMES)}")
    _active = THEMES[name]
    t = _active
    fam = font_path("regular")
    from matplotlib import font_manager as fm

    family = fm.FontProperties(fname=str(fam)).get_name()
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": [family, "DejaVu Sans"],
        "axes.unicode_minus": False,
        "figure.facecolor": t.bg,
        "savefig.facecolor": t.bg,
        "savefig.bbox": None,
        "axes.facecolor": t.panel,
        "axes.edgecolor": t.rule,
        "axes.labelcolor": t.ink,
        "axes.titlecolor": t.ink,
        "axes.titlesize": FONT_PT["panel"],
        "axes.titleweight": "regular",
        "axes.labelsize": FONT_PT["label"],
        "axes.linewidth": 0.9,
        "axes.grid": True,
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "text.color": t.ink,
        "xtick.color": t.muted,
        "ytick.color": t.muted,
        "xtick.labelcolor": t.ink,
        "ytick.labelcolor": t.ink,
        "xtick.labelsize": FONT_PT["tick"],
        "ytick.labelsize": FONT_PT["tick"],
        "xtick.direction": "out",
        "ytick.direction": "out",
        "grid.color": t.grid,
        "grid.linewidth": 0.7,
        "legend.fontsize": FONT_PT["legend"],
        "legend.frameon": False,
        "legend.labelcolor": t.ink,
        "lines.linewidth": 2.0,
        "lines.markersize": 5.5,
        "patch.linewidth": 0.8,
        "figure.dpi": 110,
    })
    return t


#: A PIL composite has no dpi: it has a pixel canvas. Point sizes are declared
#: against the same reference display width the matplotlib figures use, so a
#: 2000 px composite and a 900 px chart carry the same apparent type size.
REFERENCE_DISPLAY_PX = 900
MIN_TEXT_PX = 11


def font_px(role: str, canvas_px: int) -> int:
    """Point size for ``role`` converted to pixels on a canvas of that width.

    Never returns anything that would render below ``MIN_TEXT_PX`` at README
    width -- sub-pixel footers were one of the audited defects.
    """
    if role not in FONT_PT:
        raise KeyError(f"unknown type role {role!r}; expected one of "
                       f"{sorted(FONT_PT)}")
    scale = canvas_px / REFERENCE_DISPLAY_PX
    px = FONT_PT[role] * 96.0 / 72.0 * scale
    return int(round(max(px, MIN_TEXT_PX * scale)))


def pil_font(role: str, canvas_px: int, *, bold: bool = False):
    """Loaded PIL font for a type role, from the glyph-checked font file."""
    from PIL import ImageFont

    return ImageFont.truetype(str(font_path("regular", bold=bold)),
                              font_px(role, canvas_px))


# ---------------------------------------------------------------------------
# Field colormaps
# ---------------------------------------------------------------------------
def _gui_lut(n: int = 256) -> np.ndarray:
    """The GUI viewport's own blue->cyan->green->yellow->red ramp.

    Kept only so ``gui_studio.png`` can document the GUI as it actually looks.
    It is not perceptually uniform and it is not colour-blind safe, so it is
    never used for an analysis or gallery render.
    """
    ts = np.linspace(0.0, 1.0, n)
    r = np.clip(np.minimum(4.0 * ts - 2.0, 4.0 - 4.0 * ts) + 1.0, 0.0, 1.0)
    g = np.clip(np.minimum(4.0 * ts, 4.0 - 4.0 * ts), 0.0, 1.0)
    b = np.clip(np.minimum(4.0 * ts, 2.0 - 4.0 * ts) + 0.0, 0.0, 1.0)
    return np.stack([r, g, b], axis=1)


#: field kind -> colormap. Chosen by what the field *is*, not by habit.
_FIELD_CMAPS = {
    # magnitude anchored at zero (von Mises, |u|, distance): perceptually
    # uniform, monotone in lightness, safe in greyscale and under CVD.
    "magnitude": "viridis",
    # signed fields (residual, error, before-after delta): diverging, and
    # callers must pass a symmetric norm.
    "signed": "RdBu_r",
    # ordinal / categorical-ish scalar where blue-yellow reads better
    "ordinal": "cividis",
    # sequential emphasis for a second field in the same figure
    "secondary": "magma",
}


def field_cmap(kind: str = "magnitude"):
    if kind == "gui":
        return ListedColormap(_gui_lut(), name="polymesh_gui")
    try:
        return matplotlib.colormaps[_FIELD_CMAPS[kind]]
    except KeyError:
        raise ValueError(
            f"unknown field kind {kind!r}; expected one of "
            f"{sorted(list(_FIELD_CMAPS) + ['gui'])}"
        ) from None


def field_lut(kind: str = "magnitude", n: int = 256) -> np.ndarray:
    """(n, 3) float LUT in [0, 1] -- for PIL/PyVista generators."""
    if kind == "gui":
        return _gui_lut(n)
    cmap = field_cmap(kind)
    return np.array([cmap(i / (n - 1))[:3] for i in range(n)], dtype=float)


# ---------------------------------------------------------------------------
# Units and number formatting
# ---------------------------------------------------------------------------
_SI = [(1e12, "T"), (1e9, "G"), (1e6, "M"), (1e3, "k"), (1.0, ""),
       (1e-3, "m"), (1e-6, "µ"), (1e-9, "n"), (1e-12, "p"), (1e-15, "f"),
       (1e-18, "a")]


def si(value: float, unit: str = "", digits: int = 3) -> str:
    """3.84e6, 'Pa' -> '3.84 MPa'. Exponent soup is unreadable on a colourbar."""
    if not math.isfinite(value):
        return "n/a"
    if value == 0:
        return f"0{' ' + unit if unit else ''}"
    magnitude = abs(value)
    for scale, prefix in _SI:
        if magnitude >= scale:
            break
    else:
        scale, prefix = 1e-12, "p"
    scaled = value / scale
    text = f"{scaled:.{digits}g}"
    return f"{text} {prefix}{unit}".rstrip() if unit or prefix else text


def si_prefix(reference: float, unit: str) -> tuple[float, str]:
    """Pick one SI scale for a whole axis/colourbar from its top value."""
    magnitude = abs(reference)
    for scale, prefix in _SI:
        if magnitude >= scale:
            return scale, f"{prefix}{unit}"
    return 1.0, unit


def unit_formatter(unit: str, reference: float | None = None) -> FuncFormatter:
    if reference is None:
        return FuncFormatter(lambda v, _: si(v, unit))
    scale, label = si_prefix(reference, unit)
    return FuncFormatter(lambda v, _: f"{v / scale:g}")


# ---------------------------------------------------------------------------
# Provenance
# ---------------------------------------------------------------------------
#: This repository now holds figures produced under three different reference
#: truth regimes (self-generated truth, the corrected engine, and the
#: independent Gmsh -> CalculiX chain). A reader cannot tell them apart from
#: the pixels, and a wrong-regime figure is indistinguishable from a wrong
#: result. So every figure that carries a number states, on its face, the
#: revision of the code and the digest of the data it was drawn from.
#:
#: This is deliberately not opt-in: ``footer_source`` already receives the
#: exact inputs each figure reads, so the stamp is derived there and no
#: generator can forget it.

#: Short digests keep the footer legible. Twelve hex characters is 48 bits --
#: far past any accidental collision among a few hundred artefacts, and it is
#: a prefix of the full sha256 the provenance JSON records elsewhere, so it
#: can be checked with a plain ``sha256sum``.
DIGEST_CHARS = 12
#: Hash by content, but do not re-read a 4 MB CSV once per panel: the same
#: file is stamped on six advisor figures in one process. Keyed by resolved
#: path plus (size, mtime_ns), so an edit mid-run is still picked up.
_DIGEST_CACHE: dict[tuple[str, int, int], str] = {}
#: Directory digests fold in this many members at most, newest-name-first by
#: sorted order, so a warehouse holding thousands of PNGs cannot stall a
#: figure. The count is always reported alongside, so a truncated fold is
#: visible rather than silent.
DIGEST_DIR_LIMIT = 512


def _digest_file(path: Path) -> str:
    stat = path.stat()
    key = (str(path), stat.st_size, stat.st_mtime_ns)
    cached = _DIGEST_CACHE.get(key)
    if cached is not None:
        return cached
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            hasher.update(block)
    value = hasher.hexdigest()
    _DIGEST_CACHE[key] = value
    return value


def digest(path: Path | str) -> tuple[str, int]:
    """(sha256 hex, member count) for a file or a directory of inputs.

    A directory digest is the hash of its members' digests in sorted-name
    order, so it changes if any member changes, is added or is removed --
    which is what "the data this figure read" means when the input is a
    results directory or a campaign warehouse.
    """
    p = Path(path)
    try:
        if p.is_file():
            return _digest_file(p), 1
        if p.is_dir():
            members = sorted(q for q in p.rglob("*") if q.is_file())
            folded = members[:DIGEST_DIR_LIMIT]
            hasher = hashlib.sha256()
            for member in folded:
                hasher.update(member.relative_to(p).as_posix().encode("utf-8"))
                hasher.update(_digest_file(member).encode("ascii"))
            return hasher.hexdigest(), len(members)
    except OSError:
        pass
    return "", 0


_GIT_REVISION: str | None = None


def git_revision() -> str:
    """``129de02`` or ``129de02+dirty``; ``unknown`` if this is not a checkout.

    ``+dirty`` is not decoration. Most figures in this repository are
    regenerated from a working tree mid-sweep, and a stamp that silently
    reports the last commit for a tree that no longer matches it is worse
    than no stamp at all.
    """
    global _GIT_REVISION
    if _GIT_REVISION is not None:
        return _GIT_REVISION
    override = os.environ.get("POLYMESH_GIT_REVISION")
    if override:
        _GIT_REVISION = override.strip()
        return _GIT_REVISION

    def run(*args: str) -> str | None:
        try:
            done = subprocess.run(("git", "-C", str(ROOT)) + args,
                                  capture_output=True, text=True, timeout=15)
        except (OSError, subprocess.SubprocessError):
            return None
        return done.stdout if done.returncode == 0 else None

    head = run("rev-parse", "--short", "HEAD")
    if head is None:
        _GIT_REVISION = "unknown"
        return _GIT_REVISION
    revision = head.strip()
    status = run("status", "--porcelain", "--untracked-files=no")
    if status is None:
        revision += "+unknown-tree"
    elif status.strip():
        revision += "+dirty"
    _GIT_REVISION = revision
    return revision


def _stamp_names(paths: Sequence[Path]) -> list[str]:
    """Shortest trailing path fragments that stay distinct within one stamp.

    Bare basenames collide constantly here -- two ``metrics.json`` from
    different run directories, six ``wire_feature.png`` from six warehouse
    ``t0`` directories -- and a stamp that prints the same label twice has
    lost the distinction it exists to make. So each label grows one parent at
    a time until every label in the stamp is unique.
    """
    parts = [tuple(p.parts) or (p.as_posix(),) for p in paths]
    # Grow only the labels that actually collide, and grow every member of a
    # colliding group together: bumping just one of them yields "t0/wire.png"
    # beside a bare "wire.png", which reads as two different KINDS of input
    # rather than two siblings. A single global depth is the other failure --
    # it drags an unrelated input up to "Polyhedral-FEA/bench/advisor/
    # dataset.csv" because two warehouse renders share a basename.
    depths = [1] * len(parts)
    names = [part[-1] for part in parts]
    for _ in range(max((len(part) for part in parts), default=1)):
        groups: dict[str, list[int]] = {}
        for index, name in enumerate(names):
            groups.setdefault(name, []).append(index)
        clashing = [index for members in groups.values() if len(members) > 1
                    for index in members
                    if depths[index] < len(parts[index])]
        if not clashing:
            break
        for index in clashing:
            depths[index] += 1
            names[index] = "/".join(parts[index][-depths[index]:])
    return names


#: More than this many inputs sharing a basename are folded into one combined
#: digest. Six ``wire_feature.png`` paths spelled out in full would be a
#: footer nobody reads, and the question the stamp answers -- "are these the
#: renders I think they are" -- is answered just as well by one digest over
#: the set.
FOLD_REPEATS_ABOVE = 2


def provenance(*paths: Path | str) -> str:
    """``regime: git <rev> · <name> sha256 <12 hex>`` for the given inputs.

    Missing inputs are stamped ``absent`` rather than omitted: a figure drawn
    without one of its stated sources is a fact about the figure.
    """
    resolved = [Path(path) for path in paths]
    repeats: dict[str, list[Path]] = {}
    for p in resolved:
        repeats.setdefault(p.name, []).append(p)

    folded_names = {name for name, group in repeats.items()
                    if len(group) > FOLD_REPEATS_ABOVE}
    singles = [p for p in resolved if p.name not in folded_names]
    labels = dict(zip((str(p) for p in singles), _stamp_names(singles)))

    parts = [f"git {git_revision()}"]
    seen: set[str] = set()
    for p in resolved:
        if p.name in folded_names:
            if p.name in seen:
                continue
            seen.add(p.name)
            group = repeats[p.name]
            hasher = hashlib.sha256()
            missing = 0
            for member in group:
                value, _ = digest(member)
                if not value:
                    missing += 1
                hasher.update((value or "absent").encode("ascii"))
            note = f" ({missing} absent)" if missing else ""
            parts.append(f"{p.name} x{len(group)} sha256 "
                         f"{hasher.hexdigest()[:DIGEST_CHARS]}{note}")
            continue
        name = labels[str(p)]
        value, count = digest(p)
        if not value:
            parts.append(f"{name} absent")
        elif p.is_dir():
            shown = min(count, DIGEST_DIR_LIMIT)
            folded = f"{shown} of {count}" if count > shown else f"{count}"
            parts.append(f"{name}/ sha256 {value[:DIGEST_CHARS]} ({folded} files)")
        else:
            parts.append(f"{name} sha256 {value[:DIGEST_CHARS]}")
    return "regime: " + " · ".join(parts)


def stale_against(recorded_sha: str, path: Path | str) -> str:
    """Say so when a record was computed on a different file than exists now.

    Artefacts like ``crossval_*.json`` record the sha256 of the dataset they
    were computed from. That is a fact about the record; whether it still
    matches the dataset on disk is a fact about the FIGURE, and it is the one
    a reader needs, because a chart drawn from a record computed on retired
    truth looks exactly like a current one.

    Returns "" when they agree or when there is nothing to compare.
    """
    recorded = (recorded_sha or "").strip().lower()
    if not recorded:
        return ""
    current, _ = digest(path)
    if not current:
        return f"STALE? {Path(path).name} is absent, so the record cannot be checked"
    if current.startswith(recorded) or recorded.startswith(current):
        return ""
    return (f"STALE: computed on {Path(path).name} sha256 "
            f"{recorded[:DIGEST_CHARS]}, but the file on disk is now "
            f"{current[:DIGEST_CHARS]}")


def footer_source(*paths: Path | str, note: str = "", n: int | None = None,
                  stamp: bool = True) -> str:
    """Provenance line: POSIX slashes, repo-relative, record count.

    A second line stamps the code revision and a content digest per input,
    because figures from three different truth regimes now coexist in this
    repository and only the stamp tells them apart. ``stamp=False`` exists for
    figures with no data inputs at all (a drawn diagram); it is never the
    right choice for a figure carrying a number.
    """
    parts = []
    for path in paths:
        p = Path(path)
        try:
            parts.append(p.resolve().relative_to(ROOT).as_posix())
        except (ValueError, OSError):
            parts.append(p.as_posix())
    text = "source: " + " · ".join(parts) if parts else ""
    if n is not None:
        text += f"  ({n} record{'' if n == 1 else 's'})"
    if note:
        text = f"{text} · {note}" if text else note
    if stamp:
        line = provenance(*paths)
        text = f"{text}\n{line}" if text else line
    return text


# ---------------------------------------------------------------------------
# Figure furniture
# ---------------------------------------------------------------------------
_FIG_META: dict[int, dict[str, Any]] = {}


def figure(title: str, *, subtitle: str = "", footer: str = "",
           size: str | tuple[float, float] = "full",
           nrows: int = 1, ncols: int = 1,
           share_y_axis: bool | str = True,
           **subplot_kw: Any):
    """Create a figure with the standard title/subtitle/footer geometry.

    ``share_y_axis`` defaults to True for multi-column grids. Passing a string
    opts out *and records the reason*, which is printed -- panels compared
    side by side on different scales was a real defect here.
    """
    dims = SIZES[size] if isinstance(size, str) else size
    aspect = dims[0] / dims[1]
    if aspect > MAX_ASPECT:
        raise ValueError(
            f"figure aspect {aspect:.2f}:1 exceeds the {MAX_ASPECT}:1 cap — "
            "letterboxed figures are unreadable at README width; use a grid."
        )
    assert_glyphs(title, subtitle, footer)

    sharey: Any = False
    if ncols > 1:
        if share_y_axis is True:
            sharey = "all"
        elif isinstance(share_y_axis, str):
            print(f"  note: y-axis not shared — {share_y_axis}")

    fig, axes = plt.subplots(nrows, ncols, figsize=dims, squeeze=False,
                             sharey=sharey, **subplot_kw)
    # Pin the figure to the dpi it will be *saved* at, before a single caption
    # is measured. Glyph advances are hinted, so they snap to whole pixels and
    # a line's width is not exactly proportional to dpi: measuring at one dpi
    # and rasterising at another drifts by around 1% over a long caption, which
    # is enough to push the widest subtitle past an edge it was measured to
    # clear. Measuring and drawing at one dpi makes the wrap exact.
    fig.set_dpi(save_dpi(dims[0]))

    t = theme()
    # Wrap to the canvas: a subtitle or footer running off the right edge was
    # a recurring defect, and every caption here is composed from data whose
    # length is not known when the figure is laid out.
    def fit(text: str, role: str, *, bold: bool = False) -> tuple[str, int]:
        """Wrap to the canvas by measuring the text, not by counting characters.

        A mean advance width cannot bound a proportional string. The previous
        budget divided the canvas by an average character, which a Title Case
        line full of capitals and digits comfortably exceeds -- so a caption
        that fitted the character budget could still run past the right edge
        and be clipped mid-word, which is how it presented (`textwrap` never
        breaks inside a word, so a mid-word cut means no wrap happened at all).

        Measured through the renderer that will draw the text, not from glyph
        outlines: `TextPath` extents describe the inked path and come out about
        1% under matplotlib's advance-and-kern layout, which is inside the
        margin this leaves and so still clipped the widest captions. The
        backend is Agg from module import, so a renderer always exists here.
        """
        if not text:
            return "", 0
        # Renderer widths are in pixels at the figure's dpi, so the budget has
        # to be too. Both scale together, which keeps this dpi-independent.
        limit = dims[0] * fig.dpi * CAPTION_WIDTH
        properties = FontProperties(fname=str(font_path("regular", bold=bold)),
                                    size=FONT_PT[role])
        renderer = fig.canvas.get_renderer()
        measured: dict[str, float] = {}

        def width(fragment: str) -> float:
            if not fragment:
                return 0.0
            if fragment not in measured:
                measured[fragment] = float(
                    renderer.get_text_width_height_descent(
                        fragment, properties, False)[0])
            return measured[fragment]

        lines: list[str] = []
        for paragraph in text.split("\n"):
            current = ""
            for word in paragraph.split(" "):
                candidate = word if not current else f"{current} {word}"
                # A single word wider than the canvas still gets its own line:
                # breaking inside it is what we are trying to avoid.
                if current and width(candidate) > limit:
                    lines.append(current)
                    current = word
                else:
                    current = candidate
            lines.append(current)
        return "\n".join(lines), len(lines)

    title_text, title_lines = fit(title, "title", bold=True)
    subtitle_text, subtitle_lines = fit(subtitle, "subtitle")
    footer_text, footer_lines = fit(footer, "footer")

    line_h = 1.55 / (dims[1] * 72.0)
    top = 0.99
    if title_text:
        fig.text(0.012, top, title_text, ha="left", va="top",
                 fontsize=FONT_PT["title"], weight="bold", color=t.ink)
        top -= title_lines * FONT_PT["title"] * line_h + 0.012
    if subtitle_text:
        fig.text(0.012, top, subtitle_text, ha="left", va="top",
                 fontsize=FONT_PT["subtitle"], color=t.muted)
    if footer_text:
        fig.text(0.012, 0.012, footer_text, ha="left", va="bottom",
                 fontsize=FONT_PT["footer"], color=t.muted)

    head = 0.985 - (title_lines * FONT_PT["title"] * line_h + 0.012 if title_text else 0.0) \
        - (subtitle_lines * FONT_PT["subtitle"] * line_h + 0.022 if subtitle_text else 0.0)
    foot = (0.022 + footer_lines * FONT_PT["footer"] * line_h) if footer_text else 0.03
    _FIG_META[id(fig)] = {"rect": (0.012, foot, 0.988, head),
                          "title": title, "subtitle": subtitle,
                          "footer": footer}
    return fig, axes


def panel_title(ax: Any, text: str) -> None:
    assert_glyphs(text)
    ax.set_title(text, fontsize=FONT_PT["panel"], color=theme().ink,
                 loc="left", pad=6)


def axes_off(ax: Any) -> None:
    ax.set_xticks([])
    ax.set_yticks([])
    for side in ax.spines.values():
        side.set_visible(False)


def finish(fig: Any, out: Path, *, manifest: dict[str, Any] | None = None,
           max_bytes: int | None = None) -> Path:
    """Lay out, save at the derived dpi, report, and optionally record."""
    meta = _FIG_META.pop(id(fig), None)
    rect = meta["rect"] if meta else (0.012, 0.03, 0.988, 0.95)
    try:
        fig.tight_layout(rect=rect)
    except Exception:
        pass

    width_in = fig.get_size_inches()[0]
    dpi = save_dpi(width_in)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=dpi, facecolor=theme().bg,
                pil_kwargs={"optimize": True, "compress_level": 9})
    plt.close(fig)

    if max_bytes and out.stat().st_size > max_bytes:
        _shrink(out, max_bytes)

    px = round(width_in * dpi)
    try:
        shown = out.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        shown = out.as_posix()
    print(f"  wrote {shown}  ({px}px wide, {dpi} dpi, "
          f"{out.stat().st_size / 1024:.0f} KB)")

    if manifest is not None:
        manifest.setdefault("images", []).append({
            "file": out.name,
            "width_px": px,
            "dpi": dpi,
            "bytes": out.stat().st_size,
            "theme": theme().name,
            **({"title": meta["title"], "caption": meta["subtitle"],
                "source": meta["footer"]} if meta else {}),
        })
    return out


def _shrink(path: Path, max_bytes: int) -> None:
    """Palette-quantise an oversized PNG. Flat charts lose nothing visually."""
    try:
        from PIL import Image
    except ImportError:
        return
    for colors in (192, 128, 96, 64):
        with Image.open(path) as canvas:
            quantised = canvas.convert("RGB").quantize(
                colors=colors, method=Image.MEDIANCUT)
        quantised.save(path, "PNG", optimize=True, compress_level=9)
        if path.stat().st_size <= max_bytes:
            return


# ---------------------------------------------------------------------------
# Honesty primitives
# ---------------------------------------------------------------------------
@dataclass
class LogLimits:
    """What ``loglim`` had to do to the data, so the caller can say it."""
    lo: float
    hi: float
    floor: float
    at_floor: int
    nonpositive: int
    total: int
    smallest: float = math.nan
    precision: float = math.nan

    @property
    def clamped(self) -> int:
        return self.at_floor

    def note(self, precision_label: str = "machine precision") -> str:
        if not self.clamped:
            return ""
        reach = (f", min {self.smallest:.0e}"
                 if math.isfinite(self.smallest) and self.smallest > 0
                 else ", including exact zeros")
        return (f"{self.clamped:,}/{self.total:,} below {self.floor:.0e}"
                f"{reach}\n{precision_label} — drawn on the floor line, "
                "not dropped")


def loglim(ax: Any, values: Iterable[float], *, axis: str = "y",
           floor: float | None = None, quantile: float = 0.02,
           headroom: float = 1.6, draw_floor: bool = True) -> LogLimits:
    """Set honest log limits, flooring the axis under the *bulk* of the data.

    Two different thresholds matter and conflating them is what produced a
    panel spanning 1e-1 to 1e-17 with every real value crushed into the top
    decade:

    ``floor``   the physical precision limit. Anything at or below it is not a
                measurement, it is noise at machine precision.
    the axis    is floored just under the bulk, so the decades the data
                actually occupies get the ink.

    Values below the axis floor are *not* dropped: the caller pins them with
    :func:`clamp_to_floor`, the floor line is drawn, and the count plus the
    true smallest value come back so the figure can state both.
    """
    data = np.asarray(list(values), dtype=float)
    total = int(data.size)
    finite = data[np.isfinite(data)]
    nonpositive = int((finite <= 0).sum())
    positive = finite[finite > 0]
    precision = floor if floor is not None else 0.0
    if positive.size == 0:
        return LogLimits(1e-3, 1.0, floor or 1e-16, nonpositive, nonpositive,
                         total, math.nan, precision)

    # The bulk is everything above the precision limit; the axis is scaled to
    # the bulk, never to the noise underneath it.
    bulk = positive[positive > precision] if precision else positive
    if bulk.size == 0:
        bulk = positive
    axis_floor = float(np.quantile(bulk, quantile)) / headroom
    if precision:
        axis_floor = max(axis_floor, precision)
    axis_floor = 10 ** math.floor(math.log10(axis_floor))

    below = finite[~(finite > axis_floor)]
    at_floor = int(below.size)
    smallest = float(below[below > 0].min()) if (below > 0).any() else math.nan
    hi = float(bulk.max()) * headroom
    lo = axis_floor / headroom

    setter = ax.set_ylim if axis == "y" else ax.set_xlim
    scaler = ax.set_yscale if axis == "y" else ax.set_xscale
    scaler("log")
    setter(lo, hi)

    if draw_floor and at_floor:
        t = theme()
        drawer = ax.axhline if axis == "y" else ax.axvline
        drawer(axis_floor, color=t.rule, linewidth=0.9, linestyle=(0, (2, 2)),
               zorder=1)
    return LogLimits(lo, hi, float(axis_floor), at_floor, nonpositive, total,
                     smallest, precision)


def clamp_to_floor(values: Iterable[float], floor: float) -> np.ndarray:
    """Pin non-positive and sub-floor values onto the floor line."""
    data = np.asarray(list(values), dtype=float)
    out = np.where(np.isfinite(data) & (data > floor), data, floor)
    return out


def share_y(axes: Sequence[Any], reason: str | None = None) -> None:
    """Force a common y-range across panels, or record why not."""
    flat = [ax for ax in np.ravel(np.asarray(axes, dtype=object)) if ax.get_visible()]
    if reason:
        print(f"  note: y-axis not shared — {reason}")
        return
    if len(flat) < 2:
        return
    lows, highs = zip(*(ax.get_ylim() for ax in flat))
    lo, hi = min(lows), max(highs)
    for ax in flat:
        ax.set_ylim(lo, hi)


def annotate_n(ax: Any, n: int, *, excluded: int = 0, what: str = "",
               loc: str = "upper right", extra: str = "") -> None:
    """Sample size and exclusions, same corner in every figure."""
    bits = [f"n = {n:,}" + (f" {what}" if what else "")]
    if excluded:
        bits.append(f"{excluded:,} excluded")
    if extra:
        bits.append(extra)
    text = "\n".join(bits)
    x, ha = (0.985, "right") if "right" in loc else (0.015, "left")
    y, va = (0.985, "top") if "upper" in loc else (0.015, "bottom")
    ax.text(x, y, text, transform=ax.transAxes, ha=ha, va=va,
            fontsize=FONT_PT["annot"] - 0.5, color=theme().muted, zorder=6)


def tolerance_band(ax: Any, limit: float, *, label: str = "tolerance",
                   orient: str = "y") -> None:
    """One treatment for every 'inside tolerance' region."""
    t = theme()
    span = ax.axhspan if orient == "y" else ax.axvspan
    span(0.0, limit, color=t.band, alpha=0.10, linewidth=0, zorder=0)
    line = ax.axhline if orient == "y" else ax.axvline
    line(limit, color=t.band, linewidth=1.2, linestyle=(0, (4, 2)), zorder=1)
    if orient == "y":
        ax.text(0.985, limit, f" {label}", transform=ax.get_yaxis_transform(),
                ha="right", va="bottom", fontsize=FONT_PT["annot"] - 0.5,
                color=t.band)
    else:
        ax.text(limit, 0.985, f" {label}", transform=ax.get_xaxis_transform(),
                ha="left", va="top", fontsize=FONT_PT["annot"] - 0.5,
                color=t.band)



@dataclass
class Fit:
    slope: float
    intercept: float
    residual: float
    n: int

    @property
    def reportable(self) -> bool:
        """Three points is the minimum honest basis for a slope."""
        return self.n >= 3

    def label(self, expected: float | None = None) -> str:
        if not self.reportable:
            return f"n = {self.n} — too few points for a rate"
        text = f"measured order {self.slope:.2f}"
        if self.residual == self.residual:
            text += f" (r² = {self.residual:.3f})"
        if expected is not None:
            text += f", theory {expected:g}"
        return text


def fit_loglog(x: Sequence[float], y: Sequence[float]) -> Fit:
    xs = np.asarray(list(x), dtype=float)
    ys = np.asarray(list(y), dtype=float)
    keep = np.isfinite(xs) & np.isfinite(ys) & (xs > 0) & (ys > 0)
    xs, ys = np.log10(xs[keep]), np.log10(ys[keep])
    if xs.size < 2:
        return Fit(math.nan, math.nan, math.nan, int(xs.size))
    slope, intercept = np.polyfit(xs, ys, 1)
    pred = slope * xs + intercept
    ss_res = float(((ys - pred) ** 2).sum())
    ss_tot = float(((ys - ys.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else math.nan
    return Fit(float(slope), float(intercept), r2, int(xs.size))


def convergence(ax: Any, x: Sequence[float], y: Sequence[float], *,
                name: str, expected_order: float | None = None,
                label: str | None = None) -> Fit:
    """Plot a convergence series and state its measured slope.

    With fewer than three points no line is drawn between the markers: two
    points and a straight line read as a rate, and they are not one.
    """
    st = series(name, label)
    fit = fit_loglog(x, y)
    xs, ys = np.asarray(list(x), float), np.asarray(list(y), float)
    text = f"{st.label} — {fit.label(expected_order)}"
    if fit.reportable:
        ax.plot(xs, ys, color=st.color, linestyle=st.dash, marker=st.marker,
                markersize=6, markeredgecolor=theme().bg, markeredgewidth=0.6,
                linewidth=2.0, label=text, zorder=3)
    else:
        ax.plot(xs, ys, color=st.color, linestyle="none", marker=st.marker,
                markersize=7, markeredgecolor=theme().bg, markeredgewidth=0.6,
                label=text, zorder=3)
    ax.set_xscale("log")
    ax.set_yscale("log")
    return fit


def colorbar(fig: Any, mappable: Any, *, label: str, unit: str,
             clipped_high: bool = False, clipped_low: bool = False,
             ax: Any = None, **kwargs: Any):
    """Colourbar in SI units. Refuses to draw without a unit string."""
    if not unit:
        raise ValueError("colorbar requires a unit — an unlabelled field scale "
                         "is not readable")
    vmin, vmax = mappable.get_clim()
    scale, unit_label = si_prefix(max(abs(vmin), abs(vmax)), unit)
    bar = fig.colorbar(mappable, ax=ax, **kwargs)
    bar.set_label(f"{label}  [{unit_label}]", fontsize=FONT_PT["label"],
                  color=theme().ink)
    bar.formatter = FuncFormatter(
        lambda v, _: ("≥" if clipped_high and v >= vmax else
                      "≤" if clipped_low and v <= vmin else "") + f"{v / scale:g}")
    bar.update_ticks()
    bar.outline.set_edgecolor(theme().rule)
    bar.ax.tick_params(labelsize=FONT_PT["tick"], colors=theme().muted)
    for text in bar.ax.get_yticklabels():
        text.set_color(theme().ink)
    return bar


def write_manifest(path: Path, manifest: dict[str, Any], *,
                   git_rev: str | None = None) -> None:
    """Persist the manifest built up by ``finish`` calls."""
    from datetime import datetime, timezone

    manifest.setdefault("images", [])
    manifest["generated_utc"] = datetime.now(timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")
    if git_rev:
        manifest["git_rev"] = git_rev
    manifest["images"].sort(key=lambda item: item["file"])
    path.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    print(f"  wrote {path.name}  ({len(manifest['images'])} images)")


use("light")
