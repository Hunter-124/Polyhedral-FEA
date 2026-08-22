#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Hardware-portable advisor cost labels.

The label contract is deliberately independent of wall time. Direct rows carry
symbolic factor work measured from the exact reduced sparsity pattern. CG rows
carry iteration count times the per-iteration pattern work. Mesh cost is the
measured mesh time divided by the committed reference-mesh calibration for the
row's host; this keeps the target dimensionless while preserving host provenance.
"""

from __future__ import annotations

import argparse
import json
import math
from functools import lru_cache
from pathlib import Path
from typing import Mapping

ROOT = Path(__file__).resolve().parents[2]
HOSTS_DIR = ROOT / "bench" / "advisor" / "hosts"


def finite_float(value: object) -> float | None:
    try:
        parsed = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


@lru_cache(maxsize=None)
def host_calibration(host: str) -> dict[str, object] | None:
    safe_host = Path(host).name
    if not safe_host or safe_host != host:
        return None
    path = HOSTS_DIR / f"{safe_host}.json"
    if not path.is_file():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    ref_mesh_ms = finite_float(payload.get("ref_mesh_ms"))
    if ref_mesh_ms is None or ref_mesh_ms <= 0.0:
        return None
    return payload


def solve_flops(row: Mapping[str, object]) -> float | None:
    explicit = finite_float(row.get("solve_flops"))
    if explicit is not None and explicit >= 0.0:
        return explicit
    iterations = finite_float(row.get("cg_iters"))
    per_iteration = finite_float(row.get("cg_flops_per_iter"))
    if iterations is None or iterations < 0.0 or per_iteration is None or per_iteration < 0.0:
        return None
    return iterations * per_iteration


def solve_bytes(row: Mapping[str, object]) -> float | None:
    explicit = finite_float(row.get("solve_bytes"))
    if explicit is not None and explicit >= 0.0:
        return explicit
    iterations = finite_float(row.get("cg_iters"))
    per_iteration = finite_float(row.get("cg_bytes_per_iter"))
    if iterations is None or iterations < 0.0 or per_iteration is None or per_iteration < 0.0:
        return None
    return iterations * per_iteration


def mesh_work(row: Mapping[str, object]) -> float | None:
    elapsed_ms = finite_float(row.get("mesh_ms"))
    host = str(row.get("host", "") or "").strip()
    calibration = host_calibration(host)
    if elapsed_ms is None or elapsed_ms < 0.0 or calibration is None:
        return None
    reference_ms = finite_float(calibration.get("ref_mesh_ms"))
    if reference_ms is None or reference_ms <= 0.0:
        return None
    return elapsed_ms / reference_ms


def portable_cost_label(row: Mapping[str, object], head: str) -> float | None:
    if head == "solve_flops":
        return solve_flops(row)
    if head == "solve_bytes":
        return solve_bytes(row)
    if head == "mesh_work":
        return mesh_work(row)
    raise KeyError(f"unknown portable cost head: {head}")


def self_test() -> None:
    direct = {"solve_flops": "120", "solve_bytes": "80"}
    assert solve_flops(direct) == 120.0
    assert solve_bytes(direct) == 80.0

    cg = {"cg_iters": "7", "cg_flops_per_iter": "11", "cg_bytes_per_iter": "13"}
    assert solve_flops(cg) == 77.0
    assert solve_bytes(cg) == 91.0

    # Explicit measured totals remain authoritative when per-iteration fields
    # are also present on a full-solve v4 row.
    measured_cg = dict(cg, solve_flops="79", solve_bytes="97")
    assert solve_flops(measured_cg) == 79.0
    assert solve_bytes(measured_cg) == 97.0

    legacy = {"schema": "advisor-row-v3", "host": "unknown"}
    assert solve_flops(legacy) is None
    assert solve_bytes(legacy) is None
    assert mesh_work(legacy) is None

    calibration = host_calibration("hunter-pc")
    assert calibration is not None
    reference_ms = finite_float(calibration["ref_mesh_ms"])
    assert reference_ms is not None
    assert math.isclose(mesh_work({"host": "hunter-pc", "mesh_ms": reference_ms}), 1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not args.self_test:
        parser.error("choose --self-test")
    self_test()
    print("cost label self-test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
