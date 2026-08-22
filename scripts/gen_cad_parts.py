#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate product STEP fixtures under tests/fixtures/parts/.

Product geometry path is **STEP only** (ADR-0020). Optional comparison STLs
are written only when ``--export-stl-compare`` is passed — never as the
default product output.

Requires the ``OCP`` OpenCASCADE Python bindings (pythonocc-core / OCP).
Run from repo root:

    python3 scripts/gen_cad_parts.py
    python3 scripts/gen_cad_parts.py --part icecream_cone
    python3 scripts/gen_cad_parts.py --part wishbone
    python3 scripts/gen_cad_parts.py --export-stl-compare
    python3 scripts/gen_cad_parts.py --part wishbone --check

Without ``--part`` the command writes all five product fixtures. Repeat
``--part`` to regenerate a selected subset:
  tests/fixtures/parts/plate_hole.step
  tests/fixtures/parts/cylinder.step
  tests/fixtures/parts/sphere.step
  tests/fixtures/parts/icecream_cone.step
  tests/fixtures/parts/wishbone.step

With ``--export-stl-compare``, also writes sibling ``*_compare.stl`` files.

``--check`` never writes into the fixture directory. It rebuilds each selected
part from these frozen parameters into a temporary directory and compares the
checked-in STEP against the rebuild on topology and volume-level facts (solid /
face / edge counts, volume, tight bounding box). Byte-equality is deliberately
not asserted: STEP headers carry a write timestamp, so a byte compare would
fail on an unchanged solid.

Geometry only — case JSON and bench/reference truths are hand-authored
elsewhere (anti-cheat boundary).

Legacy STL-only fixtures for older campaigns remain under the same directory;
see scripts/gen_part_library.py (soft-deprecated for product path).
"""
from __future__ import annotations

import argparse
import math
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "fixtures" / "parts"

try:
    from OCP.BRepAdaptor import BRepAdaptor_Surface
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeEdge, BRepBuilderAPI_MakeWire
    from OCP.BRepClass3d import BRepClass3d_SolidClassifier
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.BRepGProp import BRepGProp
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.BRepOffsetAPI import BRepOffsetAPI_MakePipeShell
    from OCP.BRepPrimAPI import (
        BRepPrimAPI_MakeBox,
        BRepPrimAPI_MakeCone,
        BRepPrimAPI_MakeCylinder,
        BRepPrimAPI_MakeSphere,
    )
    from OCP.Bnd import Bnd_Box
    from OCP.GeomAbs import GeomAbs_Cylinder
    from OCP.GeomAPI import GeomAPI_PointsToBSpline
    from OCP.GProp import GProp_GProps
    from OCP.gp import gp_Ax2, gp_Circ, gp_Dir, gp_Pnt, gp_Vec
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.Interface import Interface_Static
    from OCP.ShapeFix import ShapeFix_Shape
    from OCP.STEPControl import STEPControl_AsIs, STEPControl_Reader, STEPControl_Writer
    from OCP.StlAPI import StlAPI_Writer
    from OCP.TColgp import TColgp_Array1OfPnt
    from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_IN, TopAbs_OUT, TopAbs_SOLID
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS
except ImportError as exc:  # pragma: no cover
    print(
        "error: OCP (OpenCASCADE Python bindings) is required to generate STEP.\n"
        "  Install e.g. `pip install cadquery-ocp` or system pythonocc-core.\n"
        f"  import failed: {exc}",
        file=sys.stderr,
    )
    sys.exit(1)


def _is_valid(shape) -> bool:
    return bool(BRepCheck_Analyzer(shape).IsValid())


def _volume(shape) -> float:
    props = GProp_GProps()
    BRepGProp.VolumeProperties_s(shape, props)
    return float(props.Mass())


def _count(shape, kind) -> int:
    """Number of sub-shapes of `kind` (TopAbs enum) reachable from `shape`."""
    explorer = TopExp_Explorer(shape, kind)
    total = 0
    while explorer.More():
        total += 1
        explorer.Next()
    return total


def _faces(shape) -> list:
    """Faces in TopExp exploration order — the order the C++ tessellator uses."""
    out = []
    explorer = TopExp_Explorer(shape, TopAbs_FACE)
    while explorer.More():
        out.append(TopoDS.Face_s(explorer.Current()))
        explorer.Next()
    return out


def _face_area(face) -> float:
    props = GProp_GProps()
    BRepGProp.SurfaceProperties_s(face, props)
    return float(props.Mass())


def _bbox(shape) -> tuple[float, float, float, float, float, float]:
    """Tight bounding box. `AddOptimal` matters here: the cheap `Add` inflates a
    swept B-spline body to its control-point hull, which is not the part."""
    box = Bnd_Box()
    BRepBndLib.AddOptimal_s(shape, box, True, False)
    return tuple(float(v) for v in box.Get())


def _display(path: Path) -> str:
    """Repo-relative when inside the tree, absolute otherwise (``--check`` uses
    a temporary directory)."""
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def _require_solid(shape, name: str) -> None:
    if not _is_valid(shape):
        raise RuntimeError(f"{name}: BRep not valid after construction")
    solid_count = _count(shape, TopAbs_SOLID)
    if solid_count != 1:
        raise RuntimeError(f"{name}: expected one connected solid, got {solid_count}")
    vol = _volume(shape)
    if vol <= 0.0:
        raise RuntimeError(f"{name}: non-positive volume ({vol})")


def write_step(shape, path: Path, *, name: str) -> None:
    """Write a manifold solid as AP214 STEP."""
    _require_solid(shape, name)
    writer = STEPControl_Writer()
    Interface_Static.SetCVal_s("write.step.schema", "AP214")
    # Prefer a stable product name in the STEP header when supported.
    Interface_Static.SetCVal_s("write.step.product.name", name)
    status = writer.Transfer(shape, STEPControl_AsIs)
    if status != IFSelect_RetDone:
        raise RuntimeError(f"{name}: STEP transfer failed ({status})")
    path.parent.mkdir(parents=True, exist_ok=True)
    status = writer.Write(str(path))
    if status != IFSelect_RetDone:
        raise RuntimeError(f"{name}: STEP write failed ({status})")
    # OpenCASCADE emits a space before some STEP line endings. Normalize only
    # trailing whitespace so generated fixtures pass patch-integrity checks
    # without changing entity ordering or numeric data.
    text = path.read_text(encoding="utf-8")
    path.write_text(
        "\n".join(line.rstrip() for line in text.splitlines()) + "\n",
        encoding="utf-8",
    )
    vol = _volume(shape)
    print(f"wrote {_display(path)}  (volume={vol:.6g} m^3)")


def read_step(path: Path):
    """Read a single-shape STEP back in — used by ``--check`` and smoke tests."""
    reader = STEPControl_Reader()
    if reader.ReadFile(str(path)) != IFSelect_RetDone:
        raise RuntimeError(f"{path}: STEP read failed")
    reader.TransferRoots()
    if reader.NbShapes() != 1:
        raise RuntimeError(f"{path}: expected one root shape, got {reader.NbShapes()}")
    return reader.Shape(1)


def step_facts(shape) -> dict:
    """Topology and volume-level facts that identify a solid without its bytes."""
    return {
        "solids": _count(shape, TopAbs_SOLID),
        "faces": _count(shape, TopAbs_FACE),
        "edges": _count(shape, TopAbs_EDGE),
        "volume_m3": _volume(shape),
        "bbox_m": _bbox(shape),
    }


def check_step(shape, path: Path, *, name: str) -> bool:
    """Compare the checked-in STEP at `path` against a fresh rebuild.

    Deliberately not a byte compare: the STEP header carries a write timestamp,
    so identical geometry produces different bytes on every run. What must hold
    is that the checked-in file still reopens as the same topology and the same
    measured solid.
    """
    if not path.exists():
        print(f"FAIL {name}: {_display(path)} is missing")
        return False
    expected = step_facts(shape)
    actual = step_facts(read_step(path))
    problems = []
    for key in ("solids", "faces", "edges"):
        if expected[key] != actual[key]:
            problems.append(f"{key} {actual[key]} != rebuilt {expected[key]}")
    if abs(actual["volume_m3"] - expected["volume_m3"]) > 1e-9 * max(
        abs(expected["volume_m3"]), 1e-12
    ):
        problems.append(
            f"volume {actual['volume_m3']!r} != rebuilt {expected['volume_m3']!r}"
        )
    for index, (got, want) in enumerate(zip(actual["bbox_m"], expected["bbox_m"])):
        if abs(got - want) > 1e-9:
            problems.append(f"bbox[{index}] {got!r} != rebuilt {want!r}")
    if problems:
        print(f"FAIL {name}: {_display(path)}")
        for problem in problems:
            print(f"       {problem}")
        return False
    lo = actual["bbox_m"][:3]
    hi = actual["bbox_m"][3:]
    print(
        f"ok   {name}: {_display(path)}  "
        f"solids={actual['solids']} faces={actual['faces']} "
        f"edges={actual['edges']} volume={actual['volume_m3']:.10g} m^3 "
        f"extent=({hi[0] - lo[0]:.6g}, {hi[1] - lo[1]:.6g}, {hi[2] - lo[2]:.6g}) m"
    )
    return True


def write_stl_compare(shape, path: Path, *, linear_deflection: float = 0.001) -> None:
    """Tessellate and write an ASCII-friendly STL for visual compare only."""
    BRepMesh_IncrementalMesh(shape, linear_deflection)
    path.parent.mkdir(parents=True, exist_ok=True)
    writer = StlAPI_Writer()
    # ASCII is easier to diff in review; fall back if the binding lacks the API.
    if hasattr(writer, "SetASCIIMode"):
        writer.SetASCIIMode(True)
    ok = writer.Write(shape, str(path))
    if not ok:
        raise RuntimeError(f"STL compare write failed: {path}")
    print(f"wrote {_display(path)}  (compare STL only)")


# ── Part constructors (SI metres) ───────────────────────────────────────────


def make_plate_hole(
    *,
    half_w: float = 0.1,
    half_h: float = 0.05,
    thickness: float = 0.01,
    hole_r: float = 0.01,
):
    """Centered plate with through-hole along z — matches legacy plate_hole dims.

    x ∈ [-half_w, half_w], y ∈ [-half_h, half_h], z ∈ [0, thickness].
    Hole axis through origin in the plate mid-plane projection (x=y=0).
    """
    if hole_r >= min(half_w, half_h):
        raise ValueError("hole_r must be smaller than plate half-extents")
    box = BRepPrimAPI_MakeBox(
        gp_Pnt(-half_w, -half_h, 0.0),
        2.0 * half_w,
        2.0 * half_h,
        thickness,
    ).Shape()
    # Cylinder slightly longer than thickness so the cut is clean through.
    margin = max(thickness * 0.5, 1e-4)
    axis = gp_Ax2(gp_Pnt(0.0, 0.0, -margin), gp_Dir(0.0, 0.0, 1.0))
    cutter = BRepPrimAPI_MakeCylinder(axis, hole_r, thickness + 2.0 * margin).Shape()
    return BRepAlgoAPI_Cut(box, cutter).Shape()


def make_cylinder(*, radius: float = 0.05, height: float = 0.2):
    """Solid cylinder, axis along +z, base circle in z=0 plane, center at origin."""
    axis = gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0))
    return BRepPrimAPI_MakeCylinder(axis, radius, height).Shape()


def make_sphere(*, radius: float = 0.05):
    """Solid sphere centered at the origin."""
    return BRepPrimAPI_MakeSphere(radius).Shape()


def make_icecream_cone(
    *,
    bottom_radius: float = 0.006,
    top_radius: float = 0.032,
    cone_height: float = 0.100,
    scoop_radius: float = 0.035,
    scoop_center_z: float = 0.112,
):
    """Upright round truncated cone fused to an overlapping spherical scoop."""
    if not 0.0 < bottom_radius < top_radius:
        raise ValueError("cone radii must satisfy 0 < bottom_radius < top_radius")
    if cone_height <= 0.0 or scoop_radius <= 0.0:
        raise ValueError("cone height and scoop radius must be positive")

    axis = gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0))
    cone = BRepPrimAPI_MakeCone(axis, bottom_radius, top_radius, cone_height).Shape()
    scoop = BRepPrimAPI_MakeSphere(
        gp_Pnt(0.0, 0.0, scoop_center_z), scoop_radius
    ).Shape()

    fusion = BRepAlgoAPI_Fuse(cone, scoop)
    fusion.Build()
    if not fusion.IsDone():
        raise RuntimeError("icecream_cone: cone/scoop boolean fuse failed")

    fixer = ShapeFix_Shape(fusion.Shape())
    fixer.Perform()
    result = fixer.Shape()
    _require_solid(result, "icecream_cone")
    return result


# ── wishbone / A-arm (frozen deterministic parameters, SI metres) ───────────
#
# Suspension-arm layout. +x points outboard toward the upright, +y is the
# chassis pivot axis (fore-aft), +z is up.
#
#   * Two chassis-side bushing eyes, coaxial on the y pivot axis at
#     y = ±0.140 m, each a sleeve bored along y.  These are the two supports.
#   * One upright-side ball-joint boss at x = 0.300 m bored along z.  This is
#     the loaded interface.
#   * Two swept arms with different z profiles carry load from the eyes to the
#     boss, and one swept cross-brace ties the arms together mid-span.
#
# The two arms are deliberately on opposite sides of the plane through the
# three eye centres, so the part is a three-dimensional fork rather than a
# perforated plate: `_require_wishbone_features` measures that and fails the
# build if the offsets ever collapse toward coplanar.
WISHBONE_EYE_CENTRES = (
    (0.020, 0.140, 0.020),
    (0.020, -0.140, 0.020),
)
# Ring and member radii are sized by minimum wall thickness, not by looks. The
# meshing pipeline snaps boundary nodes to the exact BRep inside a band of
# 1.5 * fill_h (scene.cpp), so any wall thinner than that band lets the two
# sides snap onto each other and the skin goes non-manifold. An earlier
# 24/9.5 mm eye and 28/12 mm boss left a 14.5 mm minimum wall, which failed
# the product fill guard at h = 8 mm and h = 16 mm. These radii hold a 22 mm
# bushing wall and a 24 mm boss wall against the 12 mm band at h = 8 mm.
WISHBONE_EYE_R = 0.030
WISHBONE_EYE_LEN = 0.048
WISHBONE_EYE_BORE_R = 0.008
WISHBONE_BOSS_CENTRE = (0.300, 0.000, 0.044)
WISHBONE_BOSS_R = 0.034
WISHBONE_BOSS_H = 0.060
WISHBONE_BOSS_BORE_R = 0.010
WISHBONE_ARM_R = 0.017
WISHBONE_BRACE_R = 0.013
# Fitted spine samples. Both arms start inside an eye and end inside the boss;
# the brace endpoints sit well inside the 17 mm arm radius, so the constructor's
# millimetre-scale default fit tolerance cannot break their overlap.
WISHBONE_ARM_HI_SPINE = (
    (0.020, 0.140, 0.020),
    (0.090, 0.118, 0.044),
    (0.170, 0.078, 0.058),
    (0.245, 0.035, 0.052),
    (0.300, 0.000, 0.044),
)
WISHBONE_ARM_LO_SPINE = (
    (0.020, -0.140, 0.020),
    (0.095, -0.115, 0.008),
    (0.175, -0.075, 0.016),
    (0.248, -0.032, 0.032),
    (0.300, 0.000, 0.044),
)
WISHBONE_BRACE_SPINE = (
    (0.170, 0.078, 0.058),
    (0.135, 0.030, 0.052),
    (0.130, -0.028, 0.030),
    (0.175, -0.075, 0.016),
)
# Minimum out-of-plane excursion each arm must keep away from the plane through
# the three eye centres, and the minimum z travel along each arm spine.
WISHBONE_MIN_OUT_OF_PLANE = 0.012
WISHBONE_MIN_ARM_DZ = 0.015


def _interpolated_curve(points):
    """C2 B-spline fitted through `points` at OpenCASCADE's default tolerance."""
    array = TColgp_Array1OfPnt(1, len(points))
    for index, (x, y, z) in enumerate(points, start=1):
        array.SetValue(index, gp_Pnt(x, y, z))
    return GeomAPI_PointsToBSpline(array).Curve()


def _swept_tube(points, radius: float, name: str):
    """Solid circular tube swept along the B-spline through `points`.

    The section is a circle normal to the spine at its start and is carried by
    corrected-Frenet transport, so the tube stays round along the whole span.
    """
    curve = _interpolated_curve(points)
    spine = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(curve).Edge()).Wire()
    start = curve.Value(curve.FirstParameter())
    tangent = gp_Vec()
    curve.D1(curve.FirstParameter(), gp_Pnt(), tangent)
    section = BRepBuilderAPI_MakeWire(
        BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(start, gp_Dir(tangent)), radius)).Edge()
    ).Wire()
    pipe = BRepOffsetAPI_MakePipeShell(spine)
    pipe.SetMode(False)
    pipe.Add(section, False, False)
    pipe.Build()
    if not pipe.IsDone():
        raise RuntimeError(f"{name}: sweep failed")
    if not pipe.MakeSolid():
        raise RuntimeError(f"{name}: swept shell did not close into a solid")
    return pipe.Shape(), curve


def _cylinder(base, direction, radius: float, height: float):
    axis = gp_Ax2(gp_Pnt(*base), gp_Dir(*direction))
    return BRepPrimAPI_MakeCylinder(axis, radius, height).Shape()


def _sample_curve(curve, count: int = 121):
    first, last = curve.FirstParameter(), curve.LastParameter()
    span = last - first
    out = []
    for index in range(count):
        point = curve.Value(first + span * index / (count - 1))
        out.append((point.X(), point.Y(), point.Z()))
    return out


def _classify(shape, point) -> int:
    return BRepClass3d_SolidClassifier(shape, gp_Pnt(*point), 1e-7).State()


def wishbone_bores() -> tuple[tuple[str, tuple[float, float, float], tuple[float, float, float], float, float], ...]:
    """(label, sleeve start, axis, bore radius, sleeve length) per through-bore."""
    (_, y_hi, _) = WISHBONE_EYE_CENTRES[0]
    (_, y_lo, _) = WISHBONE_EYE_CENTRES[1]
    x_eye, _, z_eye = WISHBONE_EYE_CENTRES[0]
    half = 0.5 * WISHBONE_EYE_LEN
    bx, by, bz = WISHBONE_BOSS_CENTRE
    return (
        (
            "support_y_hi",
            (x_eye, y_hi - half, z_eye),
            (0.0, 1.0, 0.0),
            WISHBONE_EYE_BORE_R,
            WISHBONE_EYE_LEN,
        ),
        (
            "support_y_lo",
            (x_eye, y_lo - half, z_eye),
            (0.0, 1.0, 0.0),
            WISHBONE_EYE_BORE_R,
            WISHBONE_EYE_LEN,
        ),
        (
            "loaded_boss",
            (bx, by, bz - 0.5 * WISHBONE_BOSS_H),
            (0.0, 0.0, 1.0),
            WISHBONE_BOSS_BORE_R,
            WISHBONE_BOSS_H,
        ),
    )


def _require_wishbone_features(shape, curves) -> None:
    """Gate the properties the part exists for, not just BRep validity.

    A valid single solid is not enough: the film claims a forked three-
    dimensional load path with three selectable bored interfaces, so each of
    those claims is measured here and raises rather than shipping a lie.
    """
    name = "wishbone"

    # 1. Three through-bores: the whole axis inside each sleeve must be void,
    #    and material must exist just outside each bore wall.
    for label, base, direction, bore_r, length in wishbone_bores():
        for step in range(11):
            travel = length * step / 10.0
            probe = tuple(base[i] + direction[i] * travel for i in range(3))
            if _classify(shape, probe) != TopAbs_OUT:
                raise RuntimeError(
                    f"{name}: {label} bore is not open at {probe} — not a through-bore"
                )
        # Radial probe midway through the remaining wall, perpendicular to the
        # bore axis (y-axis bores probe in z, the z-axis bore probes in x).
        wall = 0.5 * (bore_r + (WISHBONE_EYE_R if direction[1] else WISHBONE_BOSS_R))
        offset = (wall, 0.0, 0.0) if direction[2] else (0.0, 0.0, wall)
        centre = tuple(base[i] + direction[i] * 0.5 * length for i in range(3))
        probe = tuple(centre[i] + offset[i] for i in range(3))
        if _classify(shape, probe) != TopAbs_IN:
            raise RuntimeError(f"{name}: {label} has no wall material at {probe}")

    # 2. Exactly three cylindrical faces carry the expected bore radii, each at
    #    full sleeve length, so all three interfaces survived the booleans as
    #    single uninterrupted faces.
    expected = {}
    for label, _, _, bore_r, length in wishbone_bores():
        expected.setdefault(round(bore_r, 9), []).append((label, length))
    found: dict[float, int] = {}
    for face in _faces(shape):
        surface = BRepAdaptor_Surface(face)
        if surface.GetType() != GeomAbs_Cylinder:
            continue
        radius = round(surface.Cylinder().Radius(), 9)
        if radius not in expected:
            continue
        _, length = expected[radius][0]
        full = 2.0 * math.pi * radius * length
        if abs(_face_area(face) - full) > 1e-9:
            raise RuntimeError(
                f"{name}: bore face r={radius} area {_face_area(face)} != "
                f"full sleeve {full} — the bore was interrupted"
            )
        found[radius] = found.get(radius, 0) + 1
    for radius, entries in expected.items():
        if found.get(radius, 0) != len(entries):
            raise RuntimeError(
                f"{name}: expected {len(entries)} bore face(s) at r={radius}, "
                f"found {found.get(radius, 0)}"
            )

    # 3. The fork is genuinely three-dimensional: the arms sit on opposite
    #    sides of the plane through the three eye centres, and each arm spine
    #    travels in z.
    p0 = WISHBONE_EYE_CENTRES[0]
    p1 = WISHBONE_EYE_CENTRES[1]
    p2 = WISHBONE_BOSS_CENTRE
    u = tuple(p1[i] - p0[i] for i in range(3))
    v = tuple(p2[i] - p0[i] for i in range(3))
    normal = (
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    )
    scale = math.sqrt(sum(c * c for c in normal))
    normal = tuple(c / scale for c in normal)
    excursions = []
    for label in ("arm_hi", "arm_lo"):
        samples = _sample_curve(curves[label])
        signed = [
            sum((s[i] - p0[i]) * normal[i] for i in range(3)) for s in samples
        ]
        peak = max(signed, key=abs)
        if abs(peak) < WISHBONE_MIN_OUT_OF_PLANE:
            raise RuntimeError(
                f"{name}: {label} peak out-of-plane excursion {peak} is below "
                f"{WISHBONE_MIN_OUT_OF_PLANE} — the fork collapsed to a plate"
            )
        z_values = [s[2] for s in samples]
        if max(z_values) - min(z_values) < WISHBONE_MIN_ARM_DZ:
            raise RuntimeError(
                f"{name}: {label} z travel {max(z_values) - min(z_values)} is "
                f"below {WISHBONE_MIN_ARM_DZ} — the arm is not curved in z"
            )
        excursions.append(peak)
    if excursions[0] * excursions[1] >= 0.0:
        raise RuntimeError(
            f"{name}: both arms lie on the same side of the eye plane "
            f"({excursions}) — not a non-coplanar fork"
        )


def make_wishbone():
    """Suspension wishbone: two bored chassis eyes forked into one loaded boss.

    Two swept arms with different out-of-plane profiles plus one swept
    cross-brace are fused with three ring bosses into a single solid, then the
    three bores are cut through. All parameters are module constants, so the
    solid is a pure function of this file.
    """
    curves = {}
    arm_hi, curves["arm_hi"] = _swept_tube(
        WISHBONE_ARM_HI_SPINE, WISHBONE_ARM_R, "wishbone arm_hi"
    )
    arm_lo, curves["arm_lo"] = _swept_tube(
        WISHBONE_ARM_LO_SPINE, WISHBONE_ARM_R, "wishbone arm_lo"
    )
    brace, curves["brace"] = _swept_tube(
        WISHBONE_BRACE_SPINE, WISHBONE_BRACE_R, "wishbone brace"
    )

    half = 0.5 * WISHBONE_EYE_LEN
    eyes = [
        _cylinder(
            (cx, cy - half, cz), (0.0, 1.0, 0.0), WISHBONE_EYE_R, WISHBONE_EYE_LEN
        )
        for cx, cy, cz in WISHBONE_EYE_CENTRES
    ]
    bx, by, bz = WISHBONE_BOSS_CENTRE
    boss = _cylinder(
        (bx, by, bz - 0.5 * WISHBONE_BOSS_H),
        (0.0, 0.0, 1.0),
        WISHBONE_BOSS_R,
        WISHBONE_BOSS_H,
    )

    result = arm_hi
    for other in (arm_lo, brace, eyes[0], eyes[1], boss):
        fusion = BRepAlgoAPI_Fuse(result, other)
        fusion.SimplifyResult()
        if not fusion.IsDone():
            raise RuntimeError("wishbone: member fuse failed")
        result = fusion.Shape()
    _require_solid(result, "wishbone (fused, before bores)")

    # Cut each bore with a cylinder overhanging both sleeve ends so the opening
    # is clean through and the bore wall stays one face.
    for label, base, direction, bore_r, length in wishbone_bores():
        margin = max(0.25 * length, 1e-3)
        start = tuple(base[i] - direction[i] * margin for i in range(3))
        cutter = _cylinder(start, direction, bore_r, length + 2.0 * margin)
        cut = BRepAlgoAPI_Cut(result, cutter)
        cut.SimplifyResult()
        if not cut.IsDone():
            raise RuntimeError(f"wishbone: {label} bore cut failed")
        result = cut.Shape()

    _require_solid(result, "wishbone")
    _require_wishbone_features(result, curves)
    return result



PARTS = (
    ("plate_hole", make_plate_hole),
    ("cylinder", make_cylinder),
    ("sphere", make_sphere),
    ("icecream_cone", make_icecream_cone),
    ("wishbone", make_wishbone),
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate product STEP fixtures (ADR-0020). STL only with --export-stl-compare."
    )
    parser.add_argument(
        "--export-stl-compare",
        action="store_true",
        help="Also write tests/fixtures/parts/<name>_compare.stl (compare only, not product).",
    )
    parser.add_argument(
        "--part",
        action="append",
        choices=[name for name, _ in PARTS],
        help="Generate only this fixture (repeatable); default generates all fixtures.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=OUT,
        help=f"Output directory (default: {OUT})",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Do not write fixtures: rebuild into a temporary directory and "
             "verify the checked-in STEP still matches on topology and volume.",
    )
    args = parser.parse_args(argv)

    out_dir: Path = args.out_dir
    selected = set(args.part or (name for name, _ in PARTS))

    if args.check:
        ok = True
        with tempfile.TemporaryDirectory(prefix="gen_cad_parts_check_") as tmp:
            scratch = Path(tmp)
            for name, factory in PARTS:
                if name not in selected:
                    continue
                shape = factory()
                # Round-trip the rebuild through STEP too, so the comparison is
                # STEP-vs-STEP and never in-memory-vs-STEP.
                probe = scratch / f"{name}.step"
                write_step(shape, probe, name=name)
                ok = check_step(read_step(probe), out_dir / f"{name}.step", name=name) and ok
        print("check: topology/volume facts only; STEP headers are timestamped.")
        return 0 if ok else 1

    out_dir.mkdir(parents=True, exist_ok=True)
    for name, factory in PARTS:
        if name not in selected:
            continue
        shape = factory()
        write_step(shape, out_dir / f"{name}.step", name=name)
        if args.export_stl_compare:
            write_stl_compare(shape, out_dir / f"{name}_compare.stl")

    print(
        "product path: STEP only. "
        "Legacy STL generator: scripts/gen_part_library.py (soft-deprecated)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
