#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Cross-validate CalculiX and PolyMesh on identical structured hex cantilevers.

CalculiX provenance: 2.23 Windows binaries from
http://www.dhondt.de/calculix_2.23_4win.zip. Every rung uses C3D8/hex8 cells
with identical coordinates and connectivity. Both solvers receive the same
uniform load-face traction through consistent nodal loads with resultant
(0, 0, -1000) N; the independent assemblers are not asserted bitwise identical.
The shared tip-deflection truth is bench/reference/cantilever.json.
"""
from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker

ROOT = Path(__file__).resolve().parents[3]
CCX_VERSION = "2.23"
CCX_SOURCE_URL = "http://www.dhondt.de/calculix_2.23_4win.zip"
CCX_DEFAULT = ROOT / "tools" / "calculix" / "calculix_2.23_4win" / "ccx_static.exe"
POLYMESH_VERSION = "92455f9"
POLYMESH_DEFAULT = ROOT / "build" / "apps" / "cli" / "polymesh.exe"
REFERENCE_PATH = ROOT / "bench" / "reference" / "cantilever.json"
SCHEMA_PATH = ROOT / "bench" / "competitive" / "schema.json"
OUTPUT_PATH = ROOT / "bench" / "results" / "calculix-cantilever.json"
REFINEMENTS = ((4, 1, 1), (8, 2, 2), (16, 4, 4), (32, 8, 8))
LENGTH = 1.0
WIDTH = 0.1
HEIGHT = 0.1
YOUNGS_MODULUS = 200e9
POISSON_RATIO = 0.3
TOTAL_FORCE_Z = -1000.0


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def resolve_executable(default: Path, path_name: str) -> str | None:
    if default.is_file():
        return str(default.resolve())
    return shutil.which(path_name)


def tip_reference() -> float:
    reference = load_json(REFERENCE_PATH)
    metric = next(
        (item for item in reference["metrics"] if item.get("name") == "tip_deflection"),
        None,
    )
    if metric is None:
        raise RuntimeError(f"{REFERENCE_PATH}: missing tip_deflection reference")
    return float(metric["value"])


def node_id(i: int, j: int, k: int, nx: int, ny: int) -> int:
    return 1 + i + (nx + 1) * (j + (ny + 1) * k)


def structured_hex_mesh(
    nx: int, ny: int, nz: int
) -> tuple[list[tuple[int, float, float, float]], list[tuple[int, ...]], list[int], dict[int, float]]:
    """Create the common hex8 mesh and consistent load-face CLOAD weights."""
    nodes = []
    for k in range(nz + 1):
        for j in range(ny + 1):
            for i in range(nx + 1):
                nodes.append(
                    (
                        node_id(i, j, k, nx, ny),
                        LENGTH * i / nx,
                        WIDTH * j / ny,
                        HEIGHT * k / nz,
                    )
                )

    cells = []
    for k in range(nz):
        for j in range(ny):
            for i in range(nx):
                cells.append(
                    (
                        node_id(i, j, k, nx, ny),
                        node_id(i + 1, j, k, nx, ny),
                        node_id(i + 1, j + 1, k, nx, ny),
                        node_id(i, j + 1, k, nx, ny),
                        node_id(i, j, k + 1, nx, ny),
                        node_id(i + 1, j, k + 1, nx, ny),
                        node_id(i + 1, j + 1, k + 1, nx, ny),
                        node_id(i, j + 1, k + 1, nx, ny),
                    )
                )

    tip_nodes = [node_id(nx, j, k, nx, ny) for k in range(nz + 1) for j in range(ny + 1)]
    nodal_loads: defaultdict[int, float] = defaultdict(float)
    per_face_node_load = TOTAL_FORCE_Z / (4.0 * ny * nz)
    for k in range(nz):
        for j in range(ny):
            face = (
                node_id(nx, j, k, nx, ny),
                node_id(nx, j + 1, k, nx, ny),
                node_id(nx, j + 1, k + 1, nx, ny),
                node_id(nx, j, k + 1, nx, ny),
            )
            for node in face:
                nodal_loads[node] += per_face_node_load
    if not math.isclose(sum(nodal_loads.values()), TOTAL_FORCE_Z, rel_tol=0.0, abs_tol=1e-12):
        raise RuntimeError("structured load weights do not conserve the requested resultant")
    return nodes, cells, tip_nodes, dict(nodal_loads)


def input_id_lines(ids: list[int], width: int = 16) -> list[str]:
    """Keep CalculiX data records below its 16-entry parser limit."""
    return [", ".join(map(str, ids[index : index + width])) for index in range(0, len(ids), width)]


def write_ccx_deck(
    path: Path,
    nodes: list[tuple[int, float, float, float]],
    cells: list[tuple[int, ...]],
    tip_nodes: list[int],
    nodal_loads: dict[int, float],
) -> int:
    lines = ["*HEADING", "PolyMesh peer: structured hex cantilever", "*NODE"]
    lines.extend(f"{node}, {x:.17g}, {y:.17g}, {z:.17g}" for node, x, y, z in nodes)
    lines.append("*ELEMENT, TYPE=C3D8, ELSET=Eall")
    lines.extend(f"{element}, " + ", ".join(map(str, cell)) for element, cell in enumerate(cells, 1))
    lines.append("*NSET, NSET=TIP")
    lines.extend(input_id_lines(tip_nodes))
    lines += [
        "*MATERIAL, NAME=Steel",
        "*ELASTIC",
        f"{YOUNGS_MODULUS:.17g}, {POISSON_RATIO:.17g}",
        "*SOLID SECTION, ELSET=Eall, MATERIAL=Steel",
        "*BOUNDARY",
    ]
    fixed_nodes = [node for node, x, _, _ in nodes if x == 0.0]
    lines.extend(f"{node}, 1, 3" for node in fixed_nodes)
    lines += ["*STEP", "*STATIC", "*CLOAD"]
    lines.extend(f"{node}, 3, {load:.17g}" for node, load in sorted(nodal_loads.items()))
    lines += ["*NODE PRINT, NSET=TIP", "U", "*NODE FILE", "U", "*END STEP"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(fixed_nodes)


def write_msh(
    path: Path, nodes: list[tuple[int, float, float, float]], cells: list[tuple[int, ...]]
) -> None:
    lines = ["$MeshFormat", "2.2 0 8", "$EndMeshFormat", "$Nodes", str(len(nodes))]
    lines.extend(f"{node} {x:.17g} {y:.17g} {z:.17g}" for node, x, y, z in nodes)
    lines += ["$EndNodes", "$Elements", str(len(cells))]
    lines.extend(f"{element} 5 0 " + " ".join(map(str, cell)) for element, cell in enumerate(cells, 1))
    lines += ["$EndElements"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_ccx_tip_displacement(dat_path: Path, tip_nodes: list[int]) -> float:
    if not dat_path.is_file():
        raise RuntimeError(f"ccx did not write {dat_path.name}")
    values: dict[int, float] = {}
    reading_displacements = False
    for line in dat_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "displacements" in line.lower():
            reading_displacements = True
            continue
        if not reading_displacements:
            continue
        fields = line.split()
        if len(fields) < 4 or not fields[0].isdigit():
            continue
        node = int(fields[0])
        if node in tip_nodes:
            try:
                values[node] = float(fields[3].replace("D", "E").replace("d", "e"))
            except ValueError as exc:
                raise RuntimeError(f"cannot parse ccx U3 line {line!r}") from exc
    missing = sorted(set(tip_nodes).difference(values))
    if missing or not all(math.isfinite(value) for value in values.values()):
        raise RuntimeError(f"ambiguous ccx TIP displacement; missing or invalid nodes {missing}")
    return max(abs(values[node]) for node in tip_nodes)


def parse_polymesh_tip_displacement(vtu_path: Path, expected_tip_nodes: int) -> float:
    root = ET.parse(vtu_path).getroot()
    points_array = root.find(".//Points/DataArray")
    displacement_array = next(
        (array for array in root.findall(".//PointData/DataArray") if array.get("Name") == "displacement"),
        None,
    )
    if points_array is None or not points_array.text or displacement_array is None or not displacement_array.text:
        raise RuntimeError(f"{vtu_path}: missing points or displacement data")
    points = [float(value) for value in points_array.text.split()]
    displacement = [float(value) for value in displacement_array.text.split()]
    if len(points) % 3 or len(displacement) != len(points):
        raise RuntimeError(f"{vtu_path}: malformed point/displacement array")
    tip_u3 = [
        abs(displacement[index + 2])
        for index in range(0, len(points), 3)
        if math.isclose(points[index], LENGTH, rel_tol=0.0, abs_tol=1e-12)
    ]
    if len(tip_u3) != expected_tip_nodes or not all(math.isfinite(value) for value in tip_u3):
        raise RuntimeError(f"{vtu_path}: ambiguous TIP U3 probe ({len(tip_u3)} nodes)")
    return max(tip_u3)


def run_checked(command: list[str], cwd: Path, env: dict[str, str], name: str) -> tuple[float, subprocess.CompletedProcess[str]]:
    start = time.perf_counter()
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    wall = time.perf_counter() - start
    if result.returncode != 0:
        raise RuntimeError(
            f"{name} failed with exit {result.returncode}:\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return wall, result


def result_row(
    solver: str,
    version: str,
    refinement: tuple[int, int, int],
    dofs: int,
    wall: float,
    tip: float,
    reference: float,
    timestamp: str,
) -> dict:
    nx, ny, nz = refinement
    return {
        "schema_version": 1,
        "solver": solver,
        "version": version,
        "case_id": "cantilever",
        "dofs": dofs,
        "wall_time_s": {"mesh": None, "solve": wall, "total": wall},
        "accuracy": {
            "name": "tip_deflection_rel_err",
            "value": abs(tip - reference) / abs(reference),
            "unit": "ratio",
        },
        "label": f"{solver}-cantilever-hex-{nx}x{ny}x{nz}",
        "timestamp": timestamp,
        "notes": (
            f"structured hex8 {nx}x{ny}x{nz}; max_abs_tip_u3={tip:.12g}; "
            f"tip_deflection_reference={reference:.12g}; matched uniform load-face traction "
            f"with resultant Fz={TOTAL_FORCE_Z:.12g} N; CalculiX uses equivalent CLOAD "
            "weights and PolyMesh uses its consistent face-load assembler (not asserted bitwise identical)"
        ),
    }


def validate_rows(rows: list[dict]) -> None:
    validator = Draft202012Validator(load_json(SCHEMA_PATH), format_checker=FormatChecker())
    for row in rows:
        validator.validate(row)


def main() -> int:
    ccx = resolve_executable(CCX_DEFAULT, "ccx")
    polymesh = resolve_executable(POLYMESH_DEFAULT, "polymesh")
    if not ccx or not polymesh:
        missing = ", ".join(name for name, value in (("ccx", ccx), ("polymesh", polymesh)) if not value)
        print(f"missing {missing}; no comparison rows written", file=sys.stderr)
        return 1

    reference = tip_reference()
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = "1"
    rows: list[dict] = []
    table = []
    with tempfile.TemporaryDirectory() as td:
        work = Path(td)
        for refinement in REFINEMENTS:
            nx, ny, nz = refinement
            stem = f"cantilever-{nx}x{ny}x{nz}"
            nodes, cells, tip_nodes, nodal_loads = structured_hex_mesh(nx, ny, nz)
            fixed_nodes = write_ccx_deck(work / f"{stem}.inp", nodes, cells, tip_nodes, nodal_loads)
            write_msh(work / f"{stem}.msh", nodes, cells)
            dofs = 3 * (len(nodes) - fixed_nodes)

            ccx_command = [ccx, stem]
            ccx_wall, ccx_result = run_checked(ccx_command, work, env, f"CalculiX {stem}")
            ccx_tip = parse_ccx_tip_displacement(work / f"{stem}.dat", tip_nodes)

            vtu_path = work / f"{stem}.vtu"
            polymesh_command = [
                polymesh, "solve", str(work / f"{stem}.msh"), "-o", str(vtu_path),
                "-E", str(YOUNGS_MODULUS), "-nu", str(POISSON_RATIO),
                "--fix-box", "-1e-12", "0", "0", "1e-12", str(WIDTH), str(HEIGHT),
                "--load-box", str(LENGTH - 1e-12), "0", "0", str(LENGTH + 1e-12), str(WIDTH), str(HEIGHT),
                "--load-dir", "0", "0", "-1", "--force", str(abs(TOTAL_FORCE_Z)),
            ]
            polymesh_wall, polymesh_result = run_checked(polymesh_command, work, env, f"PolyMesh {stem}")
            polymesh_tip = parse_polymesh_tip_displacement(vtu_path, len(tip_nodes))
            timestamp = datetime.now(timezone.utc).isoformat()
            rows += [
                result_row("calculix", CCX_VERSION, refinement, dofs, ccx_wall, ccx_tip, reference, timestamp),
                result_row("polymesh-native", POLYMESH_VERSION, refinement, dofs, polymesh_wall, polymesh_tip, reference, timestamp),
            ]
            table.append((refinement, dofs, ccx_tip, polymesh_tip, abs(ccx_tip - polymesh_tip) / ccx_tip * 100.0))
            print(f"ccx command: {' '.join(ccx_command)} (exit {ccx_result.returncode})")
            print(f"polymesh command: {' '.join(polymesh_command)} (exit {polymesh_result.returncode})")

    validate_rows(rows)
    OUTPUT_PATH.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print("refinement | DOF | ccx |U3| | polymesh |U3| | difference (%) | ccx/ref | polymesh/ref")
    for refinement, dofs, ccx_tip, polymesh_tip, difference in table:
        print(
            f"{refinement[0]}x{refinement[1]}x{refinement[2]} | {dofs} | {ccx_tip:.12g} | "
            f"{polymesh_tip:.12g} | {difference:.6g} | "
            f"{abs(ccx_tip-reference)/reference:.6g} | {abs(polymesh_tip-reference)/reference:.6g}"
        )
    print("wrote", OUTPUT_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
