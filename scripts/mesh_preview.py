#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Timeout-guarded mesh preview: run polymesh mesh, emit JSON metrics + optional PNG.

Usage:
  python scripts/mesh_preview.py geom.stl --mesher graded --timeout 90 --out-dir bench/mesher/shots
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path


def parse_mesh_stdout(text: str) -> dict:
    info: dict = {}
    m = re.search(r"mesh:\s*(\d+)\s*nodes,\s*(\d+)\s*elems", text)
    if m:
        info["nodes"] = int(m.group(1))
        info["elems"] = int(m.group(2))
    m = re.search(r"h=([0-9.eE+-]+)\s*m", text)
    if m:
        info["h"] = float(m.group(1))
    m = re.search(r"snap max\|d\|=([0-9.eE+-]+)", text)
    if m:
        info["snap_max"] = float(m.group(1))
    for line in text.splitlines():
        low = line.lower()
        if any(k in low for k in ("graded", "hybrid", "hex", "octa", "auto h", "tet")):
            info.setdefault("notes", []).append(line.strip())
    return info


def try_render_png(vtu_path: Path, png_path: Path) -> bool:
    """Best-effort wireframe via pure-Python exterior edges, then meshio."""
    wire = Path(__file__).resolve().parent / "vtu_wire_png.py"
    if wire.is_file():
        try:
            r = subprocess.run(
                [sys.executable, str(wire), str(vtu_path), str(png_path)],
                capture_output=True,
                text=True,
                timeout=60,
            )
            if r.returncode == 0 and png_path.is_file():
                return True
        except Exception:
            pass
    try:
        import meshio  # type: ignore
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt  # type: ignore
        from mpl_toolkits.mplot3d.art3d import Line3DCollection  # type: ignore
    except Exception:
        return False
    try:
        mesh = meshio.read(str(vtu_path))
        pts = mesh.points
        segs = []
        for block in mesh.cells:
            if block.type in ("tetra", "pyramid", "hexahedron", "wedge", "triangle", "quad"):
                cells = block.data
                step = max(1, len(cells) // 8000)
                for cell in cells[::step]:
                    ids = list(cell)
                    n = len(ids)
                    for i in range(n):
                        segs.append([pts[ids[i]], pts[ids[(i + 1) % n]]])
        if not segs:
            return False
        fig = plt.figure(figsize=(8, 8))
        ax = fig.add_subplot(111, projection="3d")
        lc = Line3DCollection(segs, colors="k", linewidths=0.15, alpha=0.55)
        ax.add_collection3d(lc)
        mins = pts.min(axis=0)
        maxs = pts.max(axis=0)
        ax.set_xlim(mins[0], maxs[0])
        ax.set_ylim(mins[1], maxs[1])
        ax.set_zlim(mins[2], maxs[2])
        ax.set_axis_off()
        ax.view_init(elev=55, azim=35)
        fig.tight_layout()
        fig.savefig(png_path, dpi=120, bbox_inches="tight", facecolor="#2a3a6a")
        plt.close(fig)
        return True
    except Exception:
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description="Timeout-guarded polymesh mesh preview")
    ap.add_argument("geometry", nargs="?", default="")
    ap.add_argument("--mesher", default="graded")
    ap.add_argument("--feature", action="store_true", default=True)
    ap.add_argument("--no-feature", action="store_true")
    ap.add_argument("--mesh-size", type=float, default=0.0)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--out-dir", default="bench/mesher/shots")
    ap.add_argument("--polymesh", default="polymesh.exe")
    args = ap.parse_args()
    if not args.geometry:
        print("mesh_preview.py ok (pass geometry path to run)")
        return 0

    geom = Path(args.geometry)
    if not geom.is_file():
        print(f"missing geometry: {geom}", file=sys.stderr)
        return 2
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = f"{geom.stem}_{args.mesher}"
    vtu = out_dir / f"{stem}.vtu"
    meta = out_dir / f"{stem}.json"
    png = out_dir / f"{stem}.png"

    exe = Path(args.polymesh)
    if not exe.is_file():
        for cand in ("polymesh.exe", "build/apps/cli/polymesh.exe", "./polymesh"):
            if Path(cand).is_file():
                exe = Path(cand)
                break

    cmd = [str(exe), "mesh", str(geom), "-o", str(vtu), "--mesher", args.mesher]
    if args.mesh_size > 0:
        cmd += ["-h", str(args.mesh_size)]
    if args.feature and not args.no_feature:
        cmd.append("--feature")

    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired:
        payload = {
            "ok": False,
            "error": "timeout",
            "timeout_s": args.timeout,
            "mesher": args.mesher,
            "geometry": str(geom),
        }
        meta.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"TIMEOUT after {args.timeout}s: {' '.join(cmd)}", file=sys.stderr)
        return 124

    wall = time.perf_counter() - t0
    text = (proc.stdout or "") + "\n" + (proc.stderr or "")
    info = parse_mesh_stdout(text)
    info.update(
        {
            "ok": proc.returncode == 0,
            "returncode": proc.returncode,
            "wall_s": wall,
            "mesher": args.mesher,
            "geometry": str(geom),
            "cmd": cmd,
            "stdout": text[-4000:],
        }
    )
    if proc.returncode == 0 and vtu.is_file():
        info["png"] = try_render_png(vtu, png)
        if info.get("png"):
            info["png_path"] = str(png)
    meta.write_text(json.dumps(info, indent=2), encoding="utf-8")
    summary = {k: info[k] for k in info if k != "stdout"}
    print(json.dumps(summary, indent=2))
    return 0 if proc.returncode == 0 else proc.returncode


if __name__ == "__main__":
    sys.exit(main())
