#!/usr/bin/env python3
"""PolyMesh showcase renderer -- solves the demo parts and renders the gallery.

Produces every image in ``docs/assets/showcase/`` except ``gui_studio.png``
(an application screenshot, captured separately), plus ``manifest.json``
recording the exact command that made each one.

Design notes
------------
* VTK/pyvista renders **geometry only**, on a transparent background. The
  background gradient, title block, colour bar and captions are drawn with PIL
  afterwards. That keeps full typographic control (real Liberation Sans, real
  superscripts), gives an exact 3-stop Studio gradient, and avoids VTK's
  cramped default scalar bar entirely.
* Everything is supersampled by ``SUPERSAMPLE`` and Lanczos-downsampled to the
  final 1920x1440, which anti-aliases geometry and text together.
* The von Mises colour range is clipped robustly. A perfectly clamped face is a
  stress singularity: the peak nodal value can be millions of times the bulk
  field, which would render the whole part flat blue. Captions and the manifest
  always report BOTH the clipped range and the true peak, and the colour bar's
  top tick is prefixed with an explicit "≥" when clipping is active.

Usage
-----
    python3 scripts/render_showcase.py --all              # everything
    python3 scripts/render_showcase.py --only plate_hole   # one part
    python3 scripts/render_showcase.py --only architecture
    python3 scripts/render_showcase.py --list
    python3 scripts/render_showcase.py --stress out.vtu --out x.png
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Studio palette (shared contract with the GUI, plot_benchmarks, tiler)
# ---------------------------------------------------------------------------
CHROME_BG = "#0E1116"
PANEL_BG = "#161B22"
HEADER_BG = "#1C2330"
BORDER = "#2A3240"
TEXT = "#E6EAF0"
TEXT_DIM = "#8A93A3"
ACCENT = "#4CC2FF"
ACCENT_DIM = "#2A6E96"

VIEWPORT_TOP = "#1B2028"
VIEWPORT_MID = "#141922"
VIEWPORT_BOTTOM = "#0F131A"
PART_DEFAULT = "#8B95A5"

# ---------------------------------------------------------------------------
# Output geometry
# ---------------------------------------------------------------------------
OUT_W, OUT_H = 1920, 1440
SUPERSAMPLE = 2
BAR_ZONE = 330          # right-hand strip reserved for the colour bar
# Vertical bands reserved for the title block and the caption, so a stress
# render can be framed tightly without the model running under the text.
TOP_BAND = 200
BOT_BAND = 156
# Comparison tiles: aspect set near the plate's ~2:1 projected aspect at these
# view angles so the mesh fills the tile instead of floating in dead space.
TILE_W, TILE_H = 1500, 820      # mesh tiles for compare_meshers
# compare_grading is a near-square close-up window, so it gets its own aspect.
GRADE_W, GRADE_H = 1300, 1150   # mesh tiles for compare_grading

REPO = Path(__file__).resolve().parent.parent
PARTS_DIR = REPO / "tests/fixtures/parts"
CACHE = REPO / "build/showcase"        # gitignored VTU cache
CLI = REPO / "build/apps/cli/polymesh"
SOLVE_TIMEOUT = 300

FONT_DIRS = (
    "/usr/share/fonts/liberation-sans-fonts",
    "/usr/share/fonts/truetype/liberation",
    "/usr/share/fonts/dejavu-sans-fonts",
    "/usr/share/fonts/truetype/dejavu",
)
FONT_REGULAR = ("LiberationSans-Regular.ttf", "DejaVuSans.ttf")
FONT_BOLD = ("LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf")


def rel(path: Path) -> str:
    """Repo-relative string when possible, absolute otherwise.

    The single-image modes accept an arbitrary --out, which may live outside the
    repository; Path.relative_to would raise on those.
    """
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return str(path)


def cli_path() -> Path:
    for cand in (CLI, REPO / "polymesh"):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    raise SystemExit(
        "polymesh CLI not found; build it first (expected build/apps/cli/polymesh)"
    )


def load_font(names: tuple[str, ...], size: int) -> ImageFont.FreeTypeFont:
    for directory in FONT_DIRS:
        for name in names:
            path = Path(directory) / name
            if path.is_file():
                return ImageFont.truetype(str(path), size)
    return ImageFont.load_default(size)


# ---------------------------------------------------------------------------
# fea_colormap -- verbatim port of apps/gui/viewport.cpp:127-133
# ---------------------------------------------------------------------------
def fea_colormap(t: float) -> tuple[float, float, float]:
    """blue -> cyan -> green -> yellow -> red, exactly as the GUI computes it."""
    t = min(max(t, 0.0), 1.0)
    r = min(max(min(4.0 * t - 2.0, 4.0 - 4.0 * t) + 1.0, 0.0), 1.0)
    g = min(max(min(4.0 * t, 3.4 - 3.0 * t), 0.0), 1.0)
    b = min(max(2.0 - 4.0 * t, 0.0), 1.0)
    return (1.0 if t > 0.75 else r * 0.9, g * 0.85, b)


def fea_lut(n: int = 256) -> np.ndarray:
    """(n, 3) float LUT in [0,1]."""
    return np.array([fea_colormap(i / (n - 1)) for i in range(n)], dtype=float)


def fea_cmap():
    from matplotlib.colors import ListedColormap

    return ListedColormap(fea_lut(256), name="fea_colormap")


def hex_to_rgb(h: str) -> tuple[int, int, int]:
    h = h.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# Part / render tables
# ---------------------------------------------------------------------------
@dataclass
class Part:
    name: str
    title: str
    step: str
    mesher: str
    h: float
    E: float
    nu: float
    view: tuple[float, float, float]
    up: tuple[float, float, float]
    bc_note: str
    margin: float = 1.12
    fix_box: tuple[float, ...] | None = None
    load_box: tuple[float, ...] | None = None
    # Target peak displacement as a fraction of the bbox diagonal. Kept small:
    # warping a 10 mm-thick plate by 18 mm to "show" bending shears the hole
    # rim into something the solve never predicted.
    warp_frac: float = 0.02
    # None = let color_range pick the least-clipping readable range.
    clip_pct: float | None = None


# fix/load boxes come from the *.case.json BC contracts in tests/fixtures/parts.
# plate_hole/cantilever use the CLI default selection (fix min-x face, traction
# on the max-x face), which matches their contract's fixed face.
PARTS: list[Part] = [
    Part(
        name="plate_hole",
        title="Plate with hole",
        step="plate_hole.step",
        mesher="graded",
        h=0.006,
        E=2.1e11,
        nu=0.3,
        view=(0.25, -0.60, 1.00),
        # up=(0, dz, -dy) is the unique up that is perpendicular to the view
        # AND has no world-x component, so the plate's long axis lands exactly
        # horizontal on screen instead of rolling off-axis.
        up=(0.0, 1.00, 0.60),
        margin=1.03,
        bc_note="min-x face fixed, traction on max-x face (CLI default BC set)",
    ),
    Part(
        name="cantilever",
        title="Cantilever beam",
        step="cantilever.step",
        mesher="graded",
        h=0.03,
        E=2.0e11,
        nu=0.3,
        view=(-0.42, -1.00, 0.46),
        up=(0.0, 0.0, 1.0),
        margin=1.06,
        # A cantilever's story is its tip deflection, so it gets a larger warp
        # target than the plate; x200 on a 1 m beam is ~36 mm of droop.
        warp_frac=0.05,
        bc_note="root face (x < 1e-3 m) fixed, tip face loaded",
    ),
    Part(
        name="cylinder",
        title="Cylinder",
        step="cylinder.step",
        mesher="graded",
        h=0.012,
        E=2.0e11,
        nu=0.3,
        view=(1.00, -0.95, 0.30),
        up=(0.0, 0.0, 1.0),
        margin=1.04,
        fix_box=(-1, -1, -1, 1, 1, 0.015),
        load_box=(-1, -1, 0.195, 1, 1, 1),
        bc_note="base z < 0.015 m fixed, cap z > 0.195 m loaded",
    ),
    Part(
        name="sphere",
        title="Sphere",
        step="sphere.step",
        mesher="graded",
        h=0.008,
        E=2.0e11,
        nu=0.3,
        view=(1.00, -1.00, 0.45),
        up=(0.0, 0.0, 1.0),
        margin=1.06,
        fix_box=(-1, -1, -1, 1, 1, -0.04),
        load_box=(-1, -1, 0.04, 1, 1, 1),
        bc_note="cap z < -0.04 m fixed, cap z > 0.04 m loaded",
    ),
    Part(
        name="icecream_cone",
        title="Cone and ball",
        step="icecream_cone.step",
        mesher="graded",
        h=0.010,
        E=2.0e11,
        nu=0.3,
        view=(1.00, -1.00, 0.30),
        up=(0.0, 0.0, 1.0),
        margin=1.05,
        fix_box=(-1, -1, -1, 1, 1, 0.02),
        load_box=(-1, -1, 0.10, 1, 1, 1),
        bc_note="base z < 0.02 m fixed, ball z > 0.10 m loaded",
    ),
]

FLAGSHIP = "plate_hole"

# Hero: the same solve as the flagship gallery image, shot from a much lower
# oblique angle (~34 deg above the plate vs ~57 deg) so the bending deflection
# and the stress lobes around the hole read in 3D.
#
# Deliberately NOT a tight crop on the hole: at a reproducible h = 6 mm the
# Cartesian grid-fill opening is a ~10-gon, so an extreme close-up shows off
# mesh coarseness rather than the solve. Framing the whole plate also keeps the
# model uncut. HERO_UP = (0, dz, -dy) keeps the plate's long axis horizontal.
HERO_FOCUS = None
HERO_RADIUS = None
HERO_VIEW = (0.35, -0.90, 0.65)
HERO_UP = (0.0, 0.65, 0.90)


@dataclass
class MeshTile:
    """A mesh-only render used inside a comparison grid."""

    tag: str
    label: str
    part: str
    mesher: str
    h: float
    no_feature: bool = False
    view: tuple[float, float, float] = (0.25, -0.60, 1.00)
    # Roll-free up for that view: (0, dz, -dy) keeps the plate's long axis level.
    up: tuple[float, float, float] = (0.0, 1.00, 0.60)
    size: tuple[int, int] = (TILE_W, TILE_H)
    # Optional shared close-up window: frame a box of half-extent `window`
    # (metres, in x/y) about `focus` instead of the whole part. Set on every
    # tile of a grid so all panels share one camera.
    focus: tuple[float, float, float] | None = None
    window: float | None = None
    stats: dict = field(default_factory=dict)


# compare_meshers: one part, matched h, three topologies.
MESHER_TILES = [
    MeshTile("cmp_tet", "tet  \u00b7  Cartesian grid-fill tet4", "plate_hole", "tet", 0.006),
    MeshTile("cmp_graded", "graded  \u00b7  feature-graded tet4", "plate_hole", "graded", 0.006),
    # At this part and h the hybrid zoo emits an all-hex mesh with 2:1 fine
    # cells and transition=0, so the label names that rather than the fan
    # transition cells the mesher is also capable of but does not produce here.
    MeshTile("cmp_hybrid", "hybrid  \u00b7  all-hex bulk + 2:1 fine cells", "plate_hole", "hybrid", 0.006),
]

# compare_grading: same mesher, only the sizing field differs. h values are
# tuned so the element budgets match to ~0.2% (see the figure footer).
#
# Framed as a close-up on the hole: at whole-plate scale the refinement zone is
# a few percent of the frame and the two panels look identical. GRADE_WINDOW is
# a half-extent, so 20 mm frames 40 mm == 4.0 hole radii across. The view is
# nearer top-down than the mesher grid so the refinement rings read clearly.
# plate_hole.step: r = 0.01 m hole centred on the origin (plate 0.2 x 0.1 x 0.01 m).
HOLE_RADIUS_M = 0.01
GRADE_FOCUS = (0.0, 0.0, 0.005)
GRADE_WINDOW = 0.020
GRADE_VIEW = (0.15, -0.35, 1.00)
GRADE_UP = (0.0, 1.00, 0.35)
GRADING_TILES = [
    MeshTile("grade_uniform", "uniform sizing field  (--no-feature)", "plate_hole",
             "graded", 0.0051, no_feature=True, size=(GRADE_W, GRADE_H),
             view=GRADE_VIEW, up=GRADE_UP, focus=GRADE_FOCUS, window=GRADE_WINDOW),
    MeshTile("grade_feature", "feature-graded sizing field", "plate_hole",
             "graded", 0.0056, size=(GRADE_W, GRADE_H),
             view=GRADE_VIEW, up=GRADE_UP, focus=GRADE_FOCUS, window=GRADE_WINDOW),
]


# ---------------------------------------------------------------------------
# CLI driving
# ---------------------------------------------------------------------------
def run_cli(argv: list[str], timeout: int = SOLVE_TIMEOUT) -> tuple[str, float]:
    """Run the polymesh CLI, streaming output. Returns (stdout, wall seconds)."""
    print("    $ " + " ".join(argv), flush=True)
    start = time.perf_counter()
    proc = subprocess.Popen(
        argv, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    lines: list[str] = []
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.append(line)
            print("      " + line.rstrip(), flush=True)
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise SystemExit(f"polymesh timed out after {timeout}s: {' '.join(argv)}")
    wall = time.perf_counter() - start
    if proc.returncode != 0:
        raise SystemExit(f"polymesh failed (rc={proc.returncode}): {' '.join(argv)}")
    return "".join(lines), wall


def solve_argv(part: Part, out: Path) -> list[str]:
    argv = [
        str(cli_path()),
        "solve",
        rel(PARTS_DIR / part.step),
        "-o",
        rel(out),
        "-h",
        f"{part.h:g}",
        "--mesher",
        part.mesher,
        "-E",
        f"{part.E:g}",
        "-nu",
        f"{part.nu:g}",
    ]
    if part.fix_box:
        argv += ["--fix-box", *[f"{v:g}" for v in part.fix_box]]
    if part.load_box:
        argv += ["--load-box", *[f"{v:g}" for v in part.load_box]]
    return argv


def mesh_argv(tile: MeshTile, out: Path) -> list[str]:
    argv = [
        str(cli_path()),
        "mesh",
        rel(PARTS_DIR / f"{tile.part}.step"),
        "-h",
        f"{tile.h:g}",
        "--mesher",
        tile.mesher,
        "-o",
        rel(out),
    ]
    if tile.no_feature:
        argv.append("--no-feature")
    return argv


def parse_solve_stdout(text: str) -> dict:
    """Pull the numbers the CLI already prints on its 'solve:' summary line."""
    import re

    out: dict = {}
    m = re.search(
        r"solve:\s*(\d+)\s+nodes,\s*(\d+)\s+elems.*?max von Mises\s+([\d.eE+-]+)\s*Pa"
        r".*?max \|u\|\s+([\d.eE+-]+)\s*m",
        text,
    )
    if m:
        out["nodes"] = int(m.group(1))
        out["elems"] = int(m.group(2))
        out["max_von_mises_pa"] = float(m.group(3))
        out["max_disp_m"] = float(m.group(4))
    return out


# ---------------------------------------------------------------------------
# Camera
# ---------------------------------------------------------------------------
def fit_camera(
    plotter,
    points: np.ndarray,
    view_dir,
    up,
    width: int,
    height: int,
    margin: float = 1.12,
    focus: np.ndarray | None = None,
) -> None:
    """Frame `points` exactly: solve for the distance that fits the silhouette.

    Guarantees nothing is cut off for any view direction, unlike a
    bounding-sphere estimate which is either loose or (for elongated parts)
    wrong once the aspect ratio is taken into account.
    """
    d = np.asarray(view_dir, dtype=float)
    d /= np.linalg.norm(d)
    u = np.asarray(up, dtype=float)
    u = u - np.dot(u, d) * d
    if np.linalg.norm(u) < 1e-9:                     # up parallel to view
        alt = np.array([0.0, 0.0, 1.0])
        if abs(np.dot(alt, d)) > 0.9:
            alt = np.array([0.0, 1.0, 0.0])
        u = alt - np.dot(alt, d) * d
    u /= np.linalg.norm(u)
    r = np.cross(u, d)
    r /= np.linalg.norm(r)

    center = np.asarray(focus, dtype=float) if focus is not None else 0.5 * (
        points.min(axis=0) + points.max(axis=0)
    )
    q = points - center
    depth = q @ d          # positive == toward the camera
    sx = np.abs(q @ r)
    sy = np.abs(q @ u)

    fov = math.radians(plotter.camera.view_angle)    # vertical FOV
    half_v = math.tan(fov / 2.0)
    half_h = half_v * (width / height)
    dist = max((depth + sy / half_v).max(), (depth + sx / half_h).max()) * margin
    dist = max(dist, 1e-9)

    plotter.camera.focal_point = tuple(center)
    plotter.camera.position = tuple(center + d * dist)
    plotter.camera.up = tuple(u)
    plotter.renderer.ResetCameraClippingRange()


# ---------------------------------------------------------------------------
# PIL compositing
# ---------------------------------------------------------------------------
def gradient_canvas(w: int, h: int) -> Image.Image:
    """Exact 3-stop vertical viewport gradient (top / mid / bottom)."""
    top = np.array(hex_to_rgb(VIEWPORT_TOP), dtype=float)
    mid = np.array(hex_to_rgb(VIEWPORT_MID), dtype=float)
    bot = np.array(hex_to_rgb(VIEWPORT_BOTTOM), dtype=float)
    ys = np.linspace(0.0, 1.0, h)[:, None]
    upper = top + (mid - top) * (ys / 0.5)
    lower = mid + (bot - mid) * ((ys - 0.5) / 0.5)
    col = np.where(ys < 0.5, upper, lower)
    return Image.fromarray(
        np.repeat(col[:, None, :], w, axis=1).round().astype(np.uint8), "RGB"
    )


def sci_parts(v: float) -> tuple[str, str | None]:
    """Split a value into (mantissa string, exponent string) for typeset labels."""
    if v == 0 or not math.isfinite(v):
        return ("0", None)
    exp = int(math.floor(math.log10(abs(v))))
    mant = round(v / (10.0**exp), 2)
    if abs(mant) >= 10.0:
        mant /= 10.0
        exp += 1
    return (f"{mant:.2f}", str(exp))


def draw_sci(
    draw: ImageDraw.ImageDraw, xy, value: float, font, font_sup, fill, prefix: str = ""
) -> int:
    """Draw `prefix` + mantissa + x10^exp with a real raised exponent."""
    x, y = xy
    mant, exp = sci_parts(value)
    text = prefix + mant
    draw.text((x, y), text, font=font, fill=fill)
    x += draw.textlength(text, font=font)
    if exp is None:
        return int(x - xy[0])
    body = "\u00d710"
    draw.text((x, y), body, font=font, fill=fill)
    x += draw.textlength(body, font=font)
    asc = font.getbbox("8")[3] - font.getbbox("8")[1]
    draw.text((x, y - asc * 0.42), exp, font=font_sup, fill=fill)
    x += draw.textlength(exp, font=font_sup)
    return int(x - xy[0])


def wrap(draw, text: str, font, max_w: int) -> list[str]:
    words, lines, cur = text.split(), [], ""
    for word in words:
        trial = f"{cur} {word}".strip()
        if draw.textlength(trial, font=font) <= max_w or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def draw_colorbar(
    draw: ImageDraw.ImageDraw,
    x: int,
    y0: int,
    y1: int,
    lo: float,
    hi: float,
    clipped: bool,
    s: int,
) -> None:
    """Vertical colour bar on the fea_colormap, with typeset scientific ticks."""
    bar_w = 54 * s
    f_title = load_font(FONT_BOLD, 25 * s)
    f_tick = load_font(FONT_REGULAR, 24 * s)
    f_sup = load_font(FONT_REGULAR, 16 * s)

    # heading, above the bar
    draw.text((x, y0 - 74 * s), "von Mises", font=f_title, fill=TEXT)
    draw.text((x, y0 - 44 * s), "stress (Pa)", font=f_title, fill=TEXT)

    lut = fea_lut(512)
    height = y1 - y0
    for i in range(height):
        t = 1.0 - i / max(1, height - 1)
        rgb = lut[min(len(lut) - 1, int(t * (len(lut) - 1)))]
        col = tuple(int(round(c * 255)) for c in rgb)
        draw.rectangle([x, y0 + i, x + bar_w, y0 + i], fill=col)
    draw.rectangle([x, y0, x + bar_w, y1], outline=BORDER, width=max(1, s))

    n_ticks = 6
    for k in range(n_ticks):
        t = k / (n_ticks - 1)
        val = lo + t * (hi - lo)
        ty = int(y1 - t * height)
        draw.line([x + bar_w, ty, x + bar_w + 9 * s, ty], fill=BORDER, width=max(1, s))
        prefix = "\u2265" if (clipped and k == n_ticks - 1) else ""
        draw_sci(
            draw,
            (x + bar_w + 15 * s, ty - 14 * s),
            val,
            f_tick,
            f_sup,
            TEXT,
            prefix=prefix,
        )


def compose(
    render_rgba: np.ndarray,
    title: str | None,
    meta: str | None,
    footer_text: str | None,
    bar: dict | None,
    out_size: tuple[int, int],
    s: int,
    inset: tuple[int, int] = (0, 0),
) -> Image.Image:
    """Composite a transparent 3D render onto the Studio viewport background.

    `inset` is where the render lands, in final (pre-supersample) pixels. Stress
    renders inset vertically so the model occupies a band between the title and
    the caption and can therefore be framed tightly without colliding with text.
    """
    w, h = out_size[0] * s, out_size[1] * s
    canvas = gradient_canvas(w, h)
    layer = Image.fromarray(render_rgba, "RGBA")
    canvas.paste(layer, (inset[0] * s, inset[1] * s), layer)
    draw = ImageDraw.Draw(canvas)

    # Readability scrims: a soft vignette toward the chrome colour behind each
    # text band, so a bright stress lobe reaching the band edge can never sit
    # under the title or the caption.
    if title or footer_text:
        scrim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scrim)
        base = hex_to_rgb(CHROME_BG)
        if title:
            band = int(0.150 * h)
            for i in range(band):
                sd.rectangle([0, i, w, i], fill=(*base, int(170 * (1.0 - i / band) ** 1.6)))
        if footer_text:
            band = int(0.120 * h)
            for i in range(band):
                sd.rectangle([0, h - 1 - i, w, h - 1 - i],
                             fill=(*base, int(170 * (1.0 - i / band) ** 1.6)))
        canvas = Image.alpha_composite(canvas.convert("RGBA"), scrim).convert("RGB")
        draw = ImageDraw.Draw(canvas)

    if title:
        f_title = load_font(FONT_BOLD, 40 * s)
        draw.text((56 * s, 44 * s), title, font=f_title, fill=TEXT)
        ty = 44 * s + (f_title.getbbox(title)[3] - f_title.getbbox(title)[1]) + 18 * s
        draw.rectangle([56 * s, ty, 56 * s + 96 * s, ty + 3 * s], fill=ACCENT)
        if meta:
            f_meta = load_font(FONT_REGULAR, 25 * s)
            draw.text((56 * s, ty + 16 * s), meta, font=f_meta, fill=TEXT_DIM)

    if bar:
        draw_colorbar(
            draw,
            x=w - (BAR_ZONE - 46) * s,
            y0=(TOP_BAND + 58) * s,
            y1=(OUT_H - BOT_BAND - 46) * s,
            lo=bar["lo"],
            hi=bar["hi"],
            clipped=bar["clipped"],
            s=s,
        )

    if footer_text:
        f_foot = load_font(FONT_REGULAR, 22 * s)
        lines = wrap(draw, footer_text, f_foot, w - 112 * s)
        line_h = 30 * s
        y = h - 42 * s - line_h * len(lines)
        for line in lines:
            draw.text((56 * s, y), line, font=f_foot, fill=TEXT_DIM)
            y += line_h
        f_mark = load_font(FONT_BOLD, 23 * s)
        mark = "PolyMesh"
        draw.text(
            (w - 56 * s - draw.textlength(mark, font=f_mark), h - 66 * s),
            mark,
            font=f_mark,
            fill=ACCENT_DIM,
        )

    return canvas.resize(out_size, Image.LANCZOS)


# ---------------------------------------------------------------------------
# Renderers
# ---------------------------------------------------------------------------
def _plotter(width: int, height: int, lighting: str):
    import pyvista as pv

    p = pv.Plotter(off_screen=True, window_size=(width, height), lighting=lighting)
    p.enable_anti_aliasing("ssaa")
    return p


def _shot(p) -> np.ndarray:
    return np.asarray(p.screenshot(transparent_background=True, return_img=True))


# Candidate upper bounds, most inclusive first. "None" means the true maximum.
CLIP_CANDIDATES = (None, 99.9, 99.5, 99.0, 98.0, 97.0, 95.0, 92.0, 90.0)
# Reject a range if more than this fraction of the visible surface falls into
# the bottom quarter of it -- that is the "everything is flat blue" failure.
CLIP_CROWD_LIMIT = 0.60


def color_range(
    vm_surface: np.ndarray, clip_pct: float | None = None
) -> tuple[float, float, bool, float | None]:
    """Pick the least-clipping colour range that still shows structure.

    Percentiles are taken over the *surface* field, because that is what the
    render actually shows; interior nodes never reach a pixel. A clamped face is
    a stress singularity, so the true peak can be millions of times the bulk
    field and using it would map the whole part to flat blue.

    Returns (lo, hi, clipped, chosen_percentile).
    """
    true_max = float(vm_surface.max())
    if not math.isfinite(true_max) or true_max <= 0.0:
        return 0.0, 1.0, False, None

    def crowding(hi: float) -> float:
        return float((vm_surface < 0.25 * hi).mean()) if hi > 0 else 1.0

    if clip_pct is not None:                       # explicit per-part override
        hi = float(np.percentile(vm_surface, clip_pct))
        return 0.0, hi, hi < true_max, clip_pct

    for pct in CLIP_CANDIDATES:
        hi = true_max if pct is None else float(np.percentile(vm_surface, pct))
        if hi <= 0.0:
            continue
        if crowding(hi) <= CLIP_CROWD_LIMIT:
            return 0.0, hi, hi < true_max, pct
    hi = float(np.percentile(vm_surface, CLIP_CANDIDATES[-1]))
    return 0.0, hi, hi < true_max, CLIP_CANDIDATES[-1]


def nice_factor(x: float) -> float:
    """Snap a warp factor to 1/2/5 x 10^k so the caption reads honestly."""
    if x <= 0 or not math.isfinite(x):
        return 1.0
    exp = math.floor(math.log10(x))
    for mant in (1.0, 2.0, 5.0):
        if x < mant * 10.0**exp * 1.5:
            return mant * 10.0**exp
    return 10.0 ** (exp + 1)


def ordinal(v: float) -> str:
    """'92' -> '92nd', '99.5' -> '99.5th'. Keeps captions readable prose."""
    if v != int(v):
        return f"{v:g}th"
    n = int(v)
    if n % 100 in (11, 12, 13):
        return f"{n}th"
    suffix = {1: "st", 2: "nd", 3: "rd"}.get(n % 10, "th")
    return f"{n}{suffix}"


def render_stress(
    vtu: Path,
    out: Path,
    *,
    title: str | None,
    meta_extra: str = "",
    footer_extra: str = "",
    view=(0.3, -0.52, 1.0),
    up=(0.0, 1.0, 0.0),
    margin: float = 1.12,
    warp_frac: float = 0.02,
    clip_pct: float | None = None,
    focus: tuple[float, float, float] | None = None,
    focus_radius: float | None = None,
) -> dict:
    """Render a solve VTU: warped surface coloured by von Mises + colour bar."""
    import pyvista as pv

    grid = pv.read(str(vtu))
    vm = np.asarray(grid.point_data["von_Mises"], dtype=float)
    disp = np.asarray(grid.point_data["displacement"], dtype=float)
    umax = float(np.linalg.norm(disp, axis=1).max())

    true_max = float(vm.max())

    bounds = np.asarray(grid.points)
    diag = float(np.linalg.norm(bounds.max(axis=0) - bounds.min(axis=0)))
    factor = nice_factor(warp_frac * diag / umax) if umax > 0 else 0.0

    grid.point_data.active_vectors_name = "displacement"
    warped = grid.warp_by_vector("displacement", factor=factor)
    surf = warped.extract_surface(algorithm="dataset_surface")

    # Range comes from the surface field: that is what the render shows.
    lo, hi, clipped, chosen_pct = color_range(
        np.asarray(surf.point_data["von_Mises"], dtype=float), clip_pct
    )

    s = SUPERSAMPLE
    w3 = OUT_W - BAR_ZONE
    h3 = OUT_H - TOP_BAND - BOT_BAND
    p = _plotter(w3 * s, h3 * s, "three lights")
    p.add_mesh(
        surf,
        scalars="von_Mises",
        cmap=fea_cmap(),
        clim=(lo, hi),
        show_edges=True,
        edge_color="#101418",
        line_width=0.9 * s,
        show_scalar_bar=False,
        ambient=0.34,
        diffuse=0.78,
        specular=0.14,
        specular_power=22,
        smooth_shading=False,
        # A through-hole's near wall faces away from the camera. Without
        # back-face culling it renders unlit and reads as a black blob; culled,
        # the lit far wall shows through and the hole looks like a hole.
        culling="back",
    )
    pts = np.asarray(surf.points)
    sel = pts
    if focus is not None and focus_radius is not None:
        f = np.asarray(focus, dtype=float)
        keep = np.linalg.norm(pts - f, axis=1) <= focus_radius
        if keep.sum() > 8:
            sel = pts[keep]
    fit_camera(p, sel, view, up, w3 * s, h3 * s, margin=margin,
               focus=np.asarray(focus, dtype=float) if focus is not None else None)
    img = _shot(p)
    p.close()

    if clipped:
        where = (
            f"clipped at the {ordinal(chosen_pct)} percentile of the visible surface"
            if chosen_pct is not None
            else "clipped"
        )
        clip_note = (
            f"colour range 0 \u2013 {hi:.3g} Pa ({where}); peak nodal value "
            f"{true_max:.3g} Pa at the clamped-face stress singularity"
        )
    else:
        clip_note = f"colour range 0 \u2013 {hi:.3g} Pa (full field range)"
    footer = f"Deformation warped \u00d7{factor:g} for visibility \u00b7 {clip_note}"
    if footer_extra:
        footer += f" \u00b7 {footer_extra}"

    canvas = compose(
        img,
        title=title,
        meta=meta_extra or None,
        footer_text=footer,
        bar={"lo": lo, "hi": hi, "clipped": clipped},
        out_size=(OUT_W, OUT_H),
        s=s,
        inset=(0, TOP_BAND),
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)
    print(f"    wrote {rel(out)}  ({OUT_W}x{OUT_H})")
    return {
        "nodes": int(grid.n_points),
        "elems": int(grid.n_cells),
        "max_von_mises_pa": true_max,
        "clip_hi_pa": hi,
        "clipped": clipped,
        "warp_factor": factor,
        "max_disp_m": umax,
        "clip_pct": chosen_pct,
        "caption_clip": clip_note,
    }


def render_mesh(
    vtu: Path,
    out: Path,
    *,
    size: tuple[int, int],
    view,
    up,
    margin: float = 1.10,
    title: str | None = None,
    meta_extra: str = "",
    footer_extra: str = "",
    # Comparison grids pass a shared point set so every tile is framed by the
    # same camera; otherwise per-mesh snap differences change the apparent
    # scale and the reader compares zoom levels instead of topology.
    fit_points: np.ndarray | None = None,
    focus: tuple[float, float, float] | None = None,
) -> dict:
    """Mesh-only render: neutral metallic tone, accent-lit, edges legible."""
    import pyvista as pv

    grid = pv.read(str(vtu))
    surf = grid.extract_surface(algorithm="dataset_surface")

    s = SUPERSAMPLE
    w, h = size
    p = _plotter(w * s, h * s, "none")
    p.add_mesh(
        surf,
        color=PART_DEFAULT,
        show_edges=True,
        edge_color="#141A22",
        # These tiles get downscaled again by the grid tiler, so the lines are
        # drawn heavier than the stress renders' to survive it.
        line_width=1.7 * s,
        ambient=0.22,
        diffuse=0.62,
        specular=0.30,
        specular_power=38,
        smooth_shading=False,
        culling="back",
        show_scalar_bar=False,
    )

    # Key / fill / rim: fill and rim carry the accent so the grey part reads as
    # lit metal against the dark viewport rather than flat clay. Intensities are
    # kept well under 1.0 in total so PART_DEFAULT stays a mid grey instead of
    # blowing out to white.
    d = np.asarray(view, dtype=float)
    d /= np.linalg.norm(d)
    key = pv.Light(position=tuple(d * 4 + np.array([1.2, 0.6, 2.0])), light_type="scene light")
    key.intensity = 0.60
    key.diffuse_color = (1.0, 1.0, 1.0)
    fill = pv.Light(position=tuple(d * 4 + np.array([-2.2, -1.4, 0.4])), light_type="scene light")
    fill.intensity = 0.26
    fill.diffuse_color = hex_to_rgb(ACCENT)
    rim = pv.Light(position=tuple(-d * 4 + np.array([0.0, 0.0, 1.6])), light_type="scene light")
    rim.intensity = 0.20
    rim.diffuse_color = hex_to_rgb(ACCENT_DIM)
    for light in (key, fill, rim):
        light.positional = False
        p.add_light(light)

    fit_camera(
        p,
        np.asarray(surf.points) if fit_points is None else fit_points,
        view, up, w * s, h * s, margin=margin,
        focus=np.asarray(focus, dtype=float) if focus is not None else None,
    )
    img = _shot(p)
    p.close()

    canvas = compose(
        img,
        title=title,
        meta=meta_extra or None,
        footer_text=footer_extra or None,
        bar=None,
        out_size=size,
        s=s,
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out)
    print(f"    wrote {rel(out)}  ({w}x{h})")
    return {"nodes": int(grid.n_points), "elems": int(grid.n_cells)}


# ---------------------------------------------------------------------------
# architecture.png -- graphviz, generated inline (no .dot file on disk)
# ---------------------------------------------------------------------------
def architecture_dot() -> str:
    n = f'fillcolor="{PANEL_BG}", color="{BORDER}", fontcolor="{TEXT}"'
    acc = f'fillcolor="{HEADER_BG}", color="{ACCENT}", fontcolor="{TEXT}"'
    # Side outputs stay subordinate but must remain legible: filling them with
    # the page colour made them near-invisible against the background.
    side = f'fillcolor="{PANEL_BG}", color="{ACCENT_DIM}", fontcolor="{TEXT_DIM}"'
    return f"""
digraph polymesh {{
  rankdir=LR;
  bgcolor="{CHROME_BG}";
  splines=spline;
  nodesep=0.34;
  ranksep=0.72;
  fontname="Liberation Sans";
  node [shape=box, style="rounded,filled", fontname="Liberation Sans",
        fontsize=15, penwidth=1.6, margin="0.26,0.17", height=0.62];
  edge [color="{ACCENT}", penwidth=1.7, arrowsize=0.85,
        fontname="Liberation Sans", fontsize=12, fontcolor="{TEXT_DIM}"];

  cad      [label="STEP / B-rep CAD", {acc}];
  feat     [label="feature analysis\\ncurvature \u00b7 thin-wall", {n}];
  sizing   [label="gradient-limited\\nsizing field", {acc}];
  meshers  [label="hybrid meshers\\ntet \u00b7 hex \u00b7 prism \u00b7 pyramid \u00b7 poly-VEM", {n}];
  assembly [label="unified FE + VEM\\nassembly", {n}];
  solve    [label="solve\\nLDLT / CG", {acc}];
  zz       [label="ZZ error\\nestimate", {n}];
  hp       [label="hp-adapt\\ndecision", {acc}];

  vtu      [label="VTU export", {side}];
  gui      [label="GUI viewport", {side}];
  bench    [label="bench harness", {side}];

  cad -> feat -> sizing -> meshers -> assembly -> solve -> zz -> hp;
  hp -> sizing [label="  refine / p-elevate", constraint=false,
                style=dashed, color="{ACCENT_DIM}", fontcolor="{ACCENT}"];

  solve -> vtu  [color="{ACCENT_DIM}", arrowsize=0.7];
  solve -> gui  [color="{ACCENT_DIM}", arrowsize=0.7];
  zz    -> bench [color="{ACCENT_DIM}", arrowsize=0.7];

  {{ rank=same; vtu; gui; }}
}}
"""


def render_architecture(out: Path, width: int = 2400) -> None:
    if not shutil.which("dot"):
        raise SystemExit("graphviz 'dot' not found; cannot render architecture.png")
    out.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        ["dot", "-Tpng", f"-Gsize={width / 96:.3f},99", "-Gdpi=96"],
        input=architecture_dot().encode("utf-8"),
        capture_output=True,
        text=False,
        cwd=REPO,
    )
    if proc.returncode != 0:
        raise SystemExit(f"dot failed: {proc.stderr.decode(errors='replace')[:800]}")
    tmp = out.with_suffix(".raw.png")
    tmp.write_bytes(proc.stdout)
    img = Image.open(tmp).convert("RGB")
    tmp.unlink(missing_ok=True)

    # Crop to the ink bounding box first. Graphviz pads asymmetrically around a
    # long dashed feedback spline, which otherwise leaves the diagram sitting
    # off-centre inside a band of dead background.
    arr = np.asarray(img).astype(int)
    ink = np.abs(arr - np.array(hex_to_rgb(CHROME_BG))).sum(axis=2) > 12
    rows, cols = np.any(ink, axis=1), np.any(ink, axis=0)
    if rows.any() and cols.any():
        y0, y1 = np.nonzero(rows)[0][[0, -1]]
        x0, x1 = np.nonzero(cols)[0][[0, -1]]
        img = img.crop((int(x0), int(y0), int(x1) + 1, int(y1) + 1))

    if img.width != width:
        img = img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)
    pad = 56
    canvas = Image.new("RGB", (img.width + 2 * pad, img.height + 2 * pad), CHROME_BG)
    canvas.paste(img, (pad, pad))
    canvas.save(out)
    print(f"    wrote {rel(out)}  ({canvas.width}x{canvas.height})")


# ---------------------------------------------------------------------------
# Grids
# ---------------------------------------------------------------------------
def tile_grid(images: list[Path], labels: list[str], out: Path, title: str,
              footer: str, tile_width: int) -> None:
    argv = [
        sys.executable,
        rel(REPO / "scripts/make_compare_grid.py"),
        "--out", rel(out),
        "--title", title,
        "--labels", ",".join(labels),
        "--footer", footer,
        "--tile-width", str(tile_width),
        *[rel(p) for p in images],
    ]
    print("    $ " + " ".join(argv))
    proc = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"make_compare_grid failed:\n{proc.stdout}\n{proc.stderr}")
    print("      " + proc.stdout.strip())


def build_mesh_tiles(tiles: list[MeshTile], force: bool) -> list[Path]:
    """Mesh every tile, then render them all through one shared camera fit."""
    import pyvista as pv

    CACHE.mkdir(parents=True, exist_ok=True)
    vtus: list[Path] = []
    for tile in tiles:
        vtu = CACHE / f"{tile.tag}.vtu"
        argv = mesh_argv(tile, vtu)
        if force or not vtu.is_file():
            run_cli(argv)
        else:
            print(f"    reusing {rel(vtu)} (use --force to remesh)")
        tile.stats = {"cmd": " ".join(argv)}
        vtus.append(vtu)

    # Shared framing: the 8 corners of one box used by every tile, so the reader
    # compares topology and not zoom level.
    lo = np.full(3, np.inf)
    hi = np.full(3, -np.inf)
    for vtu in vtus:
        b = np.asarray(pv.read(str(vtu)).bounds, dtype=float).reshape(3, 2)
        lo = np.minimum(lo, b[:, 0])
        hi = np.maximum(hi, b[:, 1])

    focus = tiles[0].focus
    window = tiles[0].window
    if focus is not None and window is not None:
        # Close-up: a window about `focus` in x/y, full part depth in z so the
        # slab reads as a solid rather than a floating sheet.
        f = np.asarray(focus, dtype=float)
        lo = np.array([f[0] - window, f[1] - window, lo[2]])
        hi = np.array([f[0] + window, f[1] + window, hi[2]])
        margin = 1.02
    else:
        margin = 1.05

    corners = np.array([[x, y, z] for x in (lo[0], hi[0])
                        for y in (lo[1], hi[1])
                        for z in (lo[2], hi[2])], dtype=float)

    paths: list[Path] = []
    for tile, vtu in zip(tiles, vtus):
        png = CACHE / f"{tile.tag}.png"
        stats = render_mesh(
            vtu, png, size=tile.size, view=tile.view, up=tile.up,
            margin=margin, fit_points=corners, focus=tile.focus,
        )
        tile.stats.update(stats)
        paths.append(png)
    return paths


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def git_rev() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO, capture_output=True, text=True, check=True,
        ).stdout.strip()
    except Exception:
        return "unknown"


def fmt_h(h: float) -> str:
    return f"{h * 1000:g} mm"


def probe_gl() -> bool:
    """Cheap out-of-process check that off-screen GL works before we commit."""
    code = (
        "import pyvista as pv;pv.OFF_SCREEN=True;"
        "p=pv.Plotter(off_screen=True,window_size=(64,64));"
        "p.add_mesh(pv.Sphere());p.screenshot(return_img=True);p.close()"
    )
    return subprocess.run(
        [sys.executable, "-c", code], cwd=REPO, capture_output=True
    ).returncode == 0


def maybe_reexec_under_xvfb(argv: list[str]) -> None:
    if os.environ.get("POLYMESH_SHOWCASE_XVFB") == "1":
        return
    if probe_gl():
        return
    if not shutil.which("xvfb-run"):
        raise SystemExit(
            "off-screen GL is unavailable and xvfb-run is not installed; "
            "cannot render. Install xvfb or provide a working EGL/OSMesa VTK."
        )
    print("off-screen GL probe failed -- re-executing under xvfb-run", flush=True)
    env = dict(os.environ, POLYMESH_SHOWCASE_XVFB="1")
    os.execvpe("xvfb-run", ["xvfb-run", "-a", sys.executable, *argv], env)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render the PolyMesh showcase gallery.")
    ap.add_argument("--all", action="store_true", help="solve + render everything")
    ap.add_argument("--only", action="append", default=[],
                    help="part name or image stem; repeatable")
    ap.add_argument("--force", action="store_true",
                    help="re-run solves/meshes even when the VTU already exists")
    ap.add_argument("--list", action="store_true", help="print the table and exit")
    ap.add_argument("--outdir", type=Path, default=REPO / "docs/assets/showcase")
    ap.add_argument("--no-charts", action="store_true",
                    help="skip the plot_benchmarks.py shellout")
    ap.add_argument("--stress", type=Path, help="single-image mode: solve VTU in")
    ap.add_argument("--mesh", type=Path, help="single-image mode: mesh VTU in")
    ap.add_argument("--out", type=Path, help="single-image mode: PNG out")
    args = ap.parse_args(argv)

    if args.list:
        print("parts (gallery_<name>.png):")
        for part in PARTS:
            flag = "  <- flagship (also hero.png)" if part.name == FLAGSHIP else ""
            print(f"  {part.name:14s} mesher={part.mesher:7s} h={fmt_h(part.h):>8s}{flag}")
        print("other image stems: compare_meshers, compare_grading, architecture,")
        print("                   bench_dof_time, bench_tier1, bench_mms")
        return 0

    raw_argv = argv if argv is not None else sys.argv[1:]
    maybe_reexec_under_xvfb([str(Path(__file__).resolve()), *raw_argv])

    import pyvista as pv

    pv.OFF_SCREEN = True

    outdir: Path = args.outdir
    outdir.mkdir(parents=True, exist_ok=True)

    # ---- single-image modes -------------------------------------------------
    if args.stress or args.mesh:
        if not args.out:
            raise SystemExit("--stress/--mesh require --out")
        if args.stress:
            render_stress(args.stress, args.out, title=args.stress.stem,
                          meta_extra="single-image mode")
        else:
            render_mesh(args.mesh, args.out, size=(OUT_W, OUT_H),
                        view=(0.3, -0.52, 1.0), up=(0.0, 1.0, 0.0),
                        title=args.mesh.stem, meta_extra="single-image mode")
        return 0

    wanted = set(args.only)
    if not wanted and not args.all:
        ap.error("pass --all, --only NAME, or --list")

    def want(name: str) -> bool:
        return args.all or name in wanted

    CACHE.mkdir(parents=True, exist_ok=True)
    images: list[dict] = []
    timings: list[str] = []

    # ---- parts: solve then render ------------------------------------------
    for part in PARTS:
        if not (want(part.name) or (part.name == FLAGSHIP and want("hero"))):
            continue
        print(f"[{part.name}]")
        vtu = CACHE / f"{part.name}.vtu"
        argv_solve = solve_argv(part, vtu)
        cmd = " ".join(argv_solve)
        if args.force or not vtu.is_file():
            stdout, wall = run_cli(argv_solve)
            parsed = parse_solve_stdout(stdout)
            (CACHE / f"{part.name}.meta.json").write_text(
                json.dumps({**parsed, "wall_time_s": wall, "cmd": cmd}, indent=2)
            )
        else:
            print(f"    reusing {rel(vtu)} (use --force to re-solve)")
        meta_file = CACHE / f"{part.name}.meta.json"
        meta = json.loads(meta_file.read_text()) if meta_file.is_file() else {}
        wall = meta.get("wall_time_s")

        base_meta = (
            f"{part.mesher} mesher \u00b7 h = {fmt_h(part.h)} \u00b7 "
            f"E = {part.E:g} Pa, \u03bd = {part.nu:g}"
        )

        if want(part.name):
            info = render_stress(
                vtu, outdir / f"gallery_{part.name}.png",
                title=f"{part.title} \u2014 von Mises stress",
                meta_extra=base_meta,
                footer_extra=part.bc_note,
                view=part.view, up=part.up, margin=part.margin,
                warp_frac=part.warp_frac, clip_pct=part.clip_pct,
            )
            images.append({
                "file": f"gallery_{part.name}.png",
                "kind": "stress",
                "part": part.name,
                "mesher": part.mesher,
                "h": part.h,
                "solve_command": cmd,
                "render_command": f"python3 scripts/render_showcase.py --only {part.name}",
                "dofs": 3 * info["nodes"],
                "wall_time_s": wall,
                "max_von_mises_pa": info["max_von_mises_pa"],
                "caption": (
                    f"{part.title}: von Mises stress on the {part.mesher} mesh "
                    f"(h = {fmt_h(part.h)}, {info['nodes']:,} nodes / "
                    f"{info['elems']:,} elements, {3 * info['nodes']:,} DOF); "
                    f"{part.bc_note}; displacement warped \u00d7{info['warp_factor']:g}; "
                    f"{info['caption_clip']}."
                ),
            })
            timings.append(
                f"{part.name}: h={fmt_h(part.h)} mesher={part.mesher} "
                f"nodes={info['nodes']} elems={info['elems']} "
                f"solve_wall={wall if wall is None else round(wall, 2)}s"
            )

        if part.name == FLAGSHIP and want("hero"):
            info = render_stress(
                vtu, outdir / "hero.png",
                title=f"{part.title} \u2014 bending stress field",
                meta_extra=base_meta,
                footer_extra=f"{part.bc_note} \u00b7 low-oblique view",
                view=HERO_VIEW, up=HERO_UP, margin=1.02,
                warp_frac=part.warp_frac, clip_pct=part.clip_pct,
                focus=HERO_FOCUS, focus_radius=HERO_RADIUS,
            )
            images.append({
                "file": "hero.png",
                "kind": "stress",
                "part": part.name,
                "mesher": part.mesher,
                "h": part.h,
                "solve_command": cmd,
                "render_command": "python3 scripts/render_showcase.py --only hero",
                "dofs": 3 * info["nodes"],
                "wall_time_s": wall,
                "max_von_mises_pa": info["max_von_mises_pa"],
                "caption": (
                    f"Flagship render: von Mises stress across the "
                    f"{part.title.lower()} on the feature-graded mesh, shot from a "
                    f"low oblique angle (h = {fmt_h(part.h)}, "
                    f"{info['nodes']:,} nodes / {info['elems']:,} elements, "
                    f"{3 * info['nodes']:,} DOF); {part.bc_note}; displacement "
                    f"warped \u00d7{info['warp_factor']:g}; {info['caption_clip']}."
                ),
            })

    # ---- compare_meshers ---------------------------------------------------
    if want("compare_meshers"):
        print("[compare_meshers]")
        pngs = build_mesh_tiles(MESHER_TILES, args.force)
        counts = " \u00b7 ".join(
            f"{t.mesher}: {t.stats['nodes']:,} nodes / {t.stats['elems']:,} cells"
            for t in MESHER_TILES
        )
        foot = (
            f"plate_hole.step at h = {fmt_h(MESHER_TILES[0].h)} for all three "
            f"\u2014 {counts}. All three are Cartesian grid-fill topologies "
            f"(not Delaunay); only the cell zoo and grading differ."
        )
        tile_grid(pngs, [t.label for t in MESHER_TILES],
                  outdir / "compare_meshers.png",
                  "One part, three mesh topologies", foot, tile_width=1000)
        images.append({
            "file": "compare_meshers.png",
            "kind": "grid",
            "part": "plate_hole",
            "mesher": "tet | graded | hybrid",
            "h": MESHER_TILES[0].h,
            "solve_command": None,
            "render_command": "python3 scripts/render_showcase.py --only compare_meshers",
            "dofs": None,
            "wall_time_s": None,
            "max_von_mises_pa": None,
            "caption": (
                "Mesh-only renders of the same plate at h = "
                f"{fmt_h(MESHER_TILES[0].h)} under three meshers \u2014 " + counts
                + ". All are Cartesian grid-fill topologies, not Delaunay."
            ),
        })
        for tile in MESHER_TILES:
            images.append({
                "file": f"compare_meshers.png#{tile.mesher}",
                "kind": "mesh",
                "part": tile.part,
                "mesher": tile.mesher,
                "h": tile.h,
                "solve_command": tile.stats["cmd"],
                "render_command": "python3 scripts/render_showcase.py --only compare_meshers",
                "dofs": 3 * tile.stats["nodes"],
                "wall_time_s": None,
                "max_von_mises_pa": None,
                "caption": (
                    f"Tile of compare_meshers.png: {tile.mesher} mesher, "
                    f"{tile.stats['nodes']:,} nodes / {tile.stats['elems']:,} cells."
                ),
            })

    # ---- compare_grading ---------------------------------------------------
    if want("compare_grading"):
        print("[compare_grading]")
        pngs = build_mesh_tiles(GRADING_TILES, args.force)
        uni, gra = GRADING_TILES[0].stats, GRADING_TILES[1].stats
        cell_gap = 100.0 * abs(uni["elems"] - gra["elems"]) / max(uni["elems"], 1)
        win_mm = GRADE_WINDOW * 2000.0
        foot = (
            f"Close-up on the hole \u2014 a {win_mm:g} mm window "
            f"(\u2248{win_mm / (HOLE_RADIUS_M * 1000.0):.1f} hole radii across), identical "
            f"camera in both "
            f"panels. Same mesher and same part; only the sizing field differs. "
            f"h tuned to match the element budget: {uni['elems']:,} vs "
            f"{gra['elems']:,} cells ({cell_gap:.1f}% apart). Node counts "
            f"{uni['nodes']:,} vs {gra['nodes']:,} "
            f"({3 * uni['nodes']:,} vs {3 * gra['nodes']:,} DOF)."
        )
        tile_grid(pngs, [t.label for t in GRADING_TILES],
                  outdir / "compare_grading.png",
                  "Uniform vs feature-graded sizing at a matched element budget",
                  foot, tile_width=1300)
        images.append({
            "file": "compare_grading.png",
            "kind": "grid",
            "part": "plate_hole",
            "mesher": "graded",
            "h": None,
            "solve_command": None,
            "render_command": "python3 scripts/render_showcase.py --only compare_grading",
            "dofs": None,
            "wall_time_s": None,
            "max_von_mises_pa": None,
            "caption": (
                "Close-up on the hole (identical camera in both panels, "
                f"{GRADE_WINDOW * 2000:g} mm window): uniform (--no-feature, h = "
                f"{fmt_h(GRADING_TILES[0].h)}) vs feature-graded (h = "
                f"{fmt_h(GRADING_TILES[1].h)}) sizing on the same part and mesher, "
                f"h tuned to a matched element budget: {uni['elems']:,} vs "
                f"{gra['elems']:,} cells ({cell_gap:.1f}% apart), "
                f"{3 * uni['nodes']:,} vs {3 * gra['nodes']:,} DOF."
            ),
        })
        for tile in GRADING_TILES:
            images.append({
                "file": f"compare_grading.png#{tile.tag}",
                "kind": "mesh",
                "part": tile.part,
                "mesher": tile.mesher + ("+no-feature" if tile.no_feature else ""),
                "h": tile.h,
                "solve_command": tile.stats["cmd"],
                "render_command": "python3 scripts/render_showcase.py --only compare_grading",
                "dofs": 3 * tile.stats["nodes"],
                "wall_time_s": None,
                "max_von_mises_pa": None,
                "caption": (
                    f"Tile of compare_grading.png: {tile.label}, "
                    f"{tile.stats['nodes']:,} nodes / {tile.stats['elems']:,} cells."
                ),
            })

    # ---- architecture ------------------------------------------------------
    if want("architecture"):
        print("[architecture]")
        render_architecture(outdir / "architecture.png")
        images.append({
            "file": "architecture.png",
            "kind": "diagram",
            "part": None, "mesher": None, "h": None,
            "solve_command": None,
            "render_command": "python3 scripts/render_showcase.py --only architecture",
            "dofs": None, "wall_time_s": None, "max_von_mises_pa": None,
            "caption": (
                "PolyMesh pipeline: STEP/B-rep CAD through feature analysis and a "
                "gradient-limited sizing field into the hybrid meshers, unified "
                "FE+VEM assembly, solve, ZZ error estimate and the hp-adapt "
                "decision that feeds back into the sizing field."
            ),
        })

    # ---- benchmark charts --------------------------------------------------
    chart_specs = [
        ("bench_dof_time.png", "dof_time",
         "Feature-graded vs PolyMesh's own frozen uniform tet10 baseline on the D6 "
         "L-domain: 6,384 -> 1,248 DOF and 2.762 s -> 0.227 s at the same energy "
         "deficit band. Internal self-comparison, not a comparison against any "
         "external solver."),
        ("bench_tier1.png", "tier1",
         "Tier-1 analytical verification: every case inside tolerance, measured on "
         "structured parametric verification meshes rather than product Cartesian "
         "grid-fill meshes."),
        ("bench_mms.png", "mms",
         "Method-of-manufactured-solutions energy-norm convergence: frozen P1 "
         "elements at 0.997/0.997/2.000/2.000 against theory 1/1/2/2, and the "
         "hierarchical p-basis at 1.02/1.99/2.98/3.98 against theory 1/2/3/4."),
    ]
    chart_wanted = [c for c in chart_specs if want(c[0].removesuffix(".png"))]
    if chart_wanted and not args.no_charts:
        print("[benchmark charts]")
        chart_argv = [
            sys.executable, "scripts/plot_benchmarks.py",
            "--outdir", rel(outdir),
        ]
        for _, key, _ in chart_wanted:
            chart_argv += ["--only", key]
        print("    $ " + " ".join(chart_argv))
        proc = subprocess.run(chart_argv, cwd=REPO, capture_output=True, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"plot_benchmarks failed:\n{proc.stdout}\n{proc.stderr}")
        for line in proc.stdout.strip().splitlines():
            print("      " + line)
    for fname, _key, caption in chart_wanted:
        images.append({
            "file": fname,
            "kind": "chart",
            "part": None, "mesher": None, "h": None,
            "solve_command": None,
            "render_command": "python3 scripts/plot_benchmarks.py",
            "dofs": None, "wall_time_s": None, "max_von_mises_pa": None,
            "caption": caption,
        })

    # ---- manifest ----------------------------------------------------------
    gui = outdir / "gui_studio.png"
    if gui.is_file():
        images.append({
            "file": "gui_studio.png",
            "kind": "gui",
            "part": None, "mesher": None, "h": None,
            "solve_command": None,
            "render_command": None,
            "dofs": None, "wall_time_s": None, "max_von_mises_pa": None,
            "caption": "PolyMesh Studio desktop application.",
        })

    manifest_path = outdir / "manifest.json"
    existing: dict = {}
    if manifest_path.is_file():
        try:
            existing = json.loads(manifest_path.read_text())
        except json.JSONDecodeError:
            existing = {}
    merged: dict[str, dict] = {
        img["file"]: img for img in existing.get("images", []) if isinstance(img, dict)
    }
    for img in images:
        merged[img["file"]] = img
    # Drop records whose PNG is gone so the manifest never claims a missing file.
    merged = {
        k: v for k, v in merged.items()
        if (outdir / k.split("#", 1)[0]).is_file()
    }

    manifest = {
        "generated_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_rev": git_rev(),
        "images": sorted(merged.values(), key=lambda d: d["file"]),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\nwrote {rel(manifest_path)}  ({len(manifest['images'])} records)")

    if timings:
        print("\nsolve summary:")
        for line in timings:
            print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
