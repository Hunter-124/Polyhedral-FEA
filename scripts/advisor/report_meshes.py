# SPDX-License-Identifier: BSD-3-Clause
"""``mesh_before_after.png``: warehouse wireframe renders of coarse vs best meshes."""
from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Sequence

import matplotlib
import numpy as np

import figstyle as fs

from .paths import ADVISOR_DIR
from .report_common import OK_STATUS, corpus_rows, family_of, fmt, save, to_float

try:
    from PIL import Image
except ImportError:
    Image = None  # type: ignore[assignment]


#: Renders are read from the campaign warehouse. ``wire_feature.png`` is the
#: bore-framed camera and is preferred when present: on a flat plate the
#: whole-part camera puts the hole rim edge-on, which is exactly the detail a
#: before/after pair is about.
#:
#: It is written by::
#:
#:     python scripts/warehouse_shots.py <campaign> \
#:         --out-name wire_feature.png --hole-zoom
#:
#: which passes ``--hole-zoom --require-hole`` to scripts/vtu_wire_png.py, so
#: the file exists ONLY for a part with a measured bore. An earlier version of
#: this comment credited a ``--feature`` flag that has never existed; because
#: nothing could write the preferred name, the fallback below was the
#: permanent behaviour and the edge-on rim survived every later pass. Check
#: the flag against the script before trusting a comment like this one.
WIRE_NAMES = ("wire_feature.png", "wire.png")


def wire_path(campaigns_dir: Path, row: dict[str, str]) -> Path | None:
    base = (campaigns_dir / row["campaign"] / "runs" / row["cfg_id"]
            / row["part"] / "t0")
    for name in WIRE_NAMES:
        if (base / name).is_file():
            return base / name
    return None


def _ink(path: Path, tol: int = 18) -> tuple[Any, tuple[int, int, int, int]]:
    """Warehouse render -> dark wireframe on the page colour, plus its bbox.

    The warehouse writes near-black lines on a flat saturated background. That
    background is a render setting, not data: it carries no meaning, fights
    the rest of the report and hides the thinnest wires. Everything that is
    not background becomes ink whose darkness tracks how far the pixel sits
    from the background, so the mesh reads as a line drawing.
    """
    image = Image.open(path).convert("RGB")
    pixels = np.asarray(image).astype(np.float32)
    corners = np.array([pixels[0, 0], pixels[0, -1], pixels[-1, 0], pixels[-1, -1]])
    background = np.median(corners, axis=0)
    distance = np.abs(pixels - background).sum(axis=2)
    mask = distance > tol
    if not mask.any():
        return image, (0, 0, image.width, image.height)
    ys, xs = np.nonzero(mask)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    strength = np.clip(distance / max(float(distance.max()), 1.0), 0.0, 1.0)
    strength = strength ** 0.6  # lift the faint wires out of the background
    page = np.array(matplotlib.colors.to_rgb(fs.theme().panel)) * 255.0
    ink = np.array(matplotlib.colors.to_rgb(fs.theme().ink)) * 255.0
    blended = page + (ink - page) * strength[..., None]
    return Image.fromarray(blended.astype(np.uint8), "RGB"), box


def matched_pair(paths: Sequence[Path], pad: int = 12,
                 target: int = 620) -> list[Any]:
    """Crop a set of renders to ONE common scale and ONE common canvas.

    A before/after pair drawn at different apparent sizes is not a comparison.
    Every panel here is cropped to the union subject box, scaled by the same
    factor and padded onto the same canvas, so a size difference on the page
    is a size difference in the mesh.
    """
    loaded = [_ink(path) for path in paths]
    width = max(box[2] - box[0] for _, box in loaded) + 2 * pad
    height = max(box[3] - box[1] for _, box in loaded) + 2 * pad
    scale = min(1.0, target / max(width, height))
    canvas_size = (max(1, round(width * scale)), max(1, round(height * scale)))

    out = []
    for image, box in loaded:
        crop = image.crop((box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad))
        crop = crop.resize((max(1, round(crop.width * scale)),
                            max(1, round(crop.height * scale))), Image.LANCZOS)
        canvas = Image.new("RGB", canvas_size,
                           tuple(round(c * 255) for c in
                                 matplotlib.colors.to_rgb(fs.theme().panel)))
        canvas.paste(crop, ((canvas_size[0] - crop.width) // 2,
                            (canvas_size[1] - crop.height) // 2))
        out.append(canvas)
    return out


def _caption(row: dict[str, str]) -> str:
    text = (f"{fs.quantity_label('h_rel')} {to_float(row['h_rel']):.3g}"
            f"  ·  order {row['order']}\n"
            f"{fs.series(row['mesher']).label}  ·  "
            f"{int(to_float(row['n_dof'])):,} {fs.quantity_label('n_dof')}\n"
            f"{fs.quantity_label('accuracy_rel_err')} "
            f"{to_float(row['accuracy_rel_err']):.3g}")
    fs.assert_glyphs(text)
    return text


def pick_before_after(rows: list[dict[str, str]], campaigns_dir: Path,
                      n_parts: int = 3) -> list[tuple[str, dict, dict]]:
    """Per family, the part whose coarse->best accuracy gain is largest."""
    usable = [r for r in corpus_rows(rows)
              if r["status"] == OK_STATUS
              and math.isfinite(to_float(r["accuracy_rel_err"]))
              and math.isfinite(to_float(r["h_rel"]))
              and wire_path(campaigns_dir, r) is not None]
    by_part: dict[str, list[dict[str, str]]] = {}
    for row in usable:
        by_part.setdefault(row["part"], []).append(row)

    best_per_family: dict[str, tuple[float, str, dict, dict]] = {}
    for part, part_rows in sorted(by_part.items()):
        if len(part_rows) < 2:
            continue
        # Baseline = the coarsest mesh actually run: largest h_rel, and among
        # those the least resolved. Without the dof tie-break the "before"
        # tile can end up finer than the "after" one, since a single h_rel
        # level covers both element orders.
        coarse = max(part_rows, key=lambda r: (to_float(r["h_rel"]),
                                               -to_float(r["n_dof"])))
        best = min(part_rows, key=lambda r: to_float(r["accuracy_rel_err"]))
        if coarse is best or to_float(coarse["accuracy_rel_err"]) <= to_float(
                best["accuracy_rel_err"]):
            continue
        gain = to_float(coarse["accuracy_rel_err"]) / to_float(best["accuracy_rel_err"])
        family = family_of(part)
        if family not in best_per_family or gain > best_per_family[family][0]:
            best_per_family[family] = (gain, part, coarse, best)

    ranked = sorted(best_per_family.values(), key=lambda item: -item[0])
    return [(part, coarse, best) for _, part, coarse, best in ranked[:n_parts]]


def mesh_before_after(rows: list[dict[str, str]], campaigns_dir: Path,
                      out_dir: Path) -> bool:
    if Image is None:
        print("no data yet — Pillow is not importable (pip install pillow); "
              "skipping mesh_before_after.png")
        return False
    picks = pick_before_after(rows, campaigns_dir)
    if not picks:
        print(f"no data yet — no warehouse renders "
              f"({' or '.join(WIRE_NAMES)}) under "
              f"{campaigns_dir}/<campaign>/runs/<cfg_id>/<part>/t0/; "
              "skipping mesh_before_after.png")
        return False

    print(f"\nmesh_before_after.png — {len(picks)} parts, real warehouse renders")
    used = [wire_path(campaigns_dir, row)  # type: ignore[misc]
            for _, coarse, best in picks for row in (coarse, best)]
    cameras = {path.name for path in used}  # type: ignore[union-attr]
    # Name the render that was used, never the property it is hoped to have.
    # wire_feature.png is written by scripts/vtu_wire_png.py --hole-zoom, whose
    # detect_hole_roi ESTIMATES a ring and always returns one, hole or no hole;
    # calling that "feature-framed" on the face of the figure would assert a
    # framing nothing verified. Say which camera produced it and let the
    # panels speak.
    framing = ("close-up on the hole" if cameras == {"wire_feature.png"}
               else "the whole part" if cameras == {"wire.png"}
               else "mixed views: " + ", ".join(sorted(cameras)))
    print(f"  camera: {framing} ({', '.join(sorted(cameras))})")

    # Which parts appear is decided ONLY by the coarse->best gain, so say so,
    # and say which families were available. Picking the parts that happen to
    # have a bore-framed render would make the camera choose the data.
    shown = [family_of(part) for part, _, _ in picks]
    bores = sorted({family_of(path.parts[-3])
                    for path in campaigns_dir.rglob("wire_feature.png")})
    selection = (f"The rows are the {len(picks)} part families where going "
                 f"from the coarsest mesh to the best one helped most "
                 f"({', '.join(shown)}), chosen on that alone.")
    if bores and not any(family in bores for family in shown):
        selection += (f" The close-up view exists only for "
                      f"{', '.join(bores)}, which no row here belongs to, so "
                      "every panel shows the whole part.")

    fig, axes = fs.figure(
        "Meshes before and after — the coarsest run beside the best-accuracy "
        "run for the same part",
        subtitle=("Each row is one part, and both pictures in a row are "
                  "cropped to the same scale and the same canvas, so a size "
                  "difference on the page is a size difference in the mesh. "
                  f"Each panel shows {framing}.\n"
                  f"{selection}"),
        # Stamp the renders actually drawn, not the warehouse root: a digest
        # folded over 20,000 unrelated campaign files identifies nothing.
        footer=fs.footer_source(*used, ADVISOR_DIR / "dataset.csv",
                                note=f"{len(picks)} parts"),
        # One row per part, but the aspect cap is a FLOOR on the height, not a
        # ceiling: at a single qualifying part 10.6 x 4.1 is a 2.59:1
        # letterbox and fs.figure rightly refuses it. Grow the canvas instead
        # of dropping the part.
        size=(10.6, max(4.1 * len(picks), 10.6 / fs.MAX_ASPECT)),
        nrows=len(picks), ncols=2, share_y_axis=False)

    for row_index, (part, coarse, best) in enumerate(picks):
        gain = to_float(coarse["accuracy_rel_err"]) / to_float(best["accuracy_rel_err"])
        print(f"  {part} ({family_of(part)}): "
              f"coarse h_rel {to_float(coarse['h_rel']):.3g} order {coarse['order']} "
              f"{coarse['mesher']} n_dof {int(to_float(coarse['n_dof']))} "
              f"rel_err {fmt(to_float(coarse['accuracy_rel_err']))}"
              f"  ->  best h_rel {to_float(best['h_rel']):.3g} "
              f"order {best['order']} {best['mesher']} "
              f"n_dof {int(to_float(best['n_dof']))} "
              f"rel_err {fmt(to_float(best['accuracy_rel_err']))}"
              f"  ({gain:.1f}x better, "
              f"{to_float(best['n_dof']) / to_float(coarse['n_dof']):.1f}x the dof)")

        paths = [wire_path(campaigns_dir, coarse), wire_path(campaigns_dir, best)]
        images = matched_pair([p for p in paths if p is not None])
        panels = [("baseline — the coarsest mesh run", coarse),
                  (f"best accuracy — error {gain:.0f}× lower", best)]
        for col_index, (title, row) in enumerate(panels):
            ax = axes[row_index][col_index]
            ax.set_xticks([])
            ax.set_yticks([])
            for side in ax.spines.values():
                side.set_color(fs.theme().rule)
            if paths[col_index] is None or col_index >= len(images):
                ax.text(0.5, 0.5, "picture missing", ha="center", va="center",
                        transform=ax.transAxes, color=fs.theme().muted)
                continue
            # source and axes are near 1:1, so nearest keeps the wire lines
            # crisp instead of smearing them into grey.
            ax.imshow(np.asarray(images[col_index]), interpolation="nearest")
            caption = f"{title}\n{_caption(row)}"
            fs.panel_title(ax, caption)
            # A loc="left" title is a different Text object than ``ax.title``,
            # which is the centred one, so the linespacing that used to be set
            # on ax.title here never reached the drawn title at all. Set it on
            # the left title itself, and take it down a size while we are
            # here: the caption now spells the quantities out, and those words
            # ran off the right edge of the page at panel size.
            ax.set_title(caption, loc="left", pad=6, linespacing=1.45,
                         color=fs.theme().ink, fontsize=fs.FONT_PT["annot"])
        axes[row_index][0].set_ylabel(part, fontsize=fs.FONT_PT["label"],
                                      weight="bold", labelpad=8)

    save(fig, out_dir, "mesh_before_after.png")
    return True
