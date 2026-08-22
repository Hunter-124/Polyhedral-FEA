#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Real per-part geometric descriptors, computed offline from the STEP files.

    python scripts/advisor/geometry_features.py

Why
---
The advisor's 44-column input vector advertises geometric awareness it does not
have. Measured on the v3 corpus, ten of those columns are constant across all
3,456 rows, and the worst offender is ``curved_frac``, which is **1.0 in every
single row**: ``geom_class_of`` computes it as ``(ntri - 12) / ntri``
(``apps/testlab/main.cpp:1709``), which saturates to 1 for any real
triangulation. ``diag`` is 1.0 by construction, and ``kappa_max_h`` /
``kappa_mean_h`` are derived from the same tessellation.

That matters most for exactly the question the model was measured to fail:
matched-cost judgement. Asked "which mesher and order suit THIS geometry at a
fixed price", the model ranked below random. It could hardly do otherwise --
every part reports identical curvature, so there is no signal on which to prefer
``graded_tet`` on a curved part over ``hybrid_zoo`` on a prismatic one.

These descriptors are per-PART properties of the CAD, so they need no campaign
re-run and no C++ rebuild: they are computed here from the same OCCT kernel the
engine uses, via the OCP Python bindings, and joined into the dataset by part
name. Everything is scale-free -- divided by the bounding-box diagonal or
expressed as a fraction of total area -- so a part and a scaled copy of it get
the same descriptors, which is the property ``diag = 1.0`` was trying and
failing to express.

What is computed, and why each one
----------------------------------
``curved_area_frac``   fraction of surface AREA on non-planar faces. The honest
                       version of ``curved_frac``. Directly separates prismatic
                       parts from filleted or cylindrical ones.
``cyl_area_frac``,     area split by surface type. A part dominated by cylinders
``plane_area_frac``,   (holes, shafts) has different meshing needs from one
``other_area_frac``    dominated by spheres or freeform patches.
``min_curv_radius_rel``smallest cylinder/sphere radius over the bbox diagonal.
                       This is the feature that should drive ``h_rel``: it is the
                       length scale the mesh has to resolve.
``log_curv_radius_*``  spread of curvature radii, area-weighted. A part with one
                       tiny fillet needs local refinement; a uniformly curved
                       part needs global.
``n_faces``,           topology counts. Cheap, and they separate the families.
``n_edges``,
``n_shells``
``face_area_cv``       coefficient of variation of face areas: how uneven the
                       B-rep patchwork is.
``aspect_max``,        bounding-box aspect ratios -- the thin-wall/slenderness
``aspect_mid``         signal ``thin_min_over_diag`` was meant to carry.
``volume_frac``        solid volume over bbox volume: solidity/bulkiness.
``area_over_v23``      scale-free surface-area-to-volume, the classic
                       compactness descriptor.
``min_face_size_rel``  smallest face sqrt(area) over the bbox diagonal. The
                       scale at which the Cartesian fill can lose a feature
                       entirely, which the corrected engine now refuses to do --
                       so this is predictive of the new dominant failure mode.
                       Continuous rather than a count: a threshold count was
                       tried first and came out 0 for all 24 parts, which is the
                       same saturation defect as ``curved_frac``.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np

if __package__ in (None, ""):  # direct invocation
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    __package__ = "advisor"

from .dataset import ADVISOR_DIR, ROOT  # noqa: E402

STEP_DIR = ROOT / "bench" / "geometries" / "corpus" / "primitives"
FEATURES_CSV = ADVISOR_DIR / "geometry_features.csv"

#: A face whose sqrt(area) is below this fraction of the bbox diagonal counts as
#: a small feature -- the scale at which the Cartesian fill can lose it.
SMALL_FACE_FRACTION = 0.05

#: A face below this fraction of the total surface AREA is a "salient feature":
#: a hole wall, a boss wall, a rib flank, a fillet strip. Mirrors
#: ``kSalientFaceAreaFraction`` in ``src/pipeline/src/scene.cpp``.
SALIENT_FACE_AREA_FRACTION = 0.05

#: uv / curve sampling density. Mirrors ``kFeatureTopologySamples``: the C++
#: side asks ``geom::extract_topology`` for four interior stations per curve and
#: a 4x4 uv grid of cell centres per face, and the probe points below are the
#: same stations so the two sides read the same geometry.
FEATURE_TOPOLOGY_SAMPLES = 4

#: Relative length tolerances, mirroring ``kGeometryRelTol`` /
#: ``kOnSurfaceRelTol`` / ``kParallelTol`` in ``src/pipeline/src/scene.cpp``.
GEOMETRY_REL_TOL = 1e-7
ON_SURFACE_REL_TOL = 1e-6
PARALLEL_TOL = 1e-9

#: Creases probed for reentrancy, mirroring ``kMaxCreaseProbes``.
MAX_CREASE_PROBES = 256

#: "Sharp" is more than this far from flat, the default threshold
#: ``geom::extract_topology`` classifies with (25 degrees).
SHARP_FROM_FLAT_RAD = 25.0 * math.pi / 180.0

#: Column order of the emitted table (after ``part``).
#:
#: The last ten are the proximity / crease / singularity block of the
#: portable-cost retrain, mirroring ``pipeline::CaseFeatures`` field for field.
#: The contract's other three columns -- ``load_to_feature_dist_min_rel``,
#: ``fix_to_feature_dist_min_rel`` and ``case_load_multiaxiality`` -- are
#: deliberately absent: they are properties of a LOAD CASE, not of a solid, so
#: they come per row from ``pipeline::extract_case_features`` through the testlab
#: row's ``features`` object, exactly as ``fix_area_frac`` and
#: ``load_axis_alignment`` already do. Putting a per-case number in a per-part
#: table would silently average two load cases of one solid together.
FEATURE_NAMES: list[str] = [
    "geo_curved_area_frac", "geo_cyl_area_frac", "geo_plane_area_frac",
    "geo_other_area_frac", "geo_min_curv_radius_rel", "geo_log_curv_radius_mean",
    "geo_log_curv_radius_std", "geo_n_faces", "geo_n_edges",
    "geo_face_area_cv", "geo_aspect_max", "geo_aspect_mid", "geo_volume_frac",
    "geo_area_over_v23", "geo_min_face_size_rel",
    "geo_n_inner_loops", "geo_hole_spacing_min_rel", "geo_hole_spacing_p10_rel",
    "geo_feat_pair_dist_min_rel", "geo_feat_pair_dist_p10_rel",
    "geo_feat_pair_dist_mean_rel", "geo_dihedral_p10", "geo_dihedral_p50",
    "geo_dihedral_p90", "geo_singular_lambda_min",
]


def _occ():
    """Import OCP lazily so the module can be imported without the binding."""
    from OCP.BRep import BRep_Tool
    from OCP.BRepAdaptor import BRepAdaptor_Curve, BRepAdaptor_Curve2d, BRepAdaptor_Surface
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
    from OCP.BRepExtrema import BRepExtrema_DistShapeShape
    from OCP.BRepGProp import BRepGProp, BRepGProp_Face
    from OCP.BRepTools import BRepTools
    from OCP.BRepTopAdaptor import BRepTopAdaptor_FClass2d
    from OCP.Bnd import Bnd_Box
    from OCP.GProp import GProp_GProps
    from OCP.GeomAbs import GeomAbs_SurfaceType
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TopAbs import (TopAbs_EDGE, TopAbs_FACE, TopAbs_Orientation, TopAbs_SHELL,
                            TopAbs_State, TopAbs_VERTEX)
    from OCP.TopExp import TopExp, TopExp_Explorer
    from OCP.TopTools import (TopTools_IndexedDataMapOfShapeListOfShape,
                              TopTools_IndexedMapOfShape)
    from OCP.TopoDS import TopoDS, TopoDS_Vertex
    from OCP.gp import gp_Pnt, gp_Pnt2d, gp_Vec
    return dict(BRep_Tool=BRep_Tool, BRepAdaptor_Curve=BRepAdaptor_Curve,
                BRepAdaptor_Curve2d=BRepAdaptor_Curve2d,
                BRepAdaptor_Surface=BRepAdaptor_Surface,
                BRepBndLib=BRepBndLib, BRepBuilderAPI_MakeVertex=BRepBuilderAPI_MakeVertex,
                BRepExtrema_DistShapeShape=BRepExtrema_DistShapeShape,
                BRepGProp=BRepGProp, BRepGProp_Face=BRepGProp_Face,
                BRepTools=BRepTools, BRepTopAdaptor_FClass2d=BRepTopAdaptor_FClass2d,
                Bnd_Box=Bnd_Box,
                GProp_GProps=GProp_GProps, GeomAbs=GeomAbs_SurfaceType,
                STEPControl_Reader=STEPControl_Reader, TopAbs_EDGE=TopAbs_EDGE,
                TopAbs_FACE=TopAbs_FACE, TopAbs_SHELL=TopAbs_SHELL,
                TopAbs_VERTEX=TopAbs_VERTEX,
                TopAbs_State=TopAbs_State, TopAbs_Orientation=TopAbs_Orientation,
                TopExp=TopExp, TopExp_Explorer=TopExp_Explorer,
                TopTools_IndexedDataMapOfShapeListOfShape=(
                    TopTools_IndexedDataMapOfShapeListOfShape),
                TopTools_IndexedMapOfShape=TopTools_IndexedMapOfShape,
                TopoDS=TopoDS, TopoDS_Vertex=TopoDS_Vertex,
                gp_Pnt=gp_Pnt, gp_Pnt2d=gp_Pnt2d, gp_Vec=gp_Vec)


# --- proximity / crease / singularity mirror ---------------------------------
#
# Everything below reproduces `src/pipeline/src/scene.cpp` operation for
# operation, because the whole point of these ten columns is that the offline
# table and the shipped C++ extractor agree to roundoff on the same STEP. Where
# the C++ has to derive something the OCC kernel already knows analytically (a
# cylinder's axis, which it recovers from a rim circle because `geom` does not
# expose the analytic axis to `pipeline`), the two routes describe the SAME line
# and the columns are functions of the line, not of the representative point.


def _quantile_floor(ascending: list[float], q: float) -> float:
    """Sorted-ascending quantile, floor convention, no interpolation.

    ``numpy.percentile`` interpolates between neighbours and is deliberately not
    used: the C++ side indexes ``floor(q*(n-1))`` and a mismatch here would show
    up as a feature-parity failure rather than as a rounding difference.
    """
    if not ascending:
        return 0.0
    index = int(q * (len(ascending) - 1))
    return ascending[min(index, len(ascending) - 1)]


def _williams_lambda(omega: float) -> float:
    """Smallest Williams eigenvalue in (0,1) for a traction-free wedge of
    material opening angle ``omega``; see ``williams_lambda`` in
    ``src/pipeline/src/scene.cpp`` for the equations and the citations. Same
    fixed 4096-interval scan and 100 bisections, so the two sides agree to the
    last few bits rather than to a tolerance."""
    scan = 4096
    bisections = 100
    if not math.isfinite(omega) or omega <= math.pi or omega > 2.0 * math.pi:
        return 1.0
    smallest = 1.0
    for sign in (1.0, -1.0):
        def residual(lam: float, sign: float = sign) -> float:
            return math.sin(lam * omega) + sign * lam * math.sin(omega)

        lo = 1.0 / scan
        f_lo = residual(lo)
        for k in range(2, scan + 1):
            hi = k / scan
            f_hi = residual(hi)
            if f_lo == 0.0:
                smallest = min(smallest, lo)
                break
            if f_lo * f_hi < 0.0:
                a, b, f_a = lo, hi, f_lo
                for _ in range(bisections):
                    mid = 0.5 * (a + b)
                    f_mid = residual(mid)
                    if (f_a < 0.0) == (f_mid < 0.0):
                        a, f_a = mid, f_mid
                    else:
                        b = mid
                smallest = min(smallest, 0.5 * (a + b))
                break
            lo, f_lo = hi, f_hi
    return smallest


def _unit(vector: np.ndarray) -> np.ndarray:
    norm = float(np.linalg.norm(vector))
    return vector / norm if norm > 0.0 else vector


def _axis_distance(a: tuple[np.ndarray, np.ndarray], b: tuple[np.ndarray, np.ndarray]) -> float:
    """Distance between two axis LINES: perpendicular when parallel, skew-line
    distance otherwise. Mirrors ``axis_distance``."""
    delta = b[0] - a[0]
    cross = np.cross(a[1], b[1])
    cross_norm = float(np.linalg.norm(cross))
    if cross_norm <= PARALLEL_TOL:
        return float(np.linalg.norm(delta - float(np.dot(delta, a[1])) * a[1]))
    return abs(float(np.dot(delta, cross / cross_norm)))


def _face_interior_uv(occ: dict[str, Any], face: Any) -> tuple[float, float] | None:
    """First station of the 4x4 uv cell-centre grid that lies inside the trim --
    the same grid, the same scan order, and the same classifier tolerance
    ``geom::extract_topology`` fills ``CadFace::samples`` with."""
    try:
        umin, umax, vmin, vmax = occ["BRepTools"].UVBounds_s(face)
    except Exception:  # noqa: BLE001 - OCC failure means no usable station
        return None
    du = umax - umin
    dv = vmax - vmin
    if not (du > 0.0) or not (dv > 0.0):
        return None
    forward = occ["TopoDS"].Face_s(
        face.Oriented(occ["TopAbs_Orientation"].TopAbs_FORWARD))
    classifier = occ["BRepTopAdaptor_FClass2d"](forward, occ["BRep_Tool"].Tolerance_s(face))
    n = FEATURE_TOPOLOGY_SAMPLES
    for iu in range(n):
        u = umin + (iu + 0.5) / n * du
        for iv in range(n):
            v = vmin + (iv + 0.5) / n * dv
            if classifier.Perform(occ["gp_Pnt2d"](u, v)) == occ["TopAbs_State"].TopAbs_IN:
                return (u, v)
    return None


def _face_normal(occ: dict[str, Any], face: Any, u: float, v: float) -> np.ndarray:
    """Outward unit normal at a uv station. ``BRepGProp_Face::Normal`` already
    honours the face orientation, which is what the C++ projection path does by
    flipping REVERSED."""
    point = occ["gp_Pnt"]()
    normal = occ["gp_Vec"]()
    occ["BRepGProp_Face"](face).Normal(u, v, point, normal)
    return _unit(np.asarray([normal.X(), normal.Y(), normal.Z()], dtype=np.float64))


def _distance_to_face(occ: dict[str, Any], face: Any, point: np.ndarray) -> float:
    """Distance from a 3D point to a TRIMMED face, the same quantity
    ``geom::project_point_on_face`` reports."""
    vertex = occ["BRepBuilderAPI_MakeVertex"](
        occ["gp_Pnt"](float(point[0]), float(point[1]), float(point[2]))).Vertex()
    solver = occ["BRepExtrema_DistShapeShape"](vertex, face)
    if not solver.IsDone():
        solver.Perform()
    if not solver.IsDone() or solver.NbSolution() < 1:
        return math.inf
    return float(solver.Value())


def _edge_stations(occ: dict[str, Any], edge: Any) -> list[np.ndarray]:
    """The sampled polyline ``geom::extract_topology`` stores on a CadEdge:
    ``FEATURE_TOPOLOGY_SAMPLES + 2`` points at uniform curve parameter,
    endpoints included."""
    curve = occ["BRepAdaptor_Curve"](edge)
    u0 = curve.FirstParameter()
    u1 = curve.LastParameter()
    segments = FEATURE_TOPOLOGY_SAMPLES + 1
    stations: list[np.ndarray] = []
    for s in range(segments + 1):
        u = u0 + (s / segments) * (u1 - u0)
        p = curve.Value(u)
        stations.append(np.asarray([p.X(), p.Y(), p.Z()], dtype=np.float64))
    return stations


def _edge_circle_radius(occ: dict[str, Any], edge: Any) -> float | None:
    """The radius when this edge is a circular arc, else None. The C++ fits a
    circle through three exact stations and verifies the rest; an analytic
    circle satisfies that test and nothing else does, so reading the kernel's
    own answer here selects the same edges."""
    curve = occ["BRepAdaptor_Curve"](edge)
    if "Circle" not in str(curve.GetType()):
        return None
    return abs(curve.Circle().Radius())


def _crease_dihedral(occ: dict[str, Any], edge: Any, faces: list[Any]) -> float | None:
    """Interior dihedral in the ``geom::CadEdge::dihedral_rad`` convention
    (pi = flat), or None when the edge is a seam, an open boundary, or not sharp
    -- exactly the cases ``classify_edges`` leaves at 0."""
    if len(faces) < 2 or faces[0].IsSame(faces[1]):
        return None
    first = faces[0]
    second = None
    for candidate in faces[1:]:
        if not candidate.IsSame(first):
            second = candidate
            break
    if second is None:
        return None
    normals = []
    for face in (first, second):
        try:
            pcurve = occ["BRepAdaptor_Curve2d"](edge, face)
            uv = pcurve.Value(0.5 * (pcurve.FirstParameter() + pcurve.LastParameter()))
        except Exception:  # noqa: BLE001 - no pcurve means no measured dihedral
            return None
        normals.append(_face_normal(occ, face, uv.X(), uv.Y()))
    cosine = float(np.clip(np.dot(normals[0], normals[1]), -1.0, 1.0))
    dihedral = math.pi - math.acos(cosine)
    if abs(dihedral - math.pi) < SHARP_FROM_FLAT_RAD:
        return None  # smooth continuation, not a crease
    if not (dihedral > 0.0):
        return None
    return dihedral


def _crease_opening_angle(occ: dict[str, Any], edge: Any, faces: list[Any],
                          dihedral: float, diag: float) -> float | None:
    """Material opening angle at one crease, mirroring
    ``crease_opening_angle``: exact dihedral for the magnitude, a step-and-project
    probe for the sign."""
    stations = _edge_stations(occ, edge)
    if len(stations) < 3:
        return None
    mid = len(stations) // 2
    point = stations[mid]
    along = stations[min(mid + 1, len(stations) - 1)] - stations[mid - 1]
    if not (float(np.linalg.norm(along)) > 0.0):
        return None
    tangent = _unit(along)
    step = 1e-3 * diag

    pair = [faces[0], None]
    for candidate in faces[1:]:
        if not candidate.IsSame(faces[0]):
            pair[1] = candidate
            break
    if pair[1] is None:
        return None

    normals: list[np.ndarray] = []
    inward: list[np.ndarray] = []
    for face in pair:
        try:
            pcurve = occ["BRepAdaptor_Curve2d"](edge, face)
            uv = pcurve.Value(0.5 * (pcurve.FirstParameter() + pcurve.LastParameter()))
        except Exception:  # noqa: BLE001
            return None
        normal = _face_normal(occ, face, uv.X(), uv.Y())
        across = np.cross(normal, tangent)
        if not (float(np.linalg.norm(across)) > 0.0):
            return None
        direction = _unit(across)
        best = math.inf
        chosen = None
        for orientation in (1.0, -1.0):
            distance = _distance_to_face(occ, face, point + (orientation * step) * direction)
            if distance < best:
                best = distance
                chosen = orientation * direction
        if not (best < 0.25 * step) or chosen is None:
            return None
        normals.append(normal)
        inward.append(chosen)
    bend = float(np.dot(normals[0], inward[1])) + float(np.dot(normals[1], inward[0]))
    return 2.0 * math.pi - dihedral if bend > 0.0 else dihedral


def _proximity_features(occ: dict[str, Any], shape: Any, diag: float) -> dict[str, float]:
    """The ten part-level proximity / crease / singularity columns.

    Sentinels when there is nothing to measure, matching the defaults documented
    on ``pipeline::CaseFeatures``: count 0, distances one full diagonal, dihedral
    pi, singular exponent 1.
    """
    out = {
        "geo_n_inner_loops": 0.0,
        "geo_hole_spacing_min_rel": 1.0,
        "geo_hole_spacing_p10_rel": 1.0,
        "geo_feat_pair_dist_min_rel": 1.0,
        "geo_feat_pair_dist_p10_rel": 1.0,
        "geo_feat_pair_dist_mean_rel": 1.0,
        "geo_dihedral_p10": math.pi,
        "geo_dihedral_p50": math.pi,
        "geo_dihedral_p90": math.pi,
        "geo_singular_lambda_min": 1.0,
    }
    if not (diag > 0.0):
        return out

    vmap = occ["TopTools_IndexedMapOfShape"]()
    emap = occ["TopTools_IndexedMapOfShape"]()
    fmap = occ["TopTools_IndexedMapOfShape"]()
    occ["TopExp"].MapShapes_s(shape, occ["TopAbs_VERTEX"], vmap)
    occ["TopExp"].MapShapes_s(shape, occ["TopAbs_EDGE"], emap)
    occ["TopExp"].MapShapes_s(shape, occ["TopAbs_FACE"], fmap)
    edge_faces = occ["TopTools_IndexedDataMapOfShapeListOfShape"]()
    occ["TopExp"].MapShapesAndAncestors_s(shape, occ["TopAbs_EDGE"], occ["TopAbs_FACE"],
                                          edge_faces)

    def faces_of_edge(edge: Any) -> list[Any]:
        """Ancestor faces of an edge, in the order ``MapShapesAndAncestors``
        lists them -- the same order ``classify_edges`` reads, so "faces[0] and
        the first face not the same as it" picks the same pair on both sides."""
        if not edge_faces.Contains(edge):
            return []
        return [occ["TopoDS"].Face_s(item) for item in edge_faces.FindFromKey(edge)]

    def face_edges(face: Any) -> list[tuple[int, Any]]:
        found: list[tuple[int, Any]] = []
        explorer = occ["TopExp_Explorer"](face, occ["TopAbs_EDGE"])
        while explorer.More():
            edge = occ["TopoDS"].Edge_s(explorer.Current())
            if not occ["BRep_Tool"].Degenerated_s(edge):
                found.append((emap.FindIndex(edge), edge))
            explorer.Next()
        return found

    faces = [occ["TopoDS"].Face_s(fmap.FindKey(i)) for i in range(1, fmap.Extent() + 1)]
    areas: list[float] = []
    kinds: list[str] = []
    for face in faces:
        props = occ["GProp_GProps"]()
        occ["BRepGProp"].SurfaceProperties_s(face, props)
        areas.append(abs(props.Mass()))
        kinds.append(str(occ["BRepAdaptor_Surface"](face).GetType()))

    # --- cylindrical bores ---------------------------------------------------
    bores: list[tuple[np.ndarray, np.ndarray, float]] = []
    for index, face in enumerate(faces):
        if "Cylinder" not in kinds[index]:
            continue
        if all(_edge_circle_radius(occ, edge) is None for _, edge in face_edges(face)):
            continue  # a fillet strip or a trimmed patch, not a drilled bore
        station = _face_interior_uv(occ, face)
        if station is None:
            continue
        surface = occ["BRepAdaptor_Surface"](face)
        cylinder = surface.Cylinder()
        location = cylinder.Axis().Location()
        axis_dir = cylinder.Axis().Direction()
        point = np.asarray([location.X(), location.Y(), location.Z()], dtype=np.float64)
        axis = _unit(np.asarray([axis_dir.X(), axis_dir.Y(), axis_dir.Z()], dtype=np.float64))
        radius = abs(cylinder.Radius())
        probe_pnt = surface.Value(station[0], station[1])
        probe = np.asarray([probe_pnt.X(), probe_pnt.Y(), probe_pnt.Z()], dtype=np.float64)
        offset = probe - point
        radial_vec = offset - float(np.dot(offset, axis)) * axis
        radial_norm = float(np.linalg.norm(radial_vec))
        if not (radial_norm > GEOMETRY_REL_TOL * diag):
            continue
        normal = _face_normal(occ, face, station[0], station[1])
        if float(np.dot(normal, radial_vec / radial_norm)) >= 0.0:
            continue  # outward normal leads away from the axis: boss or shaft
        candidate = (point, axis, radius)
        duplicate = any(
            float(np.linalg.norm(np.cross(other[1], axis))) <= PARALLEL_TOL
            and _axis_distance(other, candidate) <= GEOMETRY_REL_TOL * diag
            and abs(other[2] - radius) <= GEOMETRY_REL_TOL * diag
            for other in bores)
        if not duplicate:
            bores.append(candidate)

    out["geo_n_inner_loops"] = float(len(bores))
    if len(bores) >= 2:
        spacing = sorted(_axis_distance(bores[i], bores[j]) / diag
                         for i in range(len(bores)) for j in range(i + 1, len(bores)))
        out["geo_hole_spacing_min_rel"] = spacing[0]
        out["geo_hole_spacing_p10_rel"] = _quantile_floor(spacing, 0.1)

    # --- salient-feature clusters -------------------------------------------
    total_area = sum(areas)
    if total_area > 0.0:
        limit = SALIENT_FACE_AREA_FRACTION * total_area
        positions: dict[int, np.ndarray] = {}
        salient: list[int] = []
        edges_by_face: dict[int, list[int]] = {}
        for index, face in enumerate(faces):
            if not (areas[index] < limit):
                continue
            edge_ids: list[int] = []
            vertex_ids: set[int] = set()
            for edge_id, edge in face_edges(face):
                edge_ids.append(edge_id)
                first = occ["TopoDS_Vertex"]()
                last = occ["TopoDS_Vertex"]()
                occ["TopExp"].Vertices_s(edge, first, last)
                for vertex in (first, last):
                    vertex_index = vmap.FindIndex(vertex)
                    if vertex_index > 0:
                        vertex_ids.add(vertex_index)
            if not vertex_ids:
                continue  # a fully periodic face has no vertices to stand on
            points = []
            for vertex_index in sorted(vertex_ids):
                p = occ["BRep_Tool"].Pnt_s(occ["TopoDS"].Vertex_s(vmap.FindKey(vertex_index)))
                points.append([p.X(), p.Y(), p.Z()])
            positions[index] = np.asarray(points, dtype=np.float64).mean(axis=0)
            edges_by_face[index] = edge_ids
            salient.append(index)

        parent = {index: index for index in salient}

        def root(index: int) -> int:
            while parent[index] != index:
                parent[index] = parent[parent[index]]
                index = parent[index]
            return index

        owner: dict[int, int] = {}
        for index in salient:
            for edge_id in sorted(set(edges_by_face[index])):
                if edge_id in owner:
                    a, b = root(owner[edge_id]), root(index)
                    if a != b:
                        parent[b] = a
                else:
                    owner[edge_id] = index
        groups: dict[int, tuple[np.ndarray, float]] = {}
        for index in salient:
            key = root(index)
            weight = areas[index]
            accumulated, total = groups.get(key, (np.zeros(3), 0.0))
            groups[key] = (accumulated + weight * positions[index], total + weight)
        centroids = [value / weight for value, weight in groups.values() if weight > 0.0]
        if len(centroids) >= 2:
            pairs = sorted(float(np.linalg.norm(centroids[j] - centroids[i]))
                           for i in range(len(centroids))
                           for j in range(i + 1, len(centroids)))
            out["geo_feat_pair_dist_min_rel"] = pairs[0] / diag
            out["geo_feat_pair_dist_p10_rel"] = _quantile_floor(pairs, 0.1) / diag
            out["geo_feat_pair_dist_mean_rel"] = sum(pairs) / len(pairs) / diag

    # --- creases and the Williams exponent ----------------------------------
    creases: list[tuple[float, int, Any, list[Any]]] = []
    for index in range(1, emap.Extent() + 1):
        edge = occ["TopoDS"].Edge_s(emap.FindKey(index))
        if occ["BRep_Tool"].Degenerated_s(edge):
            continue
        adjacent = faces_of_edge(edge)
        dihedral = _crease_dihedral(occ, edge, adjacent)
        if dihedral is None:
            continue
        creases.append((dihedral, index, edge, adjacent))
    creases.sort(key=lambda item: (item[0], item[1]))
    if creases:
        angles = [item[0] for item in creases]
        out["geo_dihedral_p10"] = _quantile_floor(angles, 0.1)
        out["geo_dihedral_p50"] = _quantile_floor(angles, 0.5)
        out["geo_dihedral_p90"] = _quantile_floor(angles, 0.9)
    for dihedral, _index, edge, adjacent in creases[:MAX_CREASE_PROBES]:
        opening = _crease_opening_angle(occ, edge, adjacent, dihedral, diag)
        if opening is None or not (opening > math.pi):
            continue
        out["geo_singular_lambda_min"] = _williams_lambda(opening)
        break
    return out


def _count(occ: dict[str, Any], shape: Any, kind: Any) -> int:
    explorer = occ["TopExp_Explorer"](shape, kind)
    total = 0
    while explorer.More():
        total += 1
        explorer.Next()
    return total


def extract(step_path: Path) -> dict[str, float]:
    """Every descriptor for one STEP solid."""
    occ = _occ()
    reader = occ["STEPControl_Reader"]()
    if str(reader.ReadFile(str(step_path))) != "IFSelect_ReturnStatus.IFSelect_RetDone":
        raise RuntimeError(f"cannot read {step_path}")
    reader.TransferRoots()
    shape = reader.OneShape()

    box = occ["Bnd_Box"]()
    occ["BRepBndLib"].Add_s(shape, box)
    xmin, ymin, zmin, xmax, ymax, zmax = box.Get()
    extents = sorted([xmax - xmin, ymax - ymin, zmax - zmin])
    diag = math.sqrt(sum(e * e for e in extents))
    if not (diag > 0.0):
        raise RuntimeError(f"degenerate bounding box for {step_path}")

    volume_props = occ["GProp_GProps"]()
    occ["BRepGProp"].VolumeProperties_s(shape, volume_props)
    volume = abs(volume_props.Mass())

    surface_props = occ["GProp_GProps"]()
    occ["BRepGProp"].SurfaceProperties_s(shape, surface_props)
    total_area = abs(surface_props.Mass())

    plane_area = cyl_area = other_area = 0.0
    radii: list[tuple[float, float]] = []  # (radius, area)
    areas: list[float] = []
    small_faces = 0

    explorer = occ["TopExp_Explorer"](shape, occ["TopAbs_FACE"])
    while explorer.More():
        face = occ["TopoDS"].Face_s(explorer.Current())
        props = occ["GProp_GProps"]()
        occ["BRepGProp"].SurfaceProperties_s(face, props)
        area = abs(props.Mass())
        areas.append(area)

        adaptor = occ["BRepAdaptor_Surface"](face)
        kind = adaptor.GetType()
        name = str(kind)
        if "Plane" in name:
            plane_area += area
        elif "Cylinder" in name:
            cyl_area += area
            radii.append((abs(adaptor.Cylinder().Radius()), area))
        elif "Sphere" in name:
            other_area += area
            radii.append((abs(adaptor.Sphere().Radius()), area))
        elif "Cone" in name:
            other_area += area
        elif "Torus" in name:
            other_area += area
            radii.append((abs(adaptor.Torus().MinorRadius()), area))
        else:
            other_area += area
        explorer.Next()

    area_norm = total_area if total_area > 0.0 else 1.0
    curved_area = cyl_area + other_area

    if radii:
        radius_values = np.asarray([r / diag for r, _ in radii], dtype=np.float64)
        weights = np.asarray([a for _, a in radii], dtype=np.float64)
        weights = weights / weights.sum() if weights.sum() > 0 else None
        logs = np.log10(np.maximum(radius_values, 1e-9))
        log_mean = float(np.average(logs, weights=weights))
        log_std = float(np.sqrt(np.average((logs - log_mean) ** 2, weights=weights)))
        min_radius = float(radius_values.min())
    else:
        # A fully planar part has no curvature scale. Encoding that as a large
        # radius rather than zero keeps the ordering meaningful: "nothing tight
        # to resolve" belongs at the same end as "one very gentle fillet".
        log_mean, log_std, min_radius = 0.0, 0.0, 1.0

    area_array = np.asarray(areas, dtype=np.float64)
    face_cv = float(area_array.std() / area_array.mean()) if area_array.size and \
        area_array.mean() > 0 else 0.0

    bbox_volume = extents[0] * extents[1] * extents[2]
    return {
        "geo_curved_area_frac": curved_area / area_norm,
        "geo_cyl_area_frac": cyl_area / area_norm,
        "geo_plane_area_frac": plane_area / area_norm,
        "geo_other_area_frac": other_area / area_norm,
        "geo_min_curv_radius_rel": min_radius,
        "geo_log_curv_radius_mean": log_mean,
        "geo_log_curv_radius_std": log_std,
        "geo_n_faces": float(len(areas)),
        "geo_n_edges": float(_count(occ, shape, occ["TopAbs_EDGE"])),
        "geo_min_face_size_rel": float(min(math.sqrt(a) for a in areas) / diag)
        if areas else 0.0,
        "geo_face_area_cv": face_cv,
        "geo_aspect_max": float(extents[2] / extents[0]) if extents[0] > 0 else 0.0,
        "geo_aspect_mid": float(extents[1] / extents[0]) if extents[0] > 0 else 0.0,
        "geo_volume_frac": float(volume / bbox_volume) if bbox_volume > 0 else 0.0,
        "geo_area_over_v23": float(total_area / (volume ** (2.0 / 3.0))) if volume > 0 else 0.0,
        **_proximity_features(occ, shape, diag),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--step-dir", type=Path, default=STEP_DIR)
    parser.add_argument("--out", type=Path, default=FEATURES_CSV)
    args = parser.parse_args(argv)

    paths = sorted(args.step_dir.glob("*.step"))
    if not paths:
        raise SystemExit(f"no STEP files under {args.step_dir}")

    rows: list[dict[str, Any]] = []
    for path in paths:
        try:
            features = extract(path)
        except (RuntimeError, OSError) as error:
            print(f"  skipped {path.name}: {error}")
            continue
        features["part"] = path.stem  # e.g. box_hole_s0; joined by geometry
        rows.append(features)
        print(f"{path.stem:>20}  curved_area {features['geo_curved_area_frac']:.3f}  "
              f"min_r/L {features['geo_min_curv_radius_rel']:.4f}  "
              f"faces {features['geo_n_faces']:.0f}  "
              f"aspect {features['geo_aspect_max']:.2f}  "
              f"solidity {features['geo_volume_frac']:.3f}")

    if not rows:
        raise SystemExit("no geometry could be read")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["part"] + FEATURE_NAMES)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    # The whole point is that these VARY. A descriptor constant across the
    # corpus is exactly the defect being fixed, so report it rather than let it
    # slip in beside the useful ones.
    print(f"\n{len(rows)} geometries -> {args.out}")
    print(f"{'feature':>28} {'min':>10} {'max':>10} {'distinct':>9}")
    dead: list[str] = []
    for name in FEATURE_NAMES:
        values = np.asarray([row[name] for row in rows], dtype=np.float64)
        distinct = len(set(np.round(values, 9)))
        if distinct <= 1:
            dead.append(name)
        print(f"{name:>28} {values.min():>10.4f} {values.max():>10.4f} {distinct:>9}")
    if dead:
        print(f"\nCONSTANT (useless, same defect as curved_frac): {dead}")
    else:
        print("\nevery descriptor varies across the corpus")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
