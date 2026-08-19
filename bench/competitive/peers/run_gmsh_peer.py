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
import re
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


def polymesh_version() -> str:
    """The build actually under test, never a hardcoded string."""
    result = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True
    )
    return result.stdout.strip() if result.returncode == 0 else "unknown"


POLYMESH_VERSION = polymesh_version()
H_REL_LADDER = (0.20, 0.12, 0.08)
ORDER_LADDER = (1, 2)
VARIANTS = ("gmsh", "native", "graded", "uniform")
#: Order-2 only; uniform promotion has no meaning at order 1. `native`/`graded`
#: use the SHIPPING selective promotion (adapt::mark_smooth), which leaves a mixed
#: linear/quadratic discretisation. `uniform` drives the CLI's uniform-promotion
#: mode so every eligible element is promoted, giving true order parity with
#: Gmsh's uniformly quadratic mesh. Selective is the product default; uniform
#: exists for the clean scientific comparison, not to flatter the product.
UNIFORM_ORDER_ONLY = 2
#: Pinned spelling, not discovered: `--p-elevate` keeps its selective behaviour
#: and this flag makes the promotion uniform. It does not exist in builds before
#: the mode landed, so main() verifies the binary accepts it before spending the
#: run, rather than letting every uniform row fail on an unknown argument.
UNIFORM_FLAG = "--p-elevate-uniform"
#: Cases beyond the closed-form set. These carry external (Gmsh + CalculiX)
#: references rather than analytic ones, so the peer comparison uses
#: tip_deflection: their references' primary metric is strain_energy, which our
#: CLI's VTU does not carry. That asymmetry is labelled on every row rather than
#: papered over by computing energy two different ways.
PEER_EXTRA_CASES = (
    "tube_s0_c1", "tube_s1_c1", "tube_s2_c1", "tube_s3_c1",
    "perforated_plate_s0_c1", "perforated_plate_s1_c1",
    "perforated_plate_s2_c1", "perforated_plate_s3_c1",
)
#: A probe the VTU can actually supply, per probe kind in the reference.
VTU_PROBE_KINDS = ("peak_vm_over_nominal", "tip_deflection")
#: How this harness measures tip_deflection, which is NOT the definition the
#: references and apps/testlab use (mean |u| over the corner nodes of the
#: selected load faces). The peer matrix keeps ONE probe across all three mesh
#: variants because that comparability is the whole point of the matrix; the
#: definitional offset against the reference probe is measured separately by
#: --probe-offset-check and recorded on every row.
PROBE_DEFINITION = (
    "mean |u| over every VTU node inside the case load box; identical across all "
    "mesh variants. The references and apps/testlab instead average over the "
    "CORNER nodes of the selected load faces, so peer rel_err carries a "
    "definitional offset against campaign accuracy: see probe_offset_note."
)
SOLVE_TIMEOUT_S = 600
#: Output of the most recent failing subprocess, so a refusal can be told from a
#: crash. The engine reports both through exit 1, so the message is the only
#: signal available to a caller.
last_output = ""
GMSH_HIGH_ORDER_OPTIMIZE = 2
SOLVER_BY_VARIANT = {
    "gmsh": "gmsh-mesh+polymesh-solver",
    "native": "polymesh-native",
    "graded": "polymesh-native-graded",
    "uniform": "polymesh-native-uniform-p2",
}
#: Which variants are the product's shipping behaviour, recorded per row so a
#: reader can tell the product from the parity experiment.
PRODUCT_DEFAULT_VARIANTS = ("gmsh", "native", "graded")
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


def build_supports_uniform(polymesh: str) -> bool:
    """Does this binary accept the pinned uniform-promotion flag?

    The SPELLING is pinned (UNIFORM_FLAG); this only checks that the build in
    front of us has the mode, so a run started before the build window refuses up
    front instead of burning the matrix on an unknown argument.

    The --help text is the ONLY signal used, because probing by passing the flag
    is unreliable: the CLI answers an unknown argument with its usage dump, which
    contains none of the words a probe would look for. It does exit 2 and write no
    VTU, so a stale build cannot masquerade as a successful uniform run, but
    checking help up front turns 48 failed rows into one clear message.
    """
    helped = subprocess.run([polymesh, "--help"], capture_output=True, text=True)
    return UNIFORM_FLAG in f"{helped.stdout}\n{helped.stderr}"


def peer_cases() -> list[str]:
    """The matrix case list: the closed-form set plus the declared extra cases.

    An extra case is only included when its reference AND case file both exist,
    so a half-landed family cannot silently shrink the matrix.
    """
    cases = analytic_cases()
    for case_id in PEER_EXTRA_CASES:
        reference = ROOT / "bench" / "reference" / "corpus" / f"{case_id}.json"
        case_path = (
            ROOT / "bench" / "geometries" / "corpus" / "primitives" / f"{case_id}.case.json"
        )
        if reference.is_file() and case_path.is_file():
            cases.append(case_id)
        else:
            print(f"warning: skipping {case_id}: missing reference or case file",
                  file=sys.stderr)
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
        global last_output
        last_output = f"{result.stdout}\n{result.stderr}"
        return wall, None, reason
    return wall, result, None


#: A float that cannot swallow a sentence-ending period. The engine's refusal
#: message ends "... relative error is 0.3149.", and a greedy [0-9.eE+-]+ class
#: captures that trailing dot and fails to parse.
_FLOAT = r"[-+]?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?"
#: The engine's three refusal kinds, each textually distinct by design. Matched on
#: their opening phrase, NOT on shared body text: the feature-unresolved message
#: also contains the volume-error phrase, so keying the volume guard off that
#: phrase would misclassify it.
REFUSAL_PATTERNS = (
    ("feature-unresolved",
     re.compile(rf"feature unresolved at h=({_FLOAT}) m: CAD face (\d+)")),
    ("resolution-refused", re.compile(rf"resolution refused at h=({_FLOAT})")),
    ("geometry-fill-stage-guard", re.compile(r"geometry fill-stage guard failed:")),
)
#: Only present on a resolution refusal that could sample a thickness.
THINNEST_WALL_RE = re.compile(rf"thinnest wall is ({_FLOAT}) m")
#: Error markers that mean "h is too coarse for this geometry" but that reach the
#: caller as a plain error rather than through a refusal guard. Observed on
#: tube_s0_c1 at h_rel 0.20, where the grid fill finds no cell wholly inside the
#: 1.29 mm wall.
RESOLUTION_LIMIT_HINTS = (
    ("grid fill produced no interior cell, i.e. h exceeds the local wall/feature size",
     "no interior cells"),
)
RECOMMENDED_H_RE = re.compile(rf"reduce -h to <= ({_FLOAT}) m")
VOLUME_ERROR_RE = re.compile(rf"mesh/BRep volume relative error is ({_FLOAT})")
#: engine-bugs' dedicated marker line, printed after every promotion line.
MODE_LINE_RE = re.compile(r"p-elevate-mode: (uniform|selective)")
#: The same fact carried inside vol.mesher_note, which travels into any artifact
#: that records the note even when stdout was not captured. Preferred source.
MESHER_NOTE_MODE_RE = re.compile(
    r"p-elevate=(uniform|selective) promoted=(\d+) tet10=(\d+) hex20=(\d+)"
)
PROMOTION_RE = re.compile(
    r"p-elevate: (\d+) smooth, nodes (\d+)\D+(\d+) \(tet10=(\d+) hex20=(\d+)\)"
)
MIDS_RE = re.compile(r"mids projected=(\d+) partial=(\d+) reverted=(\d+)")


def classify_failure(text: str) -> tuple[str, dict | None]:
    """Distinguish an engine REFUSAL from a genuine failure.

    The engine raises GeometryVolumeLimitError for both the feature-unresolved
    guard and the volume-completeness guard, and the CLI turns every exception
    into exit 1, so a refusal cannot be told from a crash by exit code alone --
    it has to be recognised from the message. A refusal is a legitimate,
    informative outcome (the mesher declining a size it cannot represent), so it
    is recorded as its own status rather than as an error.
    """
    for kind, pattern in REFUSAL_PATTERNS:
        match = pattern.search(text)
        if not match:
            continue
        refusal: dict = {"kind": kind, "detected_from": "engine message"}
        if kind == "feature-unresolved":
            refusal["refused_at_h_m"] = float(match.group(1))
            refusal["cad_face"] = int(match.group(2))
        elif kind == "resolution-refused":
            refusal["refused_at_h_m"] = float(match.group(1))
            wall = THINNEST_WALL_RE.search(text)
            if wall:
                refusal["thinnest_wall_m"] = float(wall.group(1))
        recommended = RECOMMENDED_H_RE.search(text)
        if recommended:
            refusal["recommended_h_m"] = float(recommended.group(1))
        # A resolution refusal happens BEFORE a mesh exists, so the volume
        # assessment travels with available=false and any volume field alongside it
        # reads 0.0. That 0.0 means NOT MEASURED, and recording it as a relative
        # error would read as a perfect volume match -- the exact "reports success
        # when it cannot verify" pattern this whole effort has been removing.
        volume = VOLUME_ERROR_RE.search(text)
        if kind == "resolution-refused":
            refusal["mesh_vs_brep_volume_rel_err"] = None
            refusal["volume_measured"] = False
            refusal["volume_note"] = (
                "no mesh exists at a resolution refusal, so the volume assessment is "
                "unavailable; any 0.0 reported alongside it means NOT MEASURED, not a "
                "perfect volume match"
            )
        elif volume:
            refusal["mesh_vs_brep_volume_rel_err"] = float(volume.group(1))
            refusal["volume_measured"] = True
        # Prefer the line the pattern matched: the engine keeps printing after the
        # error, so the last line is often unrelated progress output.
        matched_line = next(
            (line for line in text.splitlines() if pattern.search(line)),
            text.strip().splitlines()[-1] if text.strip() else None,
        )
        refusal["message"] = matched_line[:400] if matched_line else None
        return "refused", refusal
    # Messages that indicate a RESOLUTION LIMIT but do not come through either
    # documented guard. These stay status 'failed' on purpose: the engine reported
    # an error, not a refusal, and quietly relabelling an error as a benign refusal
    # is how a real bug would get buried. The hint is recorded so the difference
    # can be triaged instead of guessed at.
    for hint, marker in RESOLUTION_LIMIT_HINTS:
        if marker in text:
            return "failed", {
                "kind": "undiagnosed-resolution-limit",
                "hint": hint,
                "engine_marker": marker,
                "note": "the engine reported this as an ERROR, not through the "
                        "feature-unresolved or volume-completeness guard, so the row stays "
                        "status=failed. It reads as a size-too-coarse condition, which "
                        "arguably belongs in the refusal path: triage engine-side rather "
                        "than reclassifying it here.",
                "message": text.strip().splitlines()[-1][:400] if text.strip() else None,
            }
    return "failed", None


def promotion_summary(text: str) -> dict | None:
    """Order-2 promotion counts, so the matched-order pairing is quantified.

    Gmsh delivers a uniformly quadratic mesh. Our --p-elevate promotes only the
    ZZ-smooth-marked subset (adapt::mark_smooth) and then projects the promoted
    boundary mid-nodes onto the B-rep. Both paths therefore carry B-rep
    conforming curved mids, but the promoted FRACTION differs, so the order-2
    comparison is approximate and every row says by how much.
    """
    # The adapt loop has two promotion sites (per pass, and a converged-early
    # one), so take the LAST line: that is the discretisation actually solved.
    promotions = PROMOTION_RE.findall(text)
    mids_all = MIDS_RE.findall(text)
    if not promotions and not mids_all:
        return None
    out: dict = {}
    if promotions:
        smooth, nodes_before, nodes_after, tet10, hex20 = (
            int(value) for value in promotions[-1]
        )
        out.update(
            n_elements_promoted=smooth,
            nodes_before=nodes_before,
            nodes_after=nodes_after,
            tet10=tet10,
            hex20=hex20,
        )
        # engine-bugs: N is the count PASSED to fea::p_elevate, while tet10/hex20 are
        # counted from the resulting mesh, so promoted-but-rejected elements show up
        # as a shortfall. In uniform mode N is the eligible count and the resulting
        # counts should equal it; in selective mode N is the smooth-marked subset.
        out["n_promoted_in_result"] = tet10 + hex20
        out["all_passed_elements_promoted"] = (tet10 + hex20) == smooth
        out["n_rejected_by_promotion"] = smooth - (tet10 + hex20)
        out["promotion_lines_seen"] = len(promotions)
    # Mode as REPORTED by the engine, preferring the mesher_note segment because it
    # is self-describing and survives into artifacts; the stdout marker is the
    # fallback. Never inferred from which flag was passed.
    note_mode = MESHER_NOTE_MODE_RE.findall(text)
    line_mode = MODE_LINE_RE.findall(text)
    if note_mode:
        mode, promoted, tet10_note, hex20_note = note_mode[-1]
        out["mode_reported"] = mode
        out["mode_reported_from"] = "mesher_note"
        out["mode_note_counts"] = {
            "promoted": int(promoted), "tet10": int(tet10_note), "hex20": int(hex20_note)
        }
    elif line_mode:
        out["mode_reported"] = line_mode[-1]
        out["mode_reported_from"] = "stdout marker"
    if mids_all:
        projected, partial, reverted = (int(value) for value in mids_all[-1])
        out.update(
            mids_projected=projected,
            mids_partial=partial,
            mids_reverted=reverted,
        )
    return out or None


def projection_summary(result: subprocess.CompletedProcess[str]) -> str | None:
    for stream in (result.stdout, result.stderr):
        for line in stream.splitlines():
            if "mids projected=" in line:
                return line.strip()
    return None


def metric_for(reference: dict) -> dict:
    """The metric this harness can actually probe from a VTU.

    box_hole keeps its SCF. Everything else is compared on tip_deflection,
    including the external-reference families whose PRIMARY metric is
    strain_energy: our CLI writes no energy to the VTU, and recovering it would
    mean reimplementing each mesher's load assembly, which would compare two
    quantities computed two different ways. An honestly-labelled secondary
    metric is the better trade.
    """
    wanted = "scf" if reference.get("family") == "box_hole" else "tip_deflection"
    for metric in reference.get("metrics", []):
        if metric.get("name") != wanted:
            continue
        kind = metric.get("probe", {}).get("kind")
        if kind not in VTU_PROBE_KINDS:
            raise RuntimeError(
                f"{reference.get('part', 'case')}: probe kind {kind!r} cannot be measured "
                "from a VTU; refusing to guess a substitute"
            )
        return metric
    raise RuntimeError(f"{reference.get('part', 'case')}: no {wanted} metric to compare on")


def secondary_metric_note(reference: dict, metric: dict) -> str | None:
    """Say so on the row when the compared metric is not the reference's primary."""
    names = [m.get("name") for m in reference.get("metrics", [])]
    if not names or names[0] == metric["name"]:
        return None
    return (
        f"peer comparison uses {metric['name']}, which is NOT this reference's primary "
        f"metric ({names[0]}): our CLI's VTU carries no {names[0]}, so it cannot be "
        "measured on the native mesh variants without reimplementing their load assembly"
    )


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


def cad_bbox_diagonal(gmsh: str, work: Path, geometry: Path) -> float:
    """Exact CAD bbox diagonal from a Gmsh 1D pass: every CAD vertex is a node.

    Used for the families whose references are external and therefore carry no
    analytic `inputs` block. The analytic families keep the inputs-derived
    diagonal so their h values stay byte-identical to the previous matrix and
    remain comparable with the re-derived baseline.
    """
    geo = work / f"bbox-{geometry.stem}.geo"
    msh = work / f"bbox-{geometry.stem}.msh"
    geo.write_text(
        f'Merge "{geometry.resolve().as_posix()}";\nMesh.MeshSizeMax = 1;\n', encoding="utf-8"
    )
    _, result, error = run_checked(
        [gmsh, "-nt", "1", "-1", "-format", "msh2", "-o", str(msh), str(geo)],
        f"{geometry.stem}: Gmsh bbox",
    )
    if result is None:
        raise RuntimeError(f"{geometry.name}: Gmsh bbox pass failed ({error})")
    lines = msh.read_text(encoding="utf-8", errors="replace").splitlines()
    start = lines.index("$Nodes")
    count = int(lines[start + 1])
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    for index in range(count):
        fields = lines[start + 2 + index].split()
        for axis in range(3):
            value = float(fields[axis + 1])
            lo[axis] = min(lo[axis], value)
            hi[axis] = max(hi[axis], value)
    return math.sqrt(sum((hi[axis] - lo[axis]) ** 2 for axis in range(3)))




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
    # Prescribed nodes are MEASURED off the field, not re-derived from the box.
    # This used to count every node inside `fix_box`, which stopped matching the
    # engine when a fixture box became a selection of the boundary surface rather
    # than of every node in a volume of space (fea::boundary_nodes_within): the
    # interior nodes of the slab are free now, so the box rule over-counted the
    # constrained set and under-reported active DOF on exactly the rows this
    # matrix compares. A prescribed DOF is exactly zero in the exported field,
    # while a free node in a loaded part is not, so counting exact zeros reads the
    # constraint the solve actually applied, for a peer VTU as much as for ours.
    displacement_all = arrays.get("displacement")
    if displacement_all is None or len(displacement_all) != 3 * len(points):
        raise RuntimeError(f"{path}: missing or malformed displacement point data")
    fixed_nodes = sum(
        1
        for index in range(len(points))
        if displacement_all[3 * index : 3 * index + 3] == [0.0, 0.0, 0.0]
    )
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
    elif native_mesher not in (None, "default", "uniform"):
        raise RuntimeError(f"unsupported native mesher: {native_mesher}")
    if native_mesher is not None and order == 2:
        command.append("--p-elevate")
    if native_mesher == "uniform":
        command.append(UNIFORM_FLAG)
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
        "status": "ok",
        "probe_nodes": probe_nodes,
        "dofs": dofs,
        "probe_definition": PROBE_DEFINITION,
        "truth": {
            "value": truth,
            "metric": metric["name"],
            "probe_kind": metric["probe"]["kind"],
            "tol": metric.get("tol"),
            "source": metric.get("source"),
            "reference": f"bench/reference/corpus/{case_id}.json",
        },
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
    output: str = "",
) -> dict:
    """A row for a run that produced no probe.

    Three outcomes are kept distinct, because collapsing them loses the finding:
      refused  -- the engine declined a size it cannot represent (a legitimate,
                  informative result: the feature-unresolved or
                  volume-completeness guard fired)
      timeout  -- exceeded the wall clock
      failed   -- anything else, i.e. a genuine error
    """
    refusal = None
    if reason.startswith("timed out"):
        status = "timeout"
    else:
        status, refusal = classify_failure(output or reason)
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
        "probe_definition": PROBE_DEFINITION,
        "notes": (
            f"h_rel={h_rel:.2f}; order={order}; load=fixed resultant; "
            f"{reason}; stage={stage}"
            + ("; ENGINE REFUSAL, not an error: " + refusal["kind"]
               if refusal and status == "refused" else
               "; diagnosis: " + refusal["hint"] if refusal else "")
        ),
        **({"refusal": refusal} if refusal and status == "refused" else {}),
        **({"diagnosis": refusal} if refusal and status != "refused" else {}),
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
    case_path = (
        ROOT / "bench" / "geometries" / "corpus" / "primitives" / f"{case_id}.case.json"
    )
    case = load_json(case_path)
    metric = metric_for(reference)
    caveat = secondary_metric_note(reference, metric)
    if reference.get("truth_source") == "analytic":
        diagonal = bbox_diagonal(reference, metric)
    else:
        # External-reference families carry no analytic `inputs`; measure the CAD
        # bbox instead. The analytic families keep the inputs-derived diagonal so
        # their h values stay byte-identical to the previous matrix.
        diagonal = cad_bbox_diagonal(gmsh, work, ROOT / case["geometry"])
    mesh_size = diagonal * h_rel
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
        failure_output = ""
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
                failure_output = last_output
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
            failure_output = last_output

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
                    failure_output,
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
        ("uniform", "polymesh-native-uniform-p2", "uniform"),
    ):
        if variant not in variants:
            continue
        if variant == "uniform" and order != UNIFORM_ORDER_ONLY:
            continue
        vtu_path = work / f"{stem}-{variant}.vtu"
        total_wall, solve_result, solve_error = run_checked(
            solve_command(
                polymesh, geometry, vtu_path, case, metric, mesh_size, order, mesher
            ),
            f"{case_id}: {variant} solve",
        )
        native_output = last_output if solve_result is None else ""
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
                    native_output,
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
            promotion = promotion_summary(f"{solve_result.stdout}\n{solve_result.stderr}")
            if promotion is not None:
                # The mode is recorded from what the solve actually reported, plus
                # what was requested; it is never inferred from the flag alone.
                promotion["mode_requested"] = (
                    "uniform" if variant == "uniform" else "selective (product default)"
                )
                promotion["mode_evidence"] = summary
                promotion["product_default"] = variant in PRODUCT_DEFAULT_VARIANTS
                expected_mode = "uniform" if variant == "uniform" else "selective"
                reported = promotion.get("mode_reported")
                promotion["mode_matches_request"] = (
                    None if reported is None else reported == expected_mode
                )
                if reported is not None and reported != expected_mode:
                    promotion["order_pairing"] = (
                        f"MODE MISMATCH: requested {expected_mode} but the engine reported "
                        f"{reported}. The flag did not take effect; do not read this row as "
                        "the requested discretisation."
                    )
                    row["notes"] += (
                        f"; WARNING requested {expected_mode} promotion but engine reported "
                        f"{reported}"
                    )
                    print(f"{case_id} h={h_rel:.2f} {variant}: MODE MISMATCH "
                          f"(requested {expected_mode}, engine reported {reported})",
                          file=sys.stderr)
                promotion["order_pairing"] = (
                    "TRUE PARITY: uniform promotion, directly comparable with the order-2 "
                    "Gmsh rows."
                    if variant == "uniform" else
                    "APPROXIMATE, not order parity: Gmsh delivers a uniformly quadratic "
                    "mesh while --p-elevate promotes only the ZZ-smooth-marked subset "
                    "(adapt::mark_smooth) and projects those mid-nodes onto the B-rep. "
                    "Both paths therefore carry B-rep conforming curved mids, but the "
                    "promoted FRACTION differs; the counts above are the measure of how "
                    "close the pairing is on this row."
                )
                row["promotion"] = promotion
        rows.append(row)

    # A flag that silently does nothing is the failure mode this session has hit
    # twice in other guises, so verify the mode changed the mesh rather than
    # trusting that it did: at the same case, h and order, uniform must promote
    # strictly more elements than selective.
    promoted = {
        row["solver"]: (row.get("promotion") or {}).get("n_elements_promoted")
        for row in rows
        if row.get("promotion")
    }
    selective = promoted.get("polymesh-native")
    uniform = promoted.get("polymesh-native-uniform-p2")
    if selective is not None and uniform is not None:
        verified = uniform > selective
        for row in rows:
            if row["solver"] != "polymesh-native-uniform-p2":
                continue
            row["promotion"]["uniform_vs_selective"] = {
                "selective_promoted": selective,
                "uniform_promoted": uniform,
                "strictly_more": verified,
                "uniform_fully_promoted_what_it_passed": row["promotion"].get(
                    "all_passed_elements_promoted"
                ),
                "uniform_rejected": row["promotion"].get("n_rejected_by_promotion"),
                "expectation": "in uniform mode the count passed to p_elevate is the "
                "eligible element count, so tet10+hex20 should equal it; a shortfall means "
                "elements were rejected and the row is not full parity with Gmsh",
                "shortfall_is_expected_on_curved_families": (
                    "p_elevate declines a promotion when an inherited curved edge node "
                    "would make the promoted element invalid at a quadrature point, so a "
                    "shortfall is expected where order-2 rims are already conformed to the "
                    "B-rep (tube, stepped_shaft, box_hole bores). It is a real limit on "
                    "parity, not a defect."
                ),
            }
            if not verified:
                row["promotion"]["order_pairing"] = (
                    f"UNVERIFIED: uniform promoted {uniform} elements against selective's "
                    f"{selective}, so the uniform flag did not demonstrably change the "
                    "discretisation on this row. Do not read this row as order parity."
                )
                row["notes"] += (
                    "; WARNING uniform promotion not verified (promoted "
                    f"{uniform} vs selective {selective})"
                )
                print(f"{case_id} h={h_rel:.2f}: uniform promotion NOT verified "
                      f"({uniform} vs {selective})", file=sys.stderr)

    for row in rows:
        if caveat is not None:
            row["metric_caveat"] = caveat
            row["notes"] += f"; {caveat}"
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
    parser.add_argument("--variant", choices=VARIANTS, action="append",
                        help="run only this mesh-source variant; repeatable, so the shipping "
                             "variants can be run without uniform before that mode lands")
    parser.add_argument("--resume-from", type=Path, help="reuse completed rows from JSON")
    parser.add_argument("--out", type=Path, default=OUTPUT_PATH,
                        help="results path (default: bench/results/gmsh-peer.json). Use a "
                             "scratch path for smoke runs so a partial matrix never "
                             "overwrites the committed one")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cases = peer_cases()
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

    variants = list(dict.fromkeys(args.variant)) if args.variant else list(VARIANTS)
    gmsh = resolve_executable(args.gmsh, GMSH_DEFAULT, "gmsh")
    if not gmsh:
        print(f"gmsh not found; skip Gmsh peer (expected {GMSH_DEFAULT})")
        return 0
    polymesh = resolve_executable(args.polymesh, POLYMESH_DEFAULT, "polymesh")
    if not polymesh:
        print(f"error: polymesh CLI not found (expected {POLYMESH_DEFAULT})", file=sys.stderr)
        return 1
    if "uniform" in variants and not build_supports_uniform(polymesh):
        print(f"error: this build does not accept {UNIFORM_FLAG}, so the uniform variant "
              "cannot run. The spelling is pinned, not negotiable: the mode has not landed "
              "in this binary yet. Rebuild, or pass --variant to run the shipping variants "
              "only. Refusing to start rather than failing 48 rows on an unknown argument.",
              file=sys.stderr)
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
        if args.variant and args.resume_from is None and args.out.is_file():
            existing = load_json(args.out)
            if not isinstance(existing, list):
                raise RuntimeError(f"{args.out}: expected a JSON row array")
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

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print("wrote", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
