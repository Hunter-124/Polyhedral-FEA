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
    python3 scripts/gen_cad_parts.py --export-stl-compare

Without ``--part`` the command writes all four product fixtures. Repeat
``--part`` to regenerate a selected subset:
  tests/fixtures/parts/plate_hole.step
  tests/fixtures/parts/cylinder.step
  tests/fixtures/parts/sphere.step
  tests/fixtures/parts/icecream_cone.step

With ``--export-stl-compare``, also writes sibling ``*_compare.stl`` files.

Geometry only — case JSON and bench/reference truths are hand-authored
elsewhere (anti-cheat boundary).

Legacy STL-only fixtures for older campaigns remain under the same directory;
see scripts/gen_part_library.py (soft-deprecated for product path).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "fixtures" / "parts"

try:
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepCheck import BRepCheck_Analyzer
    from OCP.BRepGProp import BRepGProp
    from OCP.BRepMesh import BRepMesh_IncrementalMesh
    from OCP.BRepPrimAPI import (
        BRepPrimAPI_MakeBox,
        BRepPrimAPI_MakeCone,
        BRepPrimAPI_MakeCylinder,
        BRepPrimAPI_MakeSphere,
    )
    from OCP.GProp import GProp_GProps
    from OCP.gp import gp_Ax2, gp_Dir, gp_Pnt
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.Interface import Interface_Static
    from OCP.ShapeFix import ShapeFix_Shape
    from OCP.STEPControl import STEPControl_AsIs, STEPControl_Writer
    from OCP.StlAPI import StlAPI_Writer
    from OCP.TopAbs import TopAbs_SOLID
    from OCP.TopExp import TopExp_Explorer
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


def _require_solid(shape, name: str) -> None:
    if not _is_valid(shape):
        raise RuntimeError(f"{name}: BRep not valid after construction")
    solids = TopExp_Explorer(shape, TopAbs_SOLID)
    solid_count = 0
    while solids.More():
        solid_count += 1
        solids.Next()
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
    print(f"wrote {path.relative_to(ROOT)}  (volume={vol:.6g} m^3)")


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
    print(f"wrote {path.relative_to(ROOT)}  (compare STL only)")


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


PARTS = (
    ("plate_hole", make_plate_hole),
    ("cylinder", make_cylinder),
    ("sphere", make_sphere),
    ("icecream_cone", make_icecream_cone),
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
    args = parser.parse_args(argv)

    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    selected = set(args.part or (name for name, _ in PARTS))

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
