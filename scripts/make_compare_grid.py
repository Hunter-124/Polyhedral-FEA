#!/usr/bin/env python3
"""Generic dark-theme image tiler for PolyMesh showcase comparison figures.

Composes N images into a single labeled grid. Every colour, font and type size
comes from ``scripts/figstyle.py`` -- this module owns no palette of its own.
Reusable and standalone -- the showcase renderer calls it for
``compare_meshers.png`` and ``compare_grading.png``, but it will tile any images.

Two invariants are enforced here rather than left to convention, because this is
the one place every caller passes through:

* **Aspect.** The composite is never allowed past ``figstyle.MAX_ASPECT``
  (2.2:1). PIL composites get no ``fs.figure`` guard, so the same cap is
  applied by hand below; the column count is chosen to satisfy it.
* **Matched panels.** ``matched_panels`` asserts that a comparison's panels
  share one camera, one zoom window and one colour limit, and ``build_grid``
  asserts the input images are pixel-identical in size. A reader comparing
  topology must not be comparing framing.

Example
-------
    python3 scripts/make_compare_grid.py \
        --out docs/assets/showcase/compare_meshers.png \
        --title "One part, three mesh topologies" \
        --labels "tet,graded,hybrid" \
        a.png b.png c.png
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
import figstyle as fs  # noqa: E402

# Type sizes come from figstyle: fs.font_px converts a type role's point size to
# pixels for a canvas of a given width and floors it so the smallest text still
# clears MIN_TEXT_PX once a README scales the image down.


def theme() -> fs.Theme:
    """The dark stage. Comparison grids sit beside the 3D renders."""
    t = fs.theme()
    return t if t.name == "dark" else fs.use("dark")


GUTTER_FRAC = 0.0012        # separator width as a fraction of canvas width


# ---------------------------------------------------------------------------
# Matched-panel invariant
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class PanelSpec:
    """Everything about a panel that must match its siblings.

    ``label`` is free; every other field is compared exactly across the set.
    """

    label: str
    size: tuple[int, int]
    view: tuple[float, float, float]
    up: tuple[float, float, float]
    focus: tuple[float, float, float] | None = None
    window: float | None = None
    clim: tuple[float, float] | None = None


_MATCHED_FIELDS = ("size", "view", "up", "focus", "window", "clim")


def matched_panels(specs: list[PanelSpec]) -> None:
    """Raise unless every panel shares camera, zoom window and colour limits.

    A side-by-side comparison whose panels differ in framing or colour scale is
    not a comparison, so a diverging caller fails the build instead of shipping
    a figure that reads as a result.
    """
    if len(specs) < 2:
        return
    first = specs[0]
    problems = []
    for field_name in _MATCHED_FIELDS:
        ref = getattr(first, field_name)
        for other in specs[1:]:
            got = getattr(other, field_name)
            if got != ref:
                problems.append(
                    f"{field_name}: {first.label!r}={ref!r} vs {other.label!r}={got!r}"
                )
    if problems:
        raise SystemExit(
            "make_compare_grid: panels of a comparison must share camera, zoom "
            "and colour limits:\n  " + "\n  ".join(problems)
        )


# ---------------------------------------------------------------------------
# Text helpers
# ---------------------------------------------------------------------------
def text_size(draw: ImageDraw.ImageDraw, text: str, font) -> tuple[int, int]:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    return right - left, bottom - top


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: int) -> list[str]:
    """Greedy word wrap to `max_w` pixels, with explicit newlines as breaks.

    The provenance stamp arrives as the last line of the caption and has to
    stay on its own line: flowed into the prose it is unfindable.
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


def _aspect(w: int, h: int) -> float:
    return max(w, h) / max(1, min(w, h))


def choose_cols(n: int, tile_w: int, tile_h: int, label_h: int, title_h: int) -> int:
    """Column count whose composite is inside the aspect cap and reads best.

    A single row of three 1500x820 tiles is 4:1 -- unreadable once a README
    scales it to 900 px. Preferring an aspect near 3:2 puts three panels into a
    2x2 whose fourth cell carries the caption.
    """
    best, best_cost = 1, None
    for cols in range(1, n + 1):
        rows = (n + cols - 1) // cols
        w = cols * tile_w
        h = title_h + rows * (tile_h + label_h)
        aspect = _aspect(w, h)
        if aspect > fs.MAX_ASPECT:
            continue
        cost = abs(aspect - 1.5)
        if best_cost is None or cost < best_cost:
            best, best_cost = cols, cost
    if best_cost is None:                      # every layout is letterboxed
        return max(1, round(n ** 0.5))
    return best


def build_grid(
    images: list[Path],
    labels: list[str],
    title: str,
    footer: str,
    cols: int,
    tile_width: int | None,
) -> Image.Image:
    if not images:
        raise SystemExit("make_compare_grid: no input images")
    t = theme()

    loaded = []
    for path in images:
        if not path.is_file():
            raise SystemExit(f"make_compare_grid: missing input image {path}")
        loaded.append(Image.open(path).convert("RGB"))

    # Zoom invariant: identical source pixel sizes. Panels rendered at different
    # sizes cannot have been framed by one camera at one scale.
    sizes = {img.size for img in loaded}
    if len(sizes) > 1:
        raise SystemExit(
            "make_compare_grid: panels must be rendered at one size so the "
            f"reader compares content and not zoom; got {sorted(sizes)}"
        )

    base_w, base_h = loaded[0].size
    tw = tile_width or base_w
    th = max(1, round(tw * base_h / base_w))

    # Provisional type metrics off a one-row canvas width, only to size the
    # strips well enough to choose a column count; recomputed once cols is set.
    probe_w = len(loaded) * tw
    label_h = int(round(fs.font_px("panel", probe_w) * 1.9))
    title_h = int(round(fs.font_px("title", probe_w) * 2.4))

    n = len(loaded)
    cols = max(1, min(cols or choose_cols(n, tw, th, label_h, title_h), n))
    rows = (n + cols - 1) // cols

    gutter = max(1, int(round(cols * tw * GUTTER_FRAC)))
    canvas_w = cols * tw + (cols - 1) * gutter

    title_px = fs.font_px("title", canvas_w)
    label_px = fs.font_px("panel", canvas_w)
    footer_px = fs.font_px("footer", canvas_w)
    f_title = fs.pil_font("title", canvas_w, bold=True)
    f_label = fs.pil_font("panel", canvas_w, bold=True)
    f_footer = fs.pil_font("footer", canvas_w)

    label_h = int(round(label_px * 1.9))
    title_h = int(round(title_px * 2.4))
    pad = int(round(footer_px * 1.6))
    line_h = int(round(footer_px * 1.45))

    fs.assert_glyphs(title, footer, *labels)

    tiles = [img.resize((tw, th), Image.LANCZOS) for img in loaded]
    cell_h = th + label_h

    # A partly filled last row leaves exactly one empty cell for three panels in
    # a 2x2: put the caption there instead of stretching the canvas.
    spare_cell = (rows * cols - n) == 1 and cols > 1 and bool(footer)
    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    if footer and not spare_cell:
        footer_lines = wrap_text(probe, footer, f_footer, canvas_w - 2 * pad)
        footer_h = len(footer_lines) * line_h + 2 * pad
    else:
        footer_lines, footer_h = [], 0
    cell_footer_lines = (
        wrap_text(probe, footer, f_footer, tw - 2 * pad) if spare_cell else []
    )

    canvas_h = title_h + rows * cell_h + (rows - 1) * gutter + footer_h

    # Same cap fs.figure enforces for matplotlib figures, applied by hand
    # because a PIL composite never passes through it.
    if _aspect(canvas_w, canvas_h) > fs.MAX_ASPECT:
        raise SystemExit(
            f"make_compare_grid: {canvas_w}x{canvas_h} is "
            f"{_aspect(canvas_w, canvas_h):.2f}:1, past the "
            f"{fs.MAX_ASPECT}:1 cap; reduce --cols or --tile-width"
        )

    canvas = Image.new("RGB", (canvas_w, canvas_h), t.bg)
    draw = ImageDraw.Draw(canvas)

    # --- title strip --------------------------------------------------------
    draw.rectangle([0, 0, canvas_w, title_h - 1], fill=t.panel)
    if title:
        _, h = text_size(draw, title, f_title)
        draw.text((pad, (title_h - h) // 2 - title_px // 8), title,
                  font=f_title, fill=t.ink)
    rule = max(2, title_px // 12)
    draw.rectangle([0, title_h - rule, canvas_w, title_h - 1], fill=t.accent)

    # --- tiles --------------------------------------------------------------
    for idx, tile in enumerate(tiles):
        r, c = divmod(idx, cols)
        x = c * (tw + gutter)
        y = title_h + r * (cell_h + gutter)
        canvas.paste(tile, (x, y))

        draw.rectangle([x, y + th, x + tw - 1, y + th + label_h - 1], fill=t.panel)
        label = labels[idx] if idx < len(labels) else ""
        if label:
            lw, lh = text_size(draw, label, f_label)
            draw.text(
                (x + (tw - lw) // 2, y + th + (label_h - lh) // 2 - label_px // 8),
                label, font=f_label, fill=t.ink,
            )
        draw.rectangle([x, y, x + tw - 1, y + th + label_h - 1],
                       outline=t.grid, width=max(1, gutter // 2))

    # --- gutters ------------------------------------------------------------
    for c in range(1, cols):
        x = c * tw + (c - 1) * gutter
        draw.rectangle([x, title_h, x + gutter - 1, canvas_h - footer_h - 1],
                       fill=t.grid)
    for r in range(1, rows):
        y = title_h + r * cell_h + (r - 1) * gutter
        draw.rectangle([0, y, canvas_w, y + gutter - 1], fill=t.grid)

    # --- caption ------------------------------------------------------------
    if cell_footer_lines:
        r, c = divmod(n, cols)
        x = c * (tw + gutter)
        y = title_h + r * (cell_h + gutter)
        draw.rectangle([x, y, x + tw - 1, y + cell_h - 1], fill=t.panel)
        draw.rectangle([x, y, x + tw - 1, y + cell_h - 1],
                       outline=t.grid, width=max(1, gutter // 2))
        ty = y + pad
        for line in cell_footer_lines:
            draw.text((x + pad, ty), line, font=f_footer, fill=t.muted)
            ty += line_h
    elif footer_lines:
        fy = canvas_h - footer_h
        draw.rectangle([0, fy, canvas_w, canvas_h], fill=t.bg)
        y = fy + pad
        for line in footer_lines:
            draw.text((pad, y), line, font=f_footer, fill=t.muted)
            y += line_h

    return canvas


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tile images into a labeled dark-theme grid.")
    ap.add_argument("images", nargs="+", type=Path, help="input images, in grid order")
    ap.add_argument("--out", required=True, type=Path, help="output PNG path")
    ap.add_argument("--title", default="", help="title strip text")
    ap.add_argument("--labels", default="", help="comma-separated per-tile labels")
    ap.add_argument("--footer", default="", help="optional dim footer line")
    ap.add_argument("--cols", type=int, default=0,
                    help="columns (default: chosen to stay inside the aspect cap)")
    ap.add_argument("--tile-width", type=int, default=0, help="per-tile width in px")
    args = ap.parse_args(argv)

    fs.use("dark")
    labels = [s.strip() for s in args.labels.split(",")] if args.labels else []
    grid = build_grid(
        images=args.images,
        labels=labels,
        title=args.title,
        footer=args.footer,
        cols=args.cols,
        tile_width=args.tile_width or None,
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    grid.save(args.out)
    print(f"wrote {args.out}  ({grid.width}x{grid.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
