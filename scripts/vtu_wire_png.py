#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""ASCII VTU exterior wireframe PNG (no meshio). Boundary faces only.

Usage:
  python scripts/vtu_wire_png.py in.vtu out.png
  python scripts/vtu_wire_png.py in.vtu out.png --hole-zoom
  python scripts/vtu_wire_png.py in.vtu out.png --view top --roi auto
"""
from __future__ import annotations

import argparse
import math
import re
import struct
import sys
import zlib
from collections import defaultdict
from pathlib import Path


def parse_vtu_ascii(path: Path):
    text = path.read_text(encoding="utf-8", errors="ignore")
    m = re.search(r"<Points>\s*<DataArray[^>]*>(.*?)</DataArray>", text, re.S | re.I)
    if not m:
        raise RuntimeError("no Points array")
    nums = [float(x) for x in m.group(1).split()]
    pts = [(nums[i], nums[i + 1], nums[i + 2]) for i in range(0, len(nums) - 2, 3)]
    m = re.search(r'Name="connectivity"[^>]*>(.*?)</DataArray>', text, re.S | re.I)
    conn = [int(x) for x in m.group(1).split()] if m else []
    m = re.search(r'Name="offsets"[^>]*>(.*?)</DataArray>', text, re.S | re.I)
    offsets = [int(x) for x in m.group(1).split()] if m else []
    cells = []
    prev = 0
    for off in offsets:
        cells.append(conn[prev:off])
        prev = off
    return pts, cells


def cell_faces(ids):
    n = len(ids)
    if n == 4:  # tet
        f = [(0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)]
        return [[ids[i] for i in face] for face in f]
    if n == 5:  # pyramid: quad base + 4 tris
        base = [ids[0], ids[1], ids[2], ids[3]]
        tris = [
            [ids[0], ids[1], ids[4]],
            [ids[1], ids[2], ids[4]],
            [ids[2], ids[3], ids[4]],
            [ids[3], ids[0], ids[4]],
        ]
        return [base] + tris
    if n == 8:  # hex
        f = [
            (0, 1, 2, 3),
            (4, 5, 6, 7),
            (0, 1, 5, 4),
            (1, 2, 6, 5),
            (2, 3, 7, 6),
            (3, 0, 4, 7),
        ]
        return [[ids[i] for i in face] for face in f]
    if n == 6:  # prism
        f = [(0, 1, 2), (3, 4, 5), (0, 1, 4, 3), (1, 2, 5, 4), (2, 0, 3, 5)]
        return [[ids[i] for i in face] for face in f]
    return [list(ids)]


def face_key(face):
    return tuple(sorted(face))


def boundary_edges(cells):
    counts = defaultdict(int)
    faces_by_key = {}
    for cell in cells:
        for face in cell_faces(cell):
            k = face_key(face)
            counts[k] += 1
            faces_by_key[k] = face
    edges = set()
    for k, c in counts.items():
        if c != 1:
            continue
        face = faces_by_key[k]
        m = len(face)
        for i in range(m):
            a, b = face[i], face[(i + 1) % m]
            edges.add((min(a, b), max(a, b)))
    return edges


def bbox_of(pts):
    mins = [min(p[i] for p in pts) for i in range(3)]
    maxs = [max(p[i] for p in pts) for i in range(3)]
    return mins, maxs


def detect_hole_roi(pts, edges, pad_frac=0.55):
    """Estimate circular-hole ROI: densest radial ring of free-surface nodes."""
    mins, maxs = bbox_of(pts)
    cx = 0.5 * (mins[0] + maxs[0])
    cy = 0.5 * (mins[1] + maxs[1])
    cz = 0.5 * (mins[2] + maxs[2])
    # Free-surface node ids
    used = set()
    for a, b in edges:
        used.add(a)
        used.add(b)
    if not used:
        used = set(range(len(pts)))
    # Sample mid-depth free-surface nodes (hole wall + rims live near center)
    rads = []
    for i in used:
        x, y, z = pts[i]
        # Prefer nodes near mid-axis span so outer box corners do not dominate.
        if abs(z - cz) > 0.45 * max(1e-12, maxs[2] - mins[2]):
            continue
        rads.append(math.hypot(x - cx, y - cy))
    if len(rads) < 16:
        rads = [math.hypot(pts[i][0] - cx, pts[i][1] - cy) for i in used]
    rads.sort()
    # Histogram: hole radius ≈ first strong peak above small r
    rmax = max(rads) if rads else 1.0
    bins = 48
    hist = [0] * bins
    for r in rads:
        b = min(bins - 1, int(r / (rmax + 1e-12) * bins))
        hist[b] += 1
    hole_r = None
    peak = 0
    for i in range(1, bins - 1):
        if hist[i] >= peak and hist[i] >= 8:
            # prefer peaks closer to center than outer box
            if i < int(0.65 * bins):
                peak = hist[i]
                hole_r = (i + 0.5) / bins * rmax
    if hole_r is None or hole_r < 1e-9:
        # fallback: 15th percentile radius of free-surface nodes near mid
        hole_r = rads[max(0, len(rads) // 8)] if rads else 0.25 * rmax
    half = hole_r * (1.0 + pad_frac)
    # Axis of hole: longest bbox direction among remaining (z for test.stl)
    extents = [maxs[i] - mins[i] for i in range(3)]
    axis = max(range(3), key=lambda i: extents[i])
    # ROI box around hole in the two in-plane axes
    roi_min = [mins[0], mins[1], mins[2]]
    roi_max = [maxs[0], maxs[1], maxs[2]]
    for i in range(3):
        if i == axis:
            # keep a mid-slice band so we see the wall transition
            mid = 0.5 * (mins[i] + maxs[i])
            band = 0.22 * extents[i]
            roi_min[i] = mid - band
            roi_max[i] = mid + band
        else:
            c = 0.5 * (mins[i] + maxs[i])
            # for xy: use hole center; for non-z axes use geometric center
            if i == 0:
                c = cx
            elif i == 1:
                c = cy
            roi_min[i] = c - half
            roi_max[i] = c + half
    return roi_min, roi_max, hole_r, (cx, cy, cz)


def project(p, mins, scales, w, h, elev=0.6, azim=0.85, view="iso"):
    x, y, z = p
    xn = (x - mins[0]) * scales[0] - 0.5
    yn = (y - mins[1]) * scales[1] - 0.5
    zn = (z - mins[2]) * scales[2] - 0.5
    if view == "top":
        # looking down -Z (XY plane)
        x1, y2 = xn, yn
    elif view == "front":
        x1, y2 = xn, zn
    elif view == "side":
        x1, y2 = yn, zn
    else:
        ca, sa = math.cos(azim), math.sin(azim)
        ce, se = math.cos(elev), math.sin(elev)
        x1 = ca * xn + sa * yn
        y1 = -sa * xn + ca * yn
        y2 = ce * y1 - se * zn
    u = int((x1 + 0.65) / 1.3 * (w - 1))
    v = int((0.65 - y2) / 1.3 * (h - 1))
    return u, v


def draw_line(img, w, h, x0, y0, x1, y1, rgb):
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        if 0 <= x0 < w and 0 <= y0 < h:
            i = (y0 * w + x0) * 3
            img[i : i + 3] = bytes(rgb)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def write_png(path: Path, w: int, h: int, rgb: bytearray):
    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y * w * 3 : (y + 1) * w * 3])
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def point_in_roi(p, rmin, rmax):
    return all(rmin[i] - 1e-9 <= p[i] <= rmax[i] + 1e-9 for i in range(3))


def main():
    ap = argparse.ArgumentParser(description="VTU exterior wireframe PNG")
    ap.add_argument("vtu")
    ap.add_argument("png")
    ap.add_argument("--hole-zoom", action="store_true", help="auto ROI around circular hole")
    ap.add_argument("--view", default="iso", choices=["iso", "top", "front", "side"])
    ap.add_argument("--size", type=int, default=1100)
    ap.add_argument("--elev", type=float, default=0.6)
    ap.add_argument("--azim", type=float, default=0.85)
    args = ap.parse_args()

    vtu = Path(args.vtu)
    out = Path(args.png)
    pts, cells = parse_vtu_ascii(vtu)
    if not pts or not cells:
        print("empty mesh", file=sys.stderr)
        return 1
    edges = boundary_edges(cells)
    mins, maxs = bbox_of(pts)
    roi_min, roi_max = mins, maxs
    hole_r = None
    if args.hole_zoom:
        roi_min, roi_max, hole_r, _ = detect_hole_roi(pts, edges)
        # slightly expand for context
        for i in range(3):
            pad = 0.05 * max(1e-12, roi_max[i] - roi_min[i])
            roi_min[i] -= pad
            roi_max[i] += pad
        mins, maxs = roi_min, roi_max

    ext = [max(1e-12, maxs[i] - mins[i]) for i in range(3)]
    # isotropic scale so hole is not stretched
    mext = max(ext)
    scales = [1.0 / mext, 1.0 / mext, 1.0 / mext]
    # recentre mins so projection uses actual ROI center
    # (project normalizes via mins/scales)

    w = h = max(400, args.size)
    bg = (42, 58, 106)
    img = bytearray([bg[0], bg[1], bg[2]] * w * h)
    n_drawn = 0
    for a, b in edges:
        pa, pb = pts[a], pts[b]
        if args.hole_zoom:
            # keep edge if either endpoint in ROI (or midpoint)
            mid = tuple(0.5 * (pa[i] + pb[i]) for i in range(3))
            if not (
                point_in_roi(pa, roi_min, roi_max)
                or point_in_roi(pb, roi_min, roi_max)
                or point_in_roi(mid, roi_min, roi_max)
            ):
                continue
        u0, v0 = project(pa, mins, scales, w, h, args.elev, args.azim, args.view)
        u1, v1 = project(pb, mins, scales, w, h, args.elev, args.azim, args.view)
        draw_line(img, w, h, u0, v0, u1, v1, (15, 15, 18))
        n_drawn += 1
    write_png(out, w, h, img)
    extra = f", hole_r≈{hole_r:.4g}" if hole_r is not None else ""
    print(
        f"wrote {out} ({n_drawn} edges drawn / {len(edges)} exterior, "
        f"{len(cells)} cells, view={args.view}{extra})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
