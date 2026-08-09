#!/usr/bin/env python3
"""Run bounded live-BRep fidelity checks for sharp-feature CAD parts."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_CLI = REPO / "build/apps/cli/polymesh"
DEFAULT_PART = REPO / "tests/fixtures/parts/plate_hole.step"
DEFAULT_MESHERS = ("graded", "hybrid", "varyhedron")


def repo_path(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return str(path.resolve())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_provenance() -> dict:
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return {
        "commit": commit or None,
        "working_tree_dirty": status.returncode != 0 or bool(status.stdout.strip()),
    }


def validate_fidelity(mesher: str, run: dict) -> None:
    fidelity = run.get("fidelity", {})
    if not fidelity.get("available", False):
        raise RuntimeError(f"{mesher}: live-BRep fidelity was unavailable")
    if not fidelity.get("brep_valid", False) or not fidelity.get("brep_closed", False):
        raise RuntimeError(f"{mesher}: source BRep was invalid or open")
    if fidelity.get("brep_surface_sampler") != "exact_trimmed_face_uv_grid":
        raise RuntimeError(f"{mesher}: reverse fidelity did not use the exact bounded sampler")
    reference_ceiling = fidelity.get("brep_surface_sample_ceiling", 0)
    reference_count = fidelity.get("brep_surface_samples_to_mesh_boundary", {}).get(
        "count", 0
    )
    if reference_ceiling <= 0 or reference_count > reference_ceiling:
        raise RuntimeError(f"{mesher}: reverse fidelity exceeded or omitted its sample ceiling")
    reference_faces = fidelity.get("brep_surface_sample_faces", 0)
    reference_attempts = fidelity.get("brep_surface_uv_attempts", 0)
    reference_fallbacks = fidelity.get("brep_surface_fallback_vertices", 0)
    if (
        reference_faces <= 0
        or reference_attempts > 9 * reference_ceiling
        or reference_fallbacks > reference_faces
    ):
        raise RuntimeError(f"{mesher}: reverse fidelity sampler counters were inconsistent")
    required = (
        "mesh_boundary_to_brep",
        "brep_surface_samples_to_mesh_boundary",
        "normal_angle",
        "mesh_feature_to_sharp_brep_edge",
        "sharp_brep_edge_to_mesh_feature",
        "brep_vertex_to_mesh_node",
    )
    empty = [name for name in required if fidelity.get(name, {}).get("count", 0) <= 0]
    if empty:
        raise RuntimeError(
            f"{mesher}: required fidelity distributions were empty: {', '.join(empty)}"
        )
    if fidelity.get("mesh_feature_segments", 0) <= 0:
        raise RuntimeError(f"{mesher}: no boundary feature segments were classified")


def run_diag(
    cli: Path,
    part: Path,
    mesher: str,
    h: float,
    max_elems: int,
    max_dof: int,
    scratch: Path,
) -> tuple[dict, list[str]]:
    output = scratch / f"{mesher}.json"
    command = [
        str(cli),
        "diag",
        str(part),
        "-h",
        f"{h:.12g}",
        "--mesher",
        mesher,
        "--json",
        str(output),
        "--no-solve",
        "--max-elems",
        str(max_elems),
        "--max-dof",
        str(max_dof),
    ]
    completed = subprocess.run(
        command,
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{mesher}: diagnostic failed with exit {completed.returncode}\n"
            f"{completed.stdout}"
        )
    if not output.is_file():
        raise RuntimeError(f"{mesher}: diagnostic did not write {output}")
    display_command = command.copy()
    display_command[0] = repo_path(cli)
    display_command[2] = repo_path(part)
    display_command[8] = "<run>.json"
    return json.loads(output.read_text(encoding="utf-8")), display_command


def summary(run: dict) -> dict:
    fidelity = run["fidelity"]
    return {
        "mesher": run["mesher"],
        "elements": run["mesh"]["elements"],
        "nodes": run["mesh"]["nodes"],
        "quality_min": run["mesh"]["quality_min"],
        "mesh_ms": run["timing_ms"]["mesh"],
        "mesh_to_brep_p99_over_h": fidelity["mesh_boundary_to_brep"]["p99_over_h"],
        "mesh_to_brep_max_over_h": fidelity["mesh_boundary_to_brep"]["max_over_h"],
        "brep_to_mesh_p99_over_h": fidelity["brep_surface_samples_to_mesh_boundary"][
            "p99_over_h"
        ],
        "normal_p99_deg": fidelity["normal_angle"]["p99_deg"],
        "mesh_edge_to_brep_p99_over_h": fidelity["mesh_feature_to_sharp_brep_edge"]["p99_over_h"],
        "brep_edge_to_mesh_p99_over_h": fidelity["sharp_brep_edge_to_mesh_feature"]["p99_over_h"],
        "brep_vertex_to_mesh_p99_over_h": fidelity["brep_vertex_to_mesh_node"]["p99_over_h"],
        "relative_volume_error": fidelity["relative_volume_error"],
        "mesh_feature_segments": fidelity["mesh_feature_segments"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure bidirectional sampled CAD/mesh fidelity for several meshers."
    )
    parser.add_argument("--part", type=Path, default=DEFAULT_PART)
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--h", type=float, default=0.006)
    parser.add_argument("--meshers", nargs="+", default=list(DEFAULT_MESHERS))
    parser.add_argument("--max-elems", type=int, default=200_000)
    parser.add_argument("--max-dof", type=int, default=600_000)
    parser.add_argument("--label", default="working-tree")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    part = args.part if args.part.is_absolute() else REPO / args.part
    cli = args.cli if args.cli.is_absolute() else REPO / args.cli
    if not cli.is_file() or not os.access(cli, os.X_OK):
        raise SystemExit(f"error: executable CLI not found: {cli}")
    if not part.is_file():
        raise SystemExit(f"error: CAD fixture not found: {part}")
    if not (args.h > 0.0):
        raise SystemExit("error: --h must be positive and explicit")
    if args.max_elems <= 0 or args.max_dof <= 0:
        raise SystemExit("error: resource ceilings must be positive")

    runs: list[dict] = []
    commands: list[list[str]] = []
    with tempfile.TemporaryDirectory(prefix="polymesh-fidelity-") as tmp:
        scratch = Path(tmp)
        for mesher in args.meshers:
            run, command = run_diag(
                cli, part, mesher, args.h, args.max_elems, args.max_dof, scratch
            )
            validate_fidelity(mesher, run)
            runs.append(run)
            commands.append(command)

    payload = {
        "schema": "polymesh.mesher-fidelity.v2",
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "label": args.label,
        "part": repo_path(part),
        "part_sha256": sha256(part),
        "h_m": args.h,
        "resource_limits": {
            "max_elems": args.max_elems,
            "max_dof": args.max_dof,
            "brep_surface_samples": runs[0]["fidelity"][
                "brep_surface_sample_ceiling"
            ],
        },
        "provenance": {
            "git": git_provenance(),
            "executable": {
                "path": repo_path(cli),
                "sha256": sha256(cli),
                "bytes": cli.stat().st_size,
                "mtime_ns": cli.stat().st_mtime_ns,
            },
        },
        "commands": commands,
        "summary": [summary(run) for run in runs],
        "runs": runs,
    }
    out = args.out if args.out.is_absolute() else REPO / args.out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {repo_path(out)}")
    for row in payload["summary"]:
        print(
            f"{row['mesher']:10s} elems={row['elements']:6d} "
            f"surface_p99/h={row['mesh_to_brep_p99_over_h']:.4g} "
            f"reverse_p99/h={row['brep_to_mesh_p99_over_h']:.4g} "
            f"edge_reverse_p99/h={row['brep_edge_to_mesh_p99_over_h']:.4g} "
            f"normal_p99={row['normal_p99_deg']:.3g} deg"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
