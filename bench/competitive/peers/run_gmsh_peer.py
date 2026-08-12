#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare native PolyMesh meshing with Gmsh meshes on analytic corpus cases.

Peer mesher: Gmsh 4.13.1
Source: https://gmsh.info/bin/Windows/gmsh-4.13.1-Windows64.zip
Zip SHA-256: 2a98173869aa2bab77b19be361cfa371f6e4e826535e6386768c1b34ba30cd2e

With no filters, runs three variants at every analytic-case x h_rel x order:
Gmsh mesh + PolyMesh solver, native default mesher, and native feature-graded
tet mesher. Order 1 is the primary comparison. Order 2 is secondary: Gmsh
tet10 versus native --p-elevate. Native p-elevation promotes smooth-marked
elements rather than the complete mesh uniformly, so the order-2 pairing is
approximate and must not be presented as exact order parity. Gmsh order-2
meshes try Mesh.HighOrderOptimize=2, the standard elastic+optimization pass,
then fall back to mode 1 if mesh generation or the solve rejects the result:
without optimisation, valid linear shaft meshes curved into inverted tet10
elements. Use --case, --h-rel, --order, and --variant for a bounded trial.
Every solve uses PolyMesh's solver and the same VTU point-data probe.
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker

ROOT = Path(__file__).resolve().parents[3]
GMSH_VERSION = "4.13.1"
POLYMESH_VERSION = "08f9f55"
H_REL_LADDER = (0.20, 0.12, 0.08)
ORDER_LADDER = (1, 2)
VARIANTS = ("gmsh", "native", "graded")
SOLVE_TIMEOUT_S = 600
GMSH_HIGH_ORDER_OPTIMIZE = 2
SOLVER_BY_VARIANT = {
    "gmsh": "gmsh-mesh+polymesh-solver",
    "native": "polymesh-native",
    "graded": "polymesh-native-graded",
}
GMSH_DEFAULT = ROOT / "tools" / "gmsh" / "gmsh-4.13.1-Windows64" / "gmsh.exe"
POLYMESH_DEFAULT = ROOT / "build" / "apps" / "cli" / "polymesh.exe"
OUTPUT_PATH = ROOT / "bench" / "results" / "gmsh-peer.json"
SCHEMA_PATH = ROOT / "bench" / "competitive" / "schema.json"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def analytic_cases() -> list[str]:
    """Return the in-scope analytic references, derived from corpus metadata."""
    cases = []
    for path in sorted((ROOT / "bench" / "reference" / "corpus").glob("*.json")):
        reference = load_json(path)
        if reference.get("truth_source") != "analytic":
            continue
        family = reference.get("family")
        if family == "box_hole" and path.stem.endswith("_c0"):
            cases.append(path.stem)
        elif family == "stepped_shaft":
            cases.append(path.stem)
    return cases


def resolve_executable(value: str | None, default: Path, name: str) -> str | None:
    candidate = Path(value).expanduser() if value else default
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    if candidate.is_file():
        return str(candidate.resolve())
    found = shutil.which(value or name)
    return found


def run_checked(
    command: list[str], what: str
) -> tuple[float, subprocess.CompletedProcess[str] | None, str | None]:
    t0 = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=SOLVE_TIMEOUT_S,
        )
    except subprocess.TimeoutExpired as error:
        wall = time.perf_counter() - t0
        for output, stream in ((error.stdout, sys.stdout), (error.stderr, sys.stderr)):
            if output:
                text = (
                    output.decode("utf-8", errors="replace")
                    if isinstance(output, bytes)
                    else output
                )
                print(text, file=stream)
        reason = f"timed out after {SOLVE_TIMEOUT_S} s (no result)"
        print(f"{what}: {reason}", file=sys.stderr)
        return wall, None, reason
    wall = time.perf_counter() - t0
    if result.returncode != 0:
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        reason = f"failed with exit {result.returncode} (no result)"
        print(f"{what}: {reason}", file=sys.stderr)
        return wall, None, reason
    return wall, result, None


def projection_summary(result: subprocess.CompletedProcess[str]) -> str | None:
    for stream in (result.stdout, result.stderr):
        for line in stream.splitlines():
            if "mids projected=" in line:
                return line.strip()
    return None


def metric_for(reference: dict) -> dict:
    wanted = "scf" if reference.get("family") == "box_hole" else "tip_deflection"
    for metric in reference.get("metrics", []):
        if metric.get("name") == wanted:
            return metric
    raise RuntimeError(f"{reference.get('part', 'case')}: missing {wanted} analytic metric")


def bbox_diagonal(reference: dict, metric: dict) -> float:
    inputs = metric.get("inputs", {})
    family = reference.get("family")
    if family == "box_hole":
        extents = (
            2.0 * float(inputs["half_width"]),
            2.0 * float(inputs["half_height"]),
            float(inputs["thickness"]),
        )
    elif family == "stepped_shaft":
        diameter = 2.0 * float(inputs["r_root"])
        extents = (float(inputs["length"]), diameter, diameter)
    else:
        raise RuntimeError(f"unsupported analytic family: {family}")
    return math.sqrt(sum(value * value for value in extents))




def flatten_box(box: list[list[float]]) -> list[str]:
    if len(box) != 2 or any(len(corner) != 3 for corner in box):
        raise RuntimeError(f"invalid selection box: {box!r}")
    return [format(float(value), ".17g") for corner in box for value in corner]


def point_in_box(point: tuple[float, float, float], box: list[list[float]]) -> bool:
    return all(float(box[0][axis]) <= point[axis] <= float(box[1][axis]) for axis in range(3))


def parse_vtu(path: Path) -> tuple[list[tuple[float, float, float]], dict[str, list[float]]]:
    root = ET.parse(path).getroot()
    points_array = root.find(".//Points/DataArray")
    if points_array is None or not points_array.text:
        raise RuntimeError(f"{path}: missing Points DataArray")
    values = [float(value) for value in points_array.text.split()]
    if len(values) % 3 != 0:
        raise RuntimeError(f"{path}: Points array length is not divisible by 3")
    points = [tuple(values[i : i + 3]) for i in range(0, len(values), 3)]

    point_data = {}
    for array in root.findall(".//PointData/DataArray"):
        name = array.get("Name")
        if name and array.text:
            point_data[name] = [float(value) for value in array.text.split()]
    return points, point_data


def probe_vtu(
    path: Path, metric: dict, load_box: list[list[float]], fix_box: list[list[float]]
) -> tuple[float | None, int, int, int]:
    points, arrays = parse_vtu(path)
    fixed_nodes = sum(point_in_box(point, fix_box) for point in points)
    active_dofs = 3 * (len(points) - fixed_nodes)
    kind = metric["probe"]["kind"]
    if kind == "peak_vm_over_nominal":
        probe_box = metric["probe"]["select"]["box"]
        von_mises = arrays.get("von_Mises")
        if von_mises is None or len(von_mises) != len(points):
            raise RuntimeError(f"{path}: missing or malformed von_Mises point data")
        selected = [
            von_mises[index]
            for index, point in enumerate(points)
            if point_in_box(point, probe_box)
        ]
        value = (
            max(selected) / float(metric["probe"]["nominal"]) if selected else None
        )
    elif kind == "tip_deflection":
        displacement = arrays.get("displacement")
        if displacement is None or len(displacement) != 3 * len(points):
            raise RuntimeError(f"{path}: missing or malformed displacement point data")
        selected = []
        for index, point in enumerate(points):
            if point_in_box(point, load_box):
                ux, uy, uz = displacement[3 * index : 3 * index + 3]
                selected.append(math.sqrt(ux * ux + uy * uy + uz * uz))
        value = sum(selected) / len(selected) if selected else None
    else:
        raise RuntimeError(f"unsupported analytic probe kind: {kind}")
    return value, active_dofs, len(points), len(selected)


def load_arguments(case: dict, metric: dict) -> list[str]:
    if len(case.get("loads", [])) != 1:
        raise RuntimeError(f"{case.get('part', 'case')}: expected exactly one load")
    load = case["loads"][0]
    traction = [float(value) for value in load["traction"]]
    magnitude = math.sqrt(sum(value * value for value in traction))
    if not math.isfinite(magnitude) or magnitude <= 0.0:
        raise RuntimeError(f"{case.get('part', 'case')}: traction must be finite and nonzero")
    direction = [value / magnitude for value in traction]
    resultant = metric.get("inputs", {}).get("resultant_load_N")
    if resultant is None:
        area = load.get("select", {}).get("expected_area")
        resultant = magnitude * float(area) if area is not None else None
    if resultant is None or not math.isfinite(float(resultant)) or float(resultant) <= 0.0:
        raise RuntimeError(
            f"{case.get('part', 'case')}: analytic resultant load is missing or invalid"
        )
    return [
        "--load-dir",
        *(format(value, ".17g") for value in direction),
        "--force",
        format(float(resultant), ".17g"),
    ]


def solve_command(
    polymesh: str,
    source: Path,
    output: Path,
    case: dict,
    metric: dict,
    mesh_size: float,
    order: int,
    native_mesher: str | None,
) -> list[str]:
    if len(case.get("bcs", [])) != 1 or len(case.get("loads", [])) != 1:
        raise RuntimeError(f"{case.get('part', 'case')}: expected one BC and one load")
    material = case["material"]
    command = [
        polymesh,
        "solve",
        str(source),
        "-h",
        format(mesh_size, ".17g"),
        "-E",
        format(float(material["E"]), ".17g"),
        "-nu",
        format(float(material["nu"]), ".17g"),
        "--fix-box",
        *flatten_box(case["bcs"][0]["select"]["box"]),
        "--load-box",
        *flatten_box(case["loads"][0]["select"]["box"]),
        *load_arguments(case, metric),
        "-o",
        str(output),
    ]
    if native_mesher == "graded":
        command.extend(["--mesher", "graded"])
    elif native_mesher not in (None, "default"):
        raise RuntimeError(f"unsupported native mesher: {native_mesher}")
    if native_mesher is not None and order == 2:
        command.append("--p-elevate")
    return command


def write_geo(
    path: Path,
    geometry: Path,
    mesh_size: float,
    order: int,
    high_order_optimize: int | None,
) -> None:
    merge_path = geometry.resolve().as_posix().replace('"', '\\"')
    high_order_setting = (
        f"Mesh.HighOrderOptimize = {high_order_optimize};\n"
        if order == 2 and high_order_optimize is not None
        else ""
    )
    path.write_text(
        f'Merge "{merge_path}";\n'
        f"Mesh.ElementOrder = {order};\n"
        f"{high_order_setting}"
        f"Mesh.MeshSizeMax = {mesh_size:.17g};\n"
        f"Mesh.MeshSizeMin = {0.5 * mesh_size:.17g};\n",
        encoding="utf-8",
    )


def result_row(
    solver: str,
    version: str,
    case_id: str,
    h_rel: float,
    order: int,
    metric: dict,
    value: float | None,
    dofs: int,
    n_nodes: int,
    probe_nodes: int,
    mesh_wall: float | None,
    solve_wall: float | None,
    total_wall: float,
    timestamp: str,
) -> dict:
    truth = float(metric["value"])
    rel_err = abs(value - truth) / abs(truth) if value is not None else None
    probe_note = f"probe={format(value, '.12g') if value is not None else 'null'}"
    if metric["probe"]["kind"] == "peak_vm_over_nominal" and value is not None:
        nominal = float(metric["probe"]["nominal"])
        probe_note += (
            f"; raw_peak_von_mises_pa={value * nominal:.12g}; nominal_pa={nominal:.12g}"
        )
    return {
        "schema_version": 1,
        "solver": solver,
        "version": version,
        "case_id": case_id,
        "order": order,
        "h_rel": h_rel,
        "probe_nodes": probe_nodes,
        "dofs": dofs,
        "wall_time_s": {"mesh": mesh_wall, "solve": solve_wall, "total": total_wall},
        "accuracy": {
            "name": f"{metric['name']}_rel_err",
            "value": rel_err,
            "unit": "ratio",
        },
        "label": f"gmsh-peer-h{h_rel:.2f}-p{order}",
        "timestamp": timestamp,
        "notes": (
            f"h_rel={h_rel:.2f}; order={order}; load=fixed resultant; "
            f"{probe_note}; truth={truth:.12g}; "
            f"probe_nodes={probe_nodes}; nodes={n_nodes}; identical VTU nodal probe for every "
            f"mesh variant"
            + ("; probe selection contained no nodes" if value is None else "")
        ),
    }


def failed_result_row(
    solver: str,
    version: str,
    case_id: str,
    h_rel: float,
    order: int,
    metric: dict,
    mesh_wall: float | None,
    solve_wall: float | None,
    total_wall: float,
    timestamp: str,
    stage: str,
    reason: str,
) -> dict:
    status = "timeout" if reason.startswith("timed out") else "failed"
    return {
        "schema_version": 1,
        "solver": solver,
        "version": version,
        "case_id": case_id,
        "order": order,
        "h_rel": h_rel,
        "probe_nodes": None,
        "dofs": None,
        "wall_time_s": {
            "mesh": mesh_wall,
            "solve": solve_wall,
            "total": total_wall,
        },
        "accuracy": {
            "name": f"{metric['name']}_rel_err",
            "value": None,
            "unit": "ratio",
        },
        "label": f"gmsh-peer-h{h_rel:.2f}-p{order}",
        "timestamp": timestamp,
        "status": status,
        "notes": (
            f"h_rel={h_rel:.2f}; order={order}; load=fixed resultant; "
            f"{reason}; stage={stage}"
        ),
    }


def run_case(
    gmsh: str,
    polymesh: str,
    case_id: str,
    h_rel: float,
    order: int,
    variants: list[str],
    work: Path,
) -> list[dict]:
    reference = load_json(ROOT / "bench" / "reference" / "corpus" / f"{case_id}.json")
    if reference.get("truth_source") != "analytic":
        raise RuntimeError(f"{case_id}: truth_source is not analytic")
    case_path = (
        ROOT / "bench" / "geometries" / "corpus" / "primitives" / f"{case_id}.case.json"
    )
    case = load_json(case_path)
    metric = metric_for(reference)
    mesh_size = bbox_diagonal(reference, metric) * h_rel
    geometry = ROOT / case["geometry"]
    fix_box = case["bcs"][0]["select"]["box"]
    load_box = case["loads"][0]["select"]["box"]
    stem = f"{case_id}-h{h_rel:.2f}-p{order}"
    timestamp = datetime.now(timezone.utc).isoformat()
    rows = []

    if "gmsh" in variants:
        geo_path = work / f"{stem}.geo"
        msh_path = work / f"{stem}.msh"
        gmsh_vtu = work / f"{stem}-gmsh.vtu"
        gmsh_solver = "gmsh-mesh+polymesh-solver"
        gmsh_version = f"gmsh-{GMSH_VERSION}+polymesh-{POLYMESH_VERSION}"
        optimize_attempts = (
            (GMSH_HIGH_ORDER_OPTIMIZE, 1) if order == 2 else (None,)
        )
        mesh_wall = 0.0
        solve_wall = 0.0
        solve_attempted = False
        solve_result = None
        failure_stage = "Gmsh mesh"
        failure_reason = "failed without an error message"
        selected_optimize = None

        for optimize in optimize_attempts:
            selected_optimize = optimize
            write_geo(geo_path, geometry, mesh_size, order, optimize)
            attempt_mesh_wall, mesh_result, mesh_error = run_checked(
                [
                    gmsh, "-nt", "1", "-3", "-format", "msh2",
                    "-o", str(msh_path), str(geo_path),
                ],
                f"{case_id}: Gmsh",
            )
            mesh_wall += attempt_mesh_wall
            if mesh_result is None:
                failure_stage = "Gmsh mesh"
                failure_reason = mesh_error or "failed without an error message"
                continue

            solve_attempted = True
            attempt_solve_wall, solve_result, solve_error = run_checked(
                solve_command(
                    polymesh, msh_path, gmsh_vtu, case, metric, mesh_size, order, None
                ),
                f"{case_id}: Gmsh-mesh solve",
            )
            solve_wall += attempt_solve_wall
            if solve_result is not None:
                break
            failure_stage = "PolyMesh solve of Gmsh mesh"
            failure_reason = solve_error or "failed without an error message"

        if solve_result is None:
            rows.append(
                failed_result_row(
                    gmsh_solver,
                    gmsh_version,
                    case_id,
                    h_rel,
                    order,
                    metric,
                    mesh_wall,
                    solve_wall if solve_attempted else None,
                    mesh_wall + solve_wall,
                    timestamp,
                    failure_stage,
                    failure_reason,
                )
            )
        else:
            value, dofs, nodes, probe_nodes = probe_vtu(
                gmsh_vtu, metric, load_box, fix_box
            )
            rows.append(
                result_row(
                    gmsh_solver,
                    gmsh_version,
                    case_id,
                    h_rel,
                    order,
                    metric,
                    value,
                    dofs,
                    nodes,
                    probe_nodes,
                    mesh_wall,
                    solve_wall,
                    mesh_wall + solve_wall,
                    timestamp,
                )
            )
        if order == 2:
            attempted = ",".join(
                str(value)
                for value in optimize_attempts
                if value is not None and value >= selected_optimize
            )
            rows[-1]["notes"] += (
                f"; gmsh_high_order_optimize={selected_optimize}"
                f"; gmsh_high_order_optimize_attempts={attempted}"
            )

    for variant, solver, mesher in (
        ("native", "polymesh-native", "default"),
        ("graded", "polymesh-native-graded", "graded"),
    ):
        if variant not in variants:
            continue
        vtu_path = work / f"{stem}-{variant}.vtu"
        total_wall, solve_result, solve_error = run_checked(
            solve_command(
                polymesh, geometry, vtu_path, case, metric, mesh_size, order, mesher
            ),
            f"{case_id}: {variant} solve",
        )
        if solve_result is None:
            rows.append(
                failed_result_row(
                    solver,
                    POLYMESH_VERSION,
                    case_id,
                    h_rel,
                    order,
                    metric,
                    None,
                    None,
                    total_wall,
                    timestamp,
                    f"{variant} mesh + solve",
                    solve_error or "failed without an error message",
                )
            )
            continue
        value, dofs, nodes, probe_nodes = probe_vtu(vtu_path, metric, load_box, fix_box)
        row = result_row(
            solver,
            POLYMESH_VERSION,
            case_id,
            h_rel,
            order,
            metric,
            value,
            dofs,
            nodes,
            probe_nodes,
            None,
            None,
            total_wall,
            timestamp,
        )
        if order == 2:
            summary = projection_summary(solve_result)
            if summary is None:
                summary = "mids projection summary unavailable"
            else:
                print(f"{case_id} h={h_rel:.2f} {variant}: {summary}")
            row["projection"] = summary
            row["notes"] += f"; {summary}"
        rows.append(row)

    for row in rows:
        print(json.dumps(row, sort_keys=True))
    return rows


def validate_rows(rows: list[dict]) -> None:
    schema = load_json(SCHEMA_PATH)
    validator = Draft202012Validator(schema, format_checker=FormatChecker())
    for row in rows:
        validator.validate(row)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", help="run one analytic corpus case")
    parser.add_argument("--h-rel", type=float, help="run one h_rel ladder value")
    parser.add_argument("--order", type=int, choices=ORDER_LADDER, help="run one element order")
    parser.add_argument("--gmsh", help="path to gmsh.exe")
    parser.add_argument("--polymesh", help="path to polymesh.exe")
    parser.add_argument("--variant", choices=VARIANTS, help="run one mesh-source variant")
    parser.add_argument("--resume-from", type=Path, help="reuse completed rows from JSON")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases = analytic_cases()
    if args.case:
        if args.case not in cases:
            print(f"error: --case must be one of {', '.join(cases)}", file=sys.stderr)
            return 2
        cases = [args.case]
    h_values = list(H_REL_LADDER)
    if args.h_rel is not None:
        selected = next((value for value in H_REL_LADDER if math.isclose(args.h_rel, value)), None)
        if selected is None:
            print("error: --h-rel must be one of 0.20, 0.12, 0.08", file=sys.stderr)
            return 2
        h_values = [selected]
    order_values = [args.order] if args.order is not None else list(ORDER_LADDER)

    variants = [args.variant] if args.variant is not None else list(VARIANTS)
    gmsh = resolve_executable(args.gmsh, GMSH_DEFAULT, "gmsh")
    if not gmsh:
        print(f"gmsh not found; skip Gmsh peer (expected {GMSH_DEFAULT})")
        return 0
    polymesh = resolve_executable(args.polymesh, POLYMESH_DEFAULT, "polymesh")
    if not polymesh:
        print(f"error: polymesh CLI not found (expected {POLYMESH_DEFAULT})", file=sys.stderr)
        return 1

    rows = []
    if args.resume_from is not None:
        resumed = load_json(args.resume_from)
        if not isinstance(resumed, list):
            print(f"error: {args.resume_from}: expected a JSON row array", file=sys.stderr)
            return 2
        wanted_solvers = {SOLVER_BY_VARIANT[variant] for variant in variants}
        rows = [
            row for row in resumed
            if row.get("case_id") in cases
            and row.get("solver") in wanted_solvers
            and row.get("order") in order_values
            and any(math.isclose(float(row.get("h_rel", -1.0)), h) for h in h_values)
        ]
        validate_rows(rows)
        print(f"resuming with {len(rows)} completed rows from {args.resume_from}")
    completed = {
        (
            row["case_id"],
            round(float(row["h_rel"]), 8),
            int(row["order"]),
            row["solver"],
        )
        for row in rows
    }
    try:
        with tempfile.TemporaryDirectory(prefix="polymesh-gmsh-peer-") as td:
            work = Path(td)
            for case_id in cases:
                for h_rel in h_values:
                    for order in order_values:
                        pending = [
                            variant for variant in variants
                            if (
                                case_id,
                                round(h_rel, 8),
                                order,
                                SOLVER_BY_VARIANT[variant],
                            )
                            not in completed
                        ]
                        if not pending:
                            continue
                        new_rows = run_case(
                            gmsh, polymesh, case_id, h_rel, order, pending, work
                        )
                        rows.extend(new_rows)
                        completed.update(
                            (
                                row["case_id"],
                                round(float(row["h_rel"]), 8),
                                int(row["order"]),
                                row["solver"],
                            )
                            for row in new_rows
                        )
        if args.variant and args.resume_from is None and OUTPUT_PATH.is_file():
            existing = load_json(OUTPUT_PATH)
            if not isinstance(existing, list):
                raise RuntimeError(f"{OUTPUT_PATH}: expected a JSON row array")
            replacement_keys = {
                (row["solver"], row["case_id"], row["order"], row["label"]) for row in rows
            }
            rows = [
                row
                for row in existing
                if (row["solver"], row["case_id"], row.get("order"), row["label"])
                not in replacement_keys
            ] + rows
        solver_order = {
            "gmsh-mesh+polymesh-solver": 0,
            "polymesh-native": 1,
            "polymesh-native-graded": 2,
        }
        rows.sort(
            key=lambda row: (
                row["case_id"],
                row["label"],
                row["order"],
                solver_order.get(row["solver"], 99),
            )
        )
        validate_rows(rows)
    except (OSError, RuntimeError, KeyError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print("wrote", OUTPUT_PATH)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
