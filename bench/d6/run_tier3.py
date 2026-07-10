#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""D6 Tier-3 harness: uniform tet10 vs graded tet10 on L-domain.

Runs apps/bench binary `polymesh-d6-tier3`, writes competitive-schema rows under
`bench/results/`, and optionally refreshes `docs/bench/scoreboard.md`.

Usage:
  python3 bench/d6/run_tier3.py --help
  python3 bench/d6/run_tier3.py --quick          # small grids
  python3 bench/d6/run_tier3.py --full --render  # fuller suite + scoreboard
  python3 bench/d6/run_tier3.py --write-only PATH.json  # split existing raw

Does not require multi-minute ctest; --quick is the default for smoke.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
RESULTS = ROOT / "bench" / "results"
# Raw suite lives under bench/d6/ so render_scoreboard (bench/results/*.json)
# only sees the competitive headline rows.
RAW_DIR = ROOT / "bench" / "d6" / "out"
RAW_NAME = "polymesh-d6-l-domain-raw.json"
FLAT_NAME = "polymesh-d6-l-domain.json"
SUMMARY_MD = ROOT / "docs" / "bench" / "d6-tier3.md"


def find_binary() -> Path:
    env = os.environ.get("POLYMESH_D6_BIN")
    if env:
        p = Path(env)
        if p.is_file():
            return p
    candidates = [
        ROOT / "build" / "apps" / "bench" / "polymesh-d6-tier3",
        ROOT / "build" / "polymesh-d6-tier3",
    ]
    for c in candidates:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    # Search build tree shallowly
    build = ROOT / "build"
    if build.is_dir():
        for p in build.rglob("polymesh-d6-tier3"):
            if p.is_file() and os.access(p, os.X_OK):
                return p
    raise FileNotFoundError(
        "polymesh-d6-tier3 not found. Build first:\n"
        "  cmake -S . -B build -G Ninja && cmake --build build --target polymesh-d6-tier3 -j"
    )


def ensure_built(quick_hint: bool) -> Path:
    try:
        return find_binary()
    except FileNotFoundError:
        print("d6: binary missing — configuring/building polymesh-d6-tier3 …", file=sys.stderr)
        build = ROOT / "build"
        if not (build / "build.ninja").is_file() and not (build / "Makefile").is_file():
            subprocess.check_call(
                ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                 "-DPOLYMESH_WITH_GUI=OFF"],
                cwd=str(ROOT),
            )
        subprocess.check_call(
            ["cmake", "--build", str(build), "--target", "polymesh-d6-tier3", "-j"],
            cwd=str(ROOT),
        )
        return find_binary()


def split_for_scoreboard(raw: dict[str, Any]) -> list[dict[str, Any]]:
    """Emit competitive-schema rows: per-path headline + summary metrics as notes."""
    rows: list[dict[str, Any]] = []
    results = raw.get("results") or []
    # Prefer the two headline case_ids if present
    headlines = [
        r
        for r in results
        if isinstance(r, dict)
        and r.get("case_id") in ("l-domain-d6-baseline", "l-domain-d6-graded")
    ]
    if not headlines:
        headlines = [r for r in results if isinstance(r, dict)]

    summary = raw.get("summary") or {}
    for r in headlines:
        if r.get("case_id") not in ("l-domain-d6-baseline", "l-domain-d6-graded"):
            # Skip intermediate grids in the flat scoreboard file to avoid noise
            continue
        row = {
            "schema_version": 1,
            "solver": r.get("solver", "PolyMesh"),
            "version": r.get("version", "0.1.0"),
            "case_id": r["case_id"],
            "dofs": r.get("dofs"),
            "wall_time_s": r.get("wall_time_s")
            or {"mesh": None, "solve": None, "total": None},
            "accuracy": r.get("accuracy")
            or {"name": "energy_deficit_pct", "value": None, "unit": "percent"},
            "label": r.get("label") or raw.get("label") or "d6-tier3",
            "timestamp": r.get("timestamp") or raw.get("timestamp"),
            "notes": r.get("notes", ""),
        }
        if summary:
            row["notes"] = (
                f"{row['notes']}; "
                f"dof_ratio={summary.get('dof_ratio_uniform_over_graded')}; "
                f"time_ratio={summary.get('time_ratio_uniform_over_graded')}; "
                f"claim={summary.get('claim')}"
            )
        rows.append(row)

    # Extra row: D6 ratio summary as a synthetic accuracy metric for the table.
    if summary:
        rows.append(
            {
                "schema_version": 1,
                "solver": "PolyMesh",
                "version": "0.1.0",
                "case_id": "l-domain-d6-ratio",
                "dofs": summary.get("graded_dofs"),
                "wall_time_s": {
                    "mesh": None,
                    "solve": None,
                    "total": summary.get("graded_total_s"),
                },
                "accuracy": {
                    "name": "dof_ratio_uniform_over_graded",
                    "value": summary.get("dof_ratio_uniform_over_graded"),
                    "unit": "ratio",
                },
                "label": raw.get("label") or "d6-tier3",
                "timestamp": raw.get("timestamp"),
                "notes": (
                    f"Tier-3 instrument: uniform {summary.get('baseline_tag')} "
                    f"({summary.get('baseline_dofs')} DOF) vs graded "
                    f"{summary.get('graded_tag')} ({summary.get('graded_dofs')} DOF); "
                    f"time_ratio={summary.get('time_ratio_uniform_over_graded')}; "
                    f"meets_dof={summary.get('meets_dof_target')}; "
                    f"meets_time={summary.get('meets_time_target')}; "
                    f"claim={summary.get('claim')}"
                ),
            }
        )
        rows.append(
            {
                "schema_version": 1,
                "solver": "PolyMesh",
                "version": "0.1.0",
                "case_id": "l-domain-d6-ratio",
                "dofs": summary.get("graded_dofs"),
                "wall_time_s": {
                    "mesh": None,
                    "solve": None,
                    "total": summary.get("graded_total_s"),
                },
                "accuracy": {
                    "name": "time_ratio_uniform_over_graded",
                    "value": summary.get("time_ratio_uniform_over_graded"),
                    "unit": "ratio",
                },
                "label": raw.get("label") or "d6-tier3",
                "timestamp": raw.get("timestamp"),
                "notes": "Companion time ratio (uniform total / graded total) at matched claim.",
            }
        )
    return rows


def _fmt(v: Any, nd: int = 6) -> str:
    if v is None:
        return "—"
    if isinstance(v, bool):
        return "yes" if v else "no"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return f"{v:.{nd}g}"
    return str(v)


def write_summary_md(raw: dict[str, Any], path: Path) -> None:
    s = raw.get("summary") or {}
    lines = [
        "# D6 Tier-3 — L-domain uniform tet10 vs graded tet10",
        "",
        "Instrument for ROADMAP **D6** / SPEC Tier-3 (≥5× DOF, ≥3× wall time vs "
        "uniform tet10 baseline, ADR-0005). **Same assembly and linear solver**; only "
        "the mesh density strategy changes (geometric layers toward the re-entrant "
        "corner vs uniform structured tet10).",
        "",
        f"_Label: `{raw.get('label')}` · timestamp `{raw.get('timestamp')}`_",
        "",
        "## Headline (equal strain-energy match, 0.01% tol)",
        "",
        "| Path | Tag | Free DOFs | Energy (J) | Total s |",
        "|---|---|---:|---:|---:|",
    ]
    if s:
        lines.append(
            f"| uniform tet10 | `{s.get('baseline_tag')}` | **{s.get('baseline_dofs')}** | "
            f"{_fmt(s.get('baseline_energy_j'))} | **{_fmt(s.get('baseline_total_s'), 4)}** |"
        )
        lines.append(
            f"| graded tet10 | `{s.get('graded_tag')}` | **{s.get('graded_dofs')}** | "
            f"{_fmt(s.get('graded_energy_j'))} | **{_fmt(s.get('graded_total_s'), 4)}** |"
        )
        dof_r = s.get("dof_ratio_uniform_over_graded")
        time_r = s.get("time_ratio_uniform_over_graded")
        lines.extend(
            [
                "",
                "## Ratios (uniform / graded)",
                "",
                "| Metric | Value | Target | Met? |",
                "|---|---:|---:|---|",
                f"| **DOF ratio** | **{_fmt(dof_r, 4)}×** | ≥{s.get('tier3_dof_target')}× | "
                f"{_fmt(s.get('meets_dof_target'))} |",
                f"| **Wall-time ratio** | **{_fmt(time_r, 4)}×** | "
                f"≥{s.get('tier3_time_target')}× | {_fmt(s.get('meets_time_target'))} |",
                "",
                f"- **Claim mode:** `{s.get('claim')}`",
                f"- **Energy ref (max over suite):** {_fmt(s.get('energy_ref_j'))} J",
                "- Galerkin energy from below; match requires "
                r"\(E_{\mathrm{graded}} \ge E_{\mathrm{uniform}}(1-10^{-4})\).",
                "",
            ]
        )
        meets = bool(s.get("meets_dof_target")) and bool(s.get("meets_time_target"))
        if meets:
            lines.append(
                "**Status:** Tier-3 targets **met** on this L-domain instrument. "
                "Product-mesh (STL graded + ZZ adapt) on the full public geometry suite "
                "remains future work."
            )
        else:
            lines.append(
                "**Status:** Tier-3 **instrumented** — harness + measured ratios shipped; "
                "full SPEC holdout suite still open if targets not both met."
            )
    lines.extend(
        [
            "",
            "## Reproduce",
            "",
            "```sh",
            "cmake -S . -B build -G Ninja -DPOLYMESH_WITH_GUI=OFF",
            "cmake --build build --target polymesh-d6-tier3 -j",
            "python3 bench/d6/run_tier3.py --full --render",
            "```",
            "",
            "Artifacts: `bench/d6/out/polymesh-d6-l-domain-raw.json` (full suite), "
            "`bench/results/polymesh-d6-l-domain.json` (scoreboard rows).",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="D6 Tier-3: uniform tet10 vs graded tet10 L-domain bench"
    )
    ap.add_argument("--quick", action="store_true", help="small grids (default if neither flag)")
    ap.add_argument("--full", action="store_true", help="fuller grid suite (slower)")
    ap.add_argument("--label", default="d6-tier3", help="scoreboard label")
    ap.add_argument(
        "--no-build",
        action="store_true",
        help="do not auto-configure/build if binary missing",
    )
    ap.add_argument(
        "--render",
        action="store_true",
        help="run bench/competitive/render_scoreboard.py after write",
    )
    ap.add_argument(
        "--write-only",
        type=Path,
        default=None,
        help="skip run; split an existing raw JSON into scoreboard rows",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="print paths and exit 0 without running the binary",
    )
    args = ap.parse_args()

    RESULTS.mkdir(parents=True, exist_ok=True)
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    raw_path = RAW_DIR / RAW_NAME
    flat_path = RESULTS / FLAT_NAME

    if args.dry_run:
        print(f"raw={raw_path}")
        print(f"flat={flat_path}")
        print(f"summary_md={SUMMARY_MD}")
        return 0

    if args.write_only is not None:
        raw = json.loads(args.write_only.read_text(encoding="utf-8"))
    else:
        quick = not args.full
        if args.no_build:
            binary = find_binary()
        else:
            binary = ensure_built(quick)
        cmd = [str(binary), "-o", str(raw_path), "--label", args.label]
        if quick:
            cmd.append("--quick")
        print("d6: running", " ".join(cmd), file=sys.stderr)
        subprocess.check_call(cmd, cwd=str(ROOT))
        raw = json.loads(raw_path.read_text(encoding="utf-8"))

    flat = split_for_scoreboard(raw)
    flat_path.write_text(json.dumps(flat, indent=2) + "\n", encoding="utf-8")
    write_summary_md(raw, SUMMARY_MD)
    print(f"d6: wrote {raw_path}", file=sys.stderr)
    print(f"d6: wrote {flat_path} ({len(flat)} rows)", file=sys.stderr)
    print(f"d6: wrote {SUMMARY_MD}", file=sys.stderr)

    s = raw.get("summary") or {}
    if s:
        print(
            f"d6: dof_ratio={s.get('dof_ratio_uniform_over_graded')} "
            f"time_ratio={s.get('time_ratio_uniform_over_graded')} "
            f"meets_dof={s.get('meets_dof_target')} "
            f"meets_time={s.get('meets_time_target')}",
            file=sys.stderr,
        )

    if args.render:
        render = ROOT / "bench" / "competitive" / "render_scoreboard.py"
        subprocess.check_call([sys.executable, str(render)], cwd=str(ROOT))
        print("d6: refreshed docs/bench/scoreboard.md", file=sys.stderr)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001 — CLI boundary
        print(f"d6 error: {exc}", file=sys.stderr)
        raise SystemExit(1)
