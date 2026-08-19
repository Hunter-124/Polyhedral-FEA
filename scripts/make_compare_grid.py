#!/usr/bin/env python3
"""Generic dark-theme image tiler for PolyMesh showcase comparison figures.

Composes N images into a single labeled grid. Every colour, font and type size
comes from ``scripts/figstyle.py`` -- this module owns no palette of its own.
Reusable and standalone -- the showcase renderer calls it for
``compare_meshers.png`` and ``compare_grading.png``, but it will tile any images.

Three invariants are enforced here rather than left to convention, because this
is the one place every caller passes through:

* **Aspect.** The composite is never allowed past ``figstyle.MAX_ASPECT``
  (2.2:1). PIL composites get no ``fs.figure`` guard, so the same cap is
  applied by hand below; the column count is chosen to satisfy it.
* **Matched panels.** ``matched_panels`` asserts that a comparison's panels
  share one camera, one zoom window and one colour limit, and ``build_grid``
  asserts the input images are pixel-identical in size. A reader comparing
  topology must not be comparing framing.
* **Identical treatment.** Every panel is drawn as the same card: one tile
  size, one padding, one keyline drawn *outside* the image so no panel loses
  edge pixels another panel keeps, and a label strip whose height and baselines
  are shared across the whole grid even when one label is longer than the rest.
  Nothing is cropped, zoomed or re-framed per panel.

Layout
------
A card grid on the page colour: outer margin, one gutter between cards, a
masthead (title over an accent rule) above them, and the caption either in the
spare cell of a partly filled last row or in a full-width strip beneath. The
caption is split into prose, one notice per tile the engine refused, and the
provenance stamp -- drawn as one grey paragraph, a refusal reads as more prose
and the stamp is unfindable.

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
from typing import Any

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


# ---------------------------------------------------------------------------
# Composition constants
# ---------------------------------------------------------------------------
# Spacing is a fraction of the tiled width (cols * tile width), so the 2088 px
# three-panel grid and the 2715 px pair end up with the same rhythm.
#
# Measured on those two: at the old 0.004 the meshers grid got an 8 px gutter
# and no outer margin at all, so the tiles bled off three sides of the canvas
# and the gutter read as a seam between two touching images rather than as a
# separation. 0.012 is 24 px there and 31 px on the grading grid -- 11 px on
# both once a README scales them to 900 px wide.
GUTTER_FRAC = 0.012
# Margin wider than the gutter: a gutter has canvas on both sides, a margin
# only on one, so an equal margin reads as the smaller of the two. 0.016 puts
# 32 / 42 px of page colour around the grid.
MARGIN_FRAC = 0.016
# One keyline pixel per 1000 px of canvas -- 2 px at showcase width, ~1 px once
# a README scales it down. Enough to close a tile whose mesh runs pale to its
# own edge, thin enough not to read as a picture frame.
KEYLINE_PER_PX = 1000

#: A caption in a spare cell takes the largest of these type roles that fits
#: the cell box, largest first; the elastic full-width strip always uses the
#: middle one. Everything is a figstyle role, so a caption never invents a size.
_CAPTION_ROLES = ("panel", "label", "footer")

#: ``render_showcase`` writes one line per variant the engine declined, and
#: ``fs.provenance`` writes the stamp as the last line. Both have to survive in
#: what is otherwise a paragraph of grey prose, so they are matched here and
#: drawn differently: the refusal gets a warn keyline and full-strength ink, the
#: stamp its own smaller line under a rule.
_NOTICE_PREFIXES = ("DECLINED", "REFUSED")
_STAMP_PREFIX = "regime:"


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
#: One styled piece of a line: (text, font, colour).
Run = tuple[str, Any, str]


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


def line_box(runs: list[Run]) -> tuple[int, int, int]:
    """(advance width, ascent, descent) for a styled line.

    Vertical placement comes from the *font's* ascent and descent, never from
    the ink box of one particular string: "tet" and "hybrid  ·  hex bulk +
    transition cells" must sit on the same baseline, and ink-box centring would
    shift a label up or down by whether it happens to contain a descender.
    """
    width = 0.0
    ascent = descent = 0
    for text, font, _ in runs:
        if not text:
            continue
        width += font.getlength(text)
        asc, desc = font.getmetrics()
        ascent, descent = max(ascent, asc), max(descent, desc)
    return int(round(width)), ascent, descent


def draw_line(draw: ImageDraw.ImageDraw, runs: list[Run], x: int,
              baseline: int) -> None:
    """Draw a styled line left-to-right on a shared baseline."""
    for text, font, fill in runs:
        if not text:
            continue
        draw.text((x, baseline), text, font=font, fill=fill, anchor="ls")
        x += int(round(font.getlength(text)))


def keyline(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int,
            width: int, colour: str) -> None:
    """Hairline around a card, drawn outside it so no tile pixel is covered."""
    draw.rectangle([x - width, y - width, x + w - 1 + width, y + h - 1 + width],
                   outline=colour, width=width)


def _label_parts(label: str) -> tuple[str, str, str]:
    """Split a label into (name, separator, qualifier) on the caller's own mark.

    Callers spell labels as ``"tet  ·  Cartesian grid-fill tet4"`` or
    ``"uniform sizing field  (--no-feature)"``: a short identity plus a
    description. The split is on the separator the caller already used, so no
    word is added, removed or reordered.
    """
    for sep in ("\u00b7", "  "):
        head, found, tail = label.partition(sep)
        if found:
            return head.strip(), (sep if sep.strip() else ""), tail.strip()
    return label.strip(), "", ""


def _label_lines(draw: ImageDraw.ImageDraw, part: tuple[str, str, str],
                 f_name, f_qual, avail: int, *, stacked: bool) -> list[list[Run]]:
    """A label as styled lines: identity in bold ink, qualifier in muted text.

    One bold run puts the description in competition with the name it
    describes; split, the name is what the reader scans across the row and the
    description is what they read second.
    """
    t = theme()
    name, sep, qual = part
    if not name and not qual:
        return []
    if not stacked:
        runs: list[Run] = [(name, f_name, t.ink)]
        if qual:
            # The caller's own separator, in accent, or -- where the caller
            # separated with whitespace -- a gap wide enough that weight and
            # colour are not the only break. Measured at the two showcase
            # widths: "  ·  " is 54 / 70 px and four spaces 43 / 56 px, close
            # enough that the two labellings read as one system.
            runs.append((f"  {sep}  " if sep else "    ", f_qual, t.accent))
            runs.append((qual, f_qual, t.muted))
        return [runs]
    lines: list[list[Run]] = [[(name, f_name, t.ink)]] if name else []
    for line in wrap_text(draw, qual, f_qual, avail):
        lines.append([(line, f_qual, t.muted)])
    return lines


# ---------------------------------------------------------------------------
# Caption
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class _Row:
    """One laid-out caption line. ``space`` rows carry height and no text."""

    kind: str
    text: str
    height: int


def _caption_parts(footer: str) -> tuple[list[str], list[str], list[str]]:
    """Footer text split into prose paragraphs, refusal notices and stamp."""
    prose: list[str] = []
    notices: list[str] = []
    stamp: list[str] = []
    for raw in footer.split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith(_STAMP_PREFIX):
            stamp.append(line)
        elif line.upper().startswith(_NOTICE_PREFIXES):
            notices.append(line)
        else:
            prose.append(line)
    return prose, notices, stamp


class _Caption:
    """The footer, wrapped and measured for a box of a given width.

    Built once per candidate type role so ``build_grid`` can ask "does this fit
    the spare cell" before committing, and drawn either as a strip under the
    grid or inside a caption card.
    """

    def __init__(self, draw: ImageDraw.ImageDraw, footer: str, canvas_w: int,
                 width: int, role: str) -> None:
        prose, notices, stamp = _caption_parts(footer)
        self.role = role
        self.prose_px = fs.font_px(role, canvas_w)
        self.stamp_px = fs.font_px("footer", canvas_w)
        self.f_prose = fs.pil_font(role, canvas_w)
        self.f_stamp = fs.pil_font("footer", canvas_w)
        # A warn bar 16% of the type size is ~5 px at showcase width: visible
        # in the margin of the paragraph without becoming a second column.
        self.bar_w = max(2, round(self.prose_px * 0.16))
        self.indent = self.bar_w + round(self.prose_px * 0.55)
        self.block_gap = round(self.prose_px * 0.75)
        self.stamp_gap = round(self.stamp_px * 1.1) if stamp else 0
        line_h = round(self.prose_px * 1.45)

        rows: list[_Row] = []
        for paragraph in prose:
            if rows:
                rows.append(_Row("space", "", self.block_gap))
            rows += [_Row("prose", line, line_h)
                     for line in wrap_text(draw, paragraph, self.f_prose, width)]
        for notice in notices:
            if rows:
                rows.append(_Row("space", "", self.block_gap))
            rows += [_Row("notice", line, line_h)
                     for line in wrap_text(draw, notice, self.f_prose,
                                           width - self.indent)]
        self.body = rows
        stamp_line_h = round(self.stamp_px * 1.45)
        self.stamp = [_Row("stamp", line, stamp_line_h)
                      for text in stamp
                      for line in wrap_text(draw, text, self.f_stamp, width)]

    @property
    def body_h(self) -> int:
        return sum(row.height for row in self.body)

    @property
    def stamp_h(self) -> int:
        return sum(row.height for row in self.stamp)

    def expand(self, fill_to: int) -> None:
        """Spend slack on leading so a caption card fills the cell it occupies.

        A caption that stops halfway down its card leaves the grid reading as if
        a panel were missing. Leading is capped at 1.9x the type size: past that
        the lines stop reading as a paragraph.
        """
        text_rows = [row for row in self.body if row.kind != "space"]
        if not text_rows:
            return
        slack = fill_to - self.body_h
        if slack <= 0:
            return
        add = min(round(self.prose_px * 1.9) - text_rows[0].height,
                  slack // len(text_rows))
        if add <= 0:
            return
        self.body = [row if row.kind == "space"
                     else _Row(row.kind, row.text, row.height + add)
                     for row in self.body]

    def draw_rows(self, draw: ImageDraw.ImageDraw, rows: list[_Row], x: int,
                  y: int) -> int:
        """Draw rows from the top down; return the y past the last one."""
        t = theme()
        for row in rows:
            if row.kind == "space":
                y += row.height
                continue
            font = self.f_stamp if row.kind == "stamp" else self.f_prose
            ascent, descent = font.getmetrics()
            baseline = y + (row.height - (ascent + descent)) // 2 + ascent
            tx = x
            if row.kind == "notice":
                # Adjacent notice rows abut, so a wrapped refusal gets one
                # continuous bar rather than a dashed column.
                draw.rectangle([x, y, x + self.bar_w - 1, y + row.height - 1],
                               fill=t.warn)
                tx = x + self.indent
            draw.text((tx, baseline), row.text, font=font, anchor="ls",
                      fill=t.ink if row.kind == "notice" else t.muted)
            y += row.height
        return y


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
            # A tile the engine declined never reaches this module (the caller
            # drops it and states the refusal in the footer), so a missing file
            # here is a broken build, not a result: fail rather than paste a
            # placeholder that would read as a rendered panel.
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

    tiled_w = cols * tw
    gutter = max(2, int(round(tiled_w * GUTTER_FRAC)))
    content_w = tiled_w + (cols - 1) * gutter
    margin = max(gutter, int(round(tiled_w * MARGIN_FRAC)))
    canvas_w = content_w + 2 * margin
    line_w = max(1, round(canvas_w / KEYLINE_PER_PX))

    title_px = fs.font_px("title", canvas_w)
    label_px = fs.font_px("panel", canvas_w)
    f_title = fs.pil_font("title", canvas_w, bold=True)
    f_name = fs.pil_font("panel", canvas_w, bold=True)
    f_qual = fs.pil_font("panel", canvas_w)

    fs.assert_glyphs(title, footer, *labels)
    probe = ImageDraw.Draw(Image.new("RGB", (1, 1)))
    tiles = [img.resize((tw, th), Image.LANCZOS) for img in loaded]

    # --- label strips: one geometry for the whole grid ----------------------
    pad_x = round(label_px * 0.7)
    avail = tw - 2 * pad_x
    parts = [_label_parts(labels[i]) if i < len(labels) else ("", "", "")
             for i in range(n)]
    flowed = [_label_lines(probe, part, f_name, f_qual, avail, stacked=False)
              for part in parts]
    # If one label is too wide for its tile, every label stacks. Wrapping only
    # the long one would give that panel a taller strip and a different
    # treatment from its siblings.
    stacked = any(line_box(lines[0])[0] > avail for lines in flowed if lines)
    tile_lines = flowed if not stacked else [
        _label_lines(probe, part, f_name, f_qual, avail, stacked=True)
        for part in parts
    ]
    label_lines = max((len(lines) for lines in tile_lines), default=0)
    name_asc, name_desc = f_name.getmetrics()
    qual_asc, qual_desc = f_qual.getmetrics()
    label_asc, label_desc = max(name_asc, qual_asc), max(name_desc, qual_desc)
    label_line_h = label_asc + label_desc
    label_pad = round(label_px * 0.62)
    label_gap = round(label_px * 0.30)
    label_h = (2 * label_pad + label_lines * label_line_h
               + max(0, label_lines - 1) * label_gap) if label_lines else 0
    cell_h = th + label_h

    # --- masthead -----------------------------------------------------------
    if title:
        title_asc, title_desc = f_title.getmetrics()
        rule_h = max(2, round(title_px * 0.10))
        rule_gap = round(title_px * 0.42)
        head_h = (title_asc + title_desc + rule_gap + rule_h
                  + round(title_px * 0.80))
    else:
        title_asc = title_desc = rule_h = rule_gap = 0
        head_h = 0
    grid_top = margin + head_h
    grid_h = rows * cell_h + (rows - 1) * gutter

    # --- caption placement --------------------------------------------------
    # A partly filled last row leaves exactly one empty cell for three panels in
    # a 2x2: put the caption there instead of stretching the canvas.
    cap_pad = round(label_px * 0.85)
    cell_caption: _Caption | None = None
    strip_caption: _Caption | None = None
    if footer and (rows * cols - n) == 1 and cols > 1:
        for role in _CAPTION_ROLES:
            candidate = _Caption(probe, footer, canvas_w, tw - 2 * pad_x, role)
            fixed = (2 * cap_pad + candidate.stamp_h
                     + (candidate.stamp_gap * 2 + line_w if candidate.stamp else 0))
            if fixed + candidate.body_h <= cell_h:
                # Fill the card: the prose block gets the cell's slack as
                # leading and the stamp stays pinned above the bottom padding.
                candidate.expand(cell_h - fixed)
                cell_caption = candidate
                break
    if footer and cell_caption is None:
        strip_caption = _Caption(probe, footer, canvas_w, content_w, "label")

    if strip_caption is not None:
        strip_pad = round(strip_caption.prose_px * 0.95)
        footer_h = (margin + line_w + strip_pad + strip_caption.body_h
                    + strip_caption.stamp_gap + strip_caption.stamp_h)
    else:
        strip_pad = 0
        footer_h = 0

    canvas_h = grid_top + grid_h + footer_h + margin

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

    # --- masthead -----------------------------------------------------------
    if title:
        draw.text((margin, margin + title_asc), title, font=f_title,
                  fill=t.ink, anchor="ls")
        rule_y = margin + title_asc + title_desc + rule_gap
        draw.rectangle([margin, rule_y, margin + content_w - 1, rule_y + rule_h - 1],
                       fill=t.rule)
        # Accent lead-in as wide as the title, floored at 8% of the content so a
        # two-word title still gets a visible mark rather than a dash.
        lead = min(content_w, max(round(content_w * 0.08),
                                  int(round(f_title.getlength(title)))))
        draw.rectangle([margin, rule_y, margin + lead - 1, rule_y + rule_h - 1],
                       fill=t.accent)

    # --- tiles --------------------------------------------------------------
    stack_h = (label_lines * label_line_h
               + max(0, label_lines - 1) * label_gap) if label_lines else 0
    for idx, tile in enumerate(tiles):
        r, c = divmod(idx, cols)
        x = margin + c * (tw + gutter)
        y = grid_top + r * (cell_h + gutter)
        if label_h:
            draw.rectangle([x, y + th, x + tw - 1, y + cell_h - 1], fill=t.panel)
        canvas.paste(tile, (x, y))
        keyline(draw, x, y, tw, cell_h, line_w, t.rule)

        # Every strip is the same height and every line index sits on the same
        # baseline across the grid, so a one-line label and a two-line label
        # cannot look vertically misaligned.
        top = y + th + (label_h - stack_h) // 2
        for i, runs in enumerate(tile_lines[idx]):
            width, _, _ = line_box(runs)
            draw_line(draw, runs, x + (tw - width) // 2,
                      top + i * (label_line_h + label_gap) + label_asc)

    # --- caption ------------------------------------------------------------
    if cell_caption is not None:
        r, c = divmod(n, cols)
        x = margin + c * (tw + gutter)
        y = grid_top + r * (cell_h + gutter)
        draw.rectangle([x, y, x + tw - 1, y + cell_h - 1], fill=t.panel)
        keyline(draw, x, y, tw, cell_h, line_w, t.rule)
        # The stamp is pinned above the bottom padding and the prose is centred
        # in what is left, so whatever leading `expand` could not spend (it caps
        # at 1.9x the type size) shows as balanced space above and below the
        # paragraph rather than as one hole under it. Measured on the meshers
        # card: 187 px of clear panel above the first line's ink and 184 px
        # below the last line's, against the 202 / 169 that centring on the
        # rule's clearance gap as well produced.
        stamp_block = ((cell_caption.stamp_gap * 2 + line_w + cell_caption.stamp_h)
                       if cell_caption.stamp else 0)
        body_space = cell_h - 2 * cap_pad - stamp_block
        body_top = y + cap_pad + max(0, (body_space - cell_caption.body_h) // 2)
        cell_caption.draw_rows(draw, cell_caption.body, x + pad_x, body_top)
        if cell_caption.stamp:
            stamp_top = y + cell_h - cap_pad - cell_caption.stamp_h
            rule_y = stamp_top - cell_caption.stamp_gap - line_w
            draw.rectangle([x + pad_x, rule_y, x + tw - 1 - pad_x,
                            rule_y + line_w - 1], fill=t.rule)
            cell_caption.draw_rows(draw, cell_caption.stamp, x + pad_x, stamp_top)
    elif strip_caption is not None:
        y = grid_top + grid_h + margin
        draw.rectangle([margin, y, margin + content_w - 1, y + line_w - 1],
                       fill=t.rule)
        y = strip_caption.draw_rows(draw, strip_caption.body, margin,
                                    y + line_w + strip_pad)
        strip_caption.draw_rows(draw, strip_caption.stamp, margin,
                                y + strip_caption.stamp_gap)

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
