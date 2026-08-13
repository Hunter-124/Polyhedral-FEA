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

sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
import figstyle as fs  # noqa: E402


def _rgb(hex_colour: str) -> tuple[int, int, int]:
    """Parse a figstyle theme colour into the 8-bit channels the PNG writer uses."""
    h = hex_colour.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


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


#: A ring estimate is not a hole. The radial histogram below always yields a
#: densest band -- a solid box has one too -- so the ring alone cannot answer
#: "is there a hole here". A through-hole has one property a solid does not:
#: there is NO MATERIAL inside the ring. That is what gets measured, as the
#: area-normalised node density inside 0.6*r against the density of the
#: annulus just outside it, and it is what ``detected`` reports.
HOLE_VOID_RATIO = 0.15
#: Below this many sampled nodes the density comparison is noise, not evidence.
HOLE_MIN_SAMPLES = 40
#: Exit code meaning "asked for a feature render, found no feature, wrote
#: nothing". Distinct from 1 (real failure) so a caller can count it as a skip.
NO_FEATURE_EXIT = 3


class HoleROI:
    """An ROI plus whether a hole was actually found inside it.

    ``detected`` is the only field a caller may treat as a claim. ``roi_min``
    and ``roi_max`` are always populated so a caller that wants a centre crop
    regardless can still have one -- but it must then say "centre crop", not
    "hole".
    """

    __slots__ = ("roi_min", "roi_max", "hole_r", "center", "detected",
                 "void_ratio", "reason")

    def __init__(self, roi_min, roi_max, hole_r, center, detected,
                 void_ratio, reason):
        self.roi_min = roi_min
        self.roi_max = roi_max
        self.hole_r = hole_r
        self.center = center
        self.detected = detected
        self.void_ratio = void_ratio
        self.reason = reason


def _ring_radius(pts, used, axis, centre, span, bins=48):
    """Densest in-plane radial band of free-surface nodes about ``axis``.

    Returns (radius, n_samples) or (None, n) when no band clears the noise
    floor. The radius is an ESTIMATE; whether it bounds a hole is a separate
    question, answered by :func:`_void_ratio`.
    """
    u, v = [i for i in range(3) if i != axis]
    rads = []
    for i in used:
        p = pts[i]
        if abs(p[axis] - centre[axis]) > 0.45 * span:
            continue
        rads.append(math.hypot(p[u] - centre[u], p[v] - centre[v]))
    if len(rads) < 16:
        rads = [math.hypot(pts[i][u] - centre[u], pts[i][v] - centre[v])
                for i in used]
    if not rads:
        return None, 0
    rmax = max(rads)
    hist = [0] * bins
    for r in rads:
        hist[min(bins - 1, int(r / (rmax + 1e-12) * bins))] += 1
    radius = None
    peak = 0
    for i in range(1, bins - 1):
        # Prefer bands closer to the centre than the outer boundary: the outer
        # silhouette is always the densest band on any part.
        if hist[i] >= peak and hist[i] >= 8 and i < int(0.65 * bins):
            peak = hist[i]
            radius = (i + 0.5) / bins * rmax
    if radius is not None and radius < 1e-9:
        radius = None
    return radius, len(rads)


#: A bore is empty AND enclosed. Emptiness alone called an L-bracket's open
#: quadrant a hole; an angular-coverage test then called a deep narrow U
#: channel a hole, because a U's opening angle is a continuous function of two
#: jittered dimensions and crosses ANY fixed fraction you pick -- channel_s0
#: passed a 75% sector floor while channel_s1 failed it, on the same family.
#:
#: So enclosure is no longer measured as a fraction of anything. It is decided
#: topologically: in the plane normal to the candidate axis, a bore's void does
#: not reach the outside world, and an open pocket does. Flood fill from the
#: border of the slab decides it, and there is no threshold left to tune.
#:
#: The sector count survives only as the resolution of the occupancy raster.
#: The raster must resolve the void it is asked about. Pixels sized off the
#: ELEMENT length were too coarse: on a warehouse box_hole (392 cells, 2.4 mm
#: elements, 5.7 mm bore) the bore spanned two pixels and a one-pixel dilation
#: erased it, so a real bore read as solid. Pixels are therefore sized off the
#: candidate radius, and material is stamped as a disc of half an element
#: around each sample -- the same seam-closing intent as the dilation, but in
#: physical units, so it thickens the material band without swallowing a void
#: that is genuinely wider than the discretisation.
RASTER_PIXELS_PER_RADIUS = 4.0
RASTER_MAX_PIXELS = 300
RASTER_MIN_PIXELS = 24


def _occupancy(pts, cells, axis, centre, extents, radius):
    """Rasterise the mid-slab normal to ``axis``, resolving ``radius``.

    Returns (grid, nu, nv, origin_u, origin_v, (step_u, step_v)) with
    ``grid[j * nu + i]`` True where material was sampled. Nodes and cell
    centroids are both stamped, each as a disc of half an element, so a coarse
    tet cannot leave a seam the flood fill would leak through.
    """
    u, v = [i for i in range(3) if i != axis]
    span = extents[axis]
    volume = extents[0] * extents[1] * extents[2]
    element = (volume / max(1, len(cells) if cells else len(pts))) ** (1.0 / 3.0)
    pixel = max(radius / RASTER_PIXELS_PER_RADIUS, element / 8.0, 1e-12)
    nu = int(min(RASTER_MAX_PIXELS,
                 max(RASTER_MIN_PIXELS, extents[u] / pixel + 2)))
    nv = int(min(RASTER_MAX_PIXELS,
                 max(RASTER_MIN_PIXELS, extents[v] / pixel + 2)))
    origin_u = centre[u] - 0.5 * extents[u]
    origin_v = centre[v] - 0.5 * extents[v]
    step_u = extents[u] / (nu - 1)
    step_v = extents[v] / (nv - 1)

    grid = [False] * (nu * nv)
    stamp_u = max(1, int(round(0.5 * element / step_u)))
    stamp_v = max(1, int(round(0.5 * element / step_v)))

    def mark(points):
        for p in points:
            if abs(p[axis] - centre[axis]) > 0.45 * span:
                continue
            ci = int((p[u] - origin_u) / step_u)
            cj = int((p[v] - origin_v) / step_v)
            for dj in range(-stamp_v, stamp_v + 1):
                jj = cj + dj
                if not 0 <= jj < nv:
                    continue
                for di in range(-stamp_u, stamp_u + 1):
                    ii = ci + di
                    if not 0 <= ii < nu:
                        continue
                    if (di / stamp_u) ** 2 + (dj / stamp_v) ** 2 <= 1.0:
                        grid[jj * nu + ii] = True

    mark(pts)
    if cells:
        mark(_cell_centroids(pts, cells))
    return grid, nu, nv, origin_u, origin_v, (step_u, step_v)


def _void_is_enclosed(pts, cells, axis, centre, extents, radius):
    """Does the void at the candidate radius reach the outside of the part?

    A bore says no, an open pocket -- an L's quadrant, a U channel's mouth --
    says yes, and the answer is connectivity, not a tuned fraction. Returns
    (enclosed, void_pixels); ``enclosed`` is None when the raster shows no
    void at the candidate location at all.
    """
    grid, nu, nv, origin_u, origin_v, (step_u, step_v) = _occupancy(
        pts, cells, axis, centre, extents, radius)
    u, v = [i for i in range(3) if i != axis]
    ci = int((centre[u] - origin_u) / step_u)
    cj = int((centre[v] - origin_v) / step_v)
    if not (0 <= ci < nu and 0 <= cj < nv) or grid[cj * nu + ci]:
        return None, 0

    # Flood the void containing the axis, and note whether it touches the
    # border of the raster: touching means it is continuous with the outside.
    seen = [False] * (nu * nv)
    stack = [(ci, cj)]
    seen[cj * nu + ci] = True
    void = 0
    touches_border = False
    while stack:
        i, j = stack.pop()
        void += 1
        if i == 0 or j == 0 or i == nu - 1 or j == nv - 1:
            touches_border = True
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ii, jj = i + di, j + dj
            if not (0 <= ii < nu and 0 <= jj < nv):
                continue
            index = jj * nu + ii
            if seen[index] or grid[index]:
                continue
            seen[index] = True
            stack.append((ii, jj))
    return (not touches_border), void


def _void_ratio(pts, axis, centre, span, hole_r):
    """Node density inside 0.6*hole_r over the density of the annulus outside.

    Both counts are divided by their own area, so this is a density ratio and
    not an artefact of the inner disc being smaller. Emptiness only: whether
    the empty region is a bore or an open pocket is decided separately, by
    :func:`_void_is_enclosed`. Returns (ratio, sampled); ratio is None when
    there is too little to measure.
    """
    u, v = [i for i in range(3) if i != axis]
    inner_r = 0.6 * hole_r
    outer_r = 1.6 * hole_r
    inner = outer = 0
    for p in pts:
        if abs(p[axis] - centre[axis]) > 0.45 * span:
            continue
        r = math.hypot(p[u] - centre[u], p[v] - centre[v])
        if r <= inner_r:
            inner += 1
        elif r <= outer_r:
            outer += 1
    sampled = inner + outer
    if sampled < HOLE_MIN_SAMPLES or outer == 0:
        return None, sampled
    inner_area = math.pi * inner_r * inner_r
    outer_area = math.pi * (outer_r * outer_r - inner_r * inner_r)
    return ((inner / max(inner_area, 1e-30))
            / max(outer / max(outer_area, 1e-30), 1e-30)), sampled


#: An empty core must be bigger than the discretisation to mean anything: at
#: any resolution there is some radius around an axis with no cell centroid in
#: it. This sizes the core; whether it is a bore or a pocket is connectivity's
#: job, not this threshold's.
CORE_ELEMENTS = 1.5


def _cell_centroids(pts, cells):
    out = []
    for ids in cells:
        if not ids:
            continue
        sx = sy = sz = 0.0
        for i in ids:
            p = pts[i]
            sx += p[0]
            sy += p[1]
            sz += p[2]
        n = float(len(ids))
        out.append((sx / n, sy / n, sz / n))
    return out


def _empty_core(pts, cells, axis, centre, extents):
    """Largest cell-free radius about ``axis``, in absolute units and elements.

    The node-density test needs nodes near the bore wall to compare, and a
    coarse mesh does not have them: at h_rel 0.20 a 3 mm bore has 14 nodes
    around it, under any sane noise floor, so the test declines and the
    caller cannot tell "no bore" from "not enough mesh to see one". Cells are
    the robust signal at that resolution -- a bore is a region with no cells
    in it, however few elements describe its wall.
    """
    span = extents[axis]
    u, v = [i for i in range(3) if i != axis]
    slab = [q for q in _cell_centroids(pts, cells)
            if abs(q[axis] - centre[axis]) <= 0.45 * span]
    if len(slab) < 12:
        return None
    core = min(math.hypot(q[u] - centre[u], q[v] - centre[v]) for q in slab)
    volume = extents[0] * extents[1] * extents[2]
    element = (volume / max(1, len(cells))) ** (1.0 / 3.0)
    return core, core / max(element, 1e-30)


def detect_hole_roi(pts, edges, pad_frac=0.55, cells=None):
    """Find a circular bore and MEASURE whether it is really a bore.

    Two things this deliberately does not assume:

    * **The bore axis.** The previous version sampled radii in xy while taking
      its slab from the LONGEST bbox axis -- two different axes, and neither
      derived. On a plate the bore runs through the THINNEST axis, so the
      radial histogram measured across the bore instead of around it. All
      three axes are now tried and the one with the strongest void evidence
      wins, so the answer does not depend on how the part was exported.
    * **That a ring is a hole.** The densest inner radial band exists on a
      solid box too. A bore has no material inside it, and that is measured.

    Returns a :class:`HoleROI`; ``detected`` is True only when some axis shows
    a ring whose interior is empty, or -- on a mesh too coarse to carry that
    many nodes -- a cell-free core wider than ``CORE_ELEMENTS`` elements that
    material encloses. Pass ``cells`` to enable the coarse path.
    """
    mins, maxs = bbox_of(pts)
    centre = [0.5 * (mins[i] + maxs[i]) for i in range(3)]
    extents = [max(1e-12, maxs[i] - mins[i]) for i in range(3)]

    used = set()
    for a, b in edges:
        used.add(a)
        used.add(b)
    if not used:
        used = set(range(len(pts)))

    best = None          # (void_ratio, axis, radius) for the emptiest ring
    fallback = None      # (axis, radius) for the best ring with material in it
    notes = []
    for axis in range(3):
        radius, n_rad = _ring_radius(pts, used, axis, centre, extents[axis])
        name = "xyz"[axis]
        if radius is None:
            notes.append(f"{name}: no ring above the noise floor")
            continue
        ratio, sampled = _void_ratio(pts, axis, centre, extents[axis], radius)
        if ratio is None:
            notes.append(f"{name}: ring r={radius:.4g} but only {sampled} "
                         f"nodes near it (floor {HOLE_MIN_SAMPLES})")
            continue
        if ratio >= HOLE_VOID_RATIO:
            notes.append(f"{name}: ring r={radius:.4g}, interior density "
                         f"{ratio:.2f}x the annulus — encloses material")
            if fallback is None or ratio < fallback[0]:
                fallback = (ratio, axis, radius)
            continue
        enclosed, void_px = _void_is_enclosed(pts, cells, axis, centre,
                                              extents, radius)
        if not enclosed:
            notes.append(f"{name}: ring r={radius:.4g} is empty (density "
                         f"{ratio:.2f}) but its void reaches the outside of "
                         f"the part — an open pocket, not a bore")
            if fallback is None or ratio < fallback[0]:
                fallback = (ratio, axis, radius)
            continue
        notes.append(f"{name}: ring r={radius:.4g}, interior density "
                     f"{ratio:.2f}x the annulus, void enclosed ({void_px} px)")
        if best is None or ratio < best[0]:
            best = (ratio, axis, radius)

    if best is None and cells:
        # The node-density test needs a populated bore wall. At h_rel 0.20 a
        # box_hole bore has 14 nodes around it and the test correctly declines
        # -- but "declined" then looks identical to "no bore" downstream, and
        # the bore is right there in the CAD. Measure the cells instead.
        for axis in range(3):
            core = _empty_core(pts, cells, axis, centre, extents)
            if core is None:
                continue
            radius, in_elements = core
            if in_elements < CORE_ELEMENTS:
                continue
            enclosed, void_px = _void_is_enclosed(pts, cells, axis, centre,
                                                  extents, radius)
            if not enclosed:
                notes.append(f"{'xyz'[axis]}: cell-free core r={radius:.4g} "
                             f"({in_elements:.1f} elements) reaches the "
                             f"outside — an open pocket, not a bore")
                continue
            if best is None or in_elements > best[0]:
                best = (in_elements, axis, radius)
                notes.append(f"{'xyz'[axis]}: cell-free core r={radius:.4g} "
                             f"({in_elements:.1f} elements wide), void "
                             f"enclosed ({void_px} px)")
        if best is not None:
            in_elements, axis, hole_r = best
            return _roi_for(mins, maxs, centre, extents, axis, hole_r, pad_frac,
                            True, None,
                            f"bore on the {'xyz'[axis]} axis at r={hole_r:.4g}, "
                            f"found by cell occupancy: the core is "
                            f"{in_elements:.1f} elements wide and its void does "
                            f"not reach the outside. Too coarse for the "
                            f"node-density test, which needs "
                            f"{HOLE_MIN_SAMPLES} nodes near the wall")

    detected = best is not None
    if detected:
        void_ratio, axis, hole_r = best
        reason = (f"bore on the {'xyz'[axis]} axis at r={hole_r:.4g}: "
                  f"interior node density is {void_ratio:.2f} of the "
                  f"surrounding annulus (a bore is < {HOLE_VOID_RATIO:g})")
    elif fallback is not None:
        void_ratio, axis, hole_r = fallback
        reason = ("every axis encloses material — " + "; ".join(notes))
    else:
        void_ratio = None
        # Nothing measurable on any axis: fall back to a centre crop, and say
        # plainly that the radius is invented rather than measured.
        axis = max(range(3), key=lambda i: extents[i])
        u, v = [i for i in range(3) if i != axis]
        hole_r = 0.25 * max(extents[u], extents[v])
        reason = ("no measurable ring on any axis — " + "; ".join(notes))

    return _roi_for(mins, maxs, centre, extents, axis, hole_r, pad_frac,
                    detected, void_ratio, reason)


def _roi_for(mins, maxs, centre, extents, axis, hole_r, pad_frac,
             detected, void_ratio, reason):
    """Build the ROI box about ``axis`` and package the verdict with it."""
    half = hole_r * (1.0 + pad_frac)
    roi_min = list(mins)
    roi_max = list(maxs)
    for i in range(3):
        if i == axis:
            # Keep a slab about the bore axis so the wall transition is visible.
            band = 0.22 * extents[i]
            roi_min[i] = centre[i] - band
            roi_max[i] = centre[i] + band
        else:
            roi_min[i] = centre[i] - half
            roi_max[i] = centre[i] + half
    return HoleROI(roi_min, roi_max, hole_r, tuple(centre), detected,
                   void_ratio, reason)


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
    ap.add_argument(
        "--require-hole", action="store_true",
        help="with --hole-zoom, write nothing and exit 3 when no hole is "
             "actually detected. Use this whenever the output filename "
             "promises a feature (wire_feature.png): a file that exists only "
             "when the feature exists is self-describing, one that always "
             "exists and is sometimes a plain centre crop is a false promise.",
    )
    ap.add_argument("--view", default="iso", choices=["iso", "top", "front", "side"])
    ap.add_argument("--size", type=int, default=1100)
    ap.add_argument("--elev", type=float, default=0.6)
    ap.add_argument("--azim", type=float, default=0.85)
    args = ap.parse_args()
    # A mesh wireframe is a render, so it stages dark like the rest of the
    # showcase; the colours come from figstyle, never from a literal here.
    t = fs.use("dark")

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
    hole = None
    if args.hole_zoom:
        hole = detect_hole_roi(pts, edges, cells=cells)
        roi_min, roi_max = hole.roi_min, hole.roi_max
        verdict = "hole detected" if hole.detected else "NO hole detected"
        print(f"hole check {vtu}: {verdict} — {hole.reason}")
        if args.require_hole and not hole.detected:
            # Write NOTHING. The caller asked for a feature-framed render and there
            # is no feature to frame, so producing a file whose name claims one
            # would be the same false promise the flag exists to prevent.
            print(f"no circular hole detected in {vtu}: refusing to write "
                  f"{out} (--require-hole)", file=sys.stderr)
            return 3
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
    bg = _rgb(t.panel)
    ink = _rgb(t.ink)
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
        draw_line(img, w, h, u0, v0, u1, v1, ink)
        n_drawn += 1
    write_png(out, w, h, img)
    if hole is None:
        extra = ""
    elif hole.detected and hole.void_ratio is not None:
        extra = f", bore r≈{hole.hole_r:.4g} (void ratio {hole.void_ratio:.2f})"
    elif hole.detected:
        extra = f", bore r≈{hole.hole_r:.4g} (found by cell occupancy)"
    else:
        extra = f", centre crop r≈{hole.hole_r:.4g} — NOT a detected bore"
    print(
        f"wrote {out} ({n_drawn} edges drawn / {len(edges)} exterior, "
        f"{len(cells)} cells, view={args.view}{extra})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
