#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Render warehouse mesh.vtu → wire.png for a campaign (Lane V / V9b).

Walks ``bench/campaigns/<name>/runs/**/mesh.vtu`` and writes sibling
``wire.png`` via ``scripts/vtu_wire_png.py`` (pure-Python exterior edges).

Usage (repo root):
  python3 scripts/warehouse_shots.py varyhedron-short-1
  python3 scripts/warehouse_shots.py bench/campaigns/varyhedron-short-1
  python3 scripts/warehouse_shots.py varyhedron-short-1 --force
  python3 scripts/warehouse_shots.py varyhedron-short-1 --hole-zoom

Skips runs that already have wire.png unless ``--force``. Failures are
reported; exit code is non-zero only if every conversion failed when work
was expected.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIRE = Path(__file__).resolve().parent / "vtu_wire_png.py"


def resolve_campaign(arg: str) -> Path:
    p = Path(arg)
    if p.is_dir() and (p / "campaign.json").is_file():
        return p.resolve()
    cand = ROOT / "bench" / "campaigns" / arg
    if cand.is_dir() and (cand / "campaign.json").is_file():
        return cand.resolve()
    raise SystemExit(f"campaign not found: {arg}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Render warehouse mesh.vtu files to wire.png (V9b)"
    )
    ap.add_argument("campaign", help="campaign name or path under bench/campaigns/")
    ap.add_argument(
        "--force",
        action="store_true",
        help="re-render even when wire.png already exists",
    )
    ap.add_argument(
        "--hole-zoom",
        action="store_true",
        help="pass --hole-zoom to vtu_wire_png (plate_hole style ROI)",
    )
    ap.add_argument(
        "--view",
        default="iso",
        choices=["iso", "top", "front", "side"],
        help="camera view (default iso)",
    )
    ap.add_argument(
        "--size",
        type=int,
        default=1100,
        help="PNG edge size in pixels (default 1100)",
    )
    args = ap.parse_args()

    if not WIRE.is_file():
        print(f"missing {WIRE}", file=sys.stderr)
        return 2

    camp = resolve_campaign(args.campaign)
    runs = camp / "runs"
    if not runs.is_dir():
        print(f"no runs/ under {camp.relative_to(ROOT)} — nothing to do")
        return 0

    vtus = sorted(runs.glob("**/mesh.vtu"))
    if not vtus:
        print(f"no mesh.vtu under {runs.relative_to(ROOT)}")
        return 0

    ok = 0
    skip = 0
    fail = 0
    for vtu in vtus:
        png = vtu.with_name("wire.png")
        if png.is_file() and not args.force:
            skip += 1
            continue
        cmd = [
            sys.executable,
            str(WIRE),
            str(vtu),
            str(png),
            "--view",
            args.view,
            "--size",
            str(args.size),
        ]
        if args.hole_zoom:
            cmd.append("--hole-zoom")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT {vtu.relative_to(ROOT)}", file=sys.stderr)
            fail += 1
            continue
        if r.returncode != 0 or not png.is_file():
            msg = (r.stderr or r.stdout or "").strip()
            print(
                f"FAIL {vtu.relative_to(ROOT)}: {msg or f'exit {r.returncode}'}",
                file=sys.stderr,
            )
            fail += 1
            continue
        ok += 1
        rel = png.relative_to(ROOT)
        print(f"wrote {rel}")

    print(
        f"warehouse_shots: {ok} rendered, {skip} skipped (exists), "
        f"{fail} failed ({len(vtus)} mesh.vtu total)"
    )
    # Best-effort for campaign on_finish hooks: fail only if we attempted
    # work and nothing succeeded.
    if fail and ok == 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
