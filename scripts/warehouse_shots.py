#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Render warehouse mesh.vtu → wire PNGs for a campaign (Lane V / V9b).

Walks ``bench/campaigns/<name>/runs/**/mesh.vtu`` and writes a sibling PNG
(``--out-name``, default ``wire.png``) via ``scripts/vtu_wire_png.py``.

Usage (repo root) -- ``python``, NOT ``python3``: on Windows ``python3`` can
resolve to a different install than the one carrying this repo's dependencies,
and the renderer then fails on a missing numpy.
  python scripts/warehouse_shots.py varyhedron-short-1
  python scripts/warehouse_shots.py bench/campaigns/varyhedron-short-1
  python scripts/warehouse_shots.py varyhedron-short-1 --force
  python scripts/warehouse_shots.py varyhedron-short-1 --hole-zoom

The figures sweep runs TWO passes per campaign, because
``scripts/advisor/report.py`` prefers a feature-framed render and falls back to
the whole-part camera when it is missing:
  python scripts/warehouse_shots.py <name>
  python scripts/warehouse_shots.py <name> --out-name wire_feature.png --hole-zoom

Skips runs that already have the target PNG unless ``--force``. Failures are
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vtu_wire_png import NO_FEATURE_EXIT  # noqa: E402


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
    ap.add_argument(
        "--out-name",
        default="wire.png",
        help="sibling PNG filename to write (default wire.png). The figures sweep "
             "makes a second pass with --out-name wire_feature.png --hole-zoom, "
             "because scripts/advisor/report.py PREFERS a feature-framed "
             "wire_feature.png and falls back to the whole-part camera when it is "
             "absent -- which, until this option existed, was always: no code path "
             "could name the output anything but wire.png, so mesh_before_after.png "
             "has only ever shown the whole-part camera with the hole rim edge-on.",
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
    no_feature = 0
    stale_removed = 0
    stale_kept = 0
    #: A hole-zoom pass whose output name promises a feature must not produce a
    #: file for a part that has none. vtu_wire_png exits 3 and writes nothing in
    #: that case, which is a legitimate outcome, not a failure: the render is
    #: absent exactly when the feature is absent, and report.py's fallback to
    #: the whole-part camera then means what it says.
    require_hole = args.hole_zoom and args.out_name != "wire.png"
    for vtu in vtus:
        png = vtu.with_name(args.out_name)
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
        if require_hole:
            cmd.append("--require-hole")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            print(f"TIMEOUT {vtu.relative_to(ROOT)}", file=sys.stderr)
            fail += 1
            continue
        if r.returncode == NO_FEATURE_EXIT and require_hole:
            no_feature += 1
            reason = next((line for line in (r.stdout or "").splitlines()
                           if line.startswith("hole check")), "")
            detail = reason.split("— ", 1)[-1] if "— " in reason else "no hole found"
            print(f"no feature {vtu.relative_to(ROOT)}: {detail}")
            # A file left over from a pass that could not check is a false
            # promise sitting on disk, and report.py PREFERS this name over
            # wire.png -- so leaving it would keep feeding the figure a crop
            # of a part with no feature in it. Under --force (an explicit
            # re-render) delete it; otherwise refuse to touch the tree and say
            # loudly that it is stale.
            if png.is_file():
                if args.force:
                    png.unlink()
                    stale_removed += 1
                    print(f"  removed stale {png.relative_to(ROOT)} "
                          "(written before the hole check existed)")
                else:
                    stale_kept += 1
                    print(f"  STALE {png.relative_to(ROOT)} claims a feature "
                          "this mesh does not have; re-run with --force to "
                          "remove it", file=sys.stderr)
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

    tail = (f", {no_feature} no feature (nothing written)" if no_feature else "")
    if stale_removed:
        tail += f", {stale_removed} stale removed"
    if stale_kept:
        tail += f", {stale_kept} STALE kept (use --force)"
    print(
        f"warehouse_shots: {ok} rendered, {skip} skipped (exists), "
        f"{fail} failed{tail} ({len(vtus)} mesh.vtu total)"
    )
    # Best-effort for campaign on_finish hooks: fail only if we attempted work
    # and nothing succeeded. "No feature" is not attempted-and-failed work: a
    # campaign of parts with no bores would otherwise report failure for
    # correctly declining to invent one.
    if fail and ok == 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
