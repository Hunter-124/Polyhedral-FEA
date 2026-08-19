#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independent external truth for the primitive corpus: Gmsh mesh -> CalculiX solve.

Neither the PolyMesh mesher nor the PolyMesh solver takes part in the reference
values produced here; both are the system under test. The reference chain is:

  1. Gmsh 4.13.1 meshes the case's STEP part directly from CAD (tet10, order 2,
     ``Mesh.HighOrderOptimize=2``, curvature-driven sizing so holes, notches,
     fillets and cavities are resolved without a globally tiny h).
  2. CalculiX 2.23 (``ccx_static``) solves that mesh as C3D10 with the case's
     material, the case's fix box fully clamped, and the case's traction applied
     to the loaded CAD end face through energy-conjugate (consistent) nodal
     loads integrated over the curved tri6 boundary faces.
  3. The probes are computed from CalculiX output only:
       strain_energy  -- sum of CalculiX per-element ELSE internal energy,
                         cross-checked against 1/2 f.u from the applied load
                         vector and the CalculiX displacements.
       tip_deflection -- mean |u| over the corner nodes of the loaded faces
                         (the definition apps/testlab scores against).
       scf            -- peak nodal von Mises in the metric's probe box over
                         the metric's nominal stress, from the CalculiX .frd
                         extrapolated nodal stress field.

Every case is run at two refinement rungs; the relative change between them is
reported as the convergence delta. A case whose two finest rungs disagree by
more than ``--converged-at`` is flagged and is not fit to ship as truth.

Load surface: the primary rule is the case's own specification, exactly as
apps/testlab applies it -- free faces whose centroid lies inside the load box,
kept when ``|n.t_hat| > select.normal_min_dot`` (``-1`` meaning no normal
filter, empty filter falling back to box-only). Because the end-slab selection
box is a selection mechanism rather than geometry, it can also enclose a ring of
side wall; the area of the loaded CAD end face alone is therefore reported
per rung as ``end_face_area_m2`` next to ``load_area_m2``, with their relative
difference, so any divergence between the case's rule and what the closed-form
references assume is visible instead of silently baked into a truth value.
``--include-wall-strip`` forces the box-only convention for comparison.

Stages
------
``--stage validate``  run the 8 corpus cases whose truth is closed-form and
                      report percent error against the analytic value.
``--stage generate``  run the remaining cases and write the raw external
                      results JSON (never a reference file).
``--stage write``     promote a previously generated results JSON into
                      bench/reference/corpus/*.json, preserving schema, tol,
                      probe and every analytic entry. Requires --approved.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
GMSH = ROOT / "tools" / "gmsh" / "gmsh-4.13.1-Windows64" / "gmsh.exe"
CCX = ROOT / "tools" / "calculix" / "calculix_2.23_4win" / "ccx_static.exe"
GMSH_VERSION = "4.13.1"
CCX_VERSION = "2.23"
CASE_DIR = ROOT / "bench" / "geometries" / "corpus" / "primitives"
REF_DIR = ROOT / "bench" / "reference" / "corpus"
OUT_DIR = ROOT / "bench" / "reference" / "external"
TRUTH_SOURCE = "external-gmsh-mesh+calculix-solver"

# Gmsh tet10 (msh element type 11) -> Abaqus/CalculiX C3D10 node order.
# Verified against Gmsh's own `-format inp` exporter on 714/714 elements.
TET10_GMSH_TO_CCX = (0, 1, 2, 3, 4, 5, 6, 7, 9, 8)
# Local edge -> local mid-node index for a Gmsh tet10.
TET10_EDGE_MID = {(0, 1): 4, (1, 2): 5, (0, 2): 6, (0, 3): 7, (2, 3): 8, (1, 3): 9}
TET10_FACES = ((0, 1, 2), (0, 1, 3), (0, 2, 3), (1, 2, 3))
FACE_NORMAL_MIN_DOT = 0.7
AXES = {"x": 0, "y": 1, "z": 2}

# Refinement rungs: (mesh-size factor, curvature points per 2*pi), a constant
# refinement ratio of 1.45 in every prescribed length so three rungs support
# Richardson extrapolation. Truth is taken from the finest rung and the last two
# rungs give the convergence delta. Curvature sizing is deliberately moderate and
# floored by MeshSizeMin: on a small-radius cylinder (stepped_shaft r = 3.4 mm) a
# high curvature count refines the whole wall and the mesh explodes for no
# accuracy gain, so feature resolution comes from the local size field in the
# metric's probe box instead.
RUNG_REFINEMENT_RATIO = 1.45
RUNGS_ALL = ((1.0, 24), (1.0 / 1.45, 34), (1.0 / 1.45**2, 49))
RUNGS = RUNGS_ALL[:2]
MESH_SIZE_MIN_DIVISOR = 5.0
PROBE_BOX_SIZE_DIVISOR = 4.0
# Measured sensitivity of each probe kind to the mesh SIZING POLICY, from running
# box_hole_s0_c0 under two independent policies (curvature 58 with MeshSizeMin
# h/12 at 86,481 DOF versus curvature 34 with MeshSizeMin h/5 at 64,614 DOF).
# Global quantities barely move; a nodal peak stress moves 100x more, which is
# why a stress reference cannot claim the same precision as an energy reference.
MESH_POLICY_SENSITIVITY = {
    "strain_energy": 2.0e-5,
    "tip_deflection": 2.0e-5,
    "peak_vm_over_nominal": 7.44e-3,
}
# Measured family-specific sensitivity that the two-rung delta does NOT see.
# On a thin wall the element size ACROSS the wall is set by the curvature count
# (the bore asks for 2*pi*r_i/N, comfortably above MeshSizeMin, so tightening
# MeshSizeMin changes nothing), and both shipped rungs sit at a similar
# through-wall element count. Measured on the two worst-resolved regimes by
# re-solving at the shipped MeshSizeMax with the curvature count raised 34 -> 60,
# which is the knob that actually adds elements across the wall:
#   tube_s0_c1 (through-wall ~1.9 -> ~3.4 elements, DOF 167,913 -> 435,039):
#       strain_energy moved 0.141%, tip_deflection 0.134%
#   tube_s1_c1 (through-wall ~1.3 -> ~2.2 elements, DOF 117,141 -> 427,392):
#       strain_energy moved 0.143%, tip_deflection 0.141%
# The two-rung global-size delta had claimed 0.03%, so it understated the error by
# about 4x. The worst measured value, rounded up, is carried for this family.
FAMILY_EXTRA_SENSITIVITY = {
    "tube": {
        "value": 1.5e-3,
        "why": "through-wall resolution: raising the curvature count 34 -> 60 at fixed "
        "MeshSizeMax moved the answer 0.141% on tube_s0_c1 and 0.143% on tube_s1_c1 "
        "(the two worst-resolved regimes, ~1.3-1.9 elements through the wall at the "
        "shipped rung), which the two-rung global-size delta of 0.03% did not capture",
    },
}
# Coverage factor applied to the numerical (random-like) uncertainty terms. A
# known idealisation bias is added at full size instead, because it is a bias.
UNCERTAINTY_COVERAGE = 3.0
# Floor on the shipped tol. The references are far better than this; the floor
# exists so the campaign accuracy score s = 1/(1+rel/tol) stays discriminating
# over the 1-20% band where candidate meshes actually land. It is a scoring-scale
# choice, never a claim about the reference: the measured uncertainty is recorded
# separately as reference_uncertainty_rel.
TOL_FLOOR = 0.02
DEFAULT_H_REL = 0.055
# Families whose probe is a stress concentration need the feature resolved by a
# local size field, not only by the global size.
H_REL_BY_FAMILY = {
    "box_hole": 0.045,
    "plate_notch": 0.045,
    "stepped_shaft": 0.055,
    "sphere_box": 0.055,
    "channel": 0.06,
    "l_bracket": 0.06,
}
GMSH_TIMEOUT_S = 900
CCX_TIMEOUT_S = 5400


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

# --------------------------------------------------------------------------- #
# convergence + closed-form helpers
# --------------------------------------------------------------------------- #
def richardson(rungs: list[dict], ratio: float) -> dict:
    """Per-metric Richardson extrapolation over three uniformly refined rungs.

    Returns the observed order p, the extrapolated value and the estimated
    discretisation error of the finest rung, |v_fine - v_extrap| / |v_extrap|.
    A non-monotone or stalled triple has no meaningful order, so the estimate
    falls back to the raw last-rung change, which is the conservative choice.
    """
    out: dict = {}
    coarse, mid, fine = rungs
    for name, v2 in fine["values"].items():
        v0 = coarse["values"].get(name)
        v1 = mid["values"].get(name)
        if v0 is None or v1 is None or v2 is None:
            out[name] = None
            continue
        d01, d12 = v1 - v0, v2 - v1
        entry = {"values": [v0, v1, v2]}
        if d12 == 0.0 or d01 == 0.0 or d01 / d12 <= 0.0:
            entry.update(
                order=None,
                extrapolated=v2,
                error_rel=abs(d12) / abs(v2) if v2 else None,
                note="non-monotone or stalled triple; error taken as the raw last-rung change",
            )
        else:
            order = math.log(abs(d01 / d12)) / math.log(ratio)
            denominator = ratio**order - 1.0
            extrapolated = v2 + d12 / denominator if denominator != 0.0 else v2
            entry.update(
                order=order,
                extrapolated=extrapolated,
                error_rel=(
                    abs(v2 - extrapolated) / abs(extrapolated) if extrapolated else None
                ),
            )
        out[name] = entry
    return out


HOWLAND_CITATION = (
    "Howland (1930), 'On the stresses in the neighbourhood of a circular hole in a "
    "strip under tension', Phil. Trans. R. Soc. London A 229:49-86; tabulated as "
    "Roark's Formulas for Stress and Strain ch.6 (central single circular hole in a "
    "finite-width plate, axial tension) and Peterson's Stress Concentration Factors "
    "chart 4.1. Ktn = 3.000 - 3.140(d/W) + 3.667(d/W)^2 - 1.527(d/W)^3 with "
    "sigma_nom = P/[t(W-d)], so on the gross-section stress the corpus probe uses, "
    "Ktg = sigma_max/sigma_gross = Ktn/(1 - d/W). Kirsch's 3.0 is the d/W -> 0 limit."
)


def howland_ktg(d_over_w: float) -> tuple[float, float]:
    """Net-section and gross-section SCF for a hole in a finite-width plate."""
    if not 0.0 <= d_over_w < 1.0:
        raise RuntimeError(f"d/W out of range for the Howland fit: {d_over_w}")
    ktn = 3.000 - 3.140 * d_over_w + 3.667 * d_over_w**2 - 1.527 * d_over_w**3
    return ktn, ktn / (1.0 - d_over_w)


def round_up_2sf(value: float) -> float:
    """Round up to two significant figures so a shipped tol is never optimistic."""
    if value <= 0.0:
        return 0.0
    exponent = math.floor(math.log10(value)) - 1
    step = 10.0**exponent
    return round(math.ceil(value / step) * step, max(0, -exponent))



# --------------------------------------------------------------------------- #
# process helpers
# --------------------------------------------------------------------------- #
@dataclass
class Run:
    wall: float
    stdout: str
    stderr: str
    returncode: int

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def run(command: list[str], cwd: Path, timeout: float) -> Run:
    env = dict(os.environ, OMP_NUM_THREADS="1")
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            command, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return Run(time.perf_counter() - t0, "", f"timed out after {timeout} s", 124)
    return Run(time.perf_counter() - t0, proc.stdout, proc.stderr, proc.returncode)


# --------------------------------------------------------------------------- #
# Gmsh
# --------------------------------------------------------------------------- #
def geo_text(
    step: Path,
    h: float,
    curvature_points: int,
    order: int,
    high_order_optimize: int,
    refine_box: tuple[list[list[float]], float] | None,
) -> str:
    merge = step.resolve().as_posix().replace('"', '\\"')
    lines = [
        f'Merge "{merge}";',
        f"Mesh.ElementOrder = {order};",
        f"Mesh.MeshSizeMax = {h:.17g};",
        f"Mesh.MeshSizeMin = {h / MESH_SIZE_MIN_DIVISOR:.17g};",
        f"Mesh.MeshSizeFromCurvature = {curvature_points};",
        "Mesh.MeshSizeExtendFromBoundary = 1;",
        "Mesh.Optimize = 1;",
        "Mesh.OptimizeThreshold = 0.3;",
    ]
    if order == 2:
        lines.append(f"Mesh.HighOrderOptimize = {high_order_optimize};")
    if refine_box is not None:
        box, h_in = refine_box
        (x0, y0, z0), (x1, y1, z1) = box
        lines += [
            "Field[1] = Box;",
            f"Field[1].VIn = {h_in:.17g};",
            f"Field[1].VOut = {h:.17g};",
            f"Field[1].XMin = {x0:.17g}; Field[1].XMax = {x1:.17g};",
            f"Field[1].YMin = {y0:.17g}; Field[1].YMax = {y1:.17g};",
            f"Field[1].ZMin = {z0:.17g}; Field[1].ZMax = {z1:.17g};",
            f"Field[1].Thickness = {2.0 * h_in:.17g};",
            "Background Field = 1;",
        ]
    return "\n".join(lines) + "\n"


def gmsh_mesh(
    work: Path,
    stem: str,
    step: Path,
    h: float,
    curvature_points: int,
    refine_box: tuple[list[list[float]], float] | None,
    dim: int = 3,
    order: int = 2,
) -> tuple[Path, Run, int]:
    """Mesh `step`. Order-2 tries HighOrderOptimize=2 then falls back to 1."""
    msh = work / f"{stem}.msh"
    attempts = (2, 1) if order == 2 else (0,)
    last = None
    for optimize in attempts:
        geo = work / f"{stem}.geo"
        geo.write_text(
            geo_text(step, h, curvature_points, order, optimize, refine_box), encoding="utf-8"
        )
        last = run(
            [str(GMSH), "-nt", "1", f"-{dim}", "-format", "msh2", "-o", str(msh), str(geo)],
            work,
            GMSH_TIMEOUT_S,
        )
        if last.ok:
            return msh, last, optimize
    return msh, last, -1


def bbox_diagonal(work: Path, step: Path) -> float:
    """Exact CAD bbox from a 1D mesh: every CAD vertex is a mesh node."""
    msh, result, _ = gmsh_mesh(work, f"bbox-{step.stem}", step, 1.0, 0, None, dim=1, order=1)
    if not result.ok:
        raise RuntimeError(f"gmsh bbox pass failed for {step.name}: {result.stderr[-400:]}")
    nodes, _ = parse_msh2(msh, require_tets=False)
    lo = nodes.min(axis=0)
    hi = nodes.max(axis=0)
    return float(np.linalg.norm(hi - lo))


# --------------------------------------------------------------------------- #
# CAD feature measurement
# --------------------------------------------------------------------------- #

def cad_feature_sizes(work: Path, step: Path) -> dict:
    """Feature sizes of a STEP solid, measured from a curvature-refined 1D mesh.

    Nothing is taken from the generator's intent. Every CAD vertex is a mesh node
    and every CAD edge is meshed, so per-curve lengths come out of the mesh; a
    closed curve whose nodes are equidistant from their own centroid is reported
    as a circle of radius length/(2*pi), which is how hole radii, a tube's bore
    and its outer wall are recovered.
    """
    stem = f"feat-{step.stem}"
    geo = work / f"{stem}.geo"
    msh = work / f"{stem}.msh"
    geo.write_text(
        f'Merge "{step.resolve().as_posix()}";\n'
        "Mesh.MeshSizeFromCurvature = 64;\nMesh.MeshSizeMax = 1;\nMesh.MeshSizeMin = 1e-6;\n",
        encoding="utf-8",
    )
    result = run(
        [str(GMSH), "-nt", "1", "-1", "-format", "msh2", "-o", str(msh), str(geo)],
        work, GMSH_TIMEOUT_S,
    )
    if not result.ok:
        raise RuntimeError(f"gmsh 1D pass failed for {step.name}: {result.stderr[-400:]}")
    text = msh.read_text(encoding="utf-8", errors="replace").splitlines()
    i, j = text.index("$Nodes"), text.index("$Elements")
    coords = {}
    for k in range(int(text[i + 1])):
        f = text[i + 2 + k].split()
        coords[int(f[0])] = tuple(float(v) for v in f[1:4])
    curves: dict[int, dict] = {}
    for k in range(int(text[j + 1])):
        f = text[j + 2 + k].split()
        if int(f[1]) != 1:  # 2-node line
            continue
        ntags = int(f[2])
        # msh2 tags are (physical, elementary); with no physical groups the first
        # is 0 for every curve, so the elementary tag is what separates edges.
        tag = int(f[4]) if ntags >= 2 else int(f[3])
        a, b = int(f[3 + ntags]), int(f[4 + ntags])
        entry = curves.setdefault(tag, {"length": 0.0, "nodes": set()})
        entry["length"] += math.dist(coords[a], coords[b])
        entry["nodes"].update((a, b))
    points = np.asarray(list(coords.values()))
    lo, hi = points.min(axis=0), points.max(axis=0)
    diagonal = float(np.linalg.norm(hi - lo))
    circles = []
    for entry in curves.values():
        nodes = np.asarray([coords[n] for n in entry["nodes"]])
        centre = nodes.mean(axis=0)
        radii = np.linalg.norm(nodes - centre, axis=1)
        mean_r = float(radii.mean())
        if mean_r <= 0.0:
            continue
        round_enough = (radii.max() - radii.min()) / mean_r < 0.02
        closed_enough = abs(entry["length"] / (2.0 * math.pi * mean_r) - 1.0) < 0.02
        if round_enough and closed_enough:
            circles.append({"radius_m": mean_r, "centre_m": [float(v) for v in centre]})
    circles.sort(key=lambda c: c["radius_m"])
    lengths = sorted(v["length"] for v in curves.values())
    return {
        "step": step.name,
        "bbox_lo_m": [float(v) for v in lo],
        "bbox_hi_m": [float(v) for v in hi],
        "extents_m": [float(v) for v in (hi - lo)],
        "bbox_diagonal_m": diagonal,
        "n_cad_curves": len(curves),
        "shortest_cad_edge_m": lengths[0] if lengths else None,
        "circular_edges": circles,
        "method": "gmsh -1 with MeshSizeFromCurvature=64; per-curve length summed over "
        "its 1D elements; circle = closed curve with equidistant nodes, radius = L/(2*pi)",
    }


# --------------------------------------------------------------------------- #
# mesh parsing / derived topology
# --------------------------------------------------------------------------- #
def parse_msh2(path: Path, require_tets: bool = True) -> tuple[np.ndarray, np.ndarray]:
    """Return (nodes[N,3] in ascending node-id order, tet10[M,10] zero-based)."""
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        i = text.index("$Nodes")
        j = text.index("$Elements")
    except ValueError as exc:
        raise RuntimeError(f"{path.name}: not a Gmsh 2.x ASCII mesh") from exc
    n_nodes = int(text[i + 1])
    ids = np.empty(n_nodes, dtype=np.int64)
    coords = np.empty((n_nodes, 3), dtype=np.float64)
    for k in range(n_nodes):
        f = text[i + 2 + k].split()
        ids[k] = int(f[0])
        coords[k, 0] = float(f[1])
        coords[k, 1] = float(f[2])
        coords[k, 2] = float(f[3])
    if not np.array_equal(ids, np.arange(1, n_nodes + 1)):
        raise RuntimeError(f"{path.name}: node ids are not 1..N")
    n_elems = int(text[j + 1])
    tets = []
    for k in range(n_elems):
        f = text[j + 2 + k].split()
        if int(f[1]) != 11:  # 10-node second order tetrahedron
            continue
        ntags = int(f[2])
        tets.append([int(v) - 1 for v in f[3 + ntags :]])
    if require_tets and not tets:
        raise RuntimeError(f"{path.name}: no tet10 elements")
    if not tets:
        return coords, np.zeros((0, 10), dtype=np.int64)
    return coords, np.asarray(tets, dtype=np.int64)


def free_faces(tets: np.ndarray) -> np.ndarray:
    """Boundary tri6 faces [F,6]: 3 corner nodes then the 3 edge mid nodes."""
    faces = np.empty((tets.shape[0] * 4, 6), dtype=np.int64)
    for fi, (a, b, c) in enumerate(TET10_FACES):
        block = faces[fi * tets.shape[0] : (fi + 1) * tets.shape[0]]
        block[:, 0] = tets[:, a]
        block[:, 1] = tets[:, b]
        block[:, 2] = tets[:, c]
        for slot, (p, q) in enumerate(((a, b), (b, c), (a, c))):
            block[:, 3 + slot] = tets[:, TET10_EDGE_MID[tuple(sorted((p, q)))]]
    key = np.sort(faces[:, :3], axis=1)
    _, index, counts = np.unique(key, axis=0, return_index=True, return_counts=True)
    return faces[index[counts == 1]]


# tri6 (gmsh order: corners 0,1,2; mids 3=(0,1), 4=(1,2), 5=(0,2))
def _tri6_gauss(n: int = 4):
    """Collapsed (Duffy) tensor Gauss rule on the unit triangle."""
    x, w = np.polynomial.legendre.leggauss(n)
    x = 0.5 * (x + 1.0)
    w = 0.5 * w
    xi, eta, weight = [], [], []
    for a, wa in zip(x, w):
        for b, wb in zip(x, w):
            xi.append(a)
            eta.append(b * (1.0 - a))
            weight.append(wa * wb * (1.0 - a))
    return np.asarray(xi), np.asarray(eta), np.asarray(weight)


def _tri6_shape(xi: np.ndarray, eta: np.ndarray):
    l0, l1, l2 = 1.0 - xi - eta, xi, eta
    N = np.stack(
        [
            l0 * (2.0 * l0 - 1.0),
            l1 * (2.0 * l1 - 1.0),
            l2 * (2.0 * l2 - 1.0),
            4.0 * l0 * l1,
            4.0 * l1 * l2,
            4.0 * l0 * l2,
        ],
        axis=1,
    )  # [Q,6]
    dxi = np.stack(
        [
            1.0 - 4.0 * l0,
            4.0 * l1 - 1.0,
            np.zeros_like(xi),
            4.0 * (l0 - l1),
            4.0 * l2,
            -4.0 * l2,
        ],
        axis=1,
    )
    deta = np.stack(
        [
            1.0 - 4.0 * l0,
            np.zeros_like(xi),
            4.0 * l2 - 1.0,
            -4.0 * l1,
            4.0 * l1,
            4.0 * (l0 - l2),
        ],
        axis=1,
    )
    return N, dxi, deta


def face_geometry(nodes: np.ndarray, faces: np.ndarray, quadrature: int = 4):
    """Curved tri6 areas [F], centroids [F,3] and unit corner normals [F,3]."""
    xi, eta, w = _tri6_gauss(quadrature)
    N, dxi, deta = _tri6_shape(xi, eta)
    P = nodes[faces]  # [F,6,3]
    t1 = np.einsum("qk,fkd->fqd", dxi, P)
    t2 = np.einsum("qk,fkd->fqd", deta, P)
    cross = np.cross(t1, t2)
    jac = np.linalg.norm(cross, axis=2)  # [F,Q]
    area = jac @ w
    corners = P[:, :3, :]
    centroid = corners.mean(axis=1)
    n = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    norm = np.linalg.norm(n, axis=1, keepdims=True)
    unit = np.divide(n, norm, out=np.zeros_like(n), where=norm > 0.0)
    return area, centroid, unit


def consistent_face_loads(
    nodes: np.ndarray, faces: np.ndarray, traction: np.ndarray, quadrature: int = 4
) -> np.ndarray:
    """Energy-conjugate nodal load vector [N,3] for a uniform traction (Pa)."""
    xi, eta, w = _tri6_gauss(quadrature)
    N, dxi, deta = _tri6_shape(xi, eta)
    P = nodes[faces]
    t1 = np.einsum("qk,fkd->fqd", dxi, P)
    t2 = np.einsum("qk,fkd->fqd", deta, P)
    jac = np.linalg.norm(np.cross(t1, t2), axis=2)  # [F,Q]
    # weight per face/node: int N_k dS
    coeff = np.einsum("q,qk,fq->fk", w, N, jac)  # [F,6]
    out = np.zeros_like(nodes)
    for k in range(6):
        np.add.at(out, faces[:, k], coeff[:, k, None] * traction[None, :])
    return out


def in_box(points: np.ndarray, box) -> np.ndarray:
    lo = np.asarray(box[0], dtype=np.float64)
    hi = np.asarray(box[1], dtype=np.float64)
    return np.all((points >= lo) & (points <= hi), axis=1)


# --------------------------------------------------------------------------- #
# CalculiX deck + output parsing
# --------------------------------------------------------------------------- #
def num(value: float) -> str:
    """CalculiX parses input in 20-character fields; keep every number inside one."""
    text = f"{float(value):.11g}"
    if len(text) > 20:
        raise RuntimeError(f"deck field too wide for CalculiX: {text!r}")
    return text


def id_lines(ids, width: int = 8) -> list[str]:
    ids = list(ids)
    return [
        ", ".join(str(int(v) + 1) for v in ids[i : i + width]) for i in range(0, len(ids), width)
    ]


def write_deck(
    path: Path,
    nodes: np.ndarray,
    tets: np.ndarray,
    fixed: np.ndarray,
    loads: np.ndarray,
    print_nodes: np.ndarray,
    probe_elements: np.ndarray | None,
    material: dict,
    title: str,
) -> None:
    out: list[str] = ["*HEADING", title, "*NODE"]
    out += [f"{i + 1}, {num(p[0])}, {num(p[1])}, {num(p[2])}" for i, p in enumerate(nodes)]
    out.append("*ELEMENT, TYPE=C3D10, ELSET=EALL")
    ccx = tets[:, TET10_GMSH_TO_CCX] + 1
    out += [f"{i + 1}, " + ", ".join(map(str, row)) for i, row in enumerate(ccx)]
    # Quadratic-triangle corner weights are analytically zero, so only the mid
    # nodes carry load; drop the roundoff-level corner entries but still print
    # displacements on every loaded face node (the tip-deflection probe set).
    magnitude = np.linalg.norm(loads, axis=1)
    loaded = np.flatnonzero(magnitude > 1e-12 * magnitude.max())
    out.append("*NSET, NSET=NFIX")
    out += id_lines(fixed)
    out.append("*NSET, NSET=NPRINT")
    out += id_lines(print_nodes)
    if probe_elements is not None and probe_elements.size:
        out.append("*ELSET, ELSET=EPROBE")
        out += id_lines(probe_elements)
    out += [
        "*MATERIAL, NAME=MAT",
        "*ELASTIC",
        f"{num(material['E'])}, {num(material['nu'])}",
        "*SOLID SECTION, ELSET=EALL, MATERIAL=MAT",
        "*STEP",
        "*STATIC",
        "*BOUNDARY",
        "NFIX, 1, 3",
        "*CLOAD",
    ]
    for node in loaded:
        for dof in range(3):
            value = loads[node, dof]
            if value != 0.0:
                out.append(f"{node + 1}, {dof + 1}, {num(value)}")
    out += [
        "*NODE PRINT, NSET=NPRINT",
        "U",
        "*NODE PRINT, NSET=NFIX, TOTALS=ONLY",
        "RF",
        "*EL PRINT, ELSET=EALL",
        "ELSE",
    ]
    if probe_elements is not None and probe_elements.size:
        out += ["*NODE FILE", "U, S"]
    out.append("*END STEP")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


def parse_dat(path: Path) -> list[tuple[str, np.ndarray]]:
    """Split a CalculiX .dat into (header, numeric rows) blocks."""
    blocks: list[tuple[str, list[list[float]]]] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue
        head = line[0]
        if head.isdigit() or head in "+-." :
            if not blocks:
                continue
            try:
                blocks[-1][1].append([float(v.replace("D", "E")) for v in line.split()])
            except ValueError:
                continue
        else:
            blocks.append((line, []))
    return [(head, np.asarray(rows, dtype=np.float64)) for head, rows in blocks if rows]


def dat_block(blocks, keyword: str) -> np.ndarray | None:
    for head, rows in blocks:
        if keyword in head.lower():
            return rows
    return None


def parse_frd_nodal(path: Path, name: str) -> tuple[np.ndarray, np.ndarray] | None:
    """Nodal block `name` (e.g. DISP, STRESS) from a CalculiX .frd: (ids, values)."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith(" -4") and name in line:
            n_comp = int(line.split()[2])
            i += 1
            comps = 0
            while i < len(lines) and lines[i].startswith(" -5"):
                comps += 1
                i += 1
            ids: list[int] = []
            values: list[list[float]] = []
            while i < len(lines) and lines[i].startswith(" -1"):
                row = lines[i]
                ids.append(int(row[3:13]))
                # DISP declares 4 components (3 + magnitude) but writes 3; take
                # whatever the 12-character record actually carries.
                available = (len(row.rstrip()) - 13) // 12
                values.append(
                    [
                        float(row[13 + 12 * k : 25 + 12 * k])
                        for k in range(min(n_comp, comps, available))
                    ]
                )
                i += 1
            return np.asarray(ids, dtype=np.int64), np.asarray(values, dtype=np.float64)
        i += 1
    return None


def von_mises(s: np.ndarray) -> np.ndarray:
    """s columns: SXX SYY SZZ SXY SYZ SZX (CalculiX .frd order)."""
    sxx, syy, szz, sxy, syz, szx = (s[:, k] for k in range(6))
    return np.sqrt(
        0.5 * ((sxx - syy) ** 2 + (syy - szz) ** 2 + (szz - sxx) ** 2)
        + 3.0 * (sxy**2 + syz**2 + szx**2)
    )


# --------------------------------------------------------------------------- #
# case model
# --------------------------------------------------------------------------- #
@dataclass
class Case:
    case_id: str
    case: dict
    reference: dict
    step: Path
    family: str
    loaded_axis: int
    traction: np.ndarray
    fix_box: list
    load_box: list
    metrics: dict = field(default_factory=dict)

    @property
    def analytic(self) -> bool:
        return self.reference.get("truth_source") == "analytic"


def load_case(case_id: str) -> Case:
    case = load_json(CASE_DIR / f"{case_id}.case.json")
    reference = load_json(REF_DIR / f"{case_id}.json")
    if len(case["bcs"]) != 1 or len(case["loads"]) != 1:
        raise RuntimeError(f"{case_id}: expected exactly one BC and one load")
    loaded_face = case["corpus"]["loaded_face"]
    return Case(
        case_id=case_id,
        case=case,
        reference=reference,
        step=ROOT / case["geometry"],
        family=case["corpus"]["family"],
        loaded_axis=AXES[loaded_face[0]],
        traction=np.asarray([float(v) for v in case["loads"][0]["traction"]], dtype=np.float64),
        fix_box=case["bcs"][0]["select"]["box"],
        load_box=case["loads"][0]["select"]["box"],
        metrics={m["name"]: m for m in reference["metrics"]},
    )


def probe_box_of(case: Case) -> list | None:
    for metric in case.metrics.values():
        select = metric.get("probe", {}).get("select")
        if select and "box" in select:
            return select["box"]
    return None


# --------------------------------------------------------------------------- #
# one rung
# --------------------------------------------------------------------------- #
def solve_rung(
    case: Case,
    work: Path,
    h: float,
    curvature_points: int,
    include_wall_strip: bool,
    keep: bool,
    mesh_only: bool = False,
) -> dict:
    stem = f"{case.case_id}-h{h:.6g}-c{curvature_points}"
    row: dict = {
        "case_id": case.case_id,
        "h": h,
        "curvature_points": curvature_points,
        "status": "ok",
    }
    probe_box = probe_box_of(case)
    refine = None
    if probe_box is not None:
        # Resolve the stress-concentration feature: quarter of the global size in
        # the metric's own probe box.
        refine = (probe_box, h / PROBE_BOX_SIZE_DIVISOR)
    msh, mesh_run, optimize = gmsh_mesh(
        work, stem, case.step, h, curvature_points, refine
    )
    row["wall_mesh_s"] = round(mesh_run.wall, 2)
    row["gmsh_high_order_optimize"] = optimize
    if not mesh_run.ok:
        row.update(status="mesh-failed", error=(mesh_run.stderr or mesh_run.stdout)[-600:])
        return row

    nodes, tets = parse_msh2(msh)
    faces = free_faces(tets)
    area, centroid, normal = face_geometry(nodes, faces)

    # Load surface. The primary rule is the case's own specification, exactly as
    # apps/testlab applies it: free faces whose centroid is inside the load box,
    # kept when |n.t_hat| > select.normal_min_dot, with normal_min_dot = -1
    # meaning "no normal filter" and an empty filter falling back to box-only.
    # The alternative interpretation (only the loaded CAD end face, which is what
    # the closed-form references assume) is reported alongside so any divergence
    # is visible instead of silently baked into a truth value.
    sel = in_box(centroid, case.load_box)
    axis_dot = np.abs(normal[:, case.loaded_axis])
    end_face = sel & (axis_dot > FACE_NORMAL_MIN_DOT)
    min_dot = float(case.case["loads"][0]["select"].get("normal_min_dot", FACE_NORMAL_MIN_DOT))
    traction_norm = float(np.linalg.norm(case.traction))
    if min_dot > -1.0 and traction_norm > 0.0:
        t_hat = case.traction / traction_norm
        case_rule = sel & (np.abs(normal @ t_hat) > min_dot)
        if not case_rule.any():
            case_rule = sel
    else:
        case_rule = sel
    chosen = sel if include_wall_strip else case_rule
    if not chosen.any():
        row.update(status="empty-load-selection")
        return row

    load_faces = faces[chosen]
    loads = consistent_face_loads(nodes, load_faces, case.traction)
    # Fixture nodes: BOUNDARY nodes inside the fix box, which is what the engine
    # applies (fea::boundary_nodes_within, reached from apps/cli select_end). The
    # rule used to be "every node inside the box", and on these corpus slabs that
    # is a different problem, not a different discretisation of the same one: an
    # element whose nodes all fall inside a slab is strain-free, so the volume
    # rule embeds a rigid inclusion the engine no longer creates. Comparing a
    # CalculiX run under one rule against a PolyMesh run under the other would
    # report a mesh/solver discrepancy that is really a BC discrepancy.
    boundary_nodes = np.unique(faces.reshape(-1))
    on_boundary = np.zeros(nodes.shape[0], dtype=bool)
    on_boundary[boundary_nodes] = True
    fixed = np.flatnonzero(in_box(nodes, case.fix_box) & on_boundary)
    if fixed.size < 3:
        row.update(status="empty-fix-selection")
        return row

    probe_elements = None
    if probe_box is not None:
        element_centroid = nodes[tets[:, :4]].mean(axis=1)
        probe_elements = np.flatnonzero(in_box(element_centroid, probe_box))

    resultant = loads.sum(axis=0)
    end_face_area = float(area[end_face].sum())
    chosen_area = float(area[chosen].sum())
    row.update(
        n_nodes=int(nodes.shape[0]),
        n_elems=int(tets.shape[0]),
        n_dof=int(3 * (nodes.shape[0] - fixed.size)),
        n_fixed_nodes=int(fixed.size),
        n_load_faces=int(chosen.sum()),
        load_rule="box+normal_min_dot" if not include_wall_strip else "box-only",
        normal_min_dot=min_dot,
        load_area_m2=chosen_area,
        end_face_area_m2=end_face_area,
        box_only_area_m2=float(area[sel].sum()),
        end_face_vs_selected_rel_diff=(
            abs(chosen_area - end_face_area) / chosen_area if chosen_area > 0.0 else None
        ),
        resultant_N=[float(v) for v in resultant],
        resultant_mag_N=float(np.linalg.norm(resultant)),
    )
    expected_area = case.case["loads"][0]["select"].get("expected_area")
    if expected_area:
        row["load_area_rel_err_vs_cad"] = float(
            abs(row["load_area_m2"] - expected_area) / expected_area
        )
    if mesh_only:
        row["status"] = "mesh-only"
        if not keep:
            for suffix in (".msh", ".geo"):
                (work / f"{stem}{suffix}").unlink(missing_ok=True)
        return row

    deck = work / f"{stem}.inp"
    face_nodes = np.unique(load_faces)
    write_deck(
        deck, nodes, tets, fixed, loads, face_nodes, probe_elements, case.case["material"],
        f"external truth {case.case_id} h={h:.6g}",
    )
    ccx_run = run([str(CCX), stem], work, CCX_TIMEOUT_S)
    row["wall_solve_s"] = round(ccx_run.wall, 2)
    dat = work / f"{stem}.dat"
    if not ccx_run.ok or not dat.is_file():
        tail = (ccx_run.stdout + ccx_run.stderr)[-800:]
        row.update(status="solve-failed", error=tail)
        return row

    blocks = parse_dat(dat)
    u_rows = dat_block(blocks, "displacements")
    energy_rows = dat_block(blocks, "internal energy")
    rf_rows = dat_block(blocks, "total force")
    if u_rows is None:
        row.update(status="no-displacements", error=(ccx_run.stdout)[-600:])
        return row

    u = np.zeros_like(nodes)
    idx = u_rows[:, 0].astype(np.int64) - 1
    u[idx] = u_rows[:, 1:4]

    # strain energy: CalculiX's own element internal energy, and the work of the
    # applied load (1/2 f.u), which must agree for a linear static solve.
    work_energy = 0.5 * float(np.sum(loads * u))
    row["strain_energy_work_J"] = work_energy
    if energy_rows is not None and energy_rows.size:
        row["strain_energy_ccx_J"] = float(energy_rows[:, -1].sum())
        denom = abs(row["strain_energy_ccx_J"]) or 1.0
        row["strain_energy_rel_gap"] = abs(row["strain_energy_ccx_J"] - work_energy) / denom
    if rf_rows is not None and rf_rows.size:
        reaction = rf_rows[-1, -3:]
        row["reaction_N"] = [float(v) for v in reaction]
        row["equilibrium_rel_err"] = float(
            np.linalg.norm(reaction + resultant) / max(np.linalg.norm(resultant), 1e-30)
        )

    # tip deflection: mean |u| over the corner nodes of the loaded faces
    corner_nodes = np.unique(load_faces[:, :3])
    row["n_probe_nodes"] = int(corner_nodes.size)
    row["tip_deflection_m"] = float(np.linalg.norm(u[corner_nodes], axis=1).mean())

    if probe_box is not None:
        frd = parse_frd_nodal(work / f"{stem}.frd", "STRESS")
        if frd is None:
            row.update(status="no-stress")
        else:
            ids, values = frd
            vm = von_mises(values)
            keep_mask = in_box(nodes[ids - 1], probe_box)
            if keep_mask.any():
                row["peak_von_mises_Pa"] = float(vm[keep_mask].max())
                row["n_stress_nodes_in_box"] = int(keep_mask.sum())
            else:
                row.update(status="empty-stress-probe")
        disp = parse_frd_nodal(work / f"{stem}.frd", "DISP")
        if disp is not None:  # validates the .frd reader against the .dat print
            ids_d, values_d = disp
            order = np.argsort(ids_d)
            sorted_ids = ids_d[order]
            printed = idx + 1
            hit = np.isin(printed, sorted_ids)
            if hit.any():
                slot = order[np.searchsorted(sorted_ids, printed[hit])]
                a = values_d[slot][:, :3]
                b = u[printed[hit] - 1]
                scale = max(float(np.abs(b).max()), 1e-30)
                row["frd_vs_dat_disp_rel_err"] = float(np.abs(a - b).max() / scale)

    if not keep:
        for suffix in (".msh", ".geo", ".inp", ".dat", ".frd", ".sta", ".cvg", ".12d"):
            (work / f"{stem}{suffix}").unlink(missing_ok=True)
    return row


def metric_values(case: Case, row: dict) -> dict:
    """External values for the metric names this case's reference carries."""
    out: dict = {}
    for name, metric in case.metrics.items():
        kind = metric["probe"]["kind"]
        if kind == "strain_energy":
            value = row.get("strain_energy_ccx_J", row.get("strain_energy_work_J"))
        elif kind == "tip_deflection":
            value = row.get("tip_deflection_m")
        elif kind == "peak_vm_over_nominal":
            peak = row.get("peak_von_mises_Pa")
            nominal = float(metric["probe"]["nominal"])
            value = None if peak is None else peak / nominal
        else:
            raise RuntimeError(f"{case.case_id}: unsupported probe kind {kind}")
        out[name] = value
    return out


def run_case(
    case_id: str,
    work: Path,
    h_rel_scale: float,
    include_wall_strip: bool,
    keep: bool,
    diag_cache: dict,
    mesh_only: bool = False,
) -> dict:
    case = load_case(case_id)
    key = case.step.stem
    if key not in diag_cache:
        diag_cache[key] = bbox_diagonal(work, case.step)
    diag = diag_cache[key]
    h_rel = H_REL_BY_FAMILY.get(case.family, DEFAULT_H_REL) * h_rel_scale
    t0 = time.perf_counter()
    rungs = []
    for factor, curvature in RUNGS:
        row = solve_rung(
            case, work, diag * h_rel * factor, curvature, include_wall_strip, keep, mesh_only
        )
        row["h_rel"] = h_rel * factor
        row["bbox_diagonal_m"] = diag
        row["values"] = metric_values(case, row) if row["status"] == "ok" else {}
        rungs.append(row)
        print(
            f"  {case_id} h_rel={row['h_rel']:.4f} c={curvature}: {row['status']}"
            f" dof={row.get('n_dof')} mesh={row.get('wall_mesh_s')}s"
            f" solve={row.get('wall_solve_s')}s values={row.get('values')}",
            flush=True,
        )
    out = {
        "case_id": case_id,
        "family": case.family,
        "truth_source_existing": case.reference.get("truth_source"),
        "analytic": case.analytic,
        "bbox_diagonal_m": diag,
        "rungs": rungs,
        "wall_total_s": round(time.perf_counter() - t0, 1),
    }
    ok_rungs = [r for r in rungs if r["status"] == "ok"]
    if len(ok_rungs) >= 2:
        coarse, fine = ok_rungs[-2], ok_rungs[-1]
        deltas = {}
        for name, value in fine["values"].items():
            prior = coarse["values"].get(name)
            deltas[name] = (
                None
                if prior in (None, 0.0) or value is None
                else abs(value - prior) / abs(prior)
            )
        out["values"] = fine["values"]
        out["convergence_delta"] = deltas
        out["convergence_delta_between"] = [coarse["h_rel"], fine["h_rel"]]
        out["status"] = "ok"
        if len(ok_rungs) >= 3:
            out["richardson"] = richardson(ok_rungs[-3:], RUNG_REFINEMENT_RATIO)
    else:
        out["status"] = rungs[-1]["status"]
    if case.analytic:
        errors = {}
        for name, metric in case.metrics.items():
            value = (out.get("values") or {}).get(name)
            truth = float(metric["value"])
            errors[name] = None if value is None else (value - truth) / truth
        out["analytic_rel_err"] = errors
    return out

# --------------------------------------------------------------------------- #
# CalculiX vs PolyMesh on one identical Gmsh mesh (solver-independence check)
# --------------------------------------------------------------------------- #
POLYMESH = ROOT / "build" / "apps" / "cli" / "polymesh.exe"


def parse_vtu_displacement(path: Path) -> tuple[np.ndarray, np.ndarray]:
    import xml.etree.ElementTree as ET

    root = ET.parse(path).getroot()
    points = root.find(".//Points/DataArray")
    disp = next(
        (a for a in root.findall(".//PointData/DataArray") if a.get("Name") == "displacement"),
        None,
    )
    if points is None or not points.text or disp is None or not disp.text:
        raise RuntimeError(f"{path.name}: missing points or displacement")
    p = np.fromstring(points.text, sep=" ").reshape(-1, 3)
    u = np.fromstring(disp.text, sep=" ").reshape(-1, 3)
    if p.shape != u.shape:
        raise RuntimeError(f"{path.name}: point/displacement mismatch")
    return p, u


def match_by_coordinates(reference: np.ndarray, other: np.ndarray) -> np.ndarray:
    """Index of every `reference` point inside `other` (identical mesh, any order)."""
    scale = max(float(np.abs(reference).max()), 1e-12)
    key = {}
    for index, point in enumerate(other):
        key[tuple(np.round(point / scale, 9))] = index
    out = np.empty(reference.shape[0], dtype=np.int64)
    for index, point in enumerate(reference):
        slot = key.get(tuple(np.round(point / scale, 9)))
        if slot is None:
            raise RuntimeError("meshes do not share node coordinates")
        out[index] = slot
    return out


def cross_check(case_id: str, work: Path, diag_cache: dict) -> dict:
    """Solve one identical Gmsh mesh with CalculiX and with the PolyMesh CLI."""
    case = load_case(case_id)
    if not POLYMESH.is_file():
        return {"case_id": case_id, "status": "polymesh-cli-missing"}
    key = case.step.stem
    if key not in diag_cache:
        diag_cache[key] = bbox_diagonal(work, case.step)
    diag = diag_cache[key]
    factor, curvature = RUNGS[-1]
    h = diag * H_REL_BY_FAMILY.get(case.family, DEFAULT_H_REL) * factor
    row = solve_rung(case, work, h, curvature, False, keep=True)
    if row["status"] != "ok":
        return {"case_id": case_id, "status": row["status"], "ccx": row}

    stem = f"{case.case_id}-h{h:.6g}-c{curvature}"
    msh = work / f"{stem}.msh"
    vtu = work / f"{stem}-polymesh.vtu"
    direction = case.traction / np.linalg.norm(case.traction)
    command = [
        str(POLYMESH), "solve", str(msh), "-o", str(vtu),
        "-h", f"{h:.17g}",
        "-E", f"{float(case.case['material']['E']):.17g}",
        "-nu", f"{float(case.case['material']['nu']):.17g}",
        "--fix-box", *[f"{float(v):.17g}" for corner in case.fix_box for v in corner],
        "--load-box", *[f"{float(v):.17g}" for corner in case.load_box for v in corner],
        "--load-dir", *[f"{v:.17g}" for v in direction],
        "--force", f"{row['resultant_mag_N']:.17g}",
    ]
    cli = run(command, work, CCX_TIMEOUT_S)
    out = {
        "case_id": case_id,
        "mesh": {"h": h, "h_rel": h / diag, "n_nodes": row["n_nodes"],
                 "n_elems": row["n_elems"], "n_dof": row["n_dof"]},
        "load": {"resultant_mag_N": row["resultant_mag_N"],
                 "load_area_m2": row["load_area_m2"],
                 "n_load_faces": row["n_load_faces"]},
        "calculix": {
            "tip_deflection_m": row["tip_deflection_m"],
            "strain_energy_J": row.get("strain_energy_ccx_J"),
            "wall_solve_s": row["wall_solve_s"],
        },
        "polymesh": {"wall_solve_s": round(cli.wall, 2)},
    }
    if not cli.ok:
        out["status"] = "polymesh-solve-failed"
        out["polymesh"]["error"] = (cli.stdout + cli.stderr)[-800:]
        return out
    for line in cli.stdout.splitlines():
        if line.startswith("load:"):
            out["polymesh"]["load_line"] = line.strip()

    nodes, tets = parse_msh2(msh)
    faces = free_faces(tets)
    _, centroid, normal = face_geometry(nodes, faces)
    min_dot = float(case.case["loads"][0]["select"].get("normal_min_dot", FACE_NORMAL_MIN_DOT))
    sel = in_box(centroid, case.load_box)
    if min_dot > -1.0:
        t_hat = case.traction / np.linalg.norm(case.traction)
        chosen = sel & (np.abs(normal @ t_hat) > min_dot)
        if not chosen.any():
            chosen = sel
    else:
        chosen = sel
    corners = np.unique(faces[chosen][:, :3])
    points, u_cli = parse_vtu_displacement(vtu)
    slot = match_by_coordinates(nodes[corners], points)
    tip_cli = float(np.linalg.norm(u_cli[slot], axis=1).mean())
    out["polymesh"]["tip_deflection_m"] = tip_cli
    # 1/2 f.u with the same applied load vector: PolyMesh prints no energy.
    loads = consistent_face_loads(nodes, faces[chosen], case.traction)
    all_slots = match_by_coordinates(nodes, points)
    out["polymesh"]["strain_energy_work_J"] = 0.5 * float(np.sum(loads * u_cli[all_slots]))
    ccx_tip = row["tip_deflection_m"]
    out["tip_deflection_rel_diff"] = abs(tip_cli - ccx_tip) / abs(ccx_tip)
    energy_ccx = row.get("strain_energy_ccx_J")
    if energy_ccx:
        out["strain_energy_rel_diff"] = abs(
            out["polymesh"]["strain_energy_work_J"] - energy_ccx
        ) / abs(energy_ccx)
    out["status"] = "ok"
    return out



# --------------------------------------------------------------------------- #
# reference promotion
# --------------------------------------------------------------------------- #
def uncertainty_terms(entry: dict, name: str, kind: str) -> dict:
    """Numerical uncertainty of one external metric value, from measurement only."""
    delta = (entry.get("convergence_delta") or {}).get(name)
    rich = (entry.get("richardson") or {}).get(name) or {}
    rich_error = rich.get("error_rel") if isinstance(rich, dict) else None
    candidates = [v for v in (delta, rich_error) if v is not None]
    discretisation = max(candidates) if candidates else None
    policy = MESH_POLICY_SENSITIVITY.get(kind, 0.0)
    extra = FAMILY_EXTRA_SENSITIVITY.get(entry.get("family"), {})
    extra_value = float(extra.get("value", 0.0))
    terms = {
        "rung_to_rung_delta": delta,
        "richardson_error_rel": rich_error,
        "richardson_order": rich.get("order") if isinstance(rich, dict) else None,
        "discretisation_rel": discretisation,
        "mesh_policy_rel": policy,
        "numerical_rel": (
            discretisation + policy + extra_value if discretisation is not None else None
        ),
    }
    if extra_value:
        terms["family_extra_rel"] = extra_value
        terms["family_extra_why"] = extra.get("why")
    return terms


def derive_tol(numerical: float | None, idealisation: float) -> tuple[float, str]:
    """tol = idealisation bias (full size) + coverage * numerical, floored."""
    if numerical is None:
        return TOL_FLOOR, "no numerical uncertainty available; tol floored"
        # unreachable for a converged case, kept explicit rather than implicit
    raw = idealisation + UNCERTAINTY_COVERAGE * numerical
    tol = round_up_2sf(max(raw, TOL_FLOOR))
    basis = (
        f"tol = max(idealisation_bias {idealisation:.4g} + {UNCERTAINTY_COVERAGE:g} x "
        f"numerical_uncertainty {numerical:.4g}, floor {TOL_FLOOR:g}) = {raw:.4g} "
        f"-> {tol:g} (rounded up, 2 s.f.). Numerical uncertainty is the measured "
        "rung-to-rung change (or the Richardson error estimate, whichever is larger) "
        "plus the measured sensitivity to the mesh sizing policy. The idealisation "
        "bias is the measured gap between the closed form and the converged 3D "
        "external solve, and is added at full size because it is a bias, not scatter."
    )
    return tol, basis


def base_provenance(entry: dict, converged_at: float) -> dict:
    rungs = [r for r in entry["rungs"] if r["status"] == "ok"]
    fine = rungs[-1]
    return {
        "pipeline": "gmsh-mesh -> calculix-solve -> probe",
        "independence": (
            "neither the PolyMesh mesher nor the PolyMesh solver takes part in this "
            "value; the geometry is read straight from the case's STEP file by Gmsh"
        ),
        "geometry_source": entry.get("geometry", "case STEP file (CAD), meshed by Gmsh"),
        "gmsh_version": GMSH_VERSION,
        "calculix_version": CCX_VERSION,
        "calculix_solver": "PaStiX direct sparse (Intel MKL BLAS), OMP_NUM_THREADS=1",
        "element": "C3D10 (quadratic tetrahedron)",
        "element_order": 2,
        "gmsh_high_order_optimize": fine["gmsh_high_order_optimize"],
        "rungs": [
            {
                "h_rel": r["h_rel"],
                "mesh_size_m": r["h"],
                "curvature_points_per_2pi": r["curvature_points"],
                "n_nodes": r["n_nodes"],
                "n_elems": r["n_elems"],
                "n_dof": r["n_dof"],
                "wall_mesh_s": r["wall_mesh_s"],
                "wall_solve_s": r["wall_solve_s"],
                "values": r["values"],
            }
            for r in rungs
        ],
        "refinement_ratio": RUNG_REFINEMENT_RATIO,
        "convergence_delta_between_h_rel": entry.get("convergence_delta_between"),
        "converged_at": converged_at,
        "load": {
            "definition": (
                "case traction (Pa) applied as energy-conjugate tri6 nodal loads over "
                "the free faces whose centroid lies in the case load box, kept when "
                "|n.t_hat| > select.normal_min_dot (the rule apps/testlab applies)"
            ),
            "normal_min_dot": fine.get("normal_min_dot"),
            "n_load_faces": fine["n_load_faces"],
            "load_area_m2": fine["load_area_m2"],
            "loaded_cad_end_face_area_m2": fine.get("end_face_area_m2"),
            "box_only_area_m2": fine.get("box_only_area_m2"),
            "end_face_vs_selected_rel_diff": fine.get("end_face_vs_selected_rel_diff"),
            "resultant_N": fine["resultant_N"],
        },
        "checks": {
            "calculix_internal_energy_vs_half_f_dot_u_rel_gap": fine.get(
                "strain_energy_rel_gap"
            ),
            "reaction_vs_applied_resultant_rel_err": fine.get("equilibrium_rel_err"),
            "frd_vs_dat_displacement_rel_err": fine.get("frd_vs_dat_disp_rel_err"),
        },
        "wall_time_s": entry["wall_total_s"],
    }


def mark_unvalidated(path: Path, reference: dict, entry: dict, why: str) -> None:
    """Keep the old truth, but say plainly that we could not validate it."""
    reference["external_validation"] = {
        "status": "failed",
        "reason": why,
        "pipeline": "gmsh-mesh -> calculix-solve -> probe",
        "gmsh_version": GMSH_VERSION,
        "calculix_version": CCX_VERSION,
        "attempted": datetime.now(timezone.utc).isoformat(),
        "note": (
            "this reference still holds its previous value, which was NOT produced by "
            "an independent tool; treat it as unvalidated until the external pipeline "
            "can represent this case"
        ),
        "rung_status": [r.get("status") for r in entry.get("rungs", [])],
    }
    dump_json(path, reference)


def write_references(
    results: list[dict], converged_at: float, approved: bool, audit_path: Path | None = None
) -> int:
    if not approved:
        print("refusing to write references without --approved", file=sys.stderr)
        return 2
    audit: list[dict] = []
    written = unvalidated = 0
    for entry in sorted(results, key=lambda e: e["case_id"]):
        case_id = entry["case_id"]
        path = REF_DIR / f"{case_id}.json"
        reference = load_json(path)
        deltas = entry.get("convergence_delta") or {}
        bad = {k: v for k, v in deltas.items() if v is None or v > converged_at}
        if entry.get("status") != "ok" or bad:
            why = (
                f"status={entry.get('status')}"
                if entry.get("status") != "ok"
                else f"two finest rungs disagree by {bad} (> converged_at {converged_at})"
            )
            mark_unvalidated(path, reference, entry, why)
            audit.append({"case_id": case_id, "action": "kept-old-value-marked-unvalidated",
                          "reason": why})
            unvalidated += 1
            continue

        provenance = base_provenance(entry, converged_at)
        was_analytic = reference.get("truth_source") == "analytic"
        rows = []
        for metric in reference["metrics"]:
            name = metric["name"]
            kind = metric["probe"]["kind"]
            external = entry["values"].get(name)
            if external is None:
                continue
            old_value, old_tol = float(metric["value"]), float(metric["tol"])
            terms = uncertainty_terms(entry, name, kind)
            metric_provenance = dict(provenance)

            if kind == "peak_vm_over_nominal" and entry["family"] == "box_hole":
                # Condition 1: the corpus "analytic 3.0" is the infinite-plate
                # Kirsch limit. These plates are finite width, so the citable value
                # is the Howland finite-width solution on the gross-section stress.
                inputs = metric.get("inputs", {})
                d_over_w = float(inputs["a_over_H"])
                ktn, ktg = howland_ktg(d_over_w)
                new_value = ktg
                idealisation = abs(external - ktg) / ktg
                source = "analytic-finite-width-howland"
                derivation = (
                    f"{HOWLAND_CITATION} Here d/W = {d_over_w:.6g}, so Ktn = {ktn:.6g} "
                    f"and Ktg = {ktg:.6g}, replacing the previous infinite-plate value "
                    f"{old_value:g}, which is {100.0 * (ktg / old_value - 1.0):+.2f}% away "
                    "and was passing its own tolerance by luck. Independently confirmed "
                    f"by Gmsh 4.13.1 + CalculiX 2.23 (C3D10): converged peak/nominal = "
                    f"{external:.6g}, i.e. {100.0 * idealisation:+.2f}% above the 2D "
                    "handbook value, which is the expected 3D thickness effect for this "
                    "plate (t/a of order 1). See provenance."
                )
                metric_provenance["closed_form"] = {
                    "citation": HOWLAND_CITATION,
                    "d_over_W": d_over_w,
                    "Ktn_net_section": ktn,
                    "Ktg_gross_section": ktg,
                    "previous_value": old_value,
                    "previous_basis": "Kirsch infinite plate (d/W -> 0)",
                }
                metric_provenance["external_confirmation"] = {
                    "converged_3d_value": external,
                    "gap_vs_closed_form_rel": idealisation,
                    "gap_explanation": (
                        "3D finite-thickness effect: the peak surface stress at "
                        "mid-thickness exceeds the 2D plane-stress solution"
                    ),
                    "richardson": (entry.get("richardson") or {}).get(name),
                }
            elif was_analytic:
                # Closed form stays the truth (ladder rank 1); the external solve
                # measures how far the idealisation sits from the 3D answer.
                new_value = old_value
                idealisation = abs(external - old_value) / abs(old_value)
                source = metric.get("source", "analytic")
                derivation = (
                    f"{metric.get('derivation', '').split(' Independently')[0]} "
                    "Independently confirmed by Gmsh 4.13.1 + CalculiX 2.23 (C3D10): "
                    f"external converged value {external:.6g}, "
                    f"{100.0 * (external / old_value - 1.0):+.2f}% versus this closed "
                    "form. The beam/plate idealisation is stiffer than the 3D solid, so "
                    "a small positive bias is expected; it is carried in tol."
                ).strip()
                metric_provenance["external_confirmation"] = {
                    "converged_3d_value": external,
                    "gap_vs_closed_form_rel": idealisation,
                    "richardson": (entry.get("richardson") or {}).get(name),
                }
            else:
                # No closed form: the external solve becomes the truth.
                new_value = external
                idealisation = 0.0
                source = TRUTH_SOURCE
                derivation = (
                    "Gmsh 4.13.1 CAD-conforming tet10 mesh of the case STEP file solved "
                    "by CalculiX 2.23 (C3D10, PaStiX direct); neither the PolyMesh "
                    "mesher nor the PolyMesh solver takes part in this value, replacing "
                    "the previous overkill-reference value that our own mesher produced. "
                    f"Measured rung-to-rung change {terms['rung_to_rung_delta']:.3g}; "
                    "see provenance for both rungs, DOF counts and the energy/equilibrium "
                    "checks."
                )
                metric.pop("source_run", None)

            tol, tol_basis = derive_tol(terms["numerical_rel"], idealisation)
            metric_provenance["uncertainty"] = {
                **terms,
                "idealisation_bias_rel": idealisation,
                "reference_uncertainty_rel": (terms["numerical_rel"] or 0.0) + idealisation,
                "tol_basis": tol_basis,
            }
            rows.append({
                "metric": name,
                "old_value": old_value,
                "new_value": new_value,
                "change_pct": 100.0 * (new_value / old_value - 1.0) if old_value else None,
                "old_tol": old_tol,
                "new_tol": tol,
                "reference_uncertainty_rel": metric_provenance["uncertainty"][
                    "reference_uncertainty_rel"
                ],
                "external_value": external,
                "source": source,
            })
            metric["value"] = new_value
            metric["tol"] = tol
            metric["source"] = source
            metric["derivation"] = derivation
            metric["provenance"] = metric_provenance

        if not rows:
            mark_unvalidated(path, reference, entry, "no external value for any metric")
            audit.append({"case_id": case_id, "action": "kept-old-value-marked-unvalidated",
                          "reason": "no external value for any metric"})
            unvalidated += 1
            continue

        if not was_analytic:
            reference["truth_source"] = TRUTH_SOURCE
        reference["external_validation"] = {
            "status": "ok",
            "pipeline": "gmsh-mesh -> calculix-solve -> probe",
            "gmsh_version": GMSH_VERSION,
            "calculix_version": CCX_VERSION,
            "generated": datetime.now(timezone.utc).isoformat(),
        }
        caveat = (entry["rungs"][-1] or {}).get("end_face_vs_selected_rel_diff")
        if caveat is not None and caveat > 0.05:
            reference["external_validation"]["load_selection_caveat"] = (
                f"the case's box+normal_min_dot rule loads a surface whose area differs "
                f"by {100.0 * caveat:.1f}% from the loaded CAD end face alone; the value "
                "above solves the case exactly as specified, but the selection itself is "
                "worth review"
            )
        dump_json(path, reference)
        written += 1
        audit.append({"case_id": case_id, "family": entry["family"],
                      "action": "external-truth-written" if not was_analytic
                      else "closed-form-retained-tol-and-provenance-updated",
                      "metrics": rows,
                      "wall_time_s": entry["wall_total_s"],
                      "n_dof_fine": entry["rungs"][-1]["n_dof"]})

    print(f"\nwrote {written} references; {unvalidated} kept old values and were marked "
          "unvalidated")
    print(f"\n{'case':<22}{'metric':<15}{'old truth':>13}{'new truth':>13}{'change':>9}"
          f"{'old tol':>9}{'new tol':>9}{'ref unc':>9}")
    for row in audit:
        if row["action"].startswith("kept-old"):
            print(f"{row['case_id']:<22}{'-- NOT VALIDATED: ' + row['reason'][:60]}")
            continue
        for m in row["metrics"]:
            change = f"{m['change_pct']:+.2f}%" if m["change_pct"] is not None else "n/a"
            print(f"{row['case_id']:<22}{m['metric']:<15}{m['old_value']:>13.6g}"
                  f"{m['new_value']:>13.6g}{change:>9}{m['old_tol']:>9.3g}"
                  f"{m['new_tol']:>9.3g}{m['reference_uncertainty_rel']:>9.2g}")
    if audit_path is not None:
        dump_json(audit_path, {"converged_at": converged_at, "written": written,
                               "unvalidated": unvalidated, "audit": audit})
        print("\nwrote audit", audit_path)
    return 0


def audit_against_git(revision: str, audit_path: Path | None) -> int:
    """Rebuild the before/after table from a git revision and the working tree.

    The promotion itself is what writes the references; this reconstructs the
    audit from the two actual file states, so it can be re-run at any time and
    never depends on the promotion's own bookkeeping.
    """
    rows = []
    for path in sorted(REF_DIR.glob("*.json")):
        relative = path.relative_to(ROOT).as_posix()
        show = subprocess.run(
            ["git", "show", f"{revision}:{relative}"], cwd=ROOT, capture_output=True, text=True
        )
        if show.returncode != 0:
            rows.append({"case_id": path.stem, "action": "new-file-not-in-" + revision})
            continue
        before, after = json.loads(show.stdout), load_json(path)
        metrics = []
        for old, new in zip(before["metrics"], after["metrics"]):
            provenance = new.get("provenance", {})
            uncertainty = provenance.get("uncertainty", {})
            metrics.append({
                "metric": new["name"],
                "old_value": old["value"],
                "new_value": new["value"],
                "change_pct": (
                    100.0 * (new["value"] / old["value"] - 1.0) if old["value"] else None
                ),
                "old_tol": old["tol"],
                "new_tol": new["tol"],
                "old_source": old.get("source"),
                "new_source": new.get("source"),
                "reference_uncertainty_rel": uncertainty.get("reference_uncertainty_rel"),
                "rung_to_rung_delta": uncertainty.get("rung_to_rung_delta"),
                "richardson_error_rel": uncertainty.get("richardson_error_rel"),
                "idealisation_bias_rel": uncertainty.get("idealisation_bias_rel"),
                "n_dof_fine": (provenance.get("rungs") or [{}])[-1].get("n_dof"),
                "wall_time_s": provenance.get("wall_time_s"),
            })
        rows.append({
            "case_id": path.stem,
            "family": after.get("family"),
            "old_truth_source": before.get("truth_source"),
            "new_truth_source": after.get("truth_source"),
            "external_validation": after.get("external_validation", {}).get("status"),
            "load_selection_caveat": (
                after.get("external_validation", {}).get("load_selection_caveat") is not None
            ),
            "metrics": metrics,
        })
    print(f"{'case':<22}{'metric':<15}{'old truth':>13}{'new truth':>13}{'change':>9}"
          f"{'old tol':>9}{'new tol':>9}{'ref unc':>9}{'dof':>9}")
    for row in rows:
        for m in row.get("metrics", []):
            change = f"{m['change_pct']:+.2f}%" if m["change_pct"] is not None else "n/a"
            unc = m["reference_uncertainty_rel"]
            print(f"{row['case_id']:<22}{m['metric']:<15}{m['old_value']:>13.6g}"
                  f"{m['new_value']:>13.6g}{change:>9}{m['old_tol']:>9.3g}"
                  f"{m['new_tol']:>9.3g}{(f'{unc:.2g}' if unc else '-'):>9}"
                  f"{(m['n_dof_fine'] or 0):>9}")
    if audit_path is not None:
        dump_json(audit_path, {"baseline_revision": revision, "cases": rows})
        print("\nwrote audit", audit_path)
    return 0

# --------------------------------------------------------------------------- #
# evidence artifact: the measurements behind the trust claims, per rung
# --------------------------------------------------------------------------- #
def load_area_evidence(entry: dict) -> dict:
    """Per-rung loaded-area record so area stability is derivable, not asserted."""
    case = load_json(CASE_DIR / f"{entry['case_id']}.case.json")
    expected = case["loads"][0]["select"].get("expected_area")
    rungs = []
    for row in entry.get("rungs", []):
        if row.get("status") not in ("ok", "mesh-only"):
            rungs.append({"h_rel": row.get("h_rel"), "status": row.get("status")})
            continue
        selected = row["load_area_m2"]
        end_face = row.get("end_face_area_m2")
        box_only = row.get("box_only_area_m2")
        rungs.append({
            "h_rel": row["h_rel"],
            "mesh_size_m": row["h"],
            "curvature_points_per_2pi": row["curvature_points"],
            "n_dof": row.get("n_dof"),
            "n_load_faces": row["n_load_faces"],
            "normal_min_dot": row.get("normal_min_dot"),
            "selected_area_m2": selected,
            "cad_end_face_area_m2": end_face,
            "box_only_area_m2": box_only,
            "wall_ring_area_m2": (
                None if (box_only is None or end_face is None) else box_only - end_face
            ),
            "wall_ring_over_end_face": (
                None if not end_face else (box_only - end_face) / end_face
            ),
            "selected_vs_cad_expected_rel_err": (
                None if not expected else abs(selected - expected) / expected
            ),
            "resultant_mag_N": row.get("resultant_mag_N"),
        })
    areas = [r["selected_area_m2"] for r in rungs if r.get("selected_area_m2")]
    drift = (
        max(abs(areas[k] - areas[k - 1]) / areas[k - 1] for k in range(1, len(areas)))
        if len(areas) > 1 else None
    )
    return {
        "case_id": entry["case_id"],
        "family": entry.get("family"),
        "cad_expected_area_m2": expected,
        "traction_Pa": [float(v) for v in case["loads"][0]["traction"]],
        "rungs": rungs,
        "max_between_rung_area_drift": drift,
        "derivation": "drift = max over consecutive rungs of |A_k - A_(k-1)| / A_(k-1); "
        "selected_vs_cad_expected_rel_err = |A_selected - expected_area| / expected_area; "
        "wall_ring_over_end_face = (box_only_area - cad_end_face_area) / cad_end_face_area",
    }


def family_feature_report(work: Path, family: str) -> list[dict]:
    """Feature sizes per part, absolute and relative to the diagonal and each rung's h."""
    out = []
    seen: set[str] = set()
    for path in sorted(CASE_DIR.glob(f"{family}_s*_c*.case.json")):
        case = load_json(path)
        step = ROOT / case["geometry"]
        if step.name in seen:
            continue
        seen.add(step.name)
        features = cad_feature_sizes(work, step)
        diagonal = features["bbox_diagonal_m"]
        h_rel = H_REL_BY_FAMILY.get(family, DEFAULT_H_REL)
        radii = [c["radius_m"] for c in features["circular_edges"]]
        # distinct radii (a through hole contributes one circle per face)
        distinct: dict[float, int] = {}
        for r in radii:
            key = round(r, 9)
            distinct[key] = distinct.get(key, 0) + 1
        # distinct axes: dedupe circles sharing an (x, y) centre
        axes: list[dict] = []
        for circle in features["circular_edges"]:
            cx, cy, _ = circle["centre_m"]
            if not any(abs(a["centre_m"][0] - cx) < 1e-9 and abs(a["centre_m"][1] - cy) < 1e-9
                       for a in axes):
                axes.append(circle)
        derived: dict = {}
        if family == "tube" and len(distinct) >= 2:
            r_inner, r_outer = min(distinct), max(distinct)
            annulus = math.pi * (r_outer**2 - r_inner**2)
            expected = case["loads"][0]["select"].get("expected_area")
            derived = {
                "r_outer_m": r_outer,
                "r_inner_m": r_inner,
                "wall_thickness_m": r_outer - r_inner,
                "wall_thickness_over_diagonal": (r_outer - r_inner) / diagonal,
                "annulus_area_from_measured_radii_m2": annulus,
                "case_expected_area_m2": expected,
                "annulus_vs_expected_rel_err": (
                    None if not expected else abs(annulus - expected) / expected
                ),
                "verification": "pi*(r_outer^2 - r_inner^2) from radii measured off the "
                "meshed CAD edges, against the expected_area the case file declares",
            }
        elif family == "perforated_plate" and axes:
            gaps = []
            for a in range(len(axes)):
                for b in range(a + 1, len(axes)):
                    ca, cb = axes[a]["centre_m"], axes[b]["centre_m"]
                    distance = math.dist((ca[0], ca[1]), (cb[0], cb[1]))
                    gaps.append(distance - axes[a]["radius_m"] - axes[b]["radius_m"])
            clearance = []
            for circle in axes:
                cx, cy, _ = circle["centre_m"]
                r = circle["radius_m"]
                clearance += [
                    cx - features["bbox_lo_m"][0] - r, features["bbox_hi_m"][0] - cx - r,
                    cy - features["bbox_lo_m"][1] - r, features["bbox_hi_m"][1] - cy - r,
                ]
            derived = {
                "n_holes": len(axes),
                "hole_radius_m": min(distinct) if distinct else None,
                "min_ligament_between_holes_m": min(gaps) if gaps else None,
                "min_hole_to_free_face_clearance_m": min(clearance) if clearance else None,
                "plate_thickness_m": features["extents_m"][2],
            }
        # Ratios are only meaningful for LENGTHS, so they are built from the
        # fields whose names end in _m; counts and areas are left alone.
        ratios = {}
        for name, value in list(derived.items()) + [
            ("shortest_cad_edge_m", features["shortest_cad_edge_m"])
        ]:
            if not name.endswith("_m") or not isinstance(value, (int, float)) or not value:
                continue
            base = name[:-2]
            ratios[f"{base}_over_diagonal"] = value / diagonal
            for factor, curvature in RUNGS:
                h = diagonal * h_rel * factor
                ratios[f"{base}_over_h_rung_c{curvature}"] = value / h
        out.append({
            "part": step.stem,
            "family": family,
            "features": features,
            "distinct_circle_radii_m": {str(r): n for r, n in sorted(distinct.items())},
            "derived": derived,
            "rungs": [
                {
                    "h_rel": h_rel * factor,
                    "mesh_size_max_m": diagonal * h_rel * factor,
                    "mesh_size_min_m": diagonal * h_rel * factor / MESH_SIZE_MIN_DIVISOR,
                    "curvature_points_per_2pi": curvature,
                    "curvature_size_on_smallest_circle_m": (
                        2.0 * math.pi * min(radii) / curvature if radii else None
                    ),
                }
                for factor, curvature in RUNGS
            ],
            "ratios": ratios,
        })
    return out


def feature_resolution_summary(rows: list[dict]) -> dict:
    """Every measured length against each rung's element size, per family.

    Reported for all lengths rather than picking one "binding" feature, so no
    judgement is baked in. The direction is stated explicitly because it is easy
    to invert: length/h BELOW 1.0 means the feature is SMALLER than a single
    element at that rung (under-resolved, and where an engine should refuse);
    ABOVE 1.0 means the feature spans more than one element.
    """
    out: dict = {}
    by_family: dict[str, list[dict]] = {}
    for row in rows:
        by_family.setdefault(row["family"], []).append(row)
    for family, parts in sorted(by_family.items()):
        suffix = f"_over_h_rung_c{RUNGS[0][1]}"
        names = sorted(
            {key[: -len(suffix)] for part in parts for key in part["ratios"]
             if key.endswith(suffix)}
        )
        features = {}
        for name in names:
            per_rung = {}
            for _, curvature in RUNGS:
                key = f"{name}_over_h_rung_c{curvature}"
                values = {part["part"]: part["ratios"][key] for part in parts
                          if key in part["ratios"]}
                if not values:
                    continue
                per_rung[f"rung_c{curvature}"] = {
                    "min_over_h": min(values.values()),
                    "max_over_h": max(values.values()),
                    "parts_smaller_than_one_element": sorted(
                        p for p, v in values.items() if v < 1.0
                    ),
                    "parts_larger_than_one_element": sorted(
                        p for p, v in values.items() if v >= 1.0
                    ),
                    "per_part_over_h": dict(sorted(values.items())),
                }
            if not per_rung:
                continue
            everything = [v for rung in per_rung.values()
                          for v in rung["per_part_over_h"].values()]
            features[name] = {
                "range_over_h_all_parts_and_rungs": [min(everything), max(everything)],
                "by_rung": per_rung,
            }
        if not features:
            continue
        smallest = min(features, key=lambda n: features[n]["range_over_h_all_parts_and_rungs"][0])
        out[family] = {
            "direction": "length/h < 1.0 means the feature is SMALLER than one element at "
            "that rung (under-resolved); > 1.0 means it spans more than one element",
            "smallest_feature_relative_to_h": smallest,
            "features": features,
        }
    return out



def evidence_stage(result_paths: list[Path], families: list[str], work: Path,
                   out_path: Path, allow_partial: bool = False) -> int:
    # Validate every input BEFORE touching the artifact. An evidence file that
    # reports success on an incomplete input set is the same defect as a load-area
    # gate that reports 0.0 when it cannot establish an expected area.
    if not result_paths:
        print("error: --stage evidence requires at least one --results file; "
              "refusing to write anything", file=sys.stderr)
        return 2
    missing_files = [p for p in result_paths if not Path(p).is_file()]
    if missing_files:
        print(f"error: --results file(s) not found: {', '.join(str(p) for p in missing_files)}",
              file=sys.stderr)
        return 2
    merged: dict[str, dict] = {}
    for path in result_paths:
        payload = load_json(path)
        if not isinstance(payload.get("cases"), list):
            print(f"error: {path}: no 'cases' array; refusing to write anything",
                  file=sys.stderr)
            return 2
        for entry in payload["cases"]:
            previous = merged.get(entry["case_id"])
            if previous is None or len(entry.get("rungs", [])) >= len(previous.get("rungs", [])):
                merged[entry["case_id"]] = entry
    expected = set(all_case_ids())
    absent = sorted(expected - set(merged))
    if absent and not allow_partial:
        print(f"error: results cover {len(merged)} of {len(expected)} corpus cases; "
              f"missing {len(absent)}: {', '.join(absent)}", file=sys.stderr)
        print("the raw output for the analytic stepped_shaft *_c1 cases lives in "
              "bench/reference/external/external-truth-validate.json, which must be passed "
              "as a --results input alongside results/raw-*.json", file=sys.stderr)
        print("refusing to write a partial artifact; pass --allow-partial to override",
              file=sys.stderr)
        return 2
    areas = [load_area_evidence(entry) for entry in sorted(merged.values(),
                                                           key=lambda e: e["case_id"])]
    if not areas:
        print("error: no cases survived parsing; refusing to write an empty artifact",
              file=sys.stderr)
        return 2
    features = [row for family in families for row in family_feature_report(work, family)]
    drifts = [a["max_between_rung_area_drift"] for a in areas
              if a["max_between_rung_area_drift"] is not None]
    errors = [r["selected_vs_cad_expected_rel_err"] for a in areas for r in a["rungs"]
              if r.get("selected_vs_cad_expected_rel_err") is not None]
    rings: dict[str, list[float]] = {}
    for a in areas:
        values = [r["wall_ring_over_end_face"] for r in a["rungs"]
                  if r.get("wall_ring_over_end_face") is not None]
        if values:
            rings.setdefault(a["family"], []).extend(values)
    payload = {
        "purpose": "The per-rung measurements behind the load-stability and feature-size "
        "claims, so a reviewer can re-derive them instead of taking prose on trust.",
        "pipeline": "gmsh-mesh -> calculix-solve -> probe",
        "gmsh_version": GMSH_VERSION,
        "calculix_version": CCX_VERSION,
        "complete": not absent,
        "cases_missing_from_inputs": absent,
        "summary": {
            "cases": len(areas),
            "corpus_cases": len(expected),
            "max_between_rung_area_drift": max(drifts) if drifts else None,
            "max_selected_vs_cad_expected_rel_err": max(errors) if errors else None,
            "wall_ring_over_end_face_by_family": {
                family: {"min": min(values), "max": max(values), "n_rungs": len(values)}
                for family, values in sorted(rings.items())
            },
            "feature_resolution_by_family": feature_resolution_summary(features),
        },
        "per_case_loaded_areas": areas,
        "feature_sizes": features,
    }
    # The content hash covers the payload with the hash field and the wall-clock
    # stamp excluded, so "regenerates identically" is a checkable claim.
    body = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    payload["content_sha256"] = hashlib.sha256(body.encode("utf-8")).hexdigest()
    payload["content_sha256_covers"] = (
        "sha256 of this payload serialised with sort_keys=True and compact separators, "
        "excluding content_sha256, content_sha256_covers and generated"
    )
    payload["generated"] = datetime.now(timezone.utc).isoformat()
    dump_json(out_path, payload)
    summary = payload["summary"]
    print(f"cases recorded: {summary['cases']} of {summary['corpus_cases']}"
          f"{'' if payload['complete'] else ' (PARTIAL)'}")
    for label, key in (("max between-rung loaded-area drift", "max_between_rung_area_drift"),
                       ("max selected-vs-CAD area error", "max_selected_vs_cad_expected_rel_err")):
        value = summary[key]
        print(f"{label}: {'n/a' if value is None else format(value, '.3e')}")
    for family, stats in summary["wall_ring_over_end_face_by_family"].items():
        print(f"wall-ring/end-face {family:<18} min {stats['min']:.3e} max {stats['max']:.3e}"
              f" over {stats['n_rungs']} rungs")
    finest = f"rung_c{RUNGS[-1][1]}"
    for family, stats in summary["feature_resolution_by_family"].items():
        for name, feature in sorted(stats["features"].items()):
            low, high = feature["range_over_h_all_parts_and_rungs"]
            under = feature["by_rung"][finest]["parts_smaller_than_one_element"]
            print(f"feature/h {family:<17}{name:<34}{low:.2f}-{high:.2f} x h"
                  + (f"; SMALLER than one element at the finest rung on {', '.join(under)}"
                     if under else "; at least one element everywhere at the finest rung"))
    print(f"content_sha256: {payload['content_sha256']}")
    print("wrote", out_path)
    return 0



# --------------------------------------------------------------------------- #
def analytic_case_ids() -> list[str]:
    return sorted(
        path.stem
        for path in REF_DIR.glob("*.json")
        if load_json(path).get("truth_source") == "analytic"
    )


def all_case_ids() -> list[str]:
    return sorted(path.name[: -len(".case.json")] for path in CASE_DIR.glob("*.case.json"))


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--stage",
                   choices=("validate", "generate", "crosscheck", "write", "audit", "evidence"),
                   default="validate")
    p.add_argument("--case", action="append", help="run only this case id (repeatable)")
    p.add_argument("--family", action="append", help="run only this family (repeatable)")
    p.add_argument("--h-scale", type=float, default=1.0, help="scale both rung sizes")
    p.add_argument("--include-wall-strip", action="store_true",
                   help="load every free face in the load box, not just the end face")
    p.add_argument("--converged-at", type=float, default=0.02,
                   help="max accepted relative change between the two finest rungs")
    p.add_argument("--rungs", type=int, default=2, choices=(2, 3),
                   help="number of refinement rungs; 3 enables Richardson extrapolation")
    p.add_argument("--out", type=Path, help="results JSON path")
    p.add_argument("--results", type=Path, action="append",
                   help="results JSON to promote (--stage write); repeatable, merged")
    p.add_argument("--audit", type=Path, help="write the before/after audit here")
    p.add_argument("--approved", action="store_true", help="allow --stage write to edit references")
    p.add_argument("--mesh-only", action="store_true",
                   help="mesh and report size/selection stats without solving (cost probe)")
    p.add_argument("--keep", action="store_true", help="keep meshes and decks")
    p.add_argument("--allow-partial", action="store_true",
                   help="let --stage evidence write an artifact that does not cover every "
                        "corpus case; the artifact records complete=false and what is missing")
    p.add_argument("--baseline", default="HEAD",
                   help="git revision the --stage audit table compares against")
    p.add_argument("--work", type=Path, help="work directory (default: TEMP/polymesh-truth)")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    global RUNGS
    if args.stage == "audit":
        return audit_against_git(
            args.baseline, args.audit or OUT_DIR / "external-truth-audit.json"
        )
    RUNGS = RUNGS_ALL[: args.rungs]
    if args.stage == "evidence":
        work = args.work or Path(os.environ.get("TEMP", "/tmp")) / "polymesh-truth"
        work.mkdir(parents=True, exist_ok=True)
        return evidence_stage(
            args.results or [], args.family or [], work,
            args.out or OUT_DIR / "external-truth-load-evidence.json",
            allow_partial=args.allow_partial,
        )
    RUNGS = RUNGS_ALL[: args.rungs]
    if args.stage == "write":
        merged: dict[str, dict] = {}
        for path in args.results or []:
            for entry in load_json(path)["cases"]:
                previous = merged.get(entry["case_id"])
                # more rungs wins, then the later file wins
                if previous is None or len(entry.get("rungs", [])) >= len(
                    previous.get("rungs", [])
                ):
                    merged[entry["case_id"]] = entry
        if not merged:
            print("error: --stage write needs at least one --results file", file=sys.stderr)
            return 2
        print(f"promoting {len(merged)} cases from {len(args.results)} results file(s)")
        return write_references(
            list(merged.values()), args.converged_at, args.approved,
            args.audit or OUT_DIR / "external-truth-audit.json",
        )

    for tool, path in (("gmsh", GMSH), ("ccx_static", CCX)):
        if not path.is_file():
            print(f"error: {tool} not found at {path}", file=sys.stderr)
            return 1
    if args.stage == "crosscheck":
        if not args.case:
            print("error: --stage crosscheck needs --case", file=sys.stderr)
            return 2
        work = args.work or Path(os.environ.get("TEMP", "/tmp")) / "polymesh-truth"
        work.mkdir(parents=True, exist_ok=True)
        diag_cache: dict[str, float] = {}
        rows = [cross_check(case_id, work, diag_cache) for case_id in args.case]
        out_path = args.out or OUT_DIR / "external-truth-crosscheck.json"
        dump_json(out_path, {
            "pipeline": "one identical gmsh mesh solved by calculix and by the polymesh CLI",
            "gmsh_version": GMSH_VERSION,
            "calculix_version": CCX_VERSION,
            "cases": rows,
        })
        for entry in rows:
            print(json.dumps(entry, indent=2))
        print("wrote", out_path)
        return 0


    if args.case:
        cases = list(dict.fromkeys(args.case))
    elif args.stage == "validate":
        cases = analytic_case_ids()
    else:
        analytic = set(analytic_case_ids())
        cases = [c for c in all_case_ids() if c not in analytic]
    if args.family:
        families = set(args.family)
        cases = [c for c in cases if load_case(c).family in families]

    work = args.work or Path(os.environ.get("TEMP", "/tmp")) / "polymesh-truth"
    work.mkdir(parents=True, exist_ok=True)
    out_path = args.out or OUT_DIR / f"external-truth-{args.stage}.json"

    print(f"{len(cases)} case(s); work={work}")
    started = datetime.now(timezone.utc).isoformat()
    t0 = time.perf_counter()
    diag_cache: dict[str, float] = {}
    results = []
    for case_id in cases:
        print(f"[{len(results) + 1}/{len(cases)}] {case_id}", flush=True)
        try:
            results.append(
                run_case(case_id, work, args.h_scale, args.include_wall_strip, args.keep,
                         diag_cache, args.mesh_only)
            )
        except (OSError, RuntimeError, KeyError, ValueError) as error:
            print(f"  {case_id}: pipeline error: {error}", flush=True)
            results.append({"case_id": case_id, "status": "pipeline-error", "error": str(error)})
        dump_json(out_path, {
            "schema_version": 1,
            "pipeline": "gmsh-mesh -> calculix-solve -> probe",
            "gmsh_version": GMSH_VERSION,
            "calculix_version": CCX_VERSION,
            "truth_source": TRUTH_SOURCE,
            "rungs": [{"h_rel_factor": f, "curvature_points": c} for f, c in RUNGS],
            "include_wall_strip": args.include_wall_strip,
            "started": started,
            "wall_total_s": round(time.perf_counter() - t0, 1),
            "cases": results,
        })

    print(f"\nwrote {out_path}  ({time.perf_counter() - t0:.1f} s total)")
    report(results, args.converged_at)
    return 0


def report(results: list[dict], converged_at: float) -> None:
    print(f"\n{'case':<22}{'status':<16}{'dof':>9}{'wall_s':>9}  values / analytic error")
    for entry in results:
        fine = (entry.get("rungs") or [{}])[-1]
        values = entry.get("values") or {}
        text = ", ".join(
            f"{k}={v:.6g}" if isinstance(v, float) else f"{k}=None" for k, v in values.items()
        )
        errors = entry.get("analytic_rel_err")
        if errors:
            text += " | err " + ", ".join(
                f"{k}={100.0 * v:+.2f}%" if v is not None else f"{k}=None"
                for k, v in errors.items()
            )
        deltas = entry.get("convergence_delta") or {}
        if deltas:
            text += " | delta " + ", ".join(
                f"{k}={100.0 * v:.2f}%" if v is not None else f"{k}=None"
                for k, v in deltas.items()
            )
            if any(v is None or v > converged_at for v in deltas.values()):
                text += "  NOT-CONVERGED"
        print(
            f"{entry['case_id']:<22}{entry.get('status', '?'):<16}"
            f"{fine.get('n_dof', 0):>9}{entry.get('wall_total_s', 0):>9}  {text}"
        )


if __name__ == "__main__":
    raise SystemExit(main())
