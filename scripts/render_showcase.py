#!/usr/bin/env python3
"""PolyMesh showcase renderer -- solves the demo parts and renders the gallery.

Produces every image in ``docs/assets/showcase/`` except ``gui_studio.png``
(an application screenshot, captured separately), plus ``manifest.json``
recording the exact command that made each one.

Design notes
------------
* Every colour, font and type size comes from ``scripts/figstyle.py``. This
  module defines no palette and names no font file; ``fs.use("dark")`` selects
  the stage and ``fs.font_path`` resolves a face with verified glyph coverage,
  so a caption character a font cannot draw fails the build instead of shipping
  as a box.
* VTK/pyvista renders **geometry only**, on a transparent background. The
  background gradient, title block, colour bar and captions are drawn with PIL
  afterwards. That keeps full typographic control, gives an exact theme
  gradient, and avoids VTK's cramped default scalar bar entirely.
* Everything is supersampled by ``SUPERSAMPLE`` and Lanczos-downsampled to the
  final 1920x1440, which anti-aliases geometry and text together. PIL type is
  sized by ``fs.font_px`` against the final canvas width, so the smallest
  caption still clears 11 px once a README scales the image down.
* Fields are coloured with ``fs.field_cmap("magnitude")`` (viridis). The GUI's
  own blue-cyan-green-yellow-red ramp is neither perceptually uniform nor
  colour-blind safe; it survives in figstyle for ``gui_studio.png``, which
  documents the GUI, and nowhere else.
* One colour-range rule, ``CLIP_PERCENTILE``, applies to every stress render and
  is stated verbatim in every footer. A perfectly clamped face is a stress
  singularity whose peak nodal value can be orders of magnitude above the bulk
  field, so captions and the manifest always report BOTH the clipped range and
  the true peak, and the colour bar's top tick is prefixed "≥" exactly when
  clipping is active.

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

sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
import figstyle as fs  # noqa: E402
from make_compare_grid import PanelSpec, matched_panels  # noqa: E402


# ---------------------------------------------------------------------------
# Theme -- every colour in this file comes from figstyle. 3D renders and the
# composites around them keep the dark stage.
# ---------------------------------------------------------------------------
def theme() -> fs.Theme:
    t = fs.theme()
    return t if t.name == "dark" else fs.use("dark")


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
    """The CLI to render with, chosen by MTIME and announced.

    This used to take the first name that existed, preferring the extensionless
    `build/apps/cli/polymesh`. On Windows the build target is `polymesh.exe`,
    so an extensionless leftover from an earlier configuration sat there
    unchanged and won every time: the showcase was regenerated for hours
    against a 15-hour-old engine and came back "bit-identical", which read as
    evidence that a mesher fix had changed nothing. It was evidence that the
    fix had never been run. Pick the newest candidate and print it, so a stale
    binary announces itself instead of quietly producing the old answer.
    """
    candidates = [
        cand
        for cand in (CLI.with_suffix(".exe"), CLI, REPO / "polymesh.exe", REPO / "polymesh")
        if cand.is_file() and os.access(cand, os.X_OK)
    ]
    if not candidates:
        raise SystemExit(
            "polymesh CLI not found; build it first (expected build/apps/cli/polymesh)"
        )
    newest = max(candidates, key=lambda c: c.stat().st_mtime)
    stamp = _dt.datetime.fromtimestamp(
        newest.stat().st_mtime, _dt.timezone.utc
    ).strftime("%Y-%m-%d %H:%M:%SZ")
    if not getattr(cli_path, "_announced", False):
        print(f"  cli: {rel(newest)} (built {stamp})")
        for other in candidates:
            if other != newest:
                print(f"       ignoring older {rel(other)}")
        cli_path._announced = True  # type: ignore[attr-defined]
    return newest


def hex_to_rgb(h: str) -> tuple[int, int, int]:
    """Parse a figstyle theme colour into 8-bit channels for PIL/PyVista."""
    h = h.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


def mix(a: str, b: str, t: float) -> tuple[int, int, int]:
    """Blend two theme colours; used for the viewport's vertical gradient."""
    ca, cb = hex_to_rgb(a), hex_to_rgb(b)
    return tuple(int(round(ca[i] + (cb[i] - ca[i]) * t)) for i in range(3))  # type: ignore[return-value]


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
    load_dir: tuple[float, float, float] = (0.0, 1.0, 0.0)
    force_n: float = 1000.0
    # Target peak displacement as a fraction of the bbox diagonal. Kept small:
    # warping a 10 mm-thick plate by 18 mm to "show" bending shears the hole
    # rim into something the solve never predicted.
    warp_frac: float = 0.02
    # There is no per-part colour-range override: CLIP_PERCENTILE is the one
    # rule, applied identically to every stress render (and stated in every
    # footer), so no figure gets a range that happens to flatter it.


# Selection boxes and load directions follow the *.case.json contracts. The
# renderer uses an explicit conserved resultant so every gallery command is
# independent of surface-facet count.
PARTS: list[Part] = [
    Part(
        name="plate_hole",
        title="Plate with hole",
        step="plate_hole.step",
        mesher="graded",
        # h was 3 mm when the shipped mesh was straight-edged and only a fine
        # lattice could round the hole. ADR-0035 puts the boundary on the exact
        # BRep instead, so 6 mm now renders a smoother hole than 3 mm ever did —
        # at ~1/8 the cells, which keeps this figure reproducible in minutes.
        h=0.006,
        E=2.1e11,
        nu=0.3,
        view=(0.25, -0.60, 1.00),
        # up=(0, dz, -dy) is the unique up that is perpendicular to the view
        # AND has no world-x component, so the plate's long axis lands exactly
        # horizontal on screen instead of rolling off-axis.
        up=(0.0, 1.00, 0.60),
        margin=1.03,
        bc_note="min-x face fixed, +x resultant on max-x face",
        load_dir=(1.0, 0.0, 0.0),
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
        bc_note="root face fixed, -z resultant on tip face",
        load_dir=(0.0, 0.0, -1.0),
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
        # The base FACE, not a 15 mm slab of the wall. A selection box names a
        # region of the boundary surface (ADR-0037, extended to fixtures), so
        # z <= 15 mm used to clamp the outer wall up to 15 mm as well, and the
        # upper edge of that strip is an artificial clamped-patch boundary with a
        # genuine stress singularity on it: at h = 12 mm it showed as a ring of
        # one-element hot spots a fifteenth of the way up the wall, which no real
        # fixture produces. 2 mm still contains the whole z = 0 face (its nodes
        # snap to z = 0 exactly), and the mesh does not move at all: `polymesh
        # mesh` at h = 12 mm with this box and with the old 15 mm one both emit
        # 234,533 nodes / 161,976 cells from the same 124 BC seeds, and the two
        # VTUs are bit-identical (sha256 68d2c65498704034 both). So this changes
        # the boundary condition and nothing else.
        fix_box=(-1, -1, -1, 1, 1, 0.002),
        load_box=(-1, -1, 0.195, 1, 1, 1),
        # The box reaches 5 mm down the wall, and since ADR-0037 the traction is
        # integrated over exactly that region rather than over whole faces, so say
        # so: the cap alone cannot be selected by an axis-aligned box without a
        # knife-edge plane at z = 200 mm, which cost 11% of the cap area when
        # tried (7.007e-3 against 7.854e-3 m^2) because snapped cap nodes land on
        # both sides of it.
        bc_note="base face (z = 0) fixed, +z resultant on the surface above "
                "z = 0.195 m (cap plus the top 5 mm of wall)",
        load_dir=(0.0, 0.0, 1.0),
        force_n=7853.981633974483,
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
        bc_note="lower cap fixed, -z resultant on upper cap",
        load_dir=(0.0, 0.0, -1.0),
        force_n=3141.592653589793,
    ),
    Part(
        name="icecream_cone",
        title="Round ice-cream cone",
        step="icecream_cone.step",
        mesher="graded",
        h=0.010,
        E=2.0e11,
        nu=0.3,
        view=(1.00, -1.00, 0.55),
        up=(0.0, 0.0, 1.0),
        margin=1.05,
        fix_box=(-1, -1, -1, 1, 1, 0.012),
        load_box=(-1, -1, 0.120, 1, 1, 1),
        bc_note="foot z <= 0.012 m fixed, -z resultant on scoop",
        load_dir=(0.0, 0.0, -1.0),
    ),
]

FLAGSHIP = "plate_hole"

# Hero: the same solve as the flagship gallery image, shot from a much lower
# oblique angle (~34 deg above the plate vs ~57 deg) so the bending deflection
# and the stress lobes around the hole read in 3D.
#
# Deliberately NOT a tight crop on the hole: at a reproducible h = 3 mm the
# Cartesian grid-fill opening has enough perimeter segments to read as round,
# framing the whole plate also keeps the model uncut. HERO_UP = (0, dz, -dy)
# keeps the plate's long axis horizontal.
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


# compare_meshers: one part, matched h, three topologies, one shared close-up
# on the hole. Whole-plate tiles put the hole at a few percent of the frame
# and the three topologies read as identical grey sheets; the 60 mm window
# (6 hole radii) is where the cell zoo actually differs. The window is wider
# than compare_grading's so the hybrid's hex bulk -> transition band fits.
CMP_FOCUS = (0.0, 0.0, 0.005)
CMP_WINDOW = 0.030
CMP_VIEW = (0.15, -0.35, 1.00)
CMP_UP = (0.0, 1.00, 0.35)
CMP_SIZE = (1300, 1150)
MESHER_TILES = [
    MeshTile("cmp_tet", "tet  ·  Cartesian grid-fill tet4", "plate_hole", "tet", 0.006,
             size=CMP_SIZE, view=CMP_VIEW, up=CMP_UP, focus=CMP_FOCUS, window=CMP_WINDOW),
    MeshTile("cmp_graded", "graded  ·  feature-graded tet4", "plate_hole", "graded", 0.006,
             size=CMP_SIZE, view=CMP_VIEW, up=CMP_UP, focus=CMP_FOCUS, window=CMP_WINDOW),
    # At this part and h the hybrid path emits a mixed transition zoo, so the
    # label names the conforming transition cells rather than claiming all-hex.
    MeshTile("cmp_hybrid", "hybrid  ·  hex bulk + transition cells", "plate_hole", "hybrid", 0.006,
             size=CMP_SIZE, view=CMP_VIEW, up=CMP_UP, focus=CMP_FOCUS, window=CMP_WINDOW),
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
             # 4.2 mm against the graded leg's 5.6 mm is the matched-element-budget
             # control this figure exists for; promotion adds no cells, so curved
             # geometry does not change the pairing. Re-tuned from 3.8 mm when the
             # mirror-symmetric even-cell lattice (ADR-0036) moved the counts: the
             # legs are 43,024 vs 45,424 cells, and the lattice quantises hard
             # enough that the neighbouring h gives 48,256 — 5.3% low against 6.2%
             # high, so this is the closest pairing available.
             "graded", 0.0042, no_feature=True, size=(GRADE_W, GRADE_H),
             view=GRADE_VIEW, up=GRADE_UP, focus=GRADE_FOCUS, window=GRADE_WINDOW),
    MeshTile("grade_feature", "feature-graded sizing field", "plate_hole",
             "graded", 0.0056, size=(GRADE_W, GRADE_H),
             view=GRADE_VIEW, up=GRADE_UP, focus=GRADE_FOCUS, window=GRADE_WINDOW),
]


# ---------------------------------------------------------------------------
# CLI driving
# ---------------------------------------------------------------------------
class MeshRefused(RuntimeError):
    """The engine declined to produce this mesh, and said why.

    A refusal is a legitimate, informative outcome of the current engine (the
    volume-completeness guard rejecting a mesh whose solid/void topology is
    incomplete), not a crash. A figure that asks for such a mesh must be able
    to report the refusal instead of aborting the whole sweep.
    """

    def __init__(self, message: str, argv: list[str]):
        super().__init__(message)
        self.message = message
        self.argv = argv


#: Engine messages that mean "declined", as opposed to "crashed". Kept
#: textually distinct engine-side on purpose; matched on the opening phrase.
REFUSAL_MARKERS = (
    "geometry fill-stage guard failed:",
    "feature unresolved at h=",
    "resolution refused at h=",
)


def run_cli(argv: list[str], timeout: int = SOLVE_TIMEOUT,
            allow_refusal: bool = False) -> tuple[str, float]:
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
    output = "".join(lines)
    if proc.returncode != 0:
        marker = next((m for m in REFUSAL_MARKERS if m in output), None)
        if allow_refusal and marker:
            detail = next((line.strip() for line in output.splitlines()
                           if marker in line), marker)
            raise MeshRefused(detail, argv)
        raise SystemExit(f"polymesh failed (rc={proc.returncode}): {' '.join(argv)}")
    return output, wall


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
    argv += [
        "--load-dir",
        *[f"{v:g}" for v in part.load_dir],
        "--force",
        f"{part.force_n:g}",
    ]
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
    """Vertical viewport gradient, panel at the top easing to page at the foot."""
    t = theme()
    top = np.array(mix(t.panel, t.grid, 0.25), dtype=float)
    mid = np.array(mix(t.panel, t.bg, 0.5), dtype=float)
    bot = np.array(hex_to_rgb(t.bg), dtype=float)
    ys = np.linspace(0.0, 1.0, h)[:, None]
    upper = top + (mid - top) * (ys / 0.5)
    lower = mid + (bot - mid) * ((ys - 0.5) / 0.5)
    col = np.where(ys < 0.5, upper, lower)
    return Image.fromarray(
        np.repeat(col[:, None, :], w, axis=1).round().astype(np.uint8), "RGB"
    )


def _font(role: str, out_w: int, s: int, *, bold: bool = False):
    """A glyph-verified font at figstyle's point size for this canvas width.

    ``out_w`` is the *final* width, so type keeps its apparent size after the
    supersampled canvas is downscaled.
    """
    return ImageFont.truetype(str(fs.font_path("regular", bold=bold)),
                              fs.font_px(role, out_w) * s)


def draw_colorbar(
    draw: ImageDraw.ImageDraw,
    x: int,
    y0: int,
    y1: int,
    lo: float,
    hi: float,
    clipped: bool,
    s: int,
    out_w: int,
) -> None:
    """Vertical viridis colour bar with SI-prefixed ticks (``0`` .. ``3.84 MPa``).

    Exponent soup (``3.84x10^6`` under a separate ``(Pa)`` heading) made the
    reader do the arithmetic; ``fs.si`` puts the unit on the number. The top
    tick carries a ``>=`` marker exactly when the range is clipped.
    """
    bar_w = 54 * s
    f_title = _font("panel", out_w, s, bold=True)
    f_tick = _font("label", out_w, s)
    t = theme()

    # Two lines: one line of "von Mises stress" would run past the right edge
    # of the bar zone at this type size. The unit lives on the ticks now.
    head_px = fs.font_px("panel", out_w)
    for i, line in enumerate(("von Mises", "stress")):
        fs.assert_glyphs(line)
        draw.text((x, y0 - int((3.5 - 1.25 * i) * head_px) * s), line,
                  font=f_title, fill=t.ink)

    lut = fs.field_lut("magnitude", 512)
    height = y1 - y0
    for i in range(height):
        frac = 1.0 - i / max(1, height - 1)
        rgb = lut[min(len(lut) - 1, int(frac * (len(lut) - 1)))]
        col = tuple(int(round(c * 255)) for c in rgb)
        draw.rectangle([x, y0 + i, x + bar_w, y0 + i], fill=col)
    draw.rectangle([x, y0, x + bar_w, y1], outline=t.grid, width=max(1, s))

    n_ticks = 6
    for k in range(n_ticks):
        frac = k / (n_ticks - 1)
        val = lo + frac * (hi - lo)
        ty = int(y1 - frac * height)
        draw.line([x + bar_w, ty, x + bar_w + 9 * s, ty], fill=t.grid,
                  width=max(1, s))
        label = fs.si(val, "Pa")
        if clipped and k == n_ticks - 1:
            label = "\u2265" + label
        fs.assert_glyphs(label)
        draw.text((x + bar_w + 15 * s, ty - 0.6 * fs.font_px("label", out_w) * s),
                  label, font=f_tick, fill=t.ink)


def wrap(draw, text: str, font, max_w: int) -> list[str]:
    """Greedy word wrap that honours explicit newlines as hard breaks.

    A caption that ends in a provenance stamp needs that stamp on its own
    line; flowing it into the prose above hides it in exactly the place a
    reader scans past.
    """
    lines: list[str] = []
    for paragraph in text.split("\n"):
        cur = ""
        for word in paragraph.split():
            trial = f"{cur} {word}".strip()
            if draw.textlength(trial, font=font) <= max_w or not cur:
                cur = trial
            else:
                lines.append(cur)
                cur = word
        if cur:
            lines.append(cur)
    return lines



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

    t = theme()
    out_w = out_size[0]
    # Readability scrims: a soft vignette toward the page colour behind each
    # text band, so a bright stress lobe reaching the band edge can never sit
    # under the title or the caption.
    if title or footer_text:
        scrim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        sd = ImageDraw.Draw(scrim)
        base = hex_to_rgb(t.bg)
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
        fs.assert_glyphs(title, meta or "")
        f_title = _font("title", out_w, s, bold=True)
        draw.text((56 * s, 44 * s), title, font=f_title, fill=t.ink)
        ty = 44 * s + (f_title.getbbox(title)[3] - f_title.getbbox(title)[1]) + 18 * s
        draw.rectangle([56 * s, ty, 56 * s + 96 * s, ty + 3 * s], fill=t.accent)
        if meta:
            f_meta = _font("label", out_w, s)
            draw.text((56 * s, ty + 16 * s), meta, font=f_meta, fill=t.muted)

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
            out_w=out_w,
        )

    if footer_text:
        fs.assert_glyphs(footer_text)
        f_foot = _font("footer", out_w, s)
        lines = wrap(draw, footer_text, f_foot, w - 112 * s)
        line_h = int(round(fs.font_px("footer", out_w) * 1.45)) * s
        y = h - 42 * s - line_h * len(lines)
        for line in lines:
            draw.text((56 * s, y), line, font=f_foot, fill=t.muted)
            y += line_h
        f_mark = _font("footer", out_w, s, bold=True)
        mark = "PolyMesh"
        draw.text(
            (w - 56 * s - draw.textlength(mark, font=f_mark), h - 66 * s),
            mark,
            font=f_mark,
            fill=t.muted,
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


# ---------------------------------------------------------------------------
# The one colour-range rule
# ---------------------------------------------------------------------------
# Every stress render clips its colour range at this percentile of the *visible
# surface* field -- one rule, one number, stated verbatim in every footer. The
# old per-figure choices (95th here, 90th there, full range elsewhere) let each
# image pick the range that flattered it.
#
# Why the surface and not the volume: interior nodes never reach a pixel. Why
# clip at all: a clamped face is a stress singularity whose nodal peak can be
# orders of magnitude above the bulk field, and mapping to it turns the whole
# part one flat colour. The true unclipped peak is always reported alongside.
CLIP_PERCENTILE = 99.0


def clip_rule_text() -> str:
    """The rule sentence. Identical in every stress footer and caption."""
    return (f"clipped at the {ordinal(CLIP_PERCENTILE)} percentile of the "
            f"visible surface field")


def color_range(vm_surface: np.ndarray) -> tuple[float, float, bool]:
    """(lo, hi, clipped) for a surface von Mises field under the one rule."""
    true_max = float(vm_surface.max())
    if not math.isfinite(true_max) or true_max <= 0.0:
        return 0.0, 1.0, False
    hi = float(np.percentile(vm_surface, CLIP_PERCENTILE))
    if hi <= 0.0:
        return 0.0, true_max, False
    return 0.0, hi, hi < true_max


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
    lo, hi, clipped = color_range(
        np.asarray(surf.point_data["von_Mises"], dtype=float)
    )

    s = SUPERSAMPLE
    w3 = OUT_W - BAR_ZONE
    h3 = OUT_H - TOP_BAND - BOT_BAND
    p = _plotter(w3 * s, h3 * s, "three lights")
    p.add_mesh(
        surf,
        scalars="von_Mises",
        # von Mises is a magnitude anchored at zero: viridis. The GUI's own
        # blue-cyan-green-yellow-red ramp is not perceptually uniform and not
        # colour-blind safe, and now lives in figstyle for gui_studio.png only.
        cmap=fs.field_cmap("magnitude"),
        clim=(lo, hi),
        show_edges=True,
        edge_color=theme().bg,
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
        # Do not assert WHAT the peak sits on. This caption used to say "at
        # the clamped-face stress singularity", and that is false on the
        # cylinder (the peak sits at the load rim) and on the plate (the peak
        # is the Kirsch concentration at the hole rim, a free surface — no BC
        # acts there at all). Print where the peak actually is and let the
        # reader match it against the stated BCs.
        peak_at = np.asarray(grid.points)[int(np.argmax(vm))] * 1e3
        clip_note = (
            f"colour range 0 \u2013 {fs.si(hi, 'Pa')} ({clip_rule_text()}); "
            f"peak nodal value {fs.si(true_max, 'Pa')}, node at "
            f"({peak_at[0]:.0f}, {peak_at[1]:.0f}, {peak_at[2]:.0f}) mm"
        )
    else:
        clip_note = (
            f"colour range 0 \u2013 {fs.si(hi, 'Pa')} (full field range; "
            f"the {ordinal(CLIP_PERCENTILE)} percentile of the visible surface "
            f"field equals the peak)"
        )
    footer = f"Deformation warped \u00d7{factor:g} for visibility \u00b7 {clip_note}"
    if footer_extra:
        footer += f" \u00b7 {footer_extra}"
    # Same stamp the matplotlib figures carry, for the same reason: these
    # renders show numbers (peak stress, DOF, warp factor) and the mesh they
    # were solved on changes with every engine fix. The VTU digest says which
    # mesh this is; the revision says which engine produced it.
    footer += "\n" + fs.provenance(vtu)

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
        "clip_pct": CLIP_PERCENTILE,
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
    t = theme()
    p.add_mesh(
        surf,
        color=t.muted,
        show_edges=True,
        edge_color=t.bg,
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
    # kept well under 1.0 in total so the part stays a mid grey instead of
    # blowing out to white.
    d = np.asarray(view, dtype=float)
    d /= np.linalg.norm(d)
    key = pv.Light(position=tuple(d * 4 + np.array([1.2, 0.6, 2.0])), light_type="scene light")
    key.intensity = 0.60
    key.diffuse_color = (1.0, 1.0, 1.0)
    fill = pv.Light(position=tuple(d * 4 + np.array([-2.2, -1.4, 0.4])), light_type="scene light")
    fill.intensity = 0.26
    fill.diffuse_color = t.accent
    rim = pv.Light(position=tuple(-d * 4 + np.array([0.0, 0.0, 1.6])), light_type="scene light")
    rim.intensity = 0.20
    rim.diffuse_color = t.band
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
# architecture.png -- drawn with matplotlib through figstyle
# ---------------------------------------------------------------------------
# Two rows of four stages, serpentine, so the diagram is ~2:1 instead of the
# 4:1 letterbox graphviz produced at rankdir=LR (its panels and 15 pt labels
# were sub-pixel once a README scaled it to 900 px). Positions are axes
# fractions of the panel figstyle lays out for an 11.0 x 5.5 in figure with a
# 1-line title, 2-line subtitle and 2-line footer: measured 10.436 x 3.712 in,
# so 1 in is 0.0958 in x and 0.2694 in y and the panel is 2.8x wider than tall.
#
# The vertical positions are one budget that fills the panel edge to edge:
#   0.012 margin | 0.091 tap | 0.085 stub | 0.205 row 0 | 0.161 feedback lane
#   | 0.205 row 1 | 0.095 stub | 0.091 tap | 0.012 margin
# The previous layout ended at 0.888 and started at 0.043, which left a 74 px
# empty band across the top of the frame with both stage rows sitting low.
ARCH_AX_IN = (10.436, 3.712)   # measured panel size in inches, see above

ARCH_ROW_Y = (0.6655, 0.2995)  # stage row centres
ARCH_COL_X = (0.13, 0.375, 0.62, 0.865)
ARCH_BOX = (0.205, 0.205)      # (width, height) of a stage box
ARCH_FEEDBACK_Y = 0.454        # mid-gap lane for the return path
ARCH_TEXT_GAP = 0.021          # label block to detail block, 0.078 in
ARCH_TAP_PAD = 0.023           # tap chip: text block to chip edge, 0.085 in

#: (column, row, label, detail, accent) -- rows are laid out serpentine, so row
#: 1 reads right-to-left and the row-to-row hop is a short vertical. Detail
#: lines are pre-wrapped: a stage box is 0.205 wide, so at 8.5 pt one detail
#: line may not exceed 0.178 axes fractions (1.86 in) and still keep a 10 pt
#: side margin. Unwrapped, "curvature · thin-wall · FFT edge denoise" measured
#: 0.223 (2.32 in) and overprinted the box to its right; the mesher list
#: measured 0.210 and touched both walls.
ARCH_STAGES = [
    (0, 0, "STEP / B-rep CAD", "", True),
    (1, 0, "feature analysis", "curvature · thin-wall\nFFT edge denoise", False),
    (2, 0, "spectral-trimmed\nsizing field", "FFT energy truncation + budget", True),
    (3, 0, "hybrid meshers", "tet · hex · prism\npyramid · poly-VEM", False),
    (3, 1, "unified FE + VEM\nassembly", "", False),
    (2, 1, "solve", "LDLT / equilibrated CG", True),
    (1, 1, "ZZ error\nestimate", "", False),
    (0, 1, "hp-adapt\ndecision", "refine · coarsen · p-elevate", True),
]

#: side outputs: (x, y, width, label, source stage index). These are taps, not
#: stages -- page-coloured fill, hairline edge, annotation type and a height
#: that follows the label, so they cannot be misread as pipeline steps.
#: Each x lies inside its source box's x-extent (centre +/- 0.1025) so the tap
#: is a straight vertical stub. The learned advisor hangs *above* row 0 because
#: its source (feature analysis) is in row 0: from the bottom row it needed a
#: 0.24-wide diagonal that crossed the feedback lane and read as an edge into
#: the hp-adapt box. Widths are the measured 9 pt label width plus ~0.21 in.
ARCH_SIDES = [
    (0.375, 0.9205, 0.175, "learned mesh advisor\n(budget-feasible)", 1),
    (0.335, 0.0570, 0.135, "bench harness", 6),
    (0.545, 0.0570, 0.123, "GUI viewport", 5),
    (0.700, 0.0570, 0.113, "VTU export", 5),
]


def _arch_center(col: int, row: int) -> tuple[float, float]:
    return ARCH_COL_X[col], ARCH_ROW_Y[row]


def _arch_line_h(pt: float, n: int = 1, linespacing: float = 1.35) -> float:
    """Height of ``n`` lines of ``pt`` type, in axes fractions of the panel.

    Checked against the rendered extents: 10 pt at 1.35 measures 0.1875 in,
    0.0505 of the 3.712 in panel, which is what this returns.
    """
    return n * pt * linespacing / 72.0 / ARCH_AX_IN[1]


def render_architecture(out: Path) -> None:
    """Pipeline diagram: two rows, tight feedback lane, subordinate taps."""
    from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

    t = theme()
    bw, bh = ARCH_BOX
    fig, axes = fs.figure(
        "PolyMesh pipeline",
        subtitle="STEP CAD to solved fields; the hp-adapt decision feeds back "
                 "into the sizing field — refine, coarsen, or p-elevate — "
                 "rather than remeshing from scratch",
        footer=fs.footer_source(Path(__file__),
                               note="drawn programmatically, no external "
                                    "diagram tool"),
        size=(11.0, 5.5),
    )
    ax = axes[0, 0]
    fs.axes_off(ax)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.set_facecolor(t.bg)
    ax.grid(False)

    def plate(cx: float, cy: float, w: float, h: float, *, edge: str,
              face: str, lw: float, rounding: float) -> None:
        ax.add_patch(FancyBboxPatch(
            (cx - w / 2, cy - h / 2), w, h,
            boxstyle=f"round,pad=0.004,rounding_size={rounding}",
            linewidth=lw, edgecolor=edge, facecolor=face, zorder=3,
        ))

    def stack(cx: float, cy: float, label: str, detail: str, *,
              label_pt: float, ink: str, detail_pt: float = 0.0,
              detail_ink: str = "") -> None:
        """Centre label and detail as one block inside a box.

        The old code offset the label by a fixed 0.022 and pinned the detail
        0.032 above the floor, which only balanced for a 1-line label with a
        1-line detail; a wrapped detail then sat off-centre in its box.
        """
        fs.assert_glyphs(label, detail)
        h_label = _arch_line_h(label_pt, label.count("\n") + 1)
        h_detail = _arch_line_h(detail_pt, detail.count("\n") + 1, 1.3) \
            if detail else 0.0
        gap = ARCH_TEXT_GAP if detail else 0.0
        top = cy + (h_label + gap + h_detail) / 2
        ax.text(cx, top, label, ha="center", va="top", zorder=4,
                fontsize=label_pt, color=ink, linespacing=1.35)
        if detail:
            ax.text(cx, top - h_label - gap, detail, ha="center", va="top",
                    zorder=4, fontsize=detail_pt, color=detail_ink,
                    linespacing=1.3)

    def arrow(p0, p1, *, color: str, dashed: bool = False,
              scale: float = 13.0, lw: float = 1.6) -> None:
        ax.add_patch(FancyArrowPatch(
            p0, p1, arrowstyle="-|>", mutation_scale=scale,
            linewidth=lw, color=color, zorder=2,
            linestyle=(0, (5, 3)) if dashed else "solid",
            shrinkA=0, shrinkB=0,
        ))

    # Stage weight: accent stroke for the four decision stages, t.rule for the
    # rest. t.grid (#2A3240) on t.panel (#161B22) was a 1.3:1 edge that
    # disappeared once the taps below it carried the same weight.
    for col, row, label, detail, accent in ARCH_STAGES:
        cx, cy = _arch_center(col, row)
        plate(cx, cy, bw, bh, edge=t.accent if accent else t.rule,
              face=t.panel, lw=1.6 if accent else 1.4, rounding=0.02)
        stack(cx, cy, label, detail, label_pt=fs.FONT_PT["label"], ink=t.ink,
              detail_pt=fs.FONT_PT["annot"] - 0.5, detail_ink=t.muted)

    # in-row flow: row 0 left-to-right, row 1 right-to-left (serpentine)
    for row, cols in ((0, (0, 1, 2)), (1, (3, 2, 1))):
        step = 1 if row == 0 else -1
        for col in cols:
            x0, y0 = _arch_center(col, row)
            x1, _ = _arch_center(col + step, row)
            arrow((x0 + step * bw / 2, y0), (x1 - step * bw / 2, y0),
                  color=t.accent)

    # row hop: meshers -> assembly, a short vertical at the right edge
    hx, hy0 = _arch_center(3, 0)
    _, hy1 = _arch_center(3, 1)
    arrow((hx, hy0 - bh / 2), (hx, hy1 + bh / 2), color=t.accent)

    # feedback: hp-adapt -> sizing field, routed through the empty row gap
    # instead of the ~350 px swoop graphviz drew through open background.
    fx, fy = _arch_center(0, 1)
    sx, sy = _arch_center(2, 0)
    lane = ARCH_FEEDBACK_Y
    ax.plot([fx, fx, sx], [fy + bh / 2, lane, lane], color=t.accent,
            linewidth=1.6, linestyle=(0, (5, 3)), zorder=2,
            solid_joinstyle="miter")
    arrow((sx, lane), (sx, sy - bh / 2), color=t.accent, dashed=True)
    feedback = "refine / coarsen / p-elevate"
    fs.assert_glyphs(feedback)
    # Above the lane, not on it: the old plate sat on the dashed run *and* on
    # the advisor diagonal that crossed it. At 0.022 the label clears the lane
    # by 13 px and the row-0 boxes above it by 31 px at 164 dpi, so it reads as
    # the lane's label rather than as a caption under "feature analysis".
    ax.text((fx + sx) / 2, lane + 0.022, feedback, ha="center", va="bottom",
            zorder=5, fontsize=fs.FONT_PT["annot"], color=t.accent)

    # side outputs: taps, deliberately lighter than the stages they hang off
    for tx, ty, tw, label, src in ARCH_SIDES:
        th = _arch_line_h(fs.FONT_PT["annot"], label.count("\n") + 1) \
            + 2 * ARCH_TAP_PAD
        plate(tx, ty, tw, th, edge=t.grid, face=t.bg, lw=1.0, rounding=0.012)
        stack(tx, ty, label, "", label_pt=fs.FONT_PT["annot"], ink=t.muted)
        col, row = ARCH_STAGES[src][0], ARCH_STAGES[src][1]
        _, py = _arch_center(col, row)
        above = ty > py
        arrow((tx, py + (bh / 2 if above else -bh / 2)),
              (tx, ty + (-th / 2 if above else th / 2)),
              color=t.rule, scale=10.0, lw=1.2)

    fs.finish(fig, out)


# ---------------------------------------------------------------------------
# Grids
# ---------------------------------------------------------------------------
def tile_grid(images: list[Path], labels: list[str], out: Path, title: str,
              footer: str, tile_width: int, cols: int = 0) -> None:
    fs.assert_glyphs(title, footer, *labels)
    argv = [
        sys.executable,
        rel(REPO / "scripts/make_compare_grid.py"),
        "--out", rel(out),
        "--title", title,
        "--labels", ",".join(labels),
        "--footer", footer,
        "--tile-width", str(tile_width),
        "--cols", str(cols),
        *[rel(p) for p in images],
    ]
    print("    $ " + " ".join(argv))
    proc = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"make_compare_grid failed:\n{proc.stdout}\n{proc.stderr}")
    print("      " + proc.stdout.strip())




#: Tiles the engine declined during this run, as (tile, reason). Collected so
#: the comparison grid can state the refusal on its face instead of silently
#: showing one fewer panel.
REFUSED_TILES: list[tuple["MeshTile", str]] = []


def build_mesh_tiles(tiles: list[MeshTile], force: bool) -> list[Path]:
    """Mesh every tile, then render them all through one shared camera fit.

    The matched-panel invariant is asserted before anything is meshed, so a tile
    table that diverges in camera, zoom window or tile size fails immediately
    rather than producing a comparison the reader cannot trust.
    """
    import pyvista as pv

    matched_panels([
        PanelSpec(label=t.label, size=t.size, view=t.view, up=t.up,
                  focus=t.focus, window=t.window)
        for t in tiles
    ])

    CACHE.mkdir(parents=True, exist_ok=True)
    vtus: list[Path] = []
    refused: list[tuple[MeshTile, str]] = []
    kept: list[MeshTile] = []
    for tile in tiles:
        vtu = CACHE / f"{tile.tag}.vtu"
        argv = mesh_argv(tile, vtu)
        if force or not vtu.is_file():
            try:
                run_cli(argv, allow_refusal=True)
            except MeshRefused as declined:
                # The engine declining a mesh is a result, not a crash: the
                # volume-completeness guard rejects a mesh whose solid/void
                # topology is incomplete. Drop the tile, keep the reason, and
                # let the caller state it on the figure.
                print(f"    REFUSED {tile.label}: {declined.message}")
                vtu.unlink(missing_ok=True)
                refused.append((tile, declined.message))
                continue
        else:
            print(f"    reusing {rel(vtu)} (use --force to remesh)")
        tile.stats = {"cmd": " ".join(argv)}
        kept.append(tile)
        vtus.append(vtu)

    tiles[:] = kept
    REFUSED_TILES.extend(refused)
    if not tiles:
        return []

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
    fs.use("dark")          # 3D renders and their composites keep the dark stage

    if args.list:
        print("parts (gallery_<name>.png):")
        for part in PARTS:
            flag = "  <- flagship (also hero.png)" if part.name == FLAGSHIP else ""
            print(f"  {part.name:14s} mesher={part.mesher:7s} h={fmt_h(part.h):>8s}{flag}")
        print("other image stems: compare_meshers, compare_grading, architecture,")
        print("                   bench_dof_time, bench_tier1, bench_mms,")
        print("                   bench_advisor_budget, hero")
        return 0

    wanted = set(args.only)
    if not wanted and not args.all and not (args.stress or args.mesh):
        ap.error("pass --all, --only NAME, or --list")

    def want(name: str) -> bool:
        return args.all or name in wanted

    # architecture.png and the benchmark charts need no GL at all, so a
    # `--only architecture` run must not demand pyvista or an X server.
    three_d = {p.name for p in PARTS} | {
        "hero", "compare_meshers", "compare_grading",
    }
    needs_gl = bool(args.stress or args.mesh or args.all
                    or (wanted & three_d))

    if needs_gl:
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
                warp_frac=part.warp_frac,
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
            fs.assert_glyphs(images[-1]["caption"])
            timings.append(
                f"{part.name}: h={fmt_h(part.h)} mesher={part.mesher} "
                f"nodes={info['nodes']} elems={info['elems']} "
                f"solve_wall={wall if wall is None else round(wall, 2)}s"
            )

        if part.name == FLAGSHIP and want("hero"):
            info = render_stress(
                vtu, outdir / "hero.png",
                title=f"{part.title} \u2014 tension stress field",
                meta_extra=base_meta,
                footer_extra=f"{part.bc_note} \u00b7 low-oblique view",
                view=HERO_VIEW, up=HERO_UP, margin=1.02,
                warp_frac=part.warp_frac,
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
            fs.assert_glyphs(images[-1]["caption"])

    # ---- compare_meshers ---------------------------------------------------
    if want("compare_meshers"):
        print("[compare_meshers]")
        wanted = list(MESHER_TILES)
        pngs = build_mesh_tiles(MESHER_TILES, args.force)
        if not MESHER_TILES:
            print("  every mesher variant was declined; no grid to draw")
        else:
            counts = " \u00b7 ".join(
                f"{t.mesher}: {t.stats['nodes']:,} nodes / {t.stats['elems']:,} cells"
                for t in MESHER_TILES
            )
            declined = [(t, why) for t, why in REFUSED_TILES if t in wanted]
            title = (f"One part, {len(MESHER_TILES)} mesh "
                     f"topolog{'y' if len(MESHER_TILES) == 1 else 'ies'}")
            foot = (
                f"plate_hole.step at h = {fmt_h(wanted[0].h)} for all "
                f"{len(wanted)} variants · close-up on the hole, a "
                f"{CMP_WINDOW * 2000.0:g} mm window "
                f"(≈{CMP_WINDOW * 2000.0 / (HOLE_RADIUS_M * 1000.0):.1f} hole radii across) · "
                f"{counts}. All are Cartesian "
                f"grid-fill topologies (not Delaunay); only the cell zoo and "
                f"grading differ. Identical camera, zoom and tile size in "
                f"every panel (asserted, not assumed)."
            )
            for tile, why in declined:
                # The engine refusing a variant at this h is the finding, not a
                # missing panel: name the variant and quote the guard's
                # verdict. The engine appends its whole mesh report after the
                # first "|"; that belongs in the manifest, not in six lines of
                # figure caption.
                verdict = why.split(" | ", 1)[0].removeprefix("error: ").strip()
                foot += (f"\nDECLINED by the engine, so it has no panel: "
                         f"{tile.mesher} at this h \u2014 {verdict}")
            foot += "\n" + fs.provenance(
                *(CACHE / f"{t.tag}.vtu" for t in MESHER_TILES))
            # 2x2 rather than one row: three 1500x820 tiles side by side made a
            # 4.1:1 letterbox whose labels were unreadable at README width. The
            # remaining cell carries the caption.
            tile_grid(pngs, [t.label for t in MESHER_TILES],
                      outdir / "compare_meshers.png",
                      title, foot, tile_width=1000, cols=2)
            images.append({
                "file": "compare_meshers.png",
                "kind": "grid",
                "part": "plate_hole",
                "mesher": " | ".join(t.mesher for t in MESHER_TILES),
                "h": MESHER_TILES[0].h,
                "solve_command": None,
                "render_command":
                    "python3 scripts/render_showcase.py --only compare_meshers",
                "dofs": None,
                "wall_time_s": None,
                "max_von_mises_pa": None,
                "caption": (
                    "Mesh-only renders of the same plate at h = "
                    f"{fmt_h(wanted[0].h)} under "
                    f"{len(MESHER_TILES)} of {len(wanted)} meshers \u2014 "
                    + counts
                    + ". All are Cartesian grid-fill topologies, not Delaunay."
                    + "".join(f" {t.mesher} was declined by the engine at this "
                              f"h: {why}" for t, why in declined)
                ),
            })
            fs.assert_glyphs(images[-1]["caption"])
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
            f"({3 * uni['nodes']:,} vs {3 * gra['nodes']:,} DOF).\n"
            + fs.provenance(*(CACHE / f"{t.tag}.vtu" for t in GRADING_TILES))
        )
        # Two panels stay side by side -- the whole point is a paired read --
        # and 2 x 1300 px keeps the composite near 1.6:1.
        tile_grid(pngs, [t.label for t in GRADING_TILES],
                  outdir / "compare_grading.png",
                  "Uniform vs feature-graded sizing at a matched element budget",
                  foot, tile_width=1300, cols=2)
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
        fs.assert_glyphs(images[-1]["caption"])
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
         "L-domain: 6,384 -> 1,248 DOF and 2.762 s -> 0.227 s, bought at an energy "
         "deficit of 0.0888% against the baseline's 0.0854% — 1.04x as large, not "
         "parity. Internal self-comparison, not a comparison against any external "
         "solver; single run, no repeats."),
        ("bench_tier1.png", "tier1",
         "Tier-1 analytical verification: all 5 cases inside tolerance, the widest "
         "margin spending 0.7% of its budget and the narrowest 59%, measured on "
         "structured parametric verification meshes rather than product Cartesian "
         "grid-fill meshes."),
        ("bench_mms.png", "mms",
         "Method-of-manufactured-solutions energy-norm convergence: frozen P1 "
         "elements at 0.997/0.997/2.000/2.000 against theory 1/1/2/2, and the "
         "hierarchical p-basis at 1.02/1.99/2.98/3.98 against theory 1/2/3/4. No "
         "measured order falls more than 0.7% below theory; p=1 sits 2.0% above "
         "it, so the bound is one-sided."),
        ("bench_advisor_budget.png", "advisor_budget",
         "The learned mesh advisor under a DOF budget (ADR-0034): the action it "
         "picks at each --advisor-max-dof cap, against that action's predicted "
         "per-case relative-error score. 0 of the 18 capped picks is over budget "
         "and this sweep contains no refusal. 13 of the 21 invoked CLI solves "
         "exited nonzero — the picked action failed to mesh or solve — and those "
         "points are ringed; every plotted score is the shipped ONNX model's "
         "prediction, not a measured outcome."),
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
