#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Convert a VTU exterior skin to a faceted BRep and measure directional fidelity.

The output contains one planar BRep face per deterministic boundary triangle.  It
is intentionally a faceted representation; this script does not fit smooth or
analytic surfaces to the mesh.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.metadata
import json
import math
import os
import platform
import re
import shlex
import sys
from collections import Counter, defaultdict
from pathlib import Path
from types import SimpleNamespace


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _file_snapshot(path: Path) -> dict[str, int | str]:
    before = path.stat()
    sha256 = _sha256(path)
    after = path.stat()
    before_identity = (before.st_size, before.st_mtime_ns)
    after_identity = (after.st_size, after.st_mtime_ns)
    if before_identity != after_identity:
        raise RuntimeError(f"input changed while it was being hashed: {path}")
    return {
        "sha256": sha256,
        "bytes": int(after.st_size),
        "mtime_ns": int(after.st_mtime_ns),
    }


def _verify_file_snapshot(
    path: Path,
    expected: dict[str, int | str],
    label: str,
) -> None:
    current = _file_snapshot(path)
    if current != expected:
        raise RuntimeError(
            f"{label} changed during round-trip evaluation; refusing to publish outputs"
        )


def _preflight_vtu(
    path: Path,
    file_bytes: int,
    max_mesh_bytes: int,
    max_mesh_points: int,
    max_mesh_cells: int,
) -> dict[str, int]:
    if file_bytes > max_mesh_bytes:
        raise RuntimeError(
            f"VTU mesh is {file_bytes} bytes, exceeding "
            f"--max-mesh-bytes={max_mesh_bytes}"
        )

    header_scan_bytes = 1024 * 1024
    with path.open("rb") as stream:
        prefix = stream.read(min(file_bytes, header_scan_bytes))
    xml_header = prefix.split(b"<AppendedData", 1)[0]
    piece_tags = re.findall(rb"<Piece\b[^>]*>", xml_header)
    if not piece_tags:
        raise RuntimeError(
            "VTU preflight found no Piece header with declared NumberOfPoints "
            "and NumberOfCells within the first 1 MiB"
        )

    def declared(piece_tag: bytes, attribute: bytes) -> int:
        match = re.search(
            rb"\b" + attribute + rb"\s*=\s*[\"']([0-9]+)[\"']",
            piece_tag,
        )
        if match is None:
            raise RuntimeError(
                f"VTU Piece header is missing {attribute.decode('ascii')}"
            )
        return int(match.group(1))

    point_counts = [declared(tag, b"NumberOfPoints") for tag in piece_tags]
    cell_counts = [declared(tag, b"NumberOfCells") for tag in piece_tags]
    if any(value <= 0 for value in (*point_counts, *cell_counts)):
        raise RuntimeError(
            "VTU Piece header declares a non-positive point or cell count"
        )
    points = sum(point_counts)
    cells = sum(cell_counts)
    if points > max_mesh_points:
        raise RuntimeError(
            f"VTU Piece headers declare {points} total points, exceeding "
            f"--max-mesh-points={max_mesh_points}"
        )
    if cells > max_mesh_cells:
        raise RuntimeError(
            f"VTU Piece headers declare {cells} total cells, exceeding "
            f"--max-mesh-cells={max_mesh_cells}"
        )
    return {
        "piece_count": len(piece_tags),
        "declared_points": points,
        "declared_cells": cells,
        "header_scan_byte_limit": header_scan_bytes,
    }


def _dependency_version(distribution: str) -> str | None:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return None


def _load_dependencies() -> SimpleNamespace:
    try:
        import numpy as np
        import pyvista as pv
        from vtkmodules.vtkCommonDataModel import vtkCellTypeUtilities
    except ImportError as exc:
        raise RuntimeError(
            "PyVista, VTK, and NumPy are required to read and inspect the VTU; "
            "install the same Python environment used by scripts/render_showcase.py "
            f"(import failed: {exc})"
        ) from exc

    try:
        from OCP.Bnd import Bnd_Box
        from OCP.BRep import BRep_Builder, BRep_Tool
        from OCP.BRepAdaptor import BRepAdaptor_Surface
        from OCP.BRepClass import BRepClass_FaceClassifier
        from OCP.BRepBndLib import BRepBndLib
        from OCP.BRepBuilderAPI import (
            BRepBuilderAPI_MakeFace,
            BRepBuilderAPI_MakePolygon,
            BRepBuilderAPI_MakeSolid,
            BRepBuilderAPI_MakeVertex,
            BRepBuilderAPI_Sewing,
        )
        from OCP.BRepCheck import BRepCheck_Analyzer
        from OCP.BRepExtrema import BRepExtrema_DistShapeShape
        from OCP.BRepTools import BRepTools
        from OCP.gp import gp_Pnt, gp_Pnt2d
        from OCP.IFSelect import IFSelect_RetDone
        from OCP.Interface import Interface_Static
        from OCP.STEPControl import (
            STEPControl_AsIs,
            STEPControl_Reader,
            STEPControl_Writer,
        )
        from OCP.TopAbs import (
            TopAbs_EDGE,
            TopAbs_FACE,
            TopAbs_IN,
            TopAbs_ON,
            TopAbs_SHELL,
            TopAbs_SOLID,
            TopAbs_VERTEX,
        )
        from OCP.TopExp import TopExp_Explorer
        from OCP.TopoDS import TopoDS, TopoDS_Shape
    except ImportError as exc:
        raise RuntimeError(
            "OCP (OpenCASCADE Python bindings) is required for BRep construction, "
            "validation, distance queries, and export; install `cadquery-ocp` or "
            f"pythonocc-core (import failed: {exc})"
        ) from exc

    return SimpleNamespace(**locals())


def _load_source(path: Path, ocp: SimpleNamespace):
    suffix = path.suffix.lower()
    if suffix in {".step", ".stp"}:
        reader = ocp.STEPControl_Reader()
        status = reader.ReadFile(str(path))
        if status != ocp.IFSelect_RetDone:
            raise RuntimeError(f"could not read source STEP {path} (status {status})")
        transferred = int(reader.TransferRoots())
        if transferred <= 0:
            raise RuntimeError(f"source STEP {path} contains no transferable roots")
        shape = reader.OneShape()
        source_format = "STEP"
    elif suffix in {".brep", ".brp"}:
        shape = ocp.TopoDS_Shape()
        builder = ocp.BRep_Builder()
        read_ok = ocp.BRepTools.Read_s(shape, str(path), builder)
        if read_ok is False:
            raise RuntimeError(f"could not read source native BREP {path}")
        source_format = "BREP"
    else:
        raise RuntimeError(
            f"unsupported source extension {suffix or '<none>'}; use .step, .stp, .brep, or .brp"
        )
    if shape.IsNull():
        raise RuntimeError(f"source CAD file produced a null shape: {path}")
    if not bool(ocp.BRepCheck_Analyzer(shape).IsValid()):
        raise RuntimeError(f"source CAD shape is not BRepCheck-valid: {path}")
    return shape, source_format


def _topology_counts(shape, ocp: SimpleNamespace) -> dict[str, int]:
    result: dict[str, int] = {}
    for label, kind in (
        ("solids", ocp.TopAbs_SOLID),
        ("shells", ocp.TopAbs_SHELL),
        ("faces", ocp.TopAbs_FACE),
        ("edges", ocp.TopAbs_EDGE),
        ("vertices", ocp.TopAbs_VERTEX),
    ):
        explorer = ocp.TopExp_Explorer(shape, kind)
        count = 0
        while explorer.More():
            count += 1
            explorer.Next()
        result[label] = count
    return result


def _bbox(shape, ocp: SimpleNamespace) -> tuple[list[float], list[float], float]:
    box = ocp.Bnd_Box()
    ocp.BRepBndLib.Add_s(shape, box)
    if box.IsVoid():
        raise RuntimeError("source CAD shape has an empty bounding box")
    xmin, ymin, zmin, xmax, ymax, zmax = (float(value) for value in box.Get())
    if not all(math.isfinite(value) for value in (xmin, ymin, zmin, xmax, ymax, zmax)):
        raise RuntimeError("source CAD bounding box contains non-finite coordinates")
    minimum = [xmin, ymin, zmin]
    maximum = [xmax, ymax, zmax]
    diagonal = math.dist(minimum, maximum)
    if not math.isfinite(diagonal) or diagonal <= 0.0:
        raise RuntimeError(f"source CAD bounding-box diagonal is not positive ({diagonal})")
    return minimum, maximum, diagonal


def _cell_type_summary(grid, vtk_cell_type_utilities, np) -> tuple[list[dict], list[str]]:
    counts = Counter(int(value) for value in np.asarray(grid.celltypes))
    summary: list[dict] = []
    unsupported: list[str] = []
    for type_id in sorted(counts):
        name = str(vtk_cell_type_utilities.GetClassNameFromTypeId(type_id) or "unknown")
        try:
            dimension = int(vtk_cell_type_utilities.GetDimension(type_id))
        except (TypeError, ValueError):
            dimension = -1
        summary.append(
            {
                "vtk_type_id": type_id,
                "vtk_class": name,
                "dimension": dimension,
                "count": counts[type_id],
            }
        )
        if dimension != 3:
            unsupported.append(f"{name} (VTK {type_id}, dimension {dimension})")
    return summary, unsupported


def _read_boundary_polygons(
    mesh_path: Path,
    max_faces: int,
    max_mesh_points: int,
    max_mesh_cells: int,
    deps: SimpleNamespace,
):
    try:
        grid = deps.pv.read(str(mesh_path))
    except Exception as exc:
        raise RuntimeError(f"could not read VTU mesh {mesh_path}: {exc}") from exc
    if not isinstance(grid, deps.pv.UnstructuredGrid):
        raise RuntimeError(
            f"unsupported mesh dataset {type(grid).__name__}; --mesh must be a VTU UnstructuredGrid"
        )
    if grid.n_cells <= 0 or grid.n_points <= 0:
        raise RuntimeError("VTU mesh has no cells or points")
    if grid.n_points > max_mesh_points or grid.n_cells > max_mesh_cells:
        raise RuntimeError(
            "loaded VTU exceeds its pre-materialization limits: "
            f"points={grid.n_points}/{max_mesh_points}, "
            f"cells={grid.n_cells}/{max_mesh_cells}"
        )

    cell_types, unsupported = _cell_type_summary(
        grid, deps.vtkCellTypeUtilities, deps.np
    )
    if unsupported:
        raise RuntimeError(
            "unsupported cell data in VTU: "
            + ", ".join(unsupported)
            + "; provide a volume mesh containing only 3-D VTK cells"
        )

    try:
        surface = grid.extract_surface(
            algorithm="dataset_surface",
            pass_pointid=False,
            pass_cellid=False,
            nonlinear_subdivision=0,
        )
    except Exception as exc:
        raise RuntimeError(f"VTK could not extract the exterior mesh skin: {exc}") from exc
    if surface.n_cells <= 0 or surface.n_points <= 0:
        raise RuntimeError("extracted exterior mesh skin is empty")

    packed = deps.np.asarray(surface.faces, dtype=deps.np.int64)
    polygons: list[tuple[int, ...]] = []
    cursor = 0
    while cursor < packed.size:
        size = int(packed[cursor])
        stop = cursor + 1 + size
        if size < 3 or stop > packed.size:
            raise RuntimeError("unsupported or malformed polygon data in extracted mesh skin")
        polygon = tuple(int(value) for value in packed[cursor + 1 : stop])
        if len(set(polygon)) != size:
            raise RuntimeError("extracted mesh skin contains a polygon with repeated vertices")
        polygons.append(polygon)
        cursor = stop
    if cursor != packed.size or len(polygons) != surface.n_cells:
        raise RuntimeError(
            "unsupported cell data in extracted skin; expected polygon cells only"
        )
    if len(polygons) > max_faces:
        raise RuntimeError(
            f"exterior boundary has {len(polygons)} polygon faces, exceeding "
            f"--max-faces={max_faces}; raise the explicit ceiling or use a smaller mesh"
        )

    edge_use: dict[tuple[int, int], list[int]] = defaultdict(lambda: [0, 0])
    for polygon in polygons:
        for start, end in zip(polygon, polygon[1:] + polygon[:1]):
            key = (start, end) if start < end else (end, start)
            edge_use[key][0] += 1
            edge_use[key][1] += 1 if (start, end) == key else -1
    boundary_edges = sum(use_count == 1 for use_count, _ in edge_use.values())
    nonmanifold_edges = sum(use_count != 2 for use_count, _ in edge_use.values())
    inconsistent_edges = sum(
        use_count == 2 and orientation_balance != 0
        for use_count, orientation_balance in edge_use.values()
    )
    if boundary_edges or nonmanifold_edges or inconsistent_edges:
        raise RuntimeError(
            "exterior mesh skin is not a consistently oriented closed 2-manifold: "
            f"boundary_edges={boundary_edges}, nonmanifold_edges={nonmanifold_edges}, "
            f"inconsistent_orientation_edges={inconsistent_edges}"
        )
    surface_points = deps.np.asarray(surface.points, dtype=deps.np.float64)
    if not deps.np.all(deps.np.isfinite(surface_points)):
        raise RuntimeError("mesh skin contains non-finite point coordinates")
    polygon_centroids = deps.np.asarray(
        [surface_points[list(polygon)].mean(axis=0) for polygon in polygons],
        dtype=deps.np.float64,
    )
    boundary_sample_candidates = deps.np.vstack(
        (surface_points, polygon_centroids)
    )


    try:
        triangles = surface.triangulate(inplace=False)
    except Exception as exc:
        raise RuntimeError(f"deterministic VTK skin triangulation failed: {exc}") from exc
    triangle_packed = deps.np.asarray(triangles.faces, dtype=deps.np.int64)
    if triangle_packed.size % 4 != 0:
        raise RuntimeError("triangulated skin contains malformed face data")
    triangle_rows = triangle_packed.reshape((-1, 4))
    if triangle_rows.size and not deps.np.all(triangle_rows[:, 0] == 3):
        raise RuntimeError("skin triangulation produced a non-triangle cell")
    if len(triangle_rows) > max_faces:
        raise RuntimeError(
            f"exterior polygons require {len(triangle_rows)} faceted BRep faces, exceeding "
            f"--max-faces={max_faces}; raise the explicit ceiling or use a smaller mesh"
        )
    triangle_ids = deps.np.asarray(triangle_rows[:, 1:4], dtype=deps.np.int64)
    points = deps.np.asarray(triangles.points, dtype=deps.np.float64)
    if not deps.np.all(deps.np.isfinite(points)):
        raise RuntimeError("triangulated mesh skin contains non-finite point coordinates")

    closure = {
        "closed": True,
        "polygon_faces": len(polygons),
        "faceted_brep_faces": len(triangle_ids),
        "unique_edges": len(edge_use),
        "boundary_edges": boundary_edges,
        "nonmanifold_edges": nonmanifold_edges,
        "inconsistent_orientation_edges": inconsistent_edges,
    }
    mesh_info = {
        "points": int(grid.n_points),
        "cells": int(grid.n_cells),
        "cell_types": cell_types,
        "skin_points": int(surface.n_points),
    }
    return points, triangle_ids, boundary_sample_candidates, closure, mesh_info


def _make_faceted_shape(points, triangle_ids, diagonal: float, ocp: SimpleNamespace):
    tolerance = max(diagonal * 1.0e-10, 1.0e-12)
    sewing = ocp.BRepBuilderAPI_Sewing(tolerance, True, True, True, False)
    for face_number, ids in enumerate(triangle_ids):
        coordinates = [points[int(index)] for index in ids]
        a, b, c = coordinates
        cross = ocp.np.cross(b - a, c - a)
        if float(ocp.np.linalg.norm(cross)) <= diagonal * diagonal * 1.0e-14:
            raise RuntimeError(
                f"boundary triangle {face_number} is degenerate and cannot form a BRep face"
            )
        polygon = ocp.BRepBuilderAPI_MakePolygon()
        for coordinate in coordinates:
            polygon.Add(ocp.gp_Pnt(*(float(value) for value in coordinate)))
        polygon.Close()
        if not polygon.IsDone():
            raise RuntimeError(f"could not construct wire for boundary triangle {face_number}")
        face_builder = ocp.BRepBuilderAPI_MakeFace(polygon.Wire(), True)
        if not face_builder.IsDone():
            raise RuntimeError(f"could not construct BRep face for boundary triangle {face_number}")
        sewing.Add(face_builder.Face())

    sewing.Perform()
    sewed = sewing.SewedShape()
    if sewed.IsNull():
        raise RuntimeError("BRep sewing produced a null shape")
    free_edges = int(sewing.NbFreeEdges())
    multiple_edges = int(sewing.NbMultipleEdges())
    if free_edges != 0 or multiple_edges != 0:
        raise RuntimeError(
            "invalid BRep sewing result: "
            f"free_edges={free_edges}, multiple_edges={multiple_edges}; "
            "the closed mesh skin did not sew into a closed shell"
        )
    if not bool(ocp.BRepCheck_Analyzer(sewed).IsValid()):
        raise RuntimeError("invalid BRep sewing result: sewn shape failed BRepCheck")

    shells = ocp.TopExp_Explorer(sewed, ocp.TopAbs_SHELL)
    shell_shapes = []
    while shells.More():
        shell_shapes.append(ocp.TopoDS.Shell_s(shells.Current()))
        shells.Next()
    if len(shell_shapes) != 1:
        raise RuntimeError(
            f"invalid BRep sewing result: expected one closed shell, found {len(shell_shapes)}"
        )

    # The mesh closure gate above is the condition for attempting a solid.
    solid_builder = ocp.BRepBuilderAPI_MakeSolid(shell_shapes[0])
    if not solid_builder.IsDone():
        raise RuntimeError("closed sewn shell could not be promoted to a faceted solid")
    result = solid_builder.Solid()
    if not bool(ocp.BRepCheck_Analyzer(result).IsValid()):
        raise RuntimeError("faceted solid failed BRepCheck after sewing")

    evidence = {
        "sewing_tolerance": tolerance,
        "sewing_free_edges": free_edges,
        "sewing_multiple_edges": multiple_edges,
        "sewn_shell_count": len(shell_shapes),
        "solid_attempted": True,
        "result_brepcheck_valid": True,
        "result_topology": _topology_counts(result, ocp),
    }
    return result, evidence


def _sorted_unique_points(points, np):
    array = np.asarray(points, dtype=np.float64).reshape((-1, 3))
    if array.size == 0:
        return array
    return np.unique(array, axis=0)


def _deterministic_subsample(points, limit: int, np):
    ordered = _sorted_unique_points(points, np)
    candidate_count = len(ordered)
    if candidate_count <= limit:
        return ordered, candidate_count
    if limit == 1:
        return ordered[[0]], candidate_count
    indices = (np.arange(limit, dtype=np.int64) * (candidate_count - 1)) // (limit - 1)
    return ordered[indices], candidate_count


def _mesh_boundary_samples(candidates, max_samples: int, np):
    return _deterministic_subsample(candidates, max_samples, np)


def _source_trimmed_face_samples(
    shape,
    max_samples: int,
    ocp: SimpleNamespace,
):
    face_explorer = ocp.TopExp_Explorer(shape, ocp.TopAbs_FACE)
    face_count = 0
    while face_explorer.More():
        face_count += 1
        face_explorer.Next()
    if face_count == 0:
        raise RuntimeError("source BRep has no faces to sample")
    if max_samples < face_count:
        raise RuntimeError(
            f"--max-samples={max_samples} cannot cover all {face_count} source "
            "BRep faces; exact trimmed-face sampling requires at least one "
            "sample budget per face"
        )

    base_quota, extra_quota_faces = divmod(max_samples, face_count)
    # Four classifier calls per allocated sample is the fixed global attempt cap.
    uv_attempt_multiplier = 4
    samples: list[tuple[float, float, float]] = []
    attempted_uv_points = 0
    accepted_trimmed_uv_points = 0
    fallback_vertex_points = 0

    face_explorer = ocp.TopExp_Explorer(shape, ocp.TopAbs_FACE)
    face_index = 0
    while face_explorer.More():
        face = ocp.TopoDS.Face_s(face_explorer.Current())
        quota = base_quota + (1 if face_index < extra_quota_faces else 0)
        surface = ocp.BRepAdaptor_Surface(face, True)
        u_min = float(surface.FirstUParameter())
        u_max = float(surface.LastUParameter())
        v_min = float(surface.FirstVParameter())
        v_max = float(surface.LastVParameter())
        bounds = (u_min, u_max, v_min, v_max)
        if not all(math.isfinite(value) for value in bounds):
            raise RuntimeError(
                f"source BRep face {face_index} has non-finite trimmed UV bounds: "
                f"{bounds}"
            )
        if u_max <= u_min or v_max <= v_min:
            raise RuntimeError(
                f"source BRep face {face_index} has invalid trimmed UV bounds: "
                f"{bounds}"
            )

        attempt_limit = uv_attempt_multiplier * quota
        u_cells = max(1, math.isqrt(attempt_limit))
        v_cells = max(1, attempt_limit // u_cells)
        accepted_on_face = 0
        classifier_tolerance = max(
            min(u_max - u_min, v_max - v_min) * 1.0e-12,
            1.0e-12,
        )
        for v_index in range(v_cells):
            v = v_min + (v_index + 0.5) * (v_max - v_min) / v_cells
            for u_index in range(u_cells):
                if accepted_on_face >= quota:
                    break
                u = u_min + (u_index + 0.5) * (u_max - u_min) / u_cells
                classifier = ocp.BRepClass_FaceClassifier(
                    face, ocp.gp_Pnt2d(u, v), classifier_tolerance
                )
                attempted_uv_points += 1
                if classifier.State() not in (ocp.TopAbs_IN, ocp.TopAbs_ON):
                    continue
                point = surface.Value(u, v)
                coordinate = (
                    float(point.X()),
                    float(point.Y()),
                    float(point.Z()),
                )
                if not all(math.isfinite(value) for value in coordinate):
                    raise RuntimeError(
                        f"source BRep face {face_index} produced a non-finite "
                        "exact surface coordinate"
                    )
                samples.append(coordinate)
                accepted_on_face += 1
            if accepted_on_face >= quota:
                break

        accepted_trimmed_uv_points += accepted_on_face
        if accepted_on_face == 0:
            # A thin/degenerate trim can miss every bounded cell center.  Use one
            # exact topological face vertex as the sole fallback.
            fallback_coordinate: tuple[float, float, float] | None = None
            vertex_explorer = ocp.TopExp_Explorer(face, ocp.TopAbs_VERTEX)
            while vertex_explorer.More():
                vertex = ocp.TopoDS.Vertex_s(vertex_explorer.Current())
                point = ocp.BRep_Tool.Pnt_s(vertex)
                coordinate = (
                    float(point.X()),
                    float(point.Y()),
                    float(point.Z()),
                )
                if not all(math.isfinite(value) for value in coordinate):
                    raise RuntimeError(
                        f"source BRep face {face_index} has a non-finite exact "
                        "face-vertex fallback coordinate"
                    )
                if fallback_coordinate is None or coordinate < fallback_coordinate:
                    fallback_coordinate = coordinate
                vertex_explorer.Next()
            if fallback_coordinate is None:
                raise RuntimeError(
                    f"source BRep face {face_index} accepted no bounded trimmed "
                    "UV sample and has no exact face vertex fallback"
                )
            samples.append(fallback_coordinate)
            fallback_vertex_points += 1

        face_index += 1
        face_explorer.Next()

    sample_array = ocp.np.asarray(samples, dtype=ocp.np.float64).reshape((-1, 3))
    details = {
        "sampler": "deterministic_bounded_exact_trimmed_face_uv",
        "face_count": face_count,
        "quota_allocation": (
            "one sample per face, then equal quotient/remainder distribution "
            "in deterministic TopExp face order"
        ),
        "uv_grid": "cell-centered rectangular parameter-space grid",
        "max_uv_attempts": uv_attempt_multiplier * max_samples,
        "uv_attempt_multiplier": uv_attempt_multiplier,
        "attempted_uv_points": attempted_uv_points,
        "accepted_trimmed_uv_points": accepted_trimmed_uv_points,
        "fallback_exact_face_vertex_points": fallback_vertex_points,
        "accepted_point_storage_limit": max_samples,
        "accepted_points": len(sample_array),
        "max_budget": max_samples,
    }
    return sample_array, details


def _exact_point_distances(points, shape, label: str, ocp: SimpleNamespace):
    distances = ocp.np.empty(len(points), dtype=ocp.np.float64)
    for index, coordinate in enumerate(points):
        query_coordinate = tuple(float(value) for value in coordinate)
        if len(query_coordinate) != 3 or not all(
            math.isfinite(value) for value in query_coordinate
        ):
            raise RuntimeError(f"{label}: query point {index} is not a finite 3-D coordinate")
        vertex_builder = ocp.BRepBuilderAPI_MakeVertex(
            ocp.gp_Pnt(*query_coordinate)
        )
        if not vertex_builder.IsDone():
            raise RuntimeError(f"{label}: could not construct query vertex {index}")
        query = ocp.BRepExtrema_DistShapeShape(vertex_builder.Vertex(), shape)
        query.Perform()
        if not query.IsDone() or query.NbSolution() <= 0:
            raise RuntimeError(f"{label}: exact OpenCASCADE distance query {index} failed")
        distance = float(query.Value())
        if not math.isfinite(distance) or distance < 0.0:
            raise RuntimeError(
                f"{label}: exact OpenCASCADE distance query {index} returned "
                f"invalid distance {distance}"
            )
        distances[index] = distance
    return distances


def _distance_statistics(distances, diagonal: float, h: float | None, np) -> dict:
    values = {
        "rms": float(np.sqrt(np.mean(np.square(distances)))),
        "p95": float(np.percentile(distances, 95)),
        "p99": float(np.percentile(distances, 99)),
        "max": float(np.max(distances)),
    }
    return {
        "absolute": values,
        "over_bbox_diagonal": {key: value / diagonal for key, value in values.items()},
        "over_h": ({key: value / h for key, value in values.items()} if h else None),
    }


def _write_shape(shape, path: Path, ocp: SimpleNamespace) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix.lower() in {".step", ".stp"}:
        ocp.Interface_Static.SetCVal_s("write.step.schema", "AP214")
        ocp.Interface_Static.SetCVal_s(
            "write.step.product.name", "faceted_mesh_roundtrip"
        )
        writer = ocp.STEPControl_Writer()
        transfer_status = writer.Transfer(shape, ocp.STEPControl_AsIs)
        if transfer_status != ocp.IFSelect_RetDone:
            raise RuntimeError(f"AP214 STEP transfer failed (status {transfer_status})")
        write_status = writer.Write(str(path))
        if write_status != ocp.IFSelect_RetDone:
            raise RuntimeError(f"AP214 STEP write failed (status {write_status})")
        return "STEP_AP214"
    write_ok = ocp.BRepTools.Write_s(shape, str(path))
    if write_ok is False or not path.is_file():
        raise RuntimeError(f"native BREP write failed: {path}")
    return "BREP_NATIVE"


def _resolved(path: Path) -> Path:
    return path.expanduser().resolve()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Build a sewn faceted BRep from a VTU exterior skin and report "
            "exact point-to-shape distances using deterministic mesh-boundary "
            "and bounded exact trimmed-face UV samples."
        )
    )
    parser.add_argument("--source", required=True, type=Path, help="source STEP or BREP")
    parser.add_argument("--mesh", required=True, type=Path, help="existing volume VTU")
    parser.add_argument("--out-brep", required=True, type=Path, help="output .step/.stp or native BREP")
    parser.add_argument("--out-json", required=True, type=Path, help="metrics/provenance JSON")
    parser.add_argument("--h", type=float, default=None, help="optional mesh scale for normalized metrics")
    parser.add_argument(
        "--max-faces",
        type=int,
        default=100_000,
        help="hard ceiling for exterior polygons and constructed triangular BRep faces",
    )
    parser.add_argument(
        "--max-samples",
        type=int,
        default=5_000,
        help="hard ceiling per directional exact-distance sample set",
    )
    parser.add_argument(
        "--max-mesh-bytes",
        type=int,
        default=2_000_000_000,
        help="pre-read hard ceiling on VTU file bytes",
    )
    parser.add_argument(
        "--max-mesh-points",
        type=int,
        default=20_000_000,
        help="pre-read hard ceiling on VTU Piece NumberOfPoints",
    )
    parser.add_argument(
        "--max-mesh-cells",
        type=int,
        default=20_000_000,
        help="pre-read hard ceiling on VTU Piece NumberOfCells",
    )
    args = parser.parse_args()

    source = _resolved(args.source)
    mesh = _resolved(args.mesh)
    out_brep = _resolved(args.out_brep)
    out_json = _resolved(args.out_json)
    if not source.is_file():
        raise RuntimeError(f"source CAD file not found: {source}")
    if not mesh.is_file():
        raise RuntimeError(f"VTU mesh file not found: {mesh}")
    if mesh.suffix.lower() != ".vtu":
        raise RuntimeError(f"--mesh must name a .vtu file, got: {mesh}")
    if args.h is not None and (not math.isfinite(args.h) or args.h <= 0.0):
        raise RuntimeError("--h must be finite and positive when supplied")
    if args.max_faces <= 0:
        raise RuntimeError("--max-faces must be positive")
    if args.max_samples <= 0:
        raise RuntimeError("--max-samples must be positive")
    if args.max_mesh_bytes <= 0:
        raise RuntimeError("--max-mesh-bytes must be positive")
    if args.max_mesh_points <= 0:
        raise RuntimeError("--max-mesh-points must be positive")
    if args.max_mesh_cells <= 0:
        raise RuntimeError("--max-mesh-cells must be positive")
    if len({source, mesh, out_brep, out_json}) != 4:
        raise RuntimeError("input and output paths must all be distinct")

    mesh_bytes_before_hash = mesh.stat().st_size
    if mesh_bytes_before_hash > args.max_mesh_bytes:
        raise RuntimeError(
            f"VTU mesh is {mesh_bytes_before_hash} bytes, exceeding "
            f"--max-mesh-bytes={args.max_mesh_bytes}"
        )

    script_path = Path(__file__).resolve()
    source_snapshot = _file_snapshot(source)
    mesh_snapshot = _file_snapshot(mesh)
    script_snapshot = _file_snapshot(script_path)
    mesh_preflight = _preflight_vtu(
        mesh,
        int(mesh_snapshot["bytes"]),
        args.max_mesh_bytes,
        args.max_mesh_points,
        args.max_mesh_cells,
    )

    deps = _load_dependencies()
    source_shape, source_format = _load_source(source, deps)
    source_minimum, source_maximum, diagonal = _bbox(source_shape, deps)
    source_topology = _topology_counts(source_shape, deps)
    if args.max_samples < source_topology["faces"]:
        raise RuntimeError(
            f"--max-samples={args.max_samples} cannot cover all "
            f"{source_topology['faces']} source BRep faces; exact trimmed-face "
            "sampling requires at least one sample budget per face"
        )

    points, triangle_ids, boundary_candidates, closure, mesh_info = (
        _read_boundary_polygons(
            mesh,
            args.max_faces,
            args.max_mesh_points,
            args.max_mesh_cells,
            deps,
        )
    )
    mesh_info["preflight"] = mesh_preflight
    faceted_shape, construction = _make_faceted_shape(
        points, triangle_ids, diagonal, deps
    )

    mesh_samples, mesh_candidates = _mesh_boundary_samples(
        boundary_candidates, args.max_samples, deps.np
    )
    source_samples, source_sampling = _source_trimmed_face_samples(
        source_shape, args.max_samples, deps
    )
    forward_distances = _exact_point_distances(
        mesh_samples, source_shape, "mesh boundary to source live BRep", deps
    )
    reverse_distances = _exact_point_distances(
        source_samples,
        faceted_shape,
        "source exact trimmed-face samples to faceted BRep",
        deps,
    )

    _verify_file_snapshot(source, source_snapshot, "source CAD input")
    _verify_file_snapshot(mesh, mesh_snapshot, "VTU mesh input")

    output_format = _write_shape(faceted_shape, out_brep, deps)
    command_argv = [sys.executable, str(script_path), *sys.argv[1:]]
    payload = {
        "schema": "polymesh.faceted-brep-roundtrip.v2",
        "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "representation": {
            "kind": "faceted_brep",
            "description": "planar triangular BRep faces sewn from the exterior VTU skin",
            "smooth_or_analytic_surface_fitting": False,
        },
        "commands": [
            {
                "kind": "invocation",
                "argv": command_argv,
                "display": shlex.join(command_argv),
                "cwd": os.getcwd(),
            }
        ],
        "provenance": {
            "script": {
                "path": str(script_path),
                **script_snapshot,
            },
            "python": sys.version,
            "platform": platform.platform(),
            "dependencies": {
                "numpy": deps.np.__version__,
                "pyvista": deps.pv.__version__,
                "vtk": _dependency_version("vtk"),
                "cadquery-ocp": _dependency_version("cadquery-ocp"),
            },
            "source": {
                "path": str(source),
                "format": source_format,
                **source_snapshot,
            },
            "mesh": {
                "path": str(mesh),
                "format": "VTU",
                **mesh_snapshot,
            },
            "output": {
                "path": str(out_brep),
                "format": output_format,
                **_file_snapshot(out_brep),
            },
        },
        "parameters": {
            "h": args.h,
            "max_faces": args.max_faces,
            "max_samples_per_direction": args.max_samples,
            "max_mesh_bytes": args.max_mesh_bytes,
            "max_mesh_points": args.max_mesh_points,
            "max_mesh_cells": args.max_mesh_cells,
            "mesh_sample_order": "exact-coordinate lexicographic order",
            "mesh_subsampling": "evenly spaced integer indices including both endpoints; no randomness",
            "source_sample_order": "TopExp face order, then cell-centered UV grid order; no randomness",
        },
        "source_brep": {
            "brepcheck_valid": True,
            "topology": source_topology,
            "bbox": {
                "minimum": source_minimum,
                "maximum": source_maximum,
                "diagonal": diagonal,
            },
        },
        "volume_mesh": mesh_info,
        "mesh_skin_closure": closure,
        "faceted_brep": construction,
        "directional_sampled_distances": {
            "query_method": "OpenCASCADE BRepExtrema_DistShapeShape exact point-to-shape minimum",
            "mesh_boundary_samples_to_source_live_brep": {
                "direction": "exterior VTU boundary sample point -> source live BRep",
                "sample_basis": "exterior skin vertices and original polygon centroids",
                "candidate_points": mesh_candidates,
                "evaluated_samples": len(mesh_samples),
                "statistics": _distance_statistics(
                    forward_distances, diagonal, args.h, deps.np
                ),
            },
            "source_exact_trimmed_face_samples_to_faceted_brep": {
                "direction": "source exact trimmed-face sample point -> constructed faceted BRep",
                "sample_basis": "bounded cell-centered UV points classified IN/ON each exact trimmed face, with exact face-vertex fallback only when none are accepted",
                "sampling": source_sampling,
                "evaluated_samples": len(source_samples),
                "statistics": _distance_statistics(
                    reverse_distances, diagonal, args.h, deps.np
                ),
            },
        },
    }

    _verify_file_snapshot(source, source_snapshot, "source CAD input")
    _verify_file_snapshot(mesh, mesh_snapshot, "VTU mesh input")
    _verify_file_snapshot(script_path, script_snapshot, "evaluator script")

    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(
        json.dumps(payload, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )
    print(f"wrote faceted BRep: {out_brep}")
    print(f"wrote metrics: {out_json}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
