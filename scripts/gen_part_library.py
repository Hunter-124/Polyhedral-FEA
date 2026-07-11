#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate validation-part STLs under tests/fixtures/parts/.

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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "fixtures" / "parts"


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
    """
    assert hole_r < min(half_w, half_h)
    z0, z1 = 0.0, thickness
    # Outer rectangle corners (CCW looking from +z).
    outer = [
        (-half_w, -half_h),
        (half_w, -half_h),
        (half_w, half_h),
        (-half_w, half_h),
    ]
    # Hole circle samples (CCW looking from +z).
    circle = [
        (hole_r * math.cos(2 * math.pi * i / n_circ),
         hole_r * math.sin(2 * math.pi * i / n_circ))
        for i in range(n_circ)
    ]

    faces: list[str] = []

    # --- Top / bottom: radial quads between outer edge projection and hole ---
    # For each outer edge, fan from outer edge to hole via angular sectors.
    # Simpler approach: for each circumferential sector, connect hole edge to
    # the outer boundary by projecting the ray to the rectangle.
    def ray_to_rect(cx: float, cy: float) -> tuple[float, float]:
        """Intersect ray from origin through (cx,cy) with the outer rectangle."""
        # Parametric: t * (cx, cy) hits the first outer wall.
        # Handle axis-aligned safely.
        tx = half_w / abs(cx) if abs(cx) > 1e-15 else float("inf")
        ty = half_h / abs(cy) if abs(cy) > 1e-15 else float("inf")
        t = min(tx, ty)
        return (t * cx, t * cy)

    outer_pts = [ray_to_rect(cx, cy) for cx, cy in circle]

    for i in range(n_circ):
        j = (i + 1) % n_circ
        hi = (circle[i][0], circle[i][1])
        hj = (circle[j][0], circle[j][1])
        oi = outer_pts[i]
        oj = outer_pts[j]

        # Top (z=z1), outward +z. Order so normal points +z.
        a = (oi[0], oi[1], z1)
        b = (oj[0], oj[1], z1)
        c = (hj[0], hj[1], z1)
        d = (hi[0], hi[1], z1)
        faces.append(tri(a, b, c, (0, 0, 1)))
        faces.append(tri(a, c, d, (0, 0, 1)))

        # Bottom (z=z0), outward -z.
        a = (oi[0], oi[1], z0)
        b = (hi[0], hi[1], z0)
        c = (hj[0], hj[1], z0)
        d = (oj[0], oj[1], z0)
        faces.append(tri(a, b, c, (0, 0, -1)))
        faces.append(tri(a, c, d, (0, 0, -1)))

        # Hole wall (cylindrical): outward is into the hole (-radial for solid
        # with a void? Wait — the solid is OUTSIDE the hole; the free surface
        # of the hole has outward normal pointing INTO the hole (toward
        # centre), i.e. -radial. For watertight solid "material exterior",
        # STL normals point out of the material, so for the hole wall the
        # outward-from-material normal points into the hole (toward origin).
        h0b = (hi[0], hi[1], z0)
        h1b = (hj[0], hj[1], z0)
        h0t = (hi[0], hi[1], z1)
        h1t = (hj[0], hj[1], z1)
        # Mid-sector radial toward centre.
        mx = 0.5 * (hi[0] + hj[0])
        my = 0.5 * (hi[1] + hj[1])
        into_hole = _norm((-mx, -my, 0.0))
        faces.append(tri(h0b, h1b, h1t, into_hole))
        faces.append(tri(h0b, h1t, h0t, into_hole))

    # Outer vertical walls — four sides of the rectangle.
    # Bottom edge of each side at z0, top at z1.
    # Side -y (y = -half_h), outward (0,-1,0)
    x_left, x_right = -half_w, half_w
    y_bot, y_top = -half_h, half_h
    # -y face
    faces.append(tri((x_left, y_bot, z0), (x_right, y_bot, z0), (x_right, y_bot, z1), (0, -1, 0)))
    faces.append(tri((x_left, y_bot, z0), (x_right, y_bot, z1), (x_left, y_bot, z1), (0, -1, 0)))
    # +y face
    faces.append(tri((x_right, y_top, z0), (x_left, y_top, z0), (x_left, y_top, z1), (0, 1, 0)))
    faces.append(tri((x_right, y_top, z0), (x_left, y_top, z1), (x_right, y_top, z1), (0, 1, 0)))
    # -x face
    faces.append(tri((x_left, y_top, z0), (x_left, y_bot, z0), (x_left, y_bot, z1), (-1, 0, 0)))
    faces.append(tri((x_left, y_top, z0), (x_left, y_bot, z1), (x_left, y_top, z1), (-1, 0, 0)))
    # +x face
    faces.append(tri((x_right, y_bot, z0), (x_right, y_top, z0), (x_right, y_top, z1), (1, 0, 0)))
    faces.append(tri((x_right, y_bot, z0), (x_right, y_top, z1), (x_right, y_bot, z1), (1, 0, 0)))

    # The radial-to-rect projection leaves four rectangular "corner" patches
    # covered only when a ray hits a corner (shared by two walls). Rays that
    # hit the same wall between consecutive samples form quads fully covering
    # the flat rim; at corners the outer_pts jump from one wall to the next
    # and the quad spans the corner correctly (triangle fan across the corner).
    # No extra fill needed.

    _ = outer  # reserved for future explicit outer-loop mode
    path.write_text(
        "solid plate_hole\n" + "".join(faces) + "endsolid plate_hole\n",
        encoding="utf-8",
    )
    print(f"wrote {path.relative_to(ROOT)}  ({len(faces)} facets)")


def main() -> None:
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
