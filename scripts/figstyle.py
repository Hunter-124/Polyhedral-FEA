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
  ``ratio_bars``      "inside tolerance" as one normalised bar against a 1.0
                      limit, instead of a value restated three ways.
  ``tolerance_band``  one visual treatment for a tolerance region.
  ``convergence``     fits and prints the measured slope instead of drawing a
                      two-point line that invites the eye to extrapolate.

Series are keyed by *name*, not by call order, so ``graded_tet`` is the same
colour in every figure forever -- and every categorical series carries a marker
and a dash pattern in lockstep with its colour, so no chart ever distinguishes
series by colour alone.
"""
from __future__ import annotations

import json
import math
import re
import textwrap
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

ROOT = Path(__file__).resolve().parents[1]

__all__ = [
    "use", "theme", "figure", "finish", "panel_title", "axes_off",
    "series", "series_handles", "SERIES_ORDER", "register_series",
    "field_cmap", "field_lut", "colorbar",
    "loglim", "share_y", "annotate_n", "ratio_bars", "tolerance_band",
    "convergence", "si", "unit_formatter", "footer_source",
    "font_path", "assert_glyphs", "GLYPHS_REQUIRED",
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
    # meshers -- the most widely shared vocabulary in the repo
    for slot, name in enumerate(["hybrid_zoo", "graded_tet", "hex",
                                 "hybrid_vem", "varyhedron", "cvt_poly"]):
        register_series(name, slot)
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


def footer_source(*paths: Path | str, note: str = "", n: int | None = None) -> str:
    """Provenance line: POSIX slashes, repo-relative, record count."""
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

    t = theme()
    # Wrap to the canvas: a subtitle or footer running off the right edge was
    # a recurring defect, and every caption here is composed from data whose
    # length is not known when the figure is laid out.
    def fit(text: str, role: str) -> tuple[str, int]:
        if not text:
            return "", 0
        columns = max(24, int(dims[0] * 72.0 / (0.55 * FONT_PT[role])))
        lines: list[str] = []
        for paragraph in text.split("\n"):
            lines.extend(textwrap.wrap(paragraph, columns) or [""])
        return "\n".join(lines), len(lines)

    title_text, title_lines = fit(title, "title")
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
    dpi = max(120, round(TARGET_PX / width_in))
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


def ratio_bars(ax: Any, labels: Sequence[str], measured: Sequence[float],
               tolerance: Sequence[float], *, unit: str = "%",
               digits: int = 3) -> list[float]:
    """Measured-over-tolerance as one bar per case against a 1.0 limit.

    Replaces the three-way restatement (bar length + absolute value + percent
    of budget) and removes the flattering x-range that came from plotting five
    different tolerances on one absolute axis.
    """
    t = theme()
    ratios = [m / tol if tol else math.nan for m, tol in zip(measured, tolerance)]
    ypos = np.arange(len(labels))[::-1]
    colors = [t.ok if r <= 1.0 else t.bad for r in ratios]
    ax.barh(ypos, ratios, height=0.62, color=colors, zorder=3,
            edgecolor="none")
    # Redundant encoding: pass/fail is also hatched, not colour alone.
    for y, r in zip(ypos, ratios):
        if r > 1.0:
            # linewidth drives the hatch stroke width in matplotlib, and the
            # stroke must contrast with the bar it overlays — a bad-on-bad
            # hatch leaves a failing bar distinguished by colour alone.
            ax.barh([y], [r], height=0.62, color="none", hatch="///",
                    edgecolor=t.panel, linewidth=0.8, zorder=4)
    ax.axvline(1.0, color=t.rule, linewidth=1.4, zorder=5)
    ax.set_yticks(ypos)
    ax.set_yticklabels(labels)
    ax.set_xlim(0, max(1.15, max([r for r in ratios if math.isfinite(r)],
                                 default=1.0) * 1.18))
    ax.grid(axis="y", visible=False)
    for y, r, m, tol in zip(ypos, ratios, measured, tolerance):
        ax.text(r + 0.02, y, f"  {m:.{digits}g}{unit} of {tol:g}{unit}",
                va="center", ha="left", fontsize=FONT_PT["annot"] - 0.5,
                color=t.ink, zorder=6)
    return ratios


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
