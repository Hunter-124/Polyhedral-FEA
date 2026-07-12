#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate *legacy* validation-part STLs under tests/fixtures/parts/.

************************************************************************
SOFT-DEPRECATED — product geometry path is STEP only (ADR-0020 / V2a/V2d).
  Prefer:  python3 scripts/gen_cad_parts.py
  This script only regenerates older STL campaign fixtures (smoke_bar,
  plate_hole, cantilever) until those cases fully migrate to STEP.
  Do not use its outputs as the product mesh/solve geometry path.
************************************************************************

Deterministic pure-Python ASCII STL writer — no CAD dependency. Run from
repo root:

    python3 scripts/gen_part_library.py

Produces:
  tests/fixtures/parts/smoke_bar.stl
  tests/fixtures/parts/plate_hole.stl
  tests/fixtures/parts/cantilever.stl

Case JSON and bench/reference truths are hand-authored (not generated) so
the anti-cheat boundary stays explicit.
"""
from __future__ import annotations

import math
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "fixtures" / "parts"

_DEPRECATION_BANNER = """\
========================================================================
SOFT-DEPRECATED (ADR-0020 / V2d): product fixtures are STEP, not STL.
  Prefer:  python3 scripts/gen_cad_parts.py
  This run only refreshes *legacy* STL fixtures for older campaigns.
  Product mesh/solve path must not treat these as primary geometry.
========================================================================
"""


def _facet(n, a, b, c) -> str:
    return (
        f"  facet normal {n[0]:.9g} {n[1]:.9g} {n[2]:.9g}\n"
        f"    outer loop\n"
        f"      vertex {a[0]:.9g} {a[1]:.9g} {a[2]:.9g}\n"
        f"      vertex {b[0]:.9g} {b[1]:.9g} {b[2]:.9g}\n"
        f"      vertex {c[0]:.9g} {c[1]:.9g} {c[2]:.9g}\n"
        f"    endloop\n"
        f"  endfacet\n"
    )


def _cross(u, v):
    return (
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    )


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _norm(v):
    L = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / L, v[1] / L, v[2] / L)


def tri(a, b, c, outward_hint=None) -> str:
    """Emit one facet with outward-ish normal (right-hand a->b->c, flipped if needed)."""
    n = _norm(_cross(_sub(b, a), _sub(c, a)))
    if outward_hint is not None:
        # Flip if normal points against the outward hint (dot product).
        if n[0] * outward_hint[0] + n[1] * outward_hint[1] + n[2] * outward_hint[2] < 0:
            a, c = c, a
            n = (-n[0], -n[1], -n[2])
    return _facet(n, a, b, c)


def write_box(path: Path, name: str, lx: float, ly: float, lz: float,
              ox: float = 0.0, oy: float = 0.0, oz: float = 0.0) -> None:
    """Axis-aligned box [ox,ox+lx] x [oy,oy+ly] x [oz,oz+lz], 12 triangles."""
    x0, x1 = ox, ox + lx
    y0, y1 = oy, oy + ly
    z0, z1 = oz, oz + lz
    # 8 corners
    p000 = (x0, y0, z0)
    p100 = (x1, y0, z0)
    p010 = (x0, y1, z0)
    p110 = (x1, y1, z0)
    p001 = (x0, y0, z1)
    p101 = (x1, y0, z1)
    p011 = (x0, y1, z1)
    p111 = (x1, y1, z1)

    faces = []
    # z = z0 (bottom, outward -z)
    faces.append(tri(p000, p010, p110, (0, 0, -1)))
    faces.append(tri(p000, p110, p100, (0, 0, -1)))
    # z = z1 (top, outward +z)
    faces.append(tri(p001, p101, p111, (0, 0, 1)))
    faces.append(tri(p001, p111, p011, (0, 0, 1)))
    # y = y0 (outward -y)
    faces.append(tri(p000, p100, p101, (0, -1, 0)))
    faces.append(tri(p000, p101, p001, (0, -1, 0)))
    # y = y1 (outward +y)
    faces.append(tri(p010, p011, p111, (0, 1, 0)))
    faces.append(tri(p010, p111, p110, (0, 1, 0)))
    # x = x0 (outward -x)
    faces.append(tri(p000, p001, p011, (-1, 0, 0)))
    faces.append(tri(p000, p011, p010, (-1, 0, 0)))
    # x = x1 (outward +x)
    faces.append(tri(p100, p110, p111, (1, 0, 0)))
    faces.append(tri(p100, p111, p101, (1, 0, 0)))

    path.write_text(f"solid {name}\n" + "".join(faces) + f"endsolid {name}\n", encoding="utf-8")
    print(f"wrote {path.relative_to(ROOT)}  ({len(faces)} facets)")


def write_plate_hole(
    path: Path,
    *,
    half_w: float = 0.1,
    half_h: float = 0.05,
    thickness: float = 0.01,
    hole_r: float = 0.01,
    n_circ: int = 48,
) -> None:
    """Centered plate with through-hole along z. Origin at plate mid-plane centre.

    x ∈ [-half_w, half_w], y ∈ [-half_h, half_h], z ∈ [0, thickness].

    Outer boundary is a true rectangle (corners included). Ray-to-rect samples
    alone would chord-cut each 90° corner and leave the top/bottom faces
    non-manifold against the vertical walls — that shows up in the volume mesh
    as fan/notch artifacts at the four outer corners after surface snap.
    """
    assert hole_r < min(half_w, half_h)
    z0, z1 = 0.0, thickness
    # Rectangle corners CCW looking from +z, starting at (+w, -h) so that
    # walking wall 0→1→2→3 (+x → +y → -x → -y) inserts the matching corner.
    # Wall id: 0=+x, 1=+y, 2=-x, 3=-y.
    corner_ccw = [
        (half_w, half_h),    # between +x and +y
        (-half_w, half_h),   # between +y and -x
        (-half_w, -half_h),  # between -x and -y
        (half_w, -half_h),   # between -y and +x
    ]
    # Hole circle samples (CCW looking from +z).
    circle = [
        (hole_r * math.cos(2 * math.pi * i / n_circ),
         hole_r * math.sin(2 * math.pi * i / n_circ))
        for i in range(n_circ)
    ]

    faces: list[str] = []

    def ray_to_rect(cx: float, cy: float) -> tuple[float, float]:
        """Intersect ray from origin through (cx,cy) with the outer rectangle."""
        tx = half_w / abs(cx) if abs(cx) > 1e-15 else float("inf")
        ty = half_h / abs(cy) if abs(cy) > 1e-15 else float("inf")
        t = min(tx, ty)
        return (t * cx, t * cy)

    def which_wall(p: tuple[float, float]) -> int:
        """Wall id for a point on the outer rectangle (not a free corner)."""
        x, y = p
        eps = 1e-12 * max(half_w, half_h)
        # Prefer axis walls by absolute residual (corner → first match is fine;
        # ray_to_rect only lands on a true corner for rare exact angles).
        scores = [
            (abs(x - half_w), 0),
            (abs(y - half_h), 1),
            (abs(x + half_w), 2),
            (abs(y + half_h), 3),
        ]
        scores.sort()
        if scores[0][0] > eps * 10:
            raise ValueError(f"outer point not on rectangle wall: {p}")
        return scores[0][1]

    def corners_between(w_from: int, w_to: int) -> list[tuple[float, float]]:
        """Rectangle corners strictly between w_from and w_to walking CCW."""
        out: list[tuple[float, float]] = []
        w = w_from
        while w != w_to:
            out.append(corner_ccw[w])
            w = (w + 1) % 4
        return out

    outer_pts = [ray_to_rect(cx, cy) for cx, cy in circle]
    # Outer perimeter edges (2D), CCW, including true corners — used for
    # vertical walls so every top/bottom outer edge has a matching wall edge.
    outer_edges: list[tuple[tuple[float, float], tuple[float, float]]] = []

    for i in range(n_circ):
        j = (i + 1) % n_circ
        hi = circle[i]
        hj = circle[j]
        oi = outer_pts[i]
        oj = outer_pts[j]
        wi = which_wall(oi)
        wj = which_wall(oj)
        mids = corners_between(wi, wj) if wi != wj else []
        # Outer path for this sector: oi → (corners…) → oj
        outer_path = [oi] + mids + [oj]
        for a, b in zip(outer_path, outer_path[1:]):
            if abs(a[0] - b[0]) + abs(a[1] - b[1]) > 1e-15:
                outer_edges.append((a, b))

        # Top / bottom: polygon oi…oj + hole arc hj←hi. One corner insert is
        # the common case (adjacent walls); fan through the hole edge.
        if not mids:
            # Same wall: two tris (oi, oj, hj, hi).
            faces.append(tri((oi[0], oi[1], z1), (oj[0], oj[1], z1),
                             (hj[0], hj[1], z1), (0, 0, 1)))
            faces.append(tri((oi[0], oi[1], z1), (hj[0], hj[1], z1),
                             (hi[0], hi[1], z1), (0, 0, 1)))
            faces.append(tri((oi[0], oi[1], z0), (hi[0], hi[1], z0),
                             (hj[0], hj[1], z0), (0, 0, -1)))
            faces.append(tri((oi[0], oi[1], z0), (hj[0], hj[1], z0),
                             (oj[0], oj[1], z0), (0, 0, -1)))
        else:
            # Cross-corner sector: fill oi→C→…→oj against hole edge (hi,hj).
            # Fan from the hole edge so the corner patch is covered (no chord).
            chain = outer_path  # oi, corners…, oj
            for a, b in zip(chain, chain[1:]):
                faces.append(tri((a[0], a[1], z1), (b[0], b[1], z1),
                                 (hj[0], hj[1], z1), (0, 0, 1)))
                faces.append(tri((a[0], a[1], z0), (hj[0], hj[1], z0),
                                 (b[0], b[1], z0), (0, 0, -1)))
            # Close remaining strip from oi to hole (hi,hj) not covered by the
            # last fan that already used hj: triangle oi-hj-hi on top.
            faces.append(tri((oi[0], oi[1], z1), (hj[0], hj[1], z1),
                             (hi[0], hi[1], z1), (0, 0, 1)))
            faces.append(tri((oi[0], oi[1], z0), (hi[0], hi[1], z0),
                             (hj[0], hj[1], z0), (0, 0, -1)))

        # Hole wall (cylindrical). Material is outside the hole; STL outward
        # normal points into the hole (toward origin).
        h0b = (hi[0], hi[1], z0)
        h1b = (hj[0], hj[1], z0)
        h0t = (hi[0], hi[1], z1)
        h1t = (hj[0], hj[1], z1)
        mx = 0.5 * (hi[0] + hj[0])
        my = 0.5 * (hi[1] + hj[1])
        into_hole = _norm((-mx, -my, 0.0))
        faces.append(tri(h0b, h1b, h1t, into_hole))
        faces.append(tri(h0b, h1t, h0t, into_hole))

    # Outer vertical walls — one quad per outer-perimeter segment so edges
    # match the top/bottom outer polyline (manifold).
    for (p0, p1) in outer_edges:
        p0b = (p0[0], p0[1], z0)
        p1b = (p1[0], p1[1], z0)
        p0t = (p0[0], p0[1], z1)
        p1t = (p1[0], p1[1], z1)
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        # CCW outer ring → outward is to the right of directed edge.
        outward = _norm((dy, -dx, 0.0))
        faces.append(tri(p0b, p1b, p1t, outward))
        faces.append(tri(p0b, p1t, p0t, outward))

    # Guard: every edge must be shared by exactly two facets. The old ray-only
    # outer path chord-cut corners and left top/wall edges unpaired → mesh
    # snap produced the corner fan/notch artifacts on this part.
    _assert_manifold_facets(faces)

    path.write_text(
        "solid plate_hole\n" + "".join(faces) + "endsolid plate_hole\n",
        encoding="utf-8",
    )
    print(f"wrote {path.relative_to(ROOT)}  ({len(faces)} facets)")


def _assert_manifold_facets(faces: list[str]) -> None:
    """Parse emitted ASCII facet blocks and require edge multiplicity 2."""
    edge_count: dict[tuple, int] = defaultdict(int)
    pat = re.compile(
        r"vertex\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s*\n"
        r"\s*vertex\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s*\n"
        r"\s*vertex\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)"
    )
    n_facets = 0
    for block in faces:
        m = pat.search(block)
        if not m:
            continue
        n_facets += 1
        g = list(map(float, m.groups()))
        v = [
            (round(g[0], 9), round(g[1], 9), round(g[2], 9)),
            (round(g[3], 9), round(g[4], 9), round(g[5], 9)),
            (round(g[6], 9), round(g[7], 9), round(g[8], 9)),
        ]
        for a, b in ((v[0], v[1]), (v[1], v[2]), (v[2], v[0])):
            e = (a, b) if a < b else (b, a)
            edge_count[e] += 1
    bad = sum(1 for c in edge_count.values() if c != 2)
    if bad:
        raise RuntimeError(
            f"plate_hole STL not manifold: {bad}/{len(edge_count)} edges "
            f"have count != 2 ({n_facets} facets)"
        )


def main() -> None:
    print(_DEPRECATION_BANNER, file=sys.stderr, end="")
    OUT.mkdir(parents=True, exist_ok=True)

    # 1. smoke_bar — 0.1 × 0.01 × 0.01 m uniaxial tension bar
    write_box(OUT / "smoke_bar.stl", "smoke_bar", 0.1, 0.01, 0.01)

    # 2. plate_hole — Kirsch plate, half-width 0.1 m, hole r=0.01 m
    write_plate_hole(
        OUT / "plate_hole.stl",
        half_w=0.1,
        half_h=0.05,
        thickness=0.01,
        hole_r=0.01,
        n_circ=48,
    )

    # 3. cantilever — 1.0 × 0.1 × 0.1 m beam along +x
    write_box(OUT / "cantilever.stl", "cantilever", 1.0, 0.1, 0.1)


if __name__ == "__main__":
    main()
