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
from collections import Counter, defaultdict
from itertools import combinations
from pathlib import Path


def _data_array(text, name):
    pattern = (
        r"<DataArray\b[^>]*\bName\s*=\s*[\"']"
        + re.escape(name)
        + r"[\"'][^>]*>(.*?)</DataArray>"
    )
    match = re.search(pattern, text, re.S | re.I)
    return match.group(1) if match else None


def parse_vtu_ascii(path: Path):
    text = path.read_text(encoding="utf-8", errors="ignore")
    match = re.search(r"<Points>\s*<DataArray[^>]*>(.*?)</DataArray>", text, re.S | re.I)
    if not match:
        raise RuntimeError("no Points array")
    nums = [float(x) for x in match.group(1).split()]
    pts = [(nums[i], nums[i + 1], nums[i + 2]) for i in range(0, len(nums) - 2, 3)]

    def ints(name):
        data = _data_array(text, name)
        return [int(x) for x in data.split()] if data is not None else []

    conn = ints("connectivity")
    offsets = ints("offsets")
    types = ints("types")
    faces = ints("faces")
    faceoffsets = ints("faceoffsets")
    cells = []
    prev = 0
    for off in offsets:
        cells.append(conn[prev:off])
        prev = off
    return pts, cells, types, faces, faceoffsets


_TET_FACES = [(0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3)]
_PYRAMID_FACES = [
    (0, 1, 2, 3),
    (0, 1, 4),
    (1, 2, 4),
    (2, 3, 4),
    (3, 0, 4),
]
_HEX_FACES = [
    (0, 1, 2, 3),
    (4, 5, 6, 7),
    (0, 1, 5, 4),
    (1, 2, 6, 5),
    (2, 3, 7, 6),
    (3, 0, 4, 7),
]
_WEDGE_FACES = [
    (0, 1, 2),
    (3, 4, 5),
    (0, 1, 4, 3),
    (1, 2, 5, 4),
    (2, 0, 3, 5),
]


def _fixed_faces(ids, local_faces):
    if not local_faces or len(ids) <= max(max(face) for face in local_faces):
        return []
    return [[ids[i] for i in face] for face in local_faces]


def _ordered_planar_hull(point_ids, pts, normal):
    """Order a planar facet's corner nodes, dropping collinear/interior nodes."""
    nx, ny, nz = normal
    nlen = math.sqrt(nx * nx + ny * ny + nz * nz)
    if nlen == 0.0:
        return []
    nx, ny, nz = nx / nlen, ny / nlen, nz / nlen
    axis = min(range(3), key=lambda i: abs((nx, ny, nz)[i]))
    ax, ay, az = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))[axis]
    ux, uy, uz = ny * az - nz * ay, nz * ax - nx * az, nx * ay - ny * ax
    ulen = math.sqrt(ux * ux + uy * uy + uz * uz)
    ux, uy, uz = ux / ulen, uy / ulen, uz / ulen
    vx, vy, vz = ny * uz - nz * uy, nz * ux - nx * uz, nx * uy - ny * ux

    projected = []
    seen_positions = set()
    for point_id in point_ids:
        x, y, z = pts[point_id]
        xy = (x * ux + y * uy + z * uz, x * vx + y * vy + z * vz)
        if xy not in seen_positions:
            projected.append((xy[0], xy[1], point_id))
            seen_positions.add(xy)
    projected.sort()
    if len(projected) < 3:
        return []

    def turn(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    span = max(
        projected[-1][0] - projected[0][0],
        max(p[1] for p in projected) - min(p[1] for p in projected),
        1.0,
    )
    tol = 1e-12 * span * span
    lower = []
    for point in projected:
        while len(lower) >= 2 and turn(lower[-2], lower[-1], point) <= tol:
            lower.pop()
        lower.append(point)
    upper = []
    for point in reversed(projected):
        while len(upper) >= 2 and turn(upper[-2], upper[-1], point) <= tol:
            upper.pop()
        upper.append(point)
    return [point[2] for point in lower[:-1] + upper[:-1]]


def convex_hull_faces(ids, pts):
    """Return polygon facets of a small 3-D point set by supporting-plane tests."""
    point_ids = list(dict.fromkeys(ids))
    if len(point_ids) < 4 or any(i < 0 or i >= len(pts) for i in point_ids):
        return []
    coords = [pts[i] for i in point_ids]
    extents = [max(p[d] for p in coords) - min(p[d] for p in coords) for d in range(3)]
    scale = max(max(extents), 1.0)
    planes = {}
    for i, j, k in combinations(range(len(point_ids)), 3):
        a, b, c = coords[i], coords[j], coords[k]
        ab = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        ac = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        normal = (
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0],
        )
        nlen = math.sqrt(sum(v * v for v in normal))
        if nlen <= 1e-12 * scale * scale:
            continue
        tol = 1e-9 * scale * nlen
        distances = [
            normal[0] * (p[0] - a[0])
            + normal[1] * (p[1] - a[1])
            + normal[2] * (p[2] - a[2])
            for p in coords
        ]
        if any(d > tol for d in distances) and any(d < -tol for d in distances):
            continue
        coplanar = frozenset(point_ids[q] for q, d in enumerate(distances) if abs(d) <= tol)
        if len(coplanar) >= 3:
            planes.setdefault(coplanar, normal)

    facets = []
    seen = set()
    for coplanar, normal in planes.items():
        face = _ordered_planar_hull(coplanar, pts, normal)
        key = frozenset(face)
        if len(face) >= 3 and key not in seen:
            facets.append(face)
            seen.add(key)
    return facets if len(facets) >= 4 else []


def _polyhedron_face_blocks(face_stream, faceoffsets, cell_count):
    blocks = [None] * cell_count
    stream_start = 0
    for cell_index in range(min(cell_count, len(faceoffsets))):
        stream_end = faceoffsets[cell_index]
        if stream_end < 0:
            continue
        if stream_end < stream_start or stream_end > len(face_stream):
            continue
        block = face_stream[stream_start:stream_end]
        stream_start = stream_end
        if not block:
            continue
        face_count = block[0]
        cursor = 1
        loops = []
        for _ in range(face_count):
            if cursor >= len(block):
                loops = []
                break
            size = block[cursor]
            cursor += 1
            if size < 3 or cursor + size > len(block):
                loops = []
                break
            loops.append(block[cursor : cursor + size])
            cursor += size
        if loops and cursor == len(block):
            blocks[cell_index] = loops
    return blocks


def cell_faces(ids, vtk_type=None, poly_faces=None, pts=None):
    if vtk_type == 10:
        return _fixed_faces(ids, _TET_FACES)
    if vtk_type == 12:
        return _fixed_faces(ids, _HEX_FACES)
    if vtk_type == 13:
        return _fixed_faces(ids, _WEDGE_FACES)
    if vtk_type == 14:
        return _fixed_faces(ids, _PYRAMID_FACES)
    if vtk_type == 24:
        return _fixed_faces(ids, _TET_FACES)
    if vtk_type == 25:
        return _fixed_faces(ids, _HEX_FACES)
    if vtk_type == 42:
        return poly_faces or []
    if vtk_type == 41:
        return convex_hull_faces(ids, pts) if pts is not None else []
    if vtk_type is not None:
        return None
    return {
        4: _fixed_faces(ids, _TET_FACES),
        5: _fixed_faces(ids, _PYRAMID_FACES),
        6: _fixed_faces(ids, _WEDGE_FACES),
        8: _fixed_faces(ids, _HEX_FACES),
    }.get(len(ids))


def face_key(face):
    return tuple(sorted(face))


def boundary_edges(pts, cells, types, face_stream, faceoffsets):
    counts = defaultdict(int)
    faces_by_key = {}
    skipped = 0
    poly_blocks = _polyhedron_face_blocks(face_stream, faceoffsets, len(cells))
    for cell_index, cell in enumerate(cells):
        vtk_type = types[cell_index] if cell_index < len(types) else None
        faces = cell_faces(cell, vtk_type, poly_blocks[cell_index], pts)
        if not faces:
            skipped += 1
            continue
        for face in faces:
            k = face_key(face)
            counts[k] += 1
            faces_by_key[k] = face
    edges = set()
    for k, count in counts.items():
        if count != 1:
            continue
        face = faces_by_key[k]
        for i in range(len(face)):
            a, b = face[i], face[(i + 1) % len(face)]
            edges.add((min(a, b), max(a, b)))
    return edges, skipped


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
    pts, cells, types, face_stream, faceoffsets = parse_vtu_ascii(vtu)
    if not pts or not cells:
        print("empty mesh", file=sys.stderr)
        return 1
    edges, skipped = boundary_edges(pts, cells, types, face_stream, faceoffsets)
    type_names = {
        10: "tet4",
        12: "hex8",
        13: "wedge6",
        14: "pyramid5",
        24: "tet10",
        25: "hex20",
        41: "convex-point-set",
        42: "polyhedron",
    }
    census = Counter(types[: len(cells)])
    census_parts = [
        f"{vtk_type}({type_names.get(vtk_type, 'unsupported')})={count}"
        for vtk_type, count in sorted(census.items())
    ]
    missing_types = len(cells) - min(len(types), len(cells))
    if missing_types:
        census_parts.append(f"missing(count-fallback)={missing_types}")
    print(f"cell-type census {vtu}: {', '.join(census_parts)}; skipped={skipped}")
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
