#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Varyhedron packing microbench (V5) — pure-Python seed packing demo.

Reports for unit-cube and sphere-like seed sets:
  - fill_fraction_proxy  (sum of non-overlapping ball volumes / domain volume;
                          balls clipped by a simple overlap discount)
  - boundary_residual    (placeholder metric until V6b CAD Hausdorff lands)
  - wall_ms              (pack + metric time)

Does not require FEA, OCC, or a built polymesh binary.

Usage (from repo root):
  python3 bench/packing/run_packing_microbench.py
  python3 bench/packing/run_packing_microbench.py --spacing 0.08 --out bench/packing/out/summary.json
"""
from __future__ import annotations

import argparse
import json
import math
import random
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

Vec3 = Tuple[float, float, float]


@dataclass(frozen=True)
class Seed:
    x: float
    y: float
    z: float
    r: float  # target bubble radius (~ h/2)


def _dist2(a: Seed, b: Seed) -> float:
    dx = a.x - b.x
    dy = a.y - b.y
    dz = a.z - b.z
    return dx * dx + dy * dy + dz * dz


def _clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def sample_boundary_edges_cube(spacing: float) -> List[Seed]:
    """Protecting-ball style seeds along the 12 edges of the unit cube [0,1]^3."""
    r = 0.5 * spacing
    seeds: List[Seed] = []
    # Axis-aligned edges: fix two coords in {0,1}, walk the free axis.
    for fixed_y in (0.0, 1.0):
        for fixed_z in (0.0, 1.0):
            n = max(2, int(round(1.0 / spacing)) + 1)
            for i in range(n):
                t = i / (n - 1)
                seeds.append(Seed(t, fixed_y, fixed_z, r))
    for fixed_x in (0.0, 1.0):
        for fixed_z in (0.0, 1.0):
            n = max(2, int(round(1.0 / spacing)) + 1)
            for i in range(n):
                t = i / (n - 1)
                seeds.append(Seed(fixed_x, t, fixed_z, r))
    for fixed_x in (0.0, 1.0):
        for fixed_y in (0.0, 1.0):
            n = max(2, int(round(1.0 / spacing)) + 1)
            for i in range(n):
                t = i / (n - 1)
                seeds.append(Seed(fixed_x, fixed_y, t, r))
    return _dedupe(seeds, tol=0.25 * spacing)


def sample_boundary_equator_sphere(spacing: float, radius: float = 0.5) -> List[Seed]:
    """Edge-protect proxy: great-circle rings on the unit ball (center 0.5^3)."""
    r_seed = 0.5 * spacing
    cx = cy = cz = 0.5
    seeds: List[Seed] = []
    # Three orthogonal great circles (CAD "feature curves" stand-in).
    n = max(8, int(round(2.0 * math.pi * radius / spacing)))
    for plane in ("xy", "xz", "yz"):
        for i in range(n):
            th = 2.0 * math.pi * i / n
            if plane == "xy":
                x = cx + radius * math.cos(th)
                y = cy + radius * math.sin(th)
                z = cz
            elif plane == "xz":
                x = cx + radius * math.cos(th)
                y = cy
                z = cz + radius * math.sin(th)
            else:
                x = cx
                y = cy + radius * math.cos(th)
                z = cz + radius * math.sin(th)
            seeds.append(Seed(x, y, z, r_seed))
    return _dedupe(seeds, tol=0.25 * spacing)


def _dedupe(seeds: Sequence[Seed], tol: float) -> List[Seed]:
    out: List[Seed] = []
    tol2 = tol * tol
    for s in seeds:
        if any(_dist2(s, t) < tol2 for t in out):
            continue
        out.append(s)
    return out


def volume_seeds_cube(spacing: float, rng: random.Random) -> List[Seed]:
    """Jittered lattice interior seeds in (0,1)^3, radius ~ spacing/2."""
    r = 0.5 * spacing
    n = max(2, int(math.floor(1.0 / spacing)))
    seeds: List[Seed] = []
    # Stay off the boundary faces so edge protect seeds own the skin.
    for i in range(n):
        for j in range(n):
            for k in range(n):
                x = (i + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                y = (j + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                z = (k + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                x, y, z = _clamp01(x), _clamp01(y), _clamp01(z)
                # Keep a thin empty collar near faces for boundary residual proxy.
                if min(x, y, z, 1.0 - x, 1.0 - y, 1.0 - z) < 0.5 * spacing:
                    continue
                seeds.append(Seed(x, y, z, r))
    return seeds


def volume_seeds_ball(spacing: float, rng: random.Random, radius: float = 0.5) -> List[Seed]:
    """Jittered seeds inside ball centered at (0.5,0.5,0.5)."""
    r = 0.5 * spacing
    cx = cy = cz = 0.5
    n = max(2, int(math.floor(1.0 / spacing)))
    seeds: List[Seed] = []
    for i in range(n):
        for j in range(n):
            for k in range(n):
                x = (i + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                y = (j + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                z = (k + 0.5) / n + rng.uniform(-0.15, 0.15) * spacing
                dx, dy, dz = x - cx, y - cy, z - cz
                rho = math.sqrt(dx * dx + dy * dy + dz * dz)
                if rho > radius - 0.5 * spacing:
                    continue
                seeds.append(Seed(x, y, z, r))
    return seeds


def bubble_relax(seeds: List[Seed], domain: str, iterations: int, rng: random.Random) -> List[Seed]:
    """Lightweight Shimada-style push: separate overlapping bubbles."""
    if not seeds or iterations <= 0:
        return list(seeds)
    pts = [Seed(s.x, s.y, s.z, s.r) for s in seeds]
    for _ in range(iterations):
        forces = [(0.0, 0.0, 0.0) for _ in pts]
        for i in range(len(pts)):
            for j in range(i + 1, len(pts)):
                a, b = pts[i], pts[j]
                d2 = _dist2(a, b)
                min_d = a.r + b.r
                if d2 <= 1e-18:
                    # Random kick for coincident seeds.
                    fx = rng.uniform(-1e-3, 1e-3)
                    fy = rng.uniform(-1e-3, 1e-3)
                    fz = rng.uniform(-1e-3, 1e-3)
                    forces[i] = (forces[i][0] + fx, forces[i][1] + fy, forces[i][2] + fz)
                    forces[j] = (forces[j][0] - fx, forces[j][1] - fy, forces[j][2] - fz)
                    continue
                d = math.sqrt(d2)
                if d >= min_d:
                    continue
                # Repel proportional to overlap fraction.
                overlap = (min_d - d) / min_d
                s = 0.35 * overlap
                ux, uy, uz = (a.x - b.x) / d, (a.y - b.y) / d, (a.z - b.z) / d
                forces[i] = (forces[i][0] + s * ux, forces[i][1] + s * uy, forces[i][2] + s * uz)
                forces[j] = (forces[j][0] - s * ux, forces[j][1] - s * uy, forces[j][2] - s * uz)
        new_pts: List[Seed] = []
        for s, f in zip(pts, forces):
            x = s.x + f[0] * s.r
            y = s.y + f[1] * s.r
            z = s.z + f[2] * s.r
            if domain == "cube":
                x, y, z = _clamp01(x), _clamp01(y), _clamp01(z)
            else:
                # Project back into unit ball if outside.
                cx = cy = cz = 0.5
                dx, dy, dz = x - cx, y - cy, z - cz
                rho = math.sqrt(dx * dx + dy * dy + dz * dz)
                max_r = 0.5 - 1e-6
                if rho > max_r and rho > 0.0:
                    scale = max_r / rho
                    x = cx + dx * scale
                    y = cy + dy * scale
                    z = cz + dz * scale
            new_pts.append(Seed(x, y, z, s.r))
        pts = new_pts
    return pts


def fill_fraction_proxy(seeds: Sequence[Seed], domain_volume: float) -> float:
    """Packed-ball volume / domain volume with pairwise overlap discount.

    Exact union of balls is unnecessary for a microbench; discount each pair's
    approximate lens volume once. Clamped to [0, 1].
    """
    if domain_volume <= 0.0 or not seeds:
        return 0.0
    vol = 0.0
    for s in seeds:
        vol += (4.0 / 3.0) * math.pi * (s.r ** 3)
    # Pairwise intersection upper-bound discount (spherical cap pair formula).
    n = len(seeds)
    for i in range(n):
        for j in range(i + 1, n):
            a, b = seeds[i], seeds[j]
            d = math.sqrt(_dist2(a, b))
            ra, rb = a.r, b.r
            if d >= ra + rb or d <= 1e-15:
                continue
            if d <= abs(ra - rb):
                # One ball inside the other — subtract the smaller once.
                r_small = min(ra, rb)
                vol -= (4.0 / 3.0) * math.pi * (r_small ** 3)
                continue
            # Standard two-sphere intersection volume.
            ra2, rb2 = ra * ra, rb * rb
            h1 = (ra2 - rb2 + d * d) / (2.0 * d)
            h2 = d - h1
            # Cap heights.
            cap_h1 = ra - h1
            cap_h2 = rb - h2
            if cap_h1 < 0.0 or cap_h2 < 0.0:
                continue
            v_int = (
                math.pi * (cap_h1 ** 2) * (3.0 * ra - cap_h1)
                + math.pi * (cap_h2 ** 2) * (3.0 * rb - cap_h2)
            ) / 3.0
            vol -= v_int
    frac = vol / domain_volume
    return max(0.0, min(1.0, frac))


def boundary_residual_placeholder(boundary: Sequence[Seed], all_seeds: Sequence[Seed]) -> float:
    """Placeholder until V6b CAD edge Hausdorff.

    Mean relative gap: for each boundary seed, how far the nearest *other*
    seed is from ideal contact distance (r_i + r_j), normalized by spacing.
    0 ≈ well-supported edge packing; larger ≈ gaps along the protected edge.
    """
    if not boundary:
        return 0.0
    interior = [s for s in all_seeds if s not in boundary]
    # Membership by identity may fail after relax copies — use coordinate match set.
    bset = {(round(s.x, 6), round(s.y, 6), round(s.z, 6)) for s in boundary}
    others = [
        s
        for s in all_seeds
        if (round(s.x, 6), round(s.y, 6), round(s.z, 6)) not in bset
    ]
    if not others:
        others = list(all_seeds)
    acc = 0.0
    for b in boundary:
        best = float("inf")
        best_gap = 0.0
        for o in others:
            d = math.sqrt(_dist2(b, o))
            if d < 1e-12:
                continue
            ideal = b.r + o.r
            gap = abs(d - ideal) / max(ideal, 1e-12)
            if d < best:
                best = d
                best_gap = gap
        if best is float("inf"):
            continue
        acc += best_gap
    return acc / max(len(boundary), 1)


def pack_case(
    name: str,
    spacing: float,
    relax_iters: int,
    seed: int,
) -> dict:
    rng = random.Random(seed)
    t0 = time.perf_counter()
    if name == "unit_cube":
        boundary = sample_boundary_edges_cube(spacing)
        volume = volume_seeds_cube(spacing, rng)
        domain_volume = 1.0
        domain = "cube"
    elif name == "sphere_like":
        boundary = sample_boundary_equator_sphere(spacing)
        volume = volume_seeds_ball(spacing, rng)
        domain_volume = (4.0 / 3.0) * math.pi * (0.5 ** 3)
        domain = "ball"
    else:
        raise ValueError(f"unknown case {name!r}")

    # Freeze boundary (CAD edge protect): only relax interior seeds, then rejoin.
    interior = bubble_relax(volume, domain=domain, iterations=relax_iters, rng=rng)
    # Mild projection of interior away from boundary seeds (protecting balls).
    protected: List[Seed] = []
    for s in interior:
        x, y, z = s.x, s.y, s.z
        for b in boundary:
            d2 = (x - b.x) ** 2 + (y - b.y) ** 2 + (z - b.z) ** 2
            min_d = s.r + b.r
            if d2 < min_d * min_d and d2 > 1e-18:
                d = math.sqrt(d2)
                push = (min_d - d) / d
                x += (x - b.x) * push
                y += (y - b.y) * push
                z += (z - b.z) * push
        if domain == "cube":
            x, y, z = _clamp01(x), _clamp01(y), _clamp01(z)
        protected.append(Seed(x, y, z, s.r))

    all_seeds = list(boundary) + protected
    fill = fill_fraction_proxy(all_seeds, domain_volume)
    residual = boundary_residual_placeholder(boundary, all_seeds)
    wall_ms = (time.perf_counter() - t0) * 1000.0

    return {
        "case": name,
        "spacing": spacing,
        "relax_iters": relax_iters,
        "rng_seed": seed,
        "n_boundary_seeds": len(boundary),
        "n_volume_seeds": len(protected),
        "n_seeds": len(all_seeds),
        "domain_volume": domain_volume,
        "fill_fraction_proxy": round(fill, 6),
        "boundary_residual": round(residual, 6),
        "boundary_residual_note": (
            "placeholder; V6b replaces with CAD edge Hausdorff residual"
        ),
        "wall_ms": round(wall_ms, 3),
    }


def main(argv: Iterable[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--spacing",
        type=float,
        default=0.1,
        help="nominal seed spacing h (default 0.1)",
    )
    ap.add_argument(
        "--relax-iters",
        type=int,
        default=8,
        help="bubble-relax iterations on interior seeds (default 8)",
    )
    ap.add_argument("--seed", type=int, default=42, help="RNG seed")
    ap.add_argument(
        "--out",
        type=Path,
        default=Path("bench/packing/out/summary.json"),
        help="output JSON path (default bench/packing/out/summary.json)",
    )
    args = ap.parse_args(list(argv) if argv is not None else None)

    cases = [
        pack_case("unit_cube", args.spacing, args.relax_iters, args.seed),
        pack_case("sphere_like", args.spacing, args.relax_iters, args.seed + 1),
    ]
    summary = {
        "bench": "varyhedron-packing-microbench",
        "node": "V5",
        "v1_algorithm": (
            "boundary-edge constrained seed packing + dual poly from refined "
            "tet scaffold with CAD edge protect; VEM on polys (ADR-0021)"
        ),
        "research_doc": "docs/research/varyhedron-packing.md",
        "spacing": args.spacing,
        "relax_iters": args.relax_iters,
        "cases": cases,
    }

    out: Path = args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"wrote {out}")
    for c in cases:
        print(
            f"  {c['case']}: seeds={c['n_seeds']} "
            f"fill={c['fill_fraction_proxy']:.4f} "
            f"boundary_residual={c['boundary_residual']:.4f} "
            f"wall_ms={c['wall_ms']:.2f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
