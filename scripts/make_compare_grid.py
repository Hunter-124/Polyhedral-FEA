#!/usr/bin/env python3
"""Generic dark-theme image tiler for PolyMesh showcase comparison figures.

Composes N images into a single labeled grid using the PolyMesh Studio palette.
Reusable and standalone -- the showcase renderer calls it for
``compare_meshers.png`` and ``compare_grading.png``, but it will tile any images.

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
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ---------------------------------------------------------------------------
# Studio palette (shared contract with the GUI and the showcase renderer)
# ---------------------------------------------------------------------------
CHROME_BG = "#0E1116"
PANEL_BG = "#161B22"
HEADER_BG = "#1C2330"
BORDER = "#2A3240"
TEXT = "#E6EAF0"
TEXT_DIM = "#8A93A3"
ACCENT = "#4CC2FF"

FONT_DIRS = (
    "/usr/share/fonts/liberation-sans-fonts",
    "/usr/share/fonts/truetype/liberation",
    "/usr/share/fonts/dejavu-sans-fonts",
    "/usr/share/fonts/truetype/dejavu",
)
FONT_REGULAR = ("LiberationSans-Regular.ttf", "DejaVuSans.ttf")
FONT_BOLD = ("LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf")

GUTTER = 2          # px, BORDER-coloured separator between tiles
LABEL_H = 52        # px, per-tile label strip
LABEL_PX = 28       # px, label type size (per assignment)
TITLE_H = 84        # px, title strip
TITLE_PX = 36
FOOTER_PX = 21
PAD = 30


def load_font(names: tuple[str, ...], size: int) -> ImageFont.FreeTypeFont:
    """Resolve the first available TTF from ``names`` across the font dirs."""
    for directory in FONT_DIRS:
        for name in names:
            path = Path(directory) / name
            if path.is_file():
                return ImageFont.truetype(str(path), size)
    return ImageFont.load_default(size)


def text_size(draw: ImageDraw.ImageDraw, text: str, font) -> tuple[int, int]:
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    return right - left, bottom - top


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font, max_w: int) -> list[str]:
    """Greedy word wrap to `max_w` pixels."""
    lines: list[str] = []
    cur = ""
    for word in text.split():
        trial = f"{cur} {word}".strip()
        if draw.textlength(trial, font=font) <= max_w or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


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

    loaded = []
    for path in images:
        if not path.is_file():
            raise SystemExit(f"make_compare_grid: missing input image {path}")
        loaded.append(Image.open(path).convert("RGB"))

    cols = max(1, min(cols or len(loaded), len(loaded)))
    rows = (len(loaded) + cols - 1) // cols

    # Normalise every tile to a common size, preserving the first aspect ratio.
    base_w, base_h = loaded[0].size
    tw = tile_width or base_w
    th = max(1, round(tw * base_h / base_w))
    tiles = [img.resize((tw, th), Image.LANCZOS) for img in loaded]

    cell_h = th + LABEL_H
    canvas_w = cols * tw + (cols - 1) * GUTTER

    f_title = load_font(FONT_BOLD, TITLE_PX)
    f_label = load_font(FONT_BOLD, LABEL_PX)
    f_footer = load_font(FONT_REGULAR, FOOTER_PX)

    # Wrap the footer before sizing the canvas: a long provenance line would
    # otherwise run straight off the right edge and lose its own numbers.
    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    footer_lines = (
        wrap_text(probe, footer, f_footer, canvas_w - 2 * PAD) if footer else []
    )
    line_h = FOOTER_PX + 8
    footer_h = (len(footer_lines) * line_h + 20) if footer_lines else 0
    canvas_h = TITLE_H + rows * cell_h + (rows - 1) * GUTTER + footer_h

    canvas = Image.new("RGB", (canvas_w, canvas_h), CHROME_BG)
    draw = ImageDraw.Draw(canvas)

    # --- title strip --------------------------------------------------------
    draw.rectangle([0, 0, canvas_w, TITLE_H - 1], fill=HEADER_BG)
    if title:
        _, h = text_size(draw, title, f_title)
        draw.text((PAD, (TITLE_H - h) // 2 - 4), title, font=f_title, fill=TEXT)
    # accent rule along the bottom of the header
    draw.rectangle([0, TITLE_H - 3, canvas_w, TITLE_H - 1], fill=ACCENT)

    # --- tiles --------------------------------------------------------------
    for idx, tile in enumerate(tiles):
        r, c = divmod(idx, cols)
        x = c * (tw + GUTTER)
        y = TITLE_H + r * (cell_h + GUTTER)
        canvas.paste(tile, (x, y))

        # label strip under the tile
        draw.rectangle([x, y + th, x + tw - 1, y + th + LABEL_H - 1], fill=PANEL_BG)
        label = labels[idx] if idx < len(labels) else ""
        if label:
            lw, lh = text_size(draw, label, f_label)
            draw.text(
                (x + (tw - lw) // 2, y + th + (LABEL_H - lh) // 2 - 3),
                label,
                font=f_label,
                fill=TEXT,
            )
        # thin border around the whole cell
        draw.rectangle([x, y, x + tw - 1, y + th + LABEL_H - 1], outline=BORDER, width=1)

    # --- gutters ------------------------------------------------------------
    for c in range(1, cols):
        x = c * tw + (c - 1) * GUTTER
        draw.rectangle([x, TITLE_H, x + GUTTER - 1, canvas_h - footer_h - 1], fill=BORDER)
    for r in range(1, rows):
        y = TITLE_H + r * cell_h + (r - 1) * GUTTER
        draw.rectangle([0, y, canvas_w, y + GUTTER - 1], fill=BORDER)

    # --- footer -------------------------------------------------------------
    if footer_lines:
        fy = canvas_h - footer_h
        draw.rectangle([0, fy, canvas_w, canvas_h], fill=CHROME_BG)
        y = fy + 6
        for line in footer_lines:
            draw.text((PAD, y), line, font=f_footer, fill=TEXT_DIM)
            y += line_h

    return canvas


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tile images into a labeled dark-theme grid.")
    ap.add_argument("images", nargs="+", type=Path, help="input images, in grid order")
    ap.add_argument("--out", required=True, type=Path, help="output PNG path")
    ap.add_argument("--title", default="", help="title strip text")
    ap.add_argument("--labels", default="", help="comma-separated per-tile labels")
    ap.add_argument("--footer", default="", help="optional dim footer line")
    ap.add_argument("--cols", type=int, default=0, help="columns (default: one row)")
    ap.add_argument("--tile-width", type=int, default=0, help="per-tile width in px")
    args = ap.parse_args(argv)

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
