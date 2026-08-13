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

#: Column order of the emitted table (after ``part``).
FEATURE_NAMES: list[str] = [
    "geo_curved_area_frac", "geo_cyl_area_frac", "geo_plane_area_frac",
    "geo_other_area_frac", "geo_min_curv_radius_rel", "geo_log_curv_radius_mean",
    "geo_log_curv_radius_std", "geo_n_faces", "geo_n_edges",
    "geo_face_area_cv", "geo_aspect_max", "geo_aspect_mid", "geo_volume_frac",
    "geo_area_over_v23", "geo_min_face_size_rel",
]


def _occ():
    """Import OCP lazily so the module can be imported without the binding."""
    from OCP.BRep import BRep_Tool
    from OCP.BRepAdaptor import BRepAdaptor_Surface
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepGProp import BRepGProp
    from OCP.Bnd import Bnd_Box
    from OCP.GProp import GProp_GProps
    from OCP.GeomAbs import GeomAbs_SurfaceType
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_SHELL
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS
    return dict(BRep_Tool=BRep_Tool, BRepAdaptor_Surface=BRepAdaptor_Surface,
                BRepBndLib=BRepBndLib, BRepGProp=BRepGProp, Bnd_Box=Bnd_Box,
                GProp_GProps=GProp_GProps, GeomAbs=GeomAbs_SurfaceType,
                STEPControl_Reader=STEPControl_Reader, TopAbs_EDGE=TopAbs_EDGE,
                TopAbs_FACE=TopAbs_FACE, TopAbs_SHELL=TopAbs_SHELL,
                TopExp_Explorer=TopExp_Explorer, TopoDS=TopoDS)


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
