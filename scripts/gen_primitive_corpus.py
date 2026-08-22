#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate the procedural primitive corpus for the learned mesh advisor (ADR-0027).

Fifteen families x four size regimes = 60 manifold STEP solids, five
boundary-condition load cases each = 300 ``*.case.json`` files, and one
``bench/reference/corpus/*.json`` truth file per case.

Everything is deterministic: the per-part seed table below is committed literally and
parameter jitter comes only from ``random.Random(seed)``. No clock, no environment, no
filesystem ordering feeds a generated value, so re-running produces byte-identical
``*.case.json``, reference and campaign JSON. STEP payloads are byte-identical too apart
from the ISO-10303-21 header, into which OpenCASCADE stamps the write timestamp
(``FILE_NAME(..., '<ISO date>', ...)``).

Product geometry is STEP only (ADR-0020); the OCP helpers (``write_step`` validity +
positive-volume gate) are imported from ``scripts/gen_cad_parts.py`` so the repo keeps a
single CAD convention.

Truth is layered:

* ``box_hole`` case ``c0`` (uniaxial tension) carries the analytic Kirsch SCF = 3.
* ``stepped_shaft`` case ``c1`` (transverse end load) carries the analytic
  Euler-Bernoulli + Cowper-shear stepped-cantilever tip deflection and its work-conjugate
  strain energy.
* Every other LEGACY case (families 1-11, archetypes c0-c2) gets a first-order beam
  surrogate marked ``"source": "provisional"`` with a 100 % tolerance;
  ``scripts/advisor/promote_truth.py`` replaces those values with the
  ``advisor-truth-0`` overkill reference answers, once, and never recomputes them.
* Cases added by the portable-cost retrain -- the four reinforcement/proximity
  families and archetypes ``c3``/``c4`` on every family -- also start from that
  surrogate, but they are deliberately EXCLUDED from the ``advisor-truth-0``
  campaign: their truth may come only from the independent Gmsh 4.13.1 ->
  CalculiX 2.23 chain (``bench/reference/external_truth.py``, ADR-0029). Promoting
  our own solver into a reference it will then be scored against is the one thing
  that would make the score self-assessment, so the internal ladder is not offered
  to them at all. Their scored probes are ``strain_energy`` and ``tip_deflection``
  only -- never a nodal peak stress (ADR-0032 section 4.1).

Run from the repo root::

    python scripts/gen_primitive_corpus.py
    python scripts/gen_primitive_corpus.py --check
    python scripts/gen_primitive_corpus.py --family stepped_shaft
    python scripts/gen_primitive_corpus.py --check-coverage
    python scripts/gen_primitive_corpus.py --self-test
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_DIR = ROOT / "scripts"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

# House CAD helpers (validity + one-solid + positive-volume gate, AP214 writer).
from gen_cad_parts import (  # noqa: E402  -- path bootstrap must precede this import
    _require_solid,
    _volume,
    make_plate_hole,
    write_step,
)

try:  # pragma: no cover - import guard mirrors scripts/gen_cad_parts.py
    from OCP.Bnd import Bnd_Box
    from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut, BRepAlgoAPI_Fuse
    from OCP.BRepBndLib import BRepBndLib
    from OCP.BRepBuilderAPI import (
        BRepBuilderAPI_GTransform,
        BRepBuilderAPI_MakeEdge,
        BRepBuilderAPI_MakeFace,
        BRepBuilderAPI_MakeWire,
    )
    from OCP.BRepGProp import BRepGProp
    from OCP.BRepOffsetAPI import BRepOffsetAPI_ThruSections
    from OCP.BRepPrimAPI import (
        BRepPrimAPI_MakeBox,
        BRepPrimAPI_MakeCylinder,
        BRepPrimAPI_MakePrism,
        BRepPrimAPI_MakeSphere,
    )
    from OCP.GeomAPI import GeomAPI_Interpolate
    from OCP.GProp import GProp_GProps
    from OCP.gp import gp_Ax2, gp_Dir, gp_GTrsf, gp_Mat, gp_Pnt, gp_Vec, gp_XYZ
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.ShapeFix import ShapeFix_Shape
    from OCP.STEPControl import STEPControl_Reader
    from OCP.TColgp import TColgp_HArray1OfPnt
except ImportError as exc:  # pragma: no cover
    print(
        "error: OCP (OpenCASCADE Python bindings) is required to generate the corpus.\n"
        "  Install with `pip install cadquery` (brings OCP) or `pip install cadquery-ocp`.\n"
        f"  import failed: {exc}",
        file=sys.stderr,
    )
    sys.exit(1)

STEP_DIR = ROOT / "bench" / "geometries" / "corpus" / "primitives"
CASE_DIR = STEP_DIR  # cases live next to their STEP; see build_advisor_dataset.load_cases
REFERENCE_DIR = ROOT / "bench" / "reference" / "corpus"
#: Manifest of cases whose truth may come only from the independent chain.
EXTERNAL_PENDING_JSON = (ROOT / "bench" / "reference" / "external"
                         / "pending-external-truth.json")
TRUTH_CAMPAIGN_DIR = ROOT / "bench" / "campaigns" / "advisor-truth-0"

# Same allowlist promote_truth.py uses: only truth this repo generated itself may
# be overwritten. Defined once in scripts/truth_guard.py so a newly added
# external source is protected in both writers without editing either.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from truth_guard import SELF_GENERATED_SOURCES, protected_metrics  # noqa: E402

FAMILIES = (
    "box_hole",
    "l_bracket",
    "plate_notch",
    "stepped_shaft",
    "channel",
    "sphere_box",
    # Added for the corpus-widening power test (+2 families, 8 total). Chosen
    # for distance from the existing six in descriptor space, not for coverage:
    # `tube` is curved AND thin-walled, `perforated_plate` carries many features
    # far below its own thickness. See docs -- the question they exist to answer
    # is whether held-out-family regret improves at all as families are added,
    # which at six families is unmeasurable (slope CI [-0.051, +0.043]).
    "tube",
    "perforated_plate",
    # Added 2026-08-15: every curved surface in the eight families above is a
    # plane, a circular cylinder, a circular cone or a sphere, so curvature is
    # zero, constant, or constant along one principal direction, and a curved
    # wall is always a surface of revolution. These three break that:
    # `ellipsoid_boss` has two continuously varying principal curvatures,
    # `lobed_shaft` a non-circular C2 periodic B-spline section, `twisted_loft` a
    # doubly-curved NURBS wall with no analytic surface anywhere.
    "ellipsoid_boss",
    "lobed_shaft",
    "twisted_loft",
    # Added 2026-08-22 for the portable-cost retrain. The eleven families above
    # are all single-load-path solids: one prismatic or revolved body, features
    # that either weaken it (holes, notches) or hang off it (bosses). None of
    # them is REINFORCED, and none puts two features close enough to interact,
    # so the proximity and load-interaction features the retrain adds have no
    # variance to learn from on the old corpus:
    #   `ribbed_plate`     parallel stiffening ribs -- a second load path, and
    #                      1-3 re-entrant rib/plate junctions per part.
    #   `gusset_bracket`   an l_bracket whose corner is braced by a triangular
    #                      web: the corner singularity is deliberately relieved.
    #   `multi_hole_plate` 2-4 bores with a DELIBERATELY near-touching pair, so
    #                      hole spacing is small against the hole radius rather
    #                      than against the part (contrast `perforated_plate`,
    #                      whose row is evenly spaced with a wide ligament).
    #   `bossed_plate`     one boss beside the load patch and one remote from
    #                      it, which is the only way load-to-feature distance
    #                      varies within a family.
    "ribbed_plate",
    "gusset_bracket",
    "multi_hole_plate",
    "bossed_plate",
)

#: Nominal overall size (m) of each size regime. Same band as tests/fixtures/parts.
REGIME_SCALE = (0.06, 0.12, 0.22, 0.38)

#: Committed seed table, family-major: SEEDS[family_index * 4 + regime_index].
#: Literal values only -- never derived from the clock, the environment or hashing.
SEEDS = (
    100003, 100019, 100043, 100057,  # box_hole         s0..s3
    200003, 200029, 200041, 200063,  # l_bracket        s0..s3
    300007, 300017, 300031, 300049,  # plate_notch      s0..s3
    400009, 400021, 400037, 400051,  # stepped_shaft    s0..s3
    500011, 500023, 500039, 500053,  # channel          s0..s3
    600013, 600027, 600041, 600059,  # sphere_box       s0..s3
    700001, 700019, 700033, 700061,  # tube             s0..s3
    800011, 800029, 800047, 800063,  # perforated_plate s0..s3
    900007, 900023, 900041, 900067,  # ellipsoid_boss   s0..s3
    1000003, 1000033, 1000037, 1000081,  # lobed_shaft  s0..s3
    1100009, 1100021, 1100053, 1100077,  # twisted_loft s0..s3
    1200011, 1200023, 1200043, 1200059,  # ribbed_plate     s0..s3
    1300021, 1300031, 1300049, 1300063,  # gusset_bracket   s0..s3
    1400017, 1400029, 1400039, 1400071,  # multi_hole_plate s0..s3
    1500013, 1500029, 1500047, 1500061,  # bossed_plate     s0..s3
)

#: Families and archetypes added by the portable-cost retrain. Named once, here,
#: because three separate rules key off "is this case new": it may not enter the
#: internal overkill truth campaign, its scored probes are restricted to
#: strain_energy / tip_deflection, and `--check-coverage` measures it.
RETRAIN_FAMILIES = ("ribbed_plate", "gusset_bracket", "multi_hole_plate",
                    "bossed_plate")
RETRAIN_ARCHETYPES = (3, 4)

MATERIAL = {"E": 2.1e11, "nu": 0.3, "rho": 7850}

#: Traction magnitudes (Pa) per case archetype. Chosen so peak strain stays ~1e-4.
TRACTION_AXIAL = 1.0e6
TRACTION_TRANSVERSE = 1.0e5
TRACTION_OBLIQUE = 2.0e5
#: c3's second region is a band of wall around the whole section, so its area is
#: an order of magnitude larger than an end face; the traction is scaled down to
#: match so neither region's resultant swamps the other and the two-region angle
#: `case_load_multiaxiality` reads is carried by comparable forces.
TRACTION_BAND = 2.0e4
#: c4 presses on the largest axis-aligned planar face, whose area is likewise
#: much larger than an end section.
PRESSURE_NORMAL = 2.0e4

#: c3's second region: an axial band centred at mid-span, half-width as a
#: fraction of the axial extent. Wide enough that a coarse mesh always has face
#: centroids inside it (h_rel tops out at 0.20, so 0.10 of the extent is at
#: least half an element), narrow enough to stay clear of both end selections.
BAND_HALF_FRAC = 0.05
#: c4's slab: half-depth along the pressed face's normal as a fraction of the
#: part's extent along that normal, plus an in-plane pad. A planar face's facet
#: centroids lie exactly in its plane, so the depth only has to stay well below
#: the distance to the nearest parallel face.
PRESSURE_SLAB_FRAC = 0.02
PRESSURE_PAD_FRAC = 0.02

#: Selection-slab depths, as a fraction of the loaded/fixed cross-section's smallest
#: extent. 0.01 reproduces the working tests/fixtures/parts/cantilever.case.json slab
#: (0.001 m on a 0.1 m section), which selects the CAD end face exactly and no wall
#: faces even at h_rel = 0.005.
LOAD_SLAB_FRAC = 0.01
FIX_SLAB_FRAC = 0.02
#: Outward padding, as a fraction of the bbox diagonal, so a select box provably
#: encloses the boundary it targets without reaching any other feature.
PAD_FRAC = 0.05

DERIV_KIRSCH = "docs/validation/hand-calcs.md#corpus-primitives-kirsch"
DERIV_CANTILEVER = "docs/validation/hand-calcs.md#corpus-primitives-cantilever"
DERIV_PROVISIONAL = "docs/validation/hand-calcs.md#corpus-primitives-provisional"

AXIS_NAMES = ("x", "y", "z")


# ── numeric hygiene ─────────────────────────────────────────────────────────


def r10(value: float) -> float:
    """Round to 10 significant digits so emitted JSON is stable and readable.

    ``+ 0.0`` normalises a signed zero: ``-0.0`` is a valid double but in a
    traction vector it reads as a direction, and it is not one. The addition is
    the exact identity on every other value, so no previously emitted number moves.
    """
    return float(f"{float(value):.10g}") + 0.0


def jitter(rng: random.Random, low: float, high: float) -> float:
    return r10(low + (high - low) * rng.random())


# ── OCP plumbing ────────────────────────────────────────────────────────────


def _box(origin: tuple[float, float, float], size: tuple[float, float, float]):
    return BRepPrimAPI_MakeBox(gp_Pnt(*origin), size[0], size[1], size[2]).Shape()


def _cyl(base: tuple[float, float, float], direction: tuple[float, float, float],
         radius: float, height: float):
    axis = gp_Ax2(gp_Pnt(*base), gp_Dir(*direction))
    return BRepPrimAPI_MakeCylinder(axis, radius, height).Shape()


def _healed(shape, name: str):
    fixer = ShapeFix_Shape(shape)
    fixer.Perform()
    healed = fixer.Shape()
    _require_solid(healed, name)
    return healed


def _fuse(first, second, name: str):
    op = BRepAlgoAPI_Fuse(first, second)
    op.Build()
    if not op.IsDone():
        raise RuntimeError(f"{name}: boolean fuse failed")
    return _healed(op.Shape(), name)


def _cut(first, second, name: str):
    op = BRepAlgoAPI_Cut(first, second)
    op.Build()
    if not op.IsDone():
        raise RuntimeError(f"{name}: boolean cut failed")
    return _healed(op.Shape(), name)


def _ellipsoid(center: tuple[float, float, float], radii: tuple[float, float, float],
               name: str):
    """Sphere scaled anisotropically — a genuine non-spherical curved surface.

    Every corpus surface before this one is a plane, a circular cylinder, a
    circular cone or a sphere: curvature is either zero, constant, or constant
    along one principal direction. An ellipsoid's two principal curvatures both
    vary continuously over the surface, and no cross-section is a circle, so a
    chordal-deviation size field and a curvature-driven refinement band have to
    resolve a field rather than a single number.
    """
    unit = BRepPrimAPI_MakeSphere(gp_Pnt(0.0, 0.0, 0.0), 1.0).Shape()
    gtrsf = gp_GTrsf()
    gtrsf.SetVectorialPart(
        gp_Mat(radii[0], 0.0, 0.0, 0.0, radii[1], 0.0, 0.0, 0.0, radii[2]))
    gtrsf.SetTranslationPart(gp_XYZ(*center))
    op = BRepBuilderAPI_GTransform(unit, gtrsf, True)
    op.Build()
    if not op.IsDone():
        raise RuntimeError(f"{name}: anisotropic scale of the unit sphere failed")
    return _healed(op.Shape(), name)


def _closed_spline_wire(points: list[tuple[float, float, float]], name: str):
    """Periodic interpolating B-spline through `points` as one closed wire.

    `GeomAPI_Interpolate` with `PeriodicFlag=True` yields a C2 periodic curve, so
    the resulting surface has no artificial crease anywhere — the mesher's sharp
    edge detector must find NO feature edge on it, which is what makes these
    families a real test of curvature-driven sizing rather than of feature
    capture. The first point must not be repeated at the end (OCC closes it).
    """
    array = TColgp_HArray1OfPnt(1, len(points))
    for index, (x, y, z) in enumerate(points, start=1):
        array.SetValue(index, gp_Pnt(x, y, z))
    interp = GeomAPI_Interpolate(array, True, 1.0e-9)
    interp.Perform()
    if not interp.IsDone():
        raise RuntimeError(f"{name}: periodic spline interpolation failed")
    edge = BRepBuilderAPI_MakeEdge(interp.Curve()).Edge()
    wire = BRepBuilderAPI_MakeWire(edge)
    if not wire.IsDone():
        raise RuntimeError(f"{name}: spline wire construction failed")
    return wire.Wire()


def _lobed_section(*, plane_x: float, mean_r: float, lobes: int, lobe_frac: float,
                   phase: float, aspect: float, n: int = 48
                   ) -> list[tuple[float, float, float]]:
    """Sample points of a closed, non-circular, lobed section in the x = const plane.

    r(theta) = mean_r * (1 + lobe_frac * cos(lobes * theta + phase)), then scaled
    by `aspect` in z. `lobe_frac` < 1/(lobes^2 - 1) keeps the curve convex, which
    is checked by the caller through the BRep validity gate rather than assumed.
    """
    points: list[tuple[float, float, float]] = []
    for i in range(n):
        theta = 2.0 * math.pi * i / n
        radius = mean_r * (1.0 + lobe_frac * math.cos(lobes * theta + phase))
        points.append((plane_x, radius * math.cos(theta),
                       aspect * radius * math.sin(theta)))
    return points


def _extruded_section(points: list[tuple[float, float, float]], length: float, name: str):
    """Prism a closed spline section along +x into a solid."""
    face = BRepBuilderAPI_MakeFace(_closed_spline_wire(points, name))
    if not face.IsDone():
        raise RuntimeError(f"{name}: spline section is not a valid planar face")
    prism = BRepPrimAPI_MakePrism(face.Face(), gp_Vec(length, 0.0, 0.0))
    prism.Build()
    if not prism.IsDone():
        raise RuntimeError(f"{name}: extrusion of the spline section failed")
    return _healed(prism.Shape(), name)


def _lofted_sections(sections: list[list[tuple[float, float, float]]], name: str):
    """Loft closed spline sections into one solid with a doubly-curved wall."""
    loft = BRepOffsetAPI_ThruSections(True, False, 1.0e-7)
    for section in sections:
        loft.AddWire(_closed_spline_wire(section, name))
    loft.Build()
    if not loft.IsDone():
        raise RuntimeError(f"{name}: loft through the spline sections failed")
    return _healed(loft.Shape(), name)


def _triangular_prism(p0: tuple[float, float, float], p1: tuple[float, float, float],
                      p2: tuple[float, float, float], extrude: tuple[float, float, float],
                      name: str):
    """Solid triangular web: the triangle p0-p1-p2 swept along `extrude`."""
    corners = [gp_Pnt(*p0), gp_Pnt(*p1), gp_Pnt(*p2)]
    wire = BRepBuilderAPI_MakeWire()
    for start, end in ((0, 1), (1, 2), (2, 0)):
        wire.Add(BRepBuilderAPI_MakeEdge(corners[start], corners[end]).Edge())
    if not wire.IsDone():
        raise RuntimeError(f"{name}: gusset triangle wire construction failed")
    face = BRepBuilderAPI_MakeFace(wire.Wire())
    if not face.IsDone():
        raise RuntimeError(f"{name}: gusset triangle is not a valid planar face")
    prism = BRepPrimAPI_MakePrism(face.Face(), gp_Vec(*extrude))
    prism.Build()
    if not prism.IsDone():
        raise RuntimeError(f"{name}: gusset extrusion failed")
    return _healed(prism.Shape(), name)


@dataclass(frozen=True)
class PlanarFace:
    """One planar BRep face whose outward normal is parallel to a bbox axis."""

    axis: int
    side: str
    """``lo`` when the outward normal points along -axis, ``hi`` along +axis."""
    area: float
    centroid: tuple[float, float, float]
    lo: tuple[float, float, float]
    hi: tuple[float, float, float]
    straight_edges: bool
    """True when every bounding edge is a straight line, i.e. a mesh of this face
    reproduces its exact area and an authored ``expected_area`` is honest."""

    @property
    def normal(self) -> tuple[float, float, float]:
        out = [0.0, 0.0, 0.0]
        out[self.axis] = 1.0 if self.side == "hi" else -1.0
        return (out[0], out[1], out[2])


def axis_aligned_planar_faces(shape, name: str, *, tol: float = 1.0e-9
                              ) -> list[PlanarFace]:
    """Every planar face of `shape` whose outward normal is a bbox axis direction.

    WHY THE RESTRICTION. A case traction is one constant vector per region, so a
    "normal pressure" is only expressible on a PLANAR face -- and a selection box
    is axis-aligned, so isolating a face with an oblique normal would need a box
    that also encloses whatever else lies in its fat bounding volume. Every family
    in the corpus has at least one axis-aligned planar face (a section end, a wall
    or a plate face), so restricting the c4 candidate set costs no family and
    removes the whole class of "the slab caught a neighbour" defect. Oblique faces
    (the gusset web's hypotenuse) are skipped, not approximated.
    """
    from OCP.BRepAdaptor import BRepAdaptor_Curve, BRepAdaptor_Surface
    from OCP.GeomAbs import GeomAbs_Line, GeomAbs_Plane
    from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_REVERSED
    from OCP.TopExp import TopExp_Explorer
    from OCP.TopoDS import TopoDS

    out: list[PlanarFace] = []
    explorer = TopExp_Explorer(shape, TopAbs_FACE)
    while explorer.More():
        face = TopoDS.Face_s(explorer.Current())
        explorer.Next()
        surface = BRepAdaptor_Surface(face)
        if surface.GetType() != GeomAbs_Plane:
            continue
        direction = surface.Plane().Axis().Direction()
        components = [direction.X(), direction.Y(), direction.Z()]
        if face.Orientation() == TopAbs_REVERSED:
            components = [-value for value in components]
        axis = max(range(3), key=lambda i: abs(components[i]))
        if abs(abs(components[axis]) - 1.0) > 1.0e-7:
            continue  # oblique normal: not addressable by an axis-aligned slab
        props = GProp_GProps()
        BRepGProp.SurfaceProperties_s(face, props)
        centre = props.CentreOfMass()
        bounds = Bnd_Box()
        BRepBndLib.Add_s(face, bounds, True)
        x0, y0, z0, x1, y1, z1 = bounds.Get()
        straight = True
        edges = TopExp_Explorer(face, TopAbs_EDGE)
        while edges.More():
            if BRepAdaptor_Curve(TopoDS.Edge_s(edges.Current())).GetType() != GeomAbs_Line:
                straight = False
                break
            edges.Next()
        area = float(props.Mass())
        if not area > tol * tol:
            continue
        out.append(PlanarFace(
            axis=axis, side="hi" if components[axis] > 0.0 else "lo",
            area=area,
            centroid=(centre.X(), centre.Y(), centre.Z()),
            lo=(x0, y0, z0), hi=(x1, y1, z1),
            straight_edges=straight,
        ))
    if not out:
        raise RuntimeError(f"{name}: no axis-aligned planar face to press on")
    return out


def _face_area_at(shape, axis: int, coord: float, name: str, *, tol: float = 1e-9) -> float:
    """Exact CAD area of the planar face whose centroid sits at `coord` on `axis`.

    Spline-bounded end faces have no closed-form area, so the authored
    `expected_area` must come from the BRep itself rather than from a formula the
    section sampling only approximates.
    """
    from OCP.TopAbs import TopAbs_FACE  # local: only this helper needs it
    from OCP.TopExp import TopExp_Explorer

    best = 0.0
    found = False
    explorer = TopExp_Explorer(shape, TopAbs_FACE)
    while explorer.More():
        face = explorer.Current()
        props = GProp_GProps()
        BRepGProp.SurfaceProperties_s(face, props)
        centre = props.CentreOfMass()
        if abs(centre.Coord(axis + 1) - coord) <= tol:
            best += float(props.Mass())
            found = True
        explorer.Next()
    if not found:
        raise RuntimeError(f"{name}: no planar face centred at axis {axis} = {coord}")
    return best


def occ_bbox(shape) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    """Axis-aligned bounds straight from OpenCASCADE (used by --check only)."""
    bounds = Bnd_Box()
    BRepBndLib.Add_s(shape, bounds, True)
    x0, y0, z0, x1, y1, z1 = bounds.Get()
    return (x0, y0, z0), (x1, y1, z1)


def read_step(path: Path):
    reader = STEPControl_Reader()
    if reader.ReadFile(str(path)) != IFSelect_RetDone:
        raise RuntimeError(f"{path}: STEP read failed")
    reader.TransferRoots()
    return reader.OneShape()


# ── corpus part descriptor ──────────────────────────────────────────────────


@dataclass
class Geometry:
    """One generated solid plus everything the BC sampler and truth writer need."""

    name: str
    family: str
    regime: int
    seed: int
    shape: Any
    lo: tuple[float, float, float]
    hi: tuple[float, float, float]
    axis: int
    """Beam/primary axis; loads act on its ``hi`` end."""
    transverse: int
    """Designated transverse axis for the bending case."""
    end_area: float
    """Exact CAD area of the loaded end face (m^2)."""
    guard_end_area: bool
    """Emit ``select.expected_area``; false for chordal (curved) load faces."""
    load_region: str
    """``end_slab`` or ``spherical_cap``."""
    fix_axis: int
    fix_side: str
    fix_char_len: float
    load_char_len: float
    span: float
    """Effective cantilever span from the fixed face to the loaded face (m)."""
    params: dict[str, float] = field(default_factory=dict)
    analytic: str | None = None
    load_face_boundary: str = "straight"
    """``straight`` | ``curved`` | ``curved_surface``.

    Declared by the builder rather than inferred, and emitted into the case, so
    the load-area path a case will take is a stated property instead of one a
    reader has to deduce from the geometry. Omitting that statement is how
    ``sphere_box`` and ``stepped_shaft`` ended up as the two blind families.

    ``straight``        planar loaded face, straight edges: the meshed area
                        equals the exact area and no rescale is needed.
    ``curved``          planar loaded face with a CURVED BOUNDARY (a disc or an
                        annulus). The mesh under-resolves the area by a chordal
                        deficit, so the traction is rescaled onto the CAD rule
                        area (``apps/testlab/load_area.hpp:18-28``).
    ``curved_surface``  the loaded face is itself curved (a spherical cap).
    """

    @property
    def diag(self) -> float:
        return math.sqrt(sum((self.hi[i] - self.lo[i]) ** 2 for i in range(3)))

    def extent(self, axis: int) -> float:
        return self.hi[axis] - self.lo[axis]


# ── families (SI metres) ────────────────────────────────────────────────────


def build_box_hole(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate-proportioned box with a central through-hole; Kirsch tension case."""
    half_w = r10(0.5 * scale)
    half_h = r10(half_w * jitter(rng, 0.45, 0.55))
    thickness = r10(half_h * jitter(rng, 0.16, 0.24))
    hole_r = r10(half_h * jitter(rng, 0.15, 0.20))
    shape = make_plate_hole(
        half_w=half_w, half_h=half_h, thickness=thickness, hole_r=hole_r
    )
    _require_solid(shape, name)
    return Geometry(
        name=name, family="box_hole", regime=-1, seed=-1, shape=shape,
        lo=(-half_w, -half_h, 0.0), hi=(half_w, half_h, thickness),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=r10(2.0 * half_w),
        params={"half_w": half_w, "half_h": half_h, "thickness": thickness,
                "hole_r": hole_r,
                "a_over_W": r10(hole_r / half_w), "a_over_H": r10(hole_r / half_h)},
        analytic="kirsch",
    )


def build_l_bracket(name: str, scale: float, rng: random.Random) -> Geometry:
    """Two prismatic legs fused into an L; fixed at the vertical leg's top face."""
    leg_x = r10(scale)
    leg_z = r10(scale * jitter(rng, 0.55, 0.75))
    depth = r10(scale * jitter(rng, 0.18, 0.28))
    wall = r10(scale * jitter(rng, 0.06, 0.10))
    if wall >= min(leg_x, leg_z) / 3.0:
        raise ValueError(f"{name}: wall {wall} too thick for legs {leg_x}/{leg_z}")
    horizontal = _box((0.0, 0.0, 0.0), (leg_x, depth, wall))
    vertical = _box((0.0, 0.0, 0.0), (wall, depth, leg_z))
    shape = _fuse(horizontal, vertical, name)
    return Geometry(
        name=name, family="l_bracket", regime=-1, seed=-1, shape=shape,
        lo=(0.0, 0.0, 0.0), hi=(leg_x, depth, leg_z),
        axis=0, transverse=2,
        end_area=r10(depth * wall), guard_end_area=True,
        load_region="end_slab",
        fix_axis=2, fix_side="hi",
        fix_char_len=wall, load_char_len=min(depth, wall),
        span=r10(leg_x + leg_z),
        params={"leg_x": leg_x, "leg_z": leg_z, "depth": depth, "wall": wall},
        analytic=None,
    )


def build_plate_notch(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate with opposed semicircular edge notches at mid-span."""
    length = r10(scale)
    half_h = r10(scale * jitter(rng, 0.22, 0.30))
    thickness = r10(scale * jitter(rng, 0.05, 0.09))
    notch_r = r10(half_h * jitter(rng, 0.20, 0.32))
    plate = _box((0.0, -half_h, 0.0), (length, 2.0 * half_h, thickness))
    margin = r10(max(thickness * 0.5, 1e-4))
    shape = plate
    for sign in (-1.0, 1.0):
        cutter = _cyl(
            (r10(0.5 * length), r10(sign * half_h), -margin),
            (0.0, 0.0, 1.0),
            notch_r,
            r10(thickness + 2.0 * margin),
        )
        shape = _cut(shape, cutter, name)
    return Geometry(
        name=name, family="plate_notch", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -half_h, 0.0), hi=(length, half_h, thickness),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=length,
        params={"length": length, "half_h": half_h, "thickness": thickness,
                "notch_r": notch_r,
                "notch_over_half_h": r10(notch_r / half_h)},
        analytic=None,
    )


def build_stepped_shaft(name: str, scale: float, rng: random.Random) -> Geometry:
    """Two coaxial cylinders fused into a stepped cantilever shaft along +x."""
    length = r10(scale)
    r_root = r10(scale * jitter(rng, 0.045, 0.060))
    r_tip = r10(r_root * jitter(rng, 0.60, 0.78))
    step_x = r10(length * jitter(rng, 0.40, 0.55))
    root = _cyl((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), r_root, step_x)
    tip = _cyl((step_x, 0.0, 0.0), (1.0, 0.0, 0.0), r_tip, r10(length - step_x))
    shape = _fuse(root, tip, name)
    return Geometry(
        name=name, family="stepped_shaft", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -r_root, -r_root), hi=(length, r_root, r_root),
        axis=0, transverse=2,
        end_area=r10(math.pi * r_tip * r_tip), guard_end_area=False,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=r10(2.0 * r_root), load_char_len=r10(2.0 * r_tip),
        span=length,
        params={"length": length, "r_root": r_root, "r_tip": r_tip, "step_x": step_x,
                "slenderness_L_over_D": r10(length / (2.0 * r_root))},
        analytic="stepped_cantilever",
        load_face_boundary="curved",
    )


def build_channel(name: str, scale: float, rng: random.Random) -> Geometry:
    """Thin-walled U channel extruded along +x, open towards +z.

    Symmetric about the ``y = 0`` plane, so a transverse ``z`` load through the
    section acts in the plane containing the shear centre and induces no torsion.
    """
    length = r10(scale)
    half_b = r10(scale * jitter(rng, 0.10, 0.14))
    depth = r10(scale * jitter(rng, 0.12, 0.18))
    flange_t = r10(half_b * jitter(rng, 0.22, 0.32))
    web_t = r10(depth * jitter(rng, 0.20, 0.30))
    if half_b - flange_t <= 0.0 or depth - web_t <= 0.0:
        raise ValueError(f"{name}: degenerate channel walls")
    outer = _box((0.0, -half_b, 0.0), (length, 2.0 * half_b, depth))
    margin = r10(max(web_t * 0.5, 1e-4))
    pocket = _box(
        (-margin, -(half_b - flange_t), web_t),
        (r10(length + 2.0 * margin), r10(2.0 * (half_b - flange_t)),
         r10(depth - web_t + margin)),
    )
    shape = _cut(outer, pocket, name)
    section_area = r10(2.0 * half_b * web_t + 2.0 * flange_t * (depth - web_t))
    return Geometry(
        name=name, family="channel", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -half_b, 0.0), hi=(length, half_b, depth),
        axis=0, transverse=2,
        end_area=section_area, guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=web_t, load_char_len=min(web_t, flange_t),
        span=length,
        params={"length": length, "half_b": half_b, "depth": depth,
                "flange_t": flange_t, "web_t": web_t, "section_area": section_area},
        analytic=None,
    )


def build_sphere_box(name: str, scale: float, rng: random.Random) -> Geometry:
    """Prismatic box with a spherical boss fused onto its +x face (one solid)."""
    length = r10(scale)
    half_w = r10(scale * jitter(rng, 0.20, 0.30))
    radius = r10(half_w * jitter(rng, 0.55, 0.80))
    body = _box((0.0, -half_w, -half_w), (length, 2.0 * half_w, 2.0 * half_w))
    boss = BRepPrimAPI_MakeSphere(gp_Pnt(length, 0.0, 0.0), radius).Shape()
    shape = _fuse(body, boss, name)
    return Geometry(
        name=name, family="sphere_box", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -half_w, -half_w), hi=(r10(length + radius), half_w, half_w),
        axis=0, transverse=2,
        end_area=r10(2.0 * math.pi * radius * radius), guard_end_area=False,
        load_region="spherical_cap",
        fix_axis=0, fix_side="lo",
        fix_char_len=r10(2.0 * half_w), load_char_len=radius,
        span=r10(length + radius),
        params={"length": length, "half_w": half_w, "boss_radius": radius,
                "box_section_area": r10(4.0 * half_w * half_w)},
        analytic=None,
        load_face_boundary="curved_surface",
    )


def build_tube(name: str, scale: float, rng: random.Random) -> Geometry:
    """Thin-walled hollow circular tube along +x; loaded on the annular end face.

    Chosen for the corpus-widening test because it is the farthest point from the
    existing six families in descriptor space: fully curved (no planar face
    carries load), genuinely thin-walled, and hollow. ``channel`` is thin-walled
    but prismatic; ``stepped_shaft`` is curved but solid. Nothing in the corpus is
    both.

    The loaded face is a planar annulus whose BOUNDARY is curved, so a mesh
    under-resolves its area by the usual chordal deficit. That is exactly the
    condition ``expected_area`` exists for: the authored value is the exact
    analytic annulus area, and testlab cross-checks it against the CAD rule area
    and rescales the traction onto the latter
    (``apps/testlab/load_area.hpp:54-91``). This family therefore exercises the
    rescaling path deliberately, unlike ``sphere_box`` and ``stepped_shaft``
    which reached it by omitting the guard altogether.
    """
    length = r10(scale)
    r_outer = r10(scale * jitter(rng, 0.070, 0.100))
    wall = r10(r_outer * jitter(rng, 0.16, 0.30))
    r_inner = r10(r_outer - wall)
    if r_inner <= 0.0:
        raise ValueError(f"{name}: degenerate tube wall")
    # The bore is cut with a cylinder longer than the tube at both ends, so the
    # cut produces a clean through-bore and never a coincident-face sliver.
    margin = r10(max(wall * 0.5, 1e-4))
    outer = _cyl((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), r_outer, length)
    bore = _cyl((r10(-margin), 0.0, 0.0), (1.0, 0.0, 0.0), r_inner,
                r10(length + 2.0 * margin))
    shape = _cut(outer, bore, name)
    _require_solid(shape, name)
    annulus = r10(math.pi * (r_outer * r_outer - r_inner * r_inner))
    return Geometry(
        name=name, family="tube", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -r_outer, -r_outer), hi=(length, r_outer, r_outer),
        axis=0, transverse=2,
        end_area=annulus, guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo", fix_char_len=r10(2.0 * r_outer),
        # load_char_len sets the load slab depth as LOAD_SLAB_FRAC * it, and for a
        # hollow section that choice is load-bearing rather than cosmetic. An end
        # slab also encloses a RING of the inner and outer cylindrical walls, of
        # area 2*pi*(r_o + r_i)*depth against an annulus of pi*wall*(r_o + r_i) --
        # a ratio of exactly 2*depth/wall, independent of the geometry. Scaling
        # the depth off the DIAMETER, as the solid shaft does, would make that
        # ratio 2*LOAD_SLAB_FRAC*2*r_o/wall, i.e. about 25% on the thinnest wall.
        #
        # It matters asymmetrically. For c1/c2 the CAD-side rule substitutes the
        # slab's thin axis at min_dot 0.7 and so drops the walls
        # (apps/testlab/main.cpp:1865-1875), but the MESH-side selector honours
        # normal_min_dot = -1 literally and keeps every face in the box
        # (main.cpp:937-975). The traction is then rescaled onto the smaller CAD
        # area, so the resultant stays right while the DISTRIBUTION smears onto
        # the walls -- the same class of defect as the sphere_box under-loads,
        # just silent because the rescale hides it in the resultant.
        #
        # Scaling off the wall instead pins the ratio at 2*LOAD_SLAB_FRAC*0.2 =
        # 0.4%, comfortably inside kAuthoredAreaTol (1%), and in practice the
        # wall triangles are excluded outright because the slab is far thinner
        # than any element. The annulus faces are unaffected: their centroids sit
        # exactly on the end plane, which the slab always contains.
        load_char_len=r10(0.2 * wall),
        span=length,
        params={"length": length, "r_outer": r_outer, "r_inner": r_inner,
                "wall": wall, "annulus_area": annulus,
                "wall_over_r": r10(wall / r_outer),
                "slenderness_L_over_D": r10(length / (2.0 * r_outer))},
        analytic=None,
        load_face_boundary="curved",
    )


def build_perforated_plate(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate with a row of through-holes along its span; loaded on the +x end.

    The second widening family, chosen for the opposite extreme: many small
    features on an otherwise prismatic solid. It is the only corpus part whose
    smallest feature is far below the plate thickness, which is precisely the
    scale at which the corrected engine now refuses to alias a feature away
    rather than silently meshing through it -- so it probes the new dominant
    failure mode directly instead of by luck.

    The loaded end face is a plain rectangle with straight edges: planar, exact
    area, no chordal deficit. Paired with ``tube`` it separates "curved loaded
    boundary" from "many small features" instead of confounding them.
    """
    half_w = r10(0.5 * scale)
    half_h = r10(half_w * jitter(rng, 0.32, 0.42))
    thickness = r10(half_h * jitter(rng, 0.18, 0.26))
    n_holes = 3 + int(rng.random() * 2.0)  # 3 or 4, deterministic under the seed
    hole_r = r10(half_h * jitter(rng, 0.14, 0.20))
    # Holes evenly spaced across the span, leaving a full pitch of material at
    # each end so neither the fixed nor the loaded face is perforated.
    pitch = r10(2.0 * half_w / (n_holes + 1))
    if pitch <= 2.2 * hole_r:
        raise ValueError(f"{name}: perforation pitch too tight for the hole radius")
    margin = r10(max(thickness * 0.5, 1e-4))
    shape = _box((-half_w, -half_h, 0.0),
                 (r10(2.0 * half_w), r10(2.0 * half_h), thickness))
    for index in range(n_holes):
        centre_x = r10(-half_w + pitch * (index + 1))
        drill = _cyl((centre_x, 0.0, r10(-margin)), (0.0, 0.0, 1.0), hole_r,
                     r10(thickness + 2.0 * margin))
        shape = _cut(shape, drill, name)
    _require_solid(shape, name)
    return Geometry(
        name=name, family="perforated_plate", regime=-1, seed=-1, shape=shape,
        lo=(-half_w, -half_h, 0.0), hi=(half_w, half_h, thickness),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=r10(2.0 * half_w),
        params={"half_w": half_w, "half_h": half_h, "thickness": thickness,
                "hole_r": hole_r, "n_holes": float(n_holes), "pitch": pitch,
                "ligament": r10(pitch - 2.0 * hole_r),
                "hole_r_over_thickness": r10(hole_r / thickness)},
        analytic=None,
    )


def build_ellipsoid_boss(name: str, scale: float, rng: random.Random) -> Geometry:
    """Prismatic box with an ELLIPSOIDAL boss fused onto its +x face (one solid).

    `sphere_box` with the one property that made the sphere easy removed. A
    sphere has a single constant curvature, so a curvature-driven size field
    resolves it with one number and a chordal-deviation estimate is exact
    everywhere. On this boss the two principal curvatures vary continuously and
    their ratio at the +x pole is (a/c)^2 against (a/b)^2 at the equator, so the
    size field has to be a field. Nothing else in the corpus asks that.
    """
    length = r10(scale)
    half_w = r10(scale * jitter(rng, 0.20, 0.30))
    radius_x = r10(half_w * jitter(rng, 0.55, 0.80))
    # Anisotropy is drawn well away from 1.0 so the part can never degenerate
    # into the sphere family it exists to be different from.
    ratio_y = r10(jitter(rng, 0.55, 0.72))
    ratio_z = r10(jitter(rng, 1.25, 1.55))
    radius_y = r10(radius_x * ratio_y)
    radius_z = r10(radius_x * ratio_z)
    if radius_z >= half_w:
        # The boss must stay inside the box's cross-section in z, or the fused
        # solid grows a second silhouette and the bbox end slab stops selecting
        # the boss alone.
        radius_z = r10(0.85 * half_w)
    body = _box((0.0, -half_w, -half_w), (length, 2.0 * half_w, 2.0 * half_w))
    boss = _ellipsoid((length, 0.0, 0.0), (radius_x, radius_y, radius_z), name)
    shape = _fuse(body, boss, name)
    # Half an ellipsoid's area has no elementary form; Thomsen's approximation is
    # accurate to ~1.06% for these ratios, and the case does not guard on it
    # (guard_end_area=False) precisely because the loaded face is curved.
    p = 1.6075
    thomsen = 4.0 * math.pi * (
        ((radius_x * radius_y) ** p + (radius_x * radius_z) ** p
         + (radius_y * radius_z) ** p) / 3.0) ** (1.0 / p)
    return Geometry(
        name=name, family="ellipsoid_boss", regime=-1, seed=-1, shape=shape,
        lo=(0.0, -half_w, -half_w), hi=(r10(length + radius_x), half_w, half_w),
        axis=0, transverse=2,
        end_area=r10(0.5 * thomsen), guard_end_area=False,
        load_region="spherical_cap",
        fix_axis=0, fix_side="lo",
        fix_char_len=r10(2.0 * half_w), load_char_len=radius_x,
        span=r10(length + radius_x),
        params={"length": length, "half_w": half_w, "boss_radius_x": radius_x,
                "boss_radius_y": radius_y, "boss_radius_z": radius_z,
                "boss_aspect_y": ratio_y, "boss_aspect_z": r10(radius_z / radius_x),
                "curvature_ratio_pole": r10((radius_x / radius_z) ** 2),
                "box_section_area": r10(4.0 * half_w * half_w)},
        analytic=None,
        load_face_boundary="curved_surface",
    )


def build_lobed_shaft(name: str, scale: float, rng: random.Random) -> Geometry:
    """Cam-like lobed shaft: a closed periodic B-spline section extruded along +x.

    The wall is a ruled B-spline surface whose curvature varies around the
    section between a tight lobe crest and a slack valley (ratio ~4-8 here), and
    it is C2 everywhere — the sharp-edge detector must find no feature edge on
    the wall at all, so refinement there can only come from curvature. Both end
    faces are planar with a spline boundary, which is the `curved` load path.
    """
    length = r10(scale)
    mean_r = r10(scale * jitter(rng, 0.10, 0.15))
    lobes = 3 if rng.random() < 0.5 else 4
    # Convexity bound for r(theta) = R(1 + f cos(n theta)) is f < 1/(n^2 - 1):
    # 0.125 for 3 lobes, 0.0667 for 4. Stay clearly inside it so the section is
    # convex and the extrusion cannot self-intersect.
    lobe_frac = r10(jitter(rng, 0.35, 0.60) / float(lobes * lobes - 1))
    aspect = r10(jitter(rng, 0.72, 0.95))
    phase = r10(jitter(rng, 0.0, 0.5 * math.pi))
    section = _lobed_section(plane_x=0.0, mean_r=mean_r, lobes=lobes,
                             lobe_frac=lobe_frac, phase=phase, aspect=aspect)
    shape = _extruded_section(section, length, name)
    end_area = r10(_face_area_at(shape, 0, length, name))
    r_max = r10(mean_r * (1.0 + lobe_frac))
    r_min = r10(mean_r * (1.0 - lobe_frac))
    return Geometry(
        name=name, family="lobed_shaft", regime=-1, seed=-1, shape=shape,
        lo=(0.0, r10(-r_max), r10(-aspect * r_max)),
        hi=(length, r_max, r10(aspect * r_max)),
        axis=0, transverse=2,
        end_area=end_area, guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=r10(2.0 * r_max),
        # Same reasoning as `tube`: an end slab scaled off the diameter also
        # encloses a ring of the spline wall. The section here is solid, so the
        # slab only has to stay far thinner than one element; scale it off the
        # smallest radius, which is the tightest length the wall offers.
        load_char_len=r10(0.5 * r_min),
        span=length,
        params={"length": length, "mean_r": mean_r, "lobes": float(lobes),
                "lobe_frac": lobe_frac, "aspect": aspect, "phase": phase,
                "r_max": r_max, "r_min": r_min,
                "end_area": end_area,
                "crest_over_valley_curvature": r10(
                    (1.0 + lobe_frac * (1.0 + float(lobes * lobes)))
                    / max(1.0 - lobe_frac * (1.0 + float(lobes * lobes)), 1e-6)),
                "slenderness_L_over_D": r10(length / (2.0 * r_max))},
        analytic=None,
        load_face_boundary="curved",
    )


def build_twisted_loft(name: str, scale: float, rng: random.Random) -> Geometry:
    """Solid lofted through three DIFFERENT lobed spline sections along +x.

    The strongest curved test in the corpus: the wall is a doubly-curved NURBS
    surface (curvature varies both around the section and along the axis, and the
    sections are rotated relative to one another so the surface is twisted), with
    no analytic surface anywhere and no sharp edge except the two end rims. It
    exists to break the assumption every earlier curved family shares — that a
    curved wall is a surface of revolution whose size field is one-dimensional.
    """
    length = r10(scale)
    root_r = r10(scale * jitter(rng, 0.11, 0.16))
    waist_r = r10(root_r * jitter(rng, 0.62, 0.78))
    tip_r = r10(root_r * jitter(rng, 0.80, 1.05))
    lobes = 3
    lobe_frac = r10(jitter(rng, 0.35, 0.60) / float(lobes * lobes - 1))
    twist = r10(jitter(rng, 0.35, 0.75))  # radians of section rotation per section
    sections = [
        _lobed_section(plane_x=0.0, mean_r=root_r, lobes=lobes,
                       lobe_frac=lobe_frac, phase=0.0, aspect=1.0),
        _lobed_section(plane_x=r10(0.5 * length), mean_r=waist_r, lobes=lobes,
                       lobe_frac=lobe_frac, phase=twist,
                       aspect=r10(jitter(rng, 0.78, 0.92))),
        _lobed_section(plane_x=length, mean_r=tip_r, lobes=lobes,
                       lobe_frac=lobe_frac, phase=r10(2.0 * twist), aspect=1.0),
    ]
    shape = _lofted_sections(sections, name)
    end_area = r10(_face_area_at(shape, 0, length, name))
    r_max = r10(max(root_r, waist_r, tip_r) * (1.0 + lobe_frac))
    return Geometry(
        name=name, family="twisted_loft", regime=-1, seed=-1, shape=shape,
        lo=(0.0, r10(-r_max), r10(-r_max)), hi=(length, r_max, r_max),
        axis=0, transverse=2,
        end_area=end_area, guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=r10(2.0 * root_r),
        load_char_len=r10(0.5 * tip_r * (1.0 - lobe_frac)),
        span=length,
        params={"length": length, "root_r": root_r, "waist_r": waist_r,
                "tip_r": tip_r, "lobes": float(lobes), "lobe_frac": lobe_frac,
                "twist_rad": twist, "end_area": end_area,
                "waist_over_root": r10(waist_r / root_r),
                "taper_ratio": r10(tip_r / root_r),
                "slenderness_L_over_D": r10(length / (2.0 * r_max))},
        analytic=None,
        load_face_boundary="curved",
    )


def build_ribbed_plate(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate stiffened by 1-3 parallel ribs on its +z face; the reinforcement family.

    Every earlier family has ONE load path: material either carries the load or
    is missing from it. A rib adds a second, much stiffer path over part of the
    span, so the stress field is set by how the load divides between plate and
    rib -- which is a function of the rib/plate stiffness ratio, not of any
    single length. The ribs stop clear of both ends, so the fixed and loaded end
    faces stay plain rectangles (exact `expected_area`) and every rib
    contributes two re-entrant junction edges plus a free rib end, which is the
    proximity/singularity structure the retrain's features exist to see.
    """
    half_w = r10(0.5 * scale)
    half_h = r10(half_w * jitter(rng, 0.34, 0.44))
    thickness = r10(half_h * jitter(rng, 0.16, 0.24))
    n_ribs = 1 + int(rng.random() * 3.0)  # 1, 2 or 3, deterministic under the seed
    rib_w = r10(thickness * jitter(rng, 0.8, 1.3))
    rib_h = r10(thickness * jitter(rng, 1.6, 2.6))
    pitch = r10(2.0 * half_h / (n_ribs + 1))
    if pitch <= 1.6 * rib_w:
        raise ValueError(f"{name}: rib pitch {pitch} too tight for rib width {rib_w}")
    rib_x0 = r10(-0.76 * half_w)
    rib_len = r10(1.52 * half_w)
    shape = _box((-half_w, -half_h, 0.0),
                 (r10(2.0 * half_w), r10(2.0 * half_h), thickness))
    for index in range(n_ribs):
        y0 = r10(-half_h + pitch * (index + 1) - 0.5 * rib_w)
        shape = _fuse(shape, _box((rib_x0, y0, thickness), (rib_len, rib_w, rib_h)), name)
    _require_solid(shape, name)
    return Geometry(
        name=name, family="ribbed_plate", regime=-1, seed=-1, shape=shape,
        lo=(-half_w, -half_h, 0.0), hi=(half_w, half_h, r10(thickness + rib_h)),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=r10(2.0 * half_w),
        params={"half_w": half_w, "half_h": half_h, "thickness": thickness,
                "n_ribs": float(n_ribs), "rib_w": rib_w, "rib_h": rib_h,
                "rib_pitch": pitch, "rib_x0": rib_x0, "rib_len": rib_len,
                "rib_h_over_thickness": r10(rib_h / thickness),
                "rib_pitch_over_rib_w": r10(pitch / rib_w),
                "stiffened_frac_of_span": r10(rib_len / (2.0 * half_w))},
        analytic=None,
    )


def build_gusset_bracket(name: str, scale: float, rng: random.Random) -> Geometry:
    """L-bracket whose inner corner is braced by a triangular gusset web.

    `l_bracket` is the corpus's re-entrant-corner family: its inner corner is a
    stress singularity whose exponent depends only on the opening angle. Bracing
    it changes that -- the web carries the corner in direct tension, so the same
    opening angle now sits on a stiffer support and the corner's contribution to
    the solution is smaller. It is the one family where the singularity strength
    and the geometry that produces it move independently, which is exactly what
    a learned regularity feature must be able to tell apart.
    """
    leg_x = r10(scale)
    leg_z = r10(scale * jitter(rng, 0.55, 0.75))
    depth = r10(scale * jitter(rng, 0.18, 0.28))
    wall = r10(scale * jitter(rng, 0.06, 0.10))
    if wall >= min(leg_x, leg_z) / 3.0:
        raise ValueError(f"{name}: wall {wall} too thick for legs {leg_x}/{leg_z}")
    # TWO thin webs rather than one. A single web -- centred or full depth -- left
    # the family's standardized descriptor centroid closer to `l_bracket` than the
    # existing corpus's closest pair (measured: 2.07 centred, 1.38 full depth,
    # against a 2.32 threshold), so it would have been a weaker transfer test than
    # a new family is worth. A pair of webs is also the more honest reinforcement:
    # it puts two braced toes on each leg at a known spacing, which is a proximity
    # configuration the corpus has nowhere else.
    # Bounded by the depth, not only by the wall: two webs at quarter and three-
    # quarter depth must clear both the side faces and each other, which needs
    # t < depth/2 whatever the wall happens to be.
    web_t = r10(min(wall * jitter(rng, 0.45, 0.70), 0.30 * depth))
    if 2.0 * web_t >= 0.8 * depth:
        raise ValueError(f"{name}: webs {web_t} too thick for depth {depth}")
    reach_x = r10((leg_x - wall) * jitter(rng, 0.45, 0.70))
    reach_z = r10((leg_z - wall) * jitter(rng, 0.45, 0.70))
    horizontal = _box((0.0, 0.0, 0.0), (leg_x, depth, wall))
    vertical = _box((0.0, 0.0, 0.0), (wall, depth, leg_z))
    shape = _fuse(horizontal, vertical, name)
    for centre in (0.25, 0.75):
        y0 = r10(centre * depth - 0.5 * web_t)
        shape = _fuse(shape, _triangular_prism(
            (wall, y0, wall), (r10(wall + reach_x), y0, wall),
            (wall, y0, r10(wall + reach_z)), (0.0, web_t, 0.0), name), name)
    _require_solid(shape, name)
    return Geometry(
        name=name, family="gusset_bracket", regime=-1, seed=-1, shape=shape,
        lo=(0.0, 0.0, 0.0), hi=(leg_x, depth, leg_z),
        axis=0, transverse=2,
        end_area=r10(depth * wall), guard_end_area=True,
        load_region="end_slab",
        fix_axis=2, fix_side="hi",
        fix_char_len=wall, load_char_len=min(depth, wall),
        span=r10(leg_x + leg_z),
        params={"leg_x": leg_x, "leg_z": leg_z, "depth": depth, "wall": wall,
                "n_gussets": 2.0, "gusset_t": web_t,
                "gusset_reach_x": reach_x, "gusset_reach_z": reach_z,
                "gusset_t_over_wall": r10(web_t / wall),
                "web_spacing": r10(0.5 * depth),
                "web_spacing_over_t": r10(0.5 * depth / web_t),
                "gusset_area": r10(reach_x * reach_z),
                "gusset_volume_frac": r10(
                    reach_x * reach_z * web_t
                    / (leg_x * depth * wall + wall * depth * (leg_z - wall)
                       + reach_x * reach_z * web_t)),
                "brace_frac_of_leg_x": r10(reach_x / leg_x)},
        analytic=None,
    )


def build_multi_hole_plate(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate with 2-4 bores of TWO radii, including a near-touching pair.

    The proximity family. `perforated_plate` already carries many holes, but its
    row is evenly spaced, single-radius and on the centre line, so hole-to-hole
    distance is a constant of the family and carries no information. Here the
    central pair is separated by 0.30-0.55 of the SMALLER hole's radius, the two
    radii differ by up to 1.8x, and the remaining bores sit off the centre line at
    the far corners. Minimum spacing, spacing/radius and mean feature-pair
    distance therefore all vary within one part, which is the only configuration
    in which a proximity feature can be shown to matter -- and it is what pulls
    the family's descriptor centroid clear of `perforated_plate`, which a
    single-radius row did not do (`--check-coverage`).
    """
    half_w = r10(0.5 * scale)
    half_h = r10(half_w * jitter(rng, 0.38, 0.50))
    thickness = r10(half_h * jitter(rng, 0.18, 0.26))
    hole_r = r10(half_h * jitter(rng, 0.16, 0.22))
    small_r = r10(hole_r * jitter(rng, 0.55, 0.85))
    n_holes = 2 + int(rng.random() * 3.0)  # 2, 3 or 4, deterministic under the seed
    gap = r10(small_r * jitter(rng, 0.30, 0.55))
    pair_dx = r10(hole_r + small_r + gap)
    bores = [(r10(-0.5 * pair_dx), 0.0, hole_r), (r10(0.5 * pair_dx), 0.0, small_r)]
    if n_holes >= 3:
        bores.append((r10(-0.55 * half_w), r10(0.45 * half_h), small_r))
    if n_holes >= 4:
        bores.append((r10(0.55 * half_w), r10(-0.45 * half_h), hole_r))
    for centre_x, centre_y, radius in bores:
        if abs(centre_x) + radius > 0.85 * half_w \
                or abs(centre_y) + radius > 0.72 * half_h:
            raise ValueError(f"{name}: bore at ({centre_x}, {centre_y}) breaks the margin")
    margin = r10(max(thickness * 0.5, 1e-4))
    shape = _box((-half_w, -half_h, 0.0),
                 (r10(2.0 * half_w), r10(2.0 * half_h), thickness))
    for centre_x, centre_y, radius in bores:
        drill = _cyl((centre_x, centre_y, r10(-margin)), (0.0, 0.0, 1.0), radius,
                     r10(thickness + 2.0 * margin))
        shape = _cut(shape, drill, name)
    _require_solid(shape, name)
    return Geometry(
        name=name, family="multi_hole_plate", regime=-1, seed=-1, shape=shape,
        lo=(-half_w, -half_h, 0.0), hi=(half_w, half_h, thickness),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=r10(2.0 * half_w),
        params={"half_w": half_w, "half_h": half_h, "thickness": thickness,
                "hole_r": hole_r, "small_hole_r": small_r,
                "radius_ratio": r10(hole_r / small_r),
                "n_holes": float(n_holes),
                "pair_gap": gap, "pair_centre_distance": pair_dx,
                "pair_gap_over_small_r": r10(gap / small_r),
                "hole_r_over_thickness": r10(hole_r / thickness)},
        analytic=None,
    )


def build_bossed_plate(name: str, scale: float, rng: random.Random) -> Geometry:
    """Plate with 1-2 cylindrical bosses: one beside the load patch, one remote.

    Load-to-feature distance is the one retrain feature that no existing family
    varies: `sphere_box` and `ellipsoid_boss` put their boss ON the loaded face,
    everything else keeps its features far from the load. Here the near boss sits
    about one diameter inboard of the loaded end and the far boss (when present)
    sits the same distance from the fixture, so within one family the same
    feature type appears at two very different distances from the load.
    """
    half_w = r10(0.5 * scale)
    half_h = r10(half_w * jitter(rng, 0.34, 0.46))
    thickness = r10(half_h * jitter(rng, 0.18, 0.26))
    boss_r = r10(half_h * jitter(rng, 0.26, 0.36))
    boss_h = r10(thickness * jitter(rng, 1.2, 2.0))
    n_bosses = 1 + int(rng.random() * 2.0)  # 1 or 2, deterministic under the seed
    centres = [(r10(half_w - boss_r * jitter(rng, 1.8, 2.4)), 0.0)]
    if n_bosses >= 2:
        centres.append((r10(-half_w + boss_r * jitter(rng, 1.8, 2.4)), 0.0))
    # The end faces must stay plain rectangles, so a boss has to keep clear of the
    # end by a fraction of its own radius -- the jitter band above guarantees at
    # least 0.8 R, and this is the guard that keeps that true if the band moves.
    for centre_x, centre_y in centres:
        if half_w - abs(centre_x) - boss_r < 0.5 * boss_r \
                or abs(centre_y) + boss_r > 0.8 * half_h:
            raise ValueError(f"{name}: boss at ({centre_x}, {centre_y}) breaks the margin")
    shape = _box((-half_w, -half_h, 0.0),
                 (r10(2.0 * half_w), r10(2.0 * half_h), thickness))
    for centre_x, centre_y in centres:
        shape = _fuse(shape, _cyl((centre_x, centre_y, thickness), (0.0, 0.0, 1.0),
                                  boss_r, boss_h), name)
    _require_solid(shape, name)
    return Geometry(
        name=name, family="bossed_plate", regime=-1, seed=-1, shape=shape,
        lo=(-half_w, -half_h, 0.0), hi=(half_w, half_h, r10(thickness + boss_h)),
        axis=0, transverse=2,
        end_area=r10(2.0 * half_h * thickness), guard_end_area=True,
        load_region="end_slab",
        fix_axis=0, fix_side="lo",
        fix_char_len=thickness, load_char_len=thickness,
        span=r10(2.0 * half_w),
        params={"half_w": half_w, "half_h": half_h, "thickness": thickness,
                "boss_r": boss_r, "boss_h": boss_h, "n_bosses": float(n_bosses),
                "near_boss_x": centres[0][0],
                "near_boss_clearance": r10(half_w - centres[0][0] - boss_r),
                "far_boss_x": centres[1][0] if n_bosses >= 2 else 0.0,
                "boss_h_over_thickness": r10(boss_h / thickness),
                "boss_r_over_half_h": r10(boss_r / half_h)},
        analytic=None,
    )


BUILDERS = {
    "box_hole": build_box_hole,
    "l_bracket": build_l_bracket,
    "plate_notch": build_plate_notch,
    "stepped_shaft": build_stepped_shaft,
    "channel": build_channel,
    "sphere_box": build_sphere_box,
    "tube": build_tube,
    "perforated_plate": build_perforated_plate,
    "ellipsoid_boss": build_ellipsoid_boss,
    "lobed_shaft": build_lobed_shaft,
    "twisted_loft": build_twisted_loft,
    "ribbed_plate": build_ribbed_plate,
    "gusset_bracket": build_gusset_bracket,
    "multi_hole_plate": build_multi_hole_plate,
    "bossed_plate": build_bossed_plate,
}


def build_geometry(family: str, regime: int) -> Geometry:
    index = FAMILIES.index(family) * len(REGIME_SCALE) + regime
    seed = SEEDS[index]
    rng = random.Random(seed)
    name = f"{family}_s{regime}"
    geom = BUILDERS[family](name, REGIME_SCALE[regime], rng)
    geom.regime = regime
    geom.seed = seed
    return geom


# ── boundary-condition sampler ──────────────────────────────────────────────


def _slab(geom: Geometry, axis: int, side: str, depth: float, pad: float
          ) -> list[list[float]]:
    """Select box covering one bbox face: `depth` inwards, `pad` outwards."""
    lo = [geom.lo[i] - pad for i in range(3)]
    hi = [geom.hi[i] + pad for i in range(3)]
    if side == "lo":
        hi[axis] = geom.lo[axis] + depth
    else:
        lo[axis] = geom.hi[axis] - depth
    return [[r10(v) for v in lo], [r10(v) for v in hi]]


def fix_box(geom: Geometry) -> list[list[float]]:
    pad = r10(PAD_FRAC * geom.diag)
    depth = r10(FIX_SLAB_FRAC * geom.fix_char_len)
    return _slab(geom, geom.fix_axis, geom.fix_side, depth, pad)


def load_box(geom: Geometry) -> list[list[float]]:
    pad = r10(PAD_FRAC * geom.diag)
    if geom.load_region == "spherical_cap":
        # The boss equator sits exactly on the box face plane x = length, so a box
        # starting there captures the protruding cap and no body wall face.
        lo = [geom.lo[i] - pad for i in range(3)]
        hi = [geom.hi[i] + pad for i in range(3)]
        lo[geom.axis] = geom.params["length"]
        return [[r10(v) for v in lo], [r10(v) for v in hi]]
    depth = r10(LOAD_SLAB_FRAC * geom.load_char_len)
    return _slab(geom, geom.axis, "hi", depth, pad)


def band_box(geom: Geometry) -> list[list[float]]:
    """Select box for c3's second region: a full-perimeter band at mid-span.

    WHY A BAND AND NOT A SECOND FACE. c3 exists to make two load regions with
    non-parallel resultants, and every family must have one. Eleven of the fifteen
    families have exactly two planar faces (the two section ends), one of which is
    clamped, so there is no second FACE to load -- a second region has to come off
    the wall. A band spanning the whole cross-section is the only wall region that
    is robust for all of them: it contains face centroids at every mesh size and
    on every section shape, hollow or solid, planar or spline, because it cuts the
    solid rather than skimming one side of it. A one-sided patch on a curved wall
    would select nothing at coarse h on a small-radius shaft.

    The band is axially clear of both end selections (it is centred at mid-span
    and 10 % of the extent wide, against end slabs of order 1 % of a section
    length), so c3's two boxes are provably disjoint.
    """
    pad = r10(PAD_FRAC * geom.diag)
    lo = [geom.lo[i] - pad for i in range(3)]
    hi = [geom.hi[i] + pad for i in range(3)]
    mid = 0.5 * (geom.lo[geom.axis] + geom.hi[geom.axis])
    half = BAND_HALF_FRAC * geom.extent(geom.axis)
    lo[geom.axis] = mid - half
    hi[geom.axis] = mid + half
    return [[r10(v) for v in lo], [r10(v) for v in hi]]


def band_area_estimate(geom: Geometry) -> float:
    """Bbox-perimeter estimate of the c3 band's loaded area (surrogate truth only).

    The band's exact area needs the section perimeter, which for four families has
    no closed form. This is the bbox side perimeter times the band length: right
    to within the section's shape factor, and used ONLY inside the provisional
    first-order surrogate that the independent Gmsh/CalculiX chain replaces. It is
    never an authored `expected_area`: the case leaves that out, so testlab
    rescales the traction onto the exact CAD rule area instead.
    """
    others = [i for i in range(3) if i != geom.axis]
    perimeter = 2.0 * (geom.extent(others[0]) + geom.extent(others[1]))
    return r10(perimeter * 2.0 * BAND_HALF_FRAC * geom.extent(geom.axis))


def _in_box(point: tuple[float, float, float], box: list[list[float]]) -> bool:
    return all(box[0][i] <= point[i] <= box[1][i] for i in range(3))


def is_primary_load_face(geom: Geometry, face: PlanarFace) -> bool:
    """Does `face` carry archetype c0's traction?

    True only for the planar face lying IN the loaded end plane of an ``end_slab``
    load region. The plane test is the load-bearing half: `stepped_shaft`'s
    shoulder annulus also faces +axis, and treating it as c0's face would have
    pushed c4 onto the tip disc -- a scaled copy of c0 -- while a genuinely
    independent face was available. A ``spherical_cap`` region loads a curved
    boss, so no planar face is its.
    """
    if geom.load_region != "end_slab" or face.axis != geom.axis or face.side != "hi":
        return False
    return abs(face.centroid[face.axis] - geom.hi[face.axis]) <= 1e-7 * geom.diag


def pressure_face(geom: Geometry) -> PlanarFace:
    """The face c4 presses on: the largest axis-aligned planar face off the fixture.

    Ties are real -- a prismatic box has four identical wall faces -- so the area
    is quantised to nine significant digits before ordering and the remainder is
    broken by axis then side. Without that, two faces whose areas differ in the
    last ulp of an OCC integration would swap between runs and the emitted case
    would not be reproducible, which is the one property this generator sells.

    A face whose centroid lies inside the fixture box is skipped: pressing on the
    clamped face is a load that does no work and a case that measures nothing.

    c0's own end face is used only as a LAST resort. Pressing on it produces the
    same face set and an antiparallel traction, so linearity makes the solution an
    exact scalar multiple of c0's and the case teaches nothing new. Four families
    (`tube`, `lobed_shaft`, `twisted_loft` and any solid of revolution) have
    exactly two planar faces and one of them is clamped, so for those the last
    resort is all there is; the case still records that it is scale-equivalent to
    c0 (`scaled_duplicate_of`) instead of pretending to be independent.
    """
    fix = fix_box(geom)
    candidates = [face for face in axis_aligned_planar_faces(geom.shape, geom.name)
                  if not _in_box(face.centroid, fix)]
    if not candidates:
        raise RuntimeError(f"{geom.name}: every planar face lies inside the fixture")
    independent = [face for face in candidates if not is_primary_load_face(geom, face)]
    return sorted(independent or candidates,
                  key=lambda f: (-float(f"{f.area:.9g}"), f.axis,
                                 0 if f.side == "lo" else 1, f.centroid))[0]


def pressure_box(geom: Geometry, face: PlanarFace) -> list[list[float]]:
    """Thin slab around one planar face, padded in-plane, straddling its plane."""
    depth = r10(max(PRESSURE_SLAB_FRAC * geom.extent(face.axis), 1e-9))
    pad = r10(PRESSURE_PAD_FRAC * geom.diag)
    lo = [face.lo[i] - pad for i in range(3)]
    hi = [face.hi[i] + pad for i in range(3)]
    plane = geom.hi[face.axis] if face.side == "hi" else geom.lo[face.axis]
    lo[face.axis] = plane - depth
    hi[face.axis] = plane + depth
    return [[r10(v) for v in lo], [r10(v) for v in hi]]


def oblique_direction(geom: Geometry, rng: random.Random) -> tuple[float, float, float]:
    """Unit vector off every axis (each |component| >= 0.26), axial part positive."""
    raw = [jitter(rng, 0.45, 1.0) for _ in range(3)]
    for i in range(3):
        if i != geom.axis and rng.random() < 0.5:
            raw[i] = -raw[i]
    norm = math.sqrt(sum(v * v for v in raw))
    return tuple(r10(v / norm) for v in raw)  # type: ignore[return-value]


def case_specs(geom: Geometry) -> list[dict[str, Any]]:
    """Five load cases per part: axial, transverse, oblique, two-region, pressure.

    A spec is the primary region plus optional extras. ``traction`` and
    ``normal_min_dot`` always describe the FIRST ``loads[]`` entry, which is the
    one testlab's tip/load-area probes read (``compute_probes`` uses
    ``loads.front()``), so the primary region of every archetype stays the region
    the scored displacement probe is defined on.
    """
    rng = random.Random(geom.seed + 1)
    axial = [0.0, 0.0, 0.0]
    axial[geom.axis] = TRACTION_AXIAL
    transverse = [0.0, 0.0, 0.0]
    transverse[geom.transverse] = -TRACTION_TRANSVERSE
    direction = oblique_direction(geom, rng)
    oblique = [r10(TRACTION_OBLIQUE * component) for component in direction]
    band = [0.0, 0.0, 0.0]
    band[geom.transverse] = -TRACTION_BAND
    face = pressure_face(geom)
    pressure = [r10(-PRESSURE_NORMAL * component) for component in face.normal]
    primary_area = (geom.end_area if geom.family != "sphere_box"
                    else geom.params["box_section_area"])

    return [
        {
            "index": 0,
            "archetype": "axial_tension",
            "traction": [r10(v) for v in axial],
            # Traction is parallel to the end-face normal, so the CAD face is
            # selected by the normal filter and expected_area prunes any overshoot.
            "normal_min_dot": 0.7,
            "analytic": geom.analytic if geom.analytic == "kirsch" else None,
        },
        {
            "index": 1,
            "archetype": "transverse_bending",
            "traction": [r10(v) for v in transverse],
            # Traction is perpendicular to the end-face normal: the alignment filter
            # would keep only traction-aligned side-wall slivers, so disable it and
            # rely on the thin slab to isolate the end face.
            "normal_min_dot": -1.0,
            "analytic": (geom.analytic
                         if geom.analytic == "stepped_cantilever" else None),
        },
        {
            "index": 2,
            "archetype": "oblique",
            "traction": oblique,
            "normal_min_dot": -1.0,
            "analytic": None,
        },
        {
            "index": 3,
            "archetype": "two_region_multiaxial",
            "traction": [r10(v) for v in axial],
            "normal_min_dot": 0.7,
            # Second region: a mid-span band carrying a transverse traction. The
            # two resultants are perpendicular by construction, which is the only
            # configuration in which `case_load_multiaxiality` is nonzero, and the
            # band is the region no family can fail to resolve (see band_box).
            "extra_regions": [{
                "box": band_box(geom),
                "traction": [r10(v) for v in band],
                # Box-only: a transverse traction on a wall band has no reason to
                # align with any wall normal, so a normal filter would keep only
                # the two side walls and drop the top and bottom.
                "normal_min_dot": -1.0,
            }],
            "corpus_extra": {"load_regions": [
                {"kind": "end_slab",
                 "face": f"{AXIS_NAMES[geom.axis]}_hi",
                 "resultant": "axial"},
                {"kind": "mid_span_band",
                 "axis": AXIS_NAMES[geom.axis],
                 "span_frac": [r10(0.5 - BAND_HALF_FRAC), r10(0.5 + BAND_HALF_FRAC)],
                 "resultant": "transverse"},
            ]},
            "analytic": None,
            "truth_regions": [
                {"area": primary_area, "span": geom.span,
                 "traction": [r10(v) for v in axial], "region": "end_slab"},
                {"area": band_area_estimate(geom), "span": r10(0.5 * geom.span),
                 "traction": [r10(v) for v in band], "region": "mid_span_band"},
            ],
        },
        {
            "index": 4,
            "archetype": "face_pressure",
            "traction": pressure,
            # Traction is antiparallel to the pressed face's own normal, so the
            # 0.7 filter keeps that face and drops every perpendicular neighbour
            # the in-plane pad reaches.
            "normal_min_dot": 0.7,
            "primary_box": pressure_box(geom, face),
            # Authored only when the face is straight-edged: a mesh of a polygon
            # reproduces its exact area, a mesh of a hole-punched or spline-bounded
            # face does not, and an authored area that a correct mesh cannot match
            # is the drift warning this guard exists to raise.
            "primary_expected_area": r10(face.area) if face.straight_edges else None,
            "corpus_extra": {
                "loaded_face": f"{AXIS_NAMES[face.axis]}_{face.side}",
                "load_face_boundary": "straight" if face.straight_edges else "curved",
                "pressure_Pa": PRESSURE_NORMAL,
                "pressure_face_area_m2": r10(face.area),
                "pressure_face_outward_normal": [r10(v) for v in face.normal],
                # Two faces can share a `loaded_face` key: stepped_shaft's tip disc
                # and its shoulder annulus both face +x. The centroid names which.
                "pressure_face_centroid_m": [r10(v) for v in face.centroid],
                # Stated, not hidden: on a solid whose only free planar face is
                # c0's, this case's solution is -PRESSURE_NORMAL/TRACTION_AXIAL
                # times c0's, so a consumer that wants independent rows can drop
                # it on this key instead of discovering the redundancy in a fit.
                **({"scaled_duplicate_of": "c0",
                    "scale_vs_c0": r10(-PRESSURE_NORMAL / TRACTION_AXIAL)}
                   if is_primary_load_face(geom, face) else {}),
            },
            "analytic": None,
            "truth_regions": [
                {"area": r10(face.area), "span": geom.span, "traction": pressure,
                 "region": (f"pressure_face_{AXIS_NAMES[face.axis]}_{face.side}"
                            f"@{r10(face.centroid[face.axis])}")},
            ],
        },
    ]


def case_json(geom: Geometry, spec: dict[str, Any]) -> dict[str, Any]:
    name = f"{geom.name}_c{spec['index']}"
    select: dict[str, Any] = {"box": spec.get("primary_box") or load_box(geom)}
    guard = (spec["primary_expected_area"] if "primary_expected_area" in spec
             else (geom.end_area if geom.guard_end_area else None))
    if guard is not None:
        select["expected_area"] = guard
    select["normal_min_dot"] = spec["normal_min_dot"]
    loads: list[dict[str, Any]] = [{"select": select, "traction": spec["traction"]}]
    for extra in spec.get("extra_regions", []):
        loads.append({
            "select": {"box": extra["box"], "normal_min_dot": extra["normal_min_dot"]},
            "traction": extra["traction"],
        })
    corpus: dict[str, Any] = {
        "family": geom.family,
        "regime": geom.regime,
        "seed": geom.seed,
        "archetype": spec["archetype"],
        "fixed_face": f"{AXIS_NAMES[geom.fix_axis]}_{geom.fix_side}",
        "loaded_face": f"{AXIS_NAMES[geom.axis]}_hi",
        "load_face_boundary": geom.load_face_boundary,
        "generator": "scripts/gen_primitive_corpus.py",
    }
    # Archetype-specific metadata overrides the primary-region defaults in place,
    # so the key ORDER of the block is the same for every case.
    corpus.update(spec.get("corpus_extra", {}))
    return {
        "part": name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "material": dict(MATERIAL),
        "bcs": [{"select": {"box": fix_box(geom)}, "fix": [True, True, True]}],
        "loads": loads,
        "reference": f"bench/reference/corpus/{name}.json",
        "corpus": corpus,
    }


# ── truth ───────────────────────────────────────────────────────────────────


def kirsch_reference(geom: Geometry, case_name: str) -> dict[str, Any]:
    """Analytic Kirsch stress-concentration truth (see hand-calcs.md)."""
    hole_r = geom.params["hole_r"]
    pad = r10(PAD_FRAC * geom.diag)
    patch = [
        [r10(-1.5 * hole_r), r10(-1.5 * hole_r), r10(geom.lo[2] - pad)],
        [r10(1.5 * hole_r), r10(1.5 * hole_r), r10(geom.hi[2] + pad)],
    ]
    return {
        "part": case_name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "family": geom.family,
        "truth_source": "analytic",
        "metrics": [
            {
                "name": "scf",
                "value": 3.0,
                "tol": 0.10,
                "probe": {
                    "kind": "mean_vm_over_nominal",
                    "nominal": TRACTION_AXIAL,
                    "select": {"box": patch},
                },
                "derivation": DERIV_KIRSCH,
                "source": "analytic",
                "inputs": {
                    "sigma_inf": TRACTION_AXIAL,
                    "hole_radius": hole_r,
                    "half_width": geom.params["half_w"],
                    "half_height": geom.params["half_h"],
                    "thickness": geom.params["thickness"],
                    "a_over_W": geom.params["a_over_W"],
                    "a_over_H": geom.params["a_over_H"],
                },
            }
        ],
    }


def stepped_cantilever_reference(geom: Geometry, case_name: str,
                                 traction: list[float]) -> dict[str, Any]:
    """Analytic stepped-cantilever tip deflection + work-conjugate strain energy."""
    young = MATERIAL["E"]
    nu = MATERIAL["nu"]
    length = geom.params["length"]
    step_x = geom.params["step_x"]
    r_root = geom.params["r_root"]
    r_tip = geom.params["r_tip"]

    area_root = math.pi * r_root * r_root
    area_tip = math.pi * r_tip * r_tip
    inertia_root = 0.25 * math.pi * r_root ** 4
    inertia_tip = 0.25 * math.pi * r_tip ** 4

    load = math.sqrt(sum(v * v for v in traction)) * area_tip
    free = length - step_x

    # delta_bend = P/E * integral_0^L (L-x)^2 / I(x) dx  (unit-load method).
    bending = (load / (3.0 * young)) * (
        (length ** 3 - free ** 3) / inertia_root + free ** 3 / inertia_tip
    )
    # Cowper (1966) shear factor for a solid circular section.
    kappa = 6.0 * (1.0 + nu) / (7.0 + 6.0 * nu)
    shear_modulus = young / (2.0 * (1.0 + nu))
    shear = (load / (kappa * shear_modulus)) * (
        step_x / area_root + free / area_tip
    )
    deflection = bending + shear
    energy = 0.5 * load * deflection

    inputs = {
        "E": young, "nu": nu, "G": r10(shear_modulus), "kappa_cowper": r10(kappa),
        "length": length, "step_x": step_x, "free_length": r10(free),
        "r_root": r_root, "r_tip": r_tip,
        "A_root": r10(area_root), "A_tip": r10(area_tip),
        "I_root": r10(inertia_root), "I_tip": r10(inertia_tip),
        "traction_magnitude": r10(math.sqrt(sum(v * v for v in traction))),
        "resultant_load_N": r10(load),
        "delta_bending_m": r10(bending),
        "delta_shear_m": r10(shear),
    }
    return {
        "part": case_name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "family": geom.family,
        "truth_source": "analytic",
        "metrics": [
            {
                "name": "tip_deflection",
                "value": r10(deflection),
                "tol": 0.15,
                "probe": {"kind": "tip_deflection"},
                "derivation": DERIV_CANTILEVER,
                "source": "analytic",
                "inputs": inputs,
            },
            {
                "name": "strain_energy",
                "value": r10(energy),
                "tol": 0.15,
                "probe": {"kind": "strain_energy"},
                "derivation": DERIV_CANTILEVER,
                "source": "analytic",
                "inputs": {"resultant_load_N": r10(load),
                           "tip_deflection_m": r10(deflection),
                           "relation": "U = 0.5 * P * delta"},
            },
        ],
    }


def _beam_region_response(*, area: float, span: float, transverse_extent: float,
                          traction: list[float], axis: int) -> dict[str, float]:
    """First-order response of ONE loaded region on an equivalent rectangular beam.

    Axial ``P*L/(E*A)`` plus Euler-Bernoulli ``P*L^3/(3*E*I)``, and the work each
    does. One definition, used by the single-region surrogate and the multi-region
    one, so the two archetype families cannot drift into different physics.
    """
    young = MATERIAL["E"]
    # Radius of gyration of an equivalent solid rectangular section.
    inertia = area * transverse_extent * transverse_extent / 12.0
    axial_t = abs(traction[axis])
    perp_sq = sum(traction[i] ** 2 for i in range(3) if i != axis)
    perp_t = math.sqrt(perp_sq)
    delta_axial = axial_t * span / young
    load_perp = perp_t * area
    delta_perp = load_perp * span ** 3 / (3.0 * young * inertia)
    return {
        "inertia": inertia,
        "axial_t": axial_t,
        "perp_t": perp_t,
        "delta_axial": delta_axial,
        "load_perp": load_perp,
        "delta_perp": delta_perp,
        "energy": 0.5 * (axial_t * area * delta_axial + load_perp * delta_perp),
    }


def provisional_reference(geom: Geometry, case_name: str,
                          traction: list[float]) -> dict[str, Any]:
    """First-order beam surrogate, superseded by the advisor-truth-0 overkill solve.

    Values exist so ``load_metrics`` can parse the reference and the truth campaign can
    run at all; the 100 % tolerance says outright that they are not a validated truth.
    ``scripts/advisor/promote_truth.py`` overwrites them with the overkill answers.
    """
    young = MATERIAL["E"]
    area = geom.end_area if geom.family != "sphere_box" \
        else geom.params["box_section_area"]
    span = geom.span
    transverse_extent = geom.extent(geom.transverse)
    response = _beam_region_response(area=area, span=span,
                                     transverse_extent=transverse_extent,
                                     traction=traction, axis=geom.axis)
    inertia = response["inertia"]
    axial_t = response["axial_t"]
    perp_t = response["perp_t"]
    delta_axial = response["delta_axial"]
    load_perp = response["load_perp"]
    delta_perp = response["delta_perp"]
    deflection = math.sqrt(delta_axial ** 2 + delta_perp ** 2)
    energy = response["energy"]
    if not (deflection > 0.0 and energy > 0.0):
        raise RuntimeError(f"{case_name}: provisional surrogate produced a null truth")

    inputs = {
        "E": young, "span_m": span, "section_area_m2": r10(area),
        "transverse_extent_m": r10(transverse_extent),
        "I_equivalent_m4": r10(inertia),
        "axial_traction_Pa": r10(axial_t),
        "transverse_traction_Pa": r10(perp_t),
        "delta_axial_m": r10(delta_axial),
        "delta_transverse_m": r10(delta_perp),
        "model": "axial P*L/(E*A) + Euler-Bernoulli P*L^3/(3*E*I) on an equivalent "
                 "rectangular section; U = 0.5 * sum(P_i * delta_i)",
    }
    note = ("Provisional first-order beam surrogate; not a validated truth. Replaced by "
            "the advisor-truth-0 overkill reference solve via "
            "scripts/advisor/promote_truth.py.")
    return {
        "part": case_name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "family": geom.family,
        "truth_source": "provisional",
        "notes": note,
        "metrics": [
            {
                "name": "strain_energy",
                "value": r10(energy),
                "tol": 1.0,
                "probe": {"kind": "strain_energy"},
                "derivation": DERIV_PROVISIONAL,
                "source": "provisional",
                "inputs": inputs,
            },
            {
                "name": "tip_deflection",
                "value": r10(deflection),
                "tol": 1.0,
                "probe": {"kind": "tip_deflection"},
                "derivation": DERIV_PROVISIONAL,
                "source": "provisional",
                "inputs": inputs,
            },
        ],
    }


def region_surrogate_reference(geom: Geometry, case_name: str,
                               regions: list[dict[str, Any]]) -> dict[str, Any]:
    """Superposed first-order surrogate for the multi-region and pressure archetypes.

    Same physics as ``provisional_reference``, summed over regions, and marked
    ``provisional`` so the truth guard treats it as this repo's own seed. It is a
    PLACEHOLDER whose only jobs are to let ``load_metrics`` parse the reference and
    to declare which probes the case is scored on; the 100 % tolerance says so.

    Unlike the legacy archetypes, these cases are NOT eligible for the internal
    overkill campaign (see ``truth_campaign``): the values here are replaced only by
    the independent Gmsh -> CalculiX chain in ``bench/reference/external_truth.py``.
    Both probes are global quantities, never a nodal peak, so refining the candidate
    mesh can only move them toward the reference (ADR-0032 section 4.1).
    """
    transverse_extent = geom.extent(geom.transverse)
    responses = [
        _beam_region_response(area=region["area"], span=region["span"],
                              transverse_extent=transverse_extent,
                              traction=region["traction"], axis=geom.axis)
        for region in regions
    ]
    delta_axial = sum(response["delta_axial"] for response in responses)
    delta_perp = sum(response["delta_perp"] for response in responses)
    deflection = math.sqrt(delta_axial ** 2 + delta_perp ** 2)
    energy = sum(response["energy"] for response in responses)
    if not (deflection > 0.0 and energy > 0.0):
        raise RuntimeError(f"{case_name}: region surrogate produced a null truth")

    inputs = {
        "E": MATERIAL["E"],
        "transverse_extent_m": r10(transverse_extent),
        "regions": [
            {
                "region": region["region"],
                "area_m2": r10(region["area"]),
                "span_m": r10(region["span"]),
                "traction_Pa": [r10(v) for v in region["traction"]],
                "axial_traction_Pa": r10(response["axial_t"]),
                "transverse_traction_Pa": r10(response["perp_t"]),
                "delta_axial_m": r10(response["delta_axial"]),
                "delta_transverse_m": r10(response["delta_perp"]),
                "work_J": r10(response["energy"]),
            }
            for region, response in zip(regions, responses)
        ],
        "delta_axial_total_m": r10(delta_axial),
        "delta_transverse_total_m": r10(delta_perp),
        "model": "per-region axial P*L/(E*A) + Euler-Bernoulli P*L^3/(3*E*I) on an "
                 "equivalent rectangular section, superposed; "
                 "U = 0.5 * sum(P_i * delta_i). The c3 band area is a bbox-perimeter "
                 "estimate (band_area_estimate), so this surrogate is weaker than the "
                 "legacy one and is scored against nothing until the external chain "
                 "replaces it.",
    }
    note = ("Provisional superposed first-order surrogate; not a validated truth. "
            "Replaced ONLY by the independent Gmsh 4.13.1 + CalculiX 2.23 chain "
            "(bench/reference/external_truth.py); this case is excluded from the "
            "advisor-truth-0 internal campaign by construction.")
    return {
        "part": case_name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "family": geom.family,
        "truth_source": "provisional",
        "notes": note,
        "metrics": [
            {
                "name": "strain_energy",
                "value": r10(energy),
                "tol": 1.0,
                "probe": {"kind": "strain_energy"},
                "derivation": DERIV_PROVISIONAL,
                "source": "provisional",
                "inputs": inputs,
            },
            {
                "name": "tip_deflection",
                "value": r10(deflection),
                "tol": 1.0,
                "probe": {"kind": "tip_deflection"},
                "derivation": DERIV_PROVISIONAL,
                "source": "provisional",
                "inputs": inputs,
            },
        ],
    }


def reference_json(geom: Geometry, spec: dict[str, Any]) -> dict[str, Any]:
    case_name = f"{geom.name}_c{spec['index']}"
    if spec["analytic"] == "kirsch":
        return kirsch_reference(geom, case_name)
    if spec["analytic"] == "stepped_cantilever":
        return stepped_cantilever_reference(geom, case_name, spec["traction"])
    if spec.get("truth_regions") is not None:
        return region_surrogate_reference(geom, case_name, spec["truth_regions"])
    return provisional_reference(geom, case_name, spec["traction"])


# ── writers ─────────────────────────────────────────────────────────────────


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, allow_nan=False)
        stream.write("\n")


#: Overkill-reference resolution ladder, finest-feasible-wins.
#:
#: The plan's single ``h_rel = 0.005`` cannot produce a reference at all: testlab
#: refuses a run when ``predict_elem_count = 6 * bbox_volume / h^3`` exceeds
#: ``2 * kMaxCampaignElems = 120000`` (apps/testlab/main.cpp:1812-1821), and at
#: ``h_rel = 0.005`` the boxiest corpus part predicts 6.28e6 elements -- every one of the
#: 72 runs would return ``status = "over_budget"`` with no ``answers``. The compiled caps
#: (``kMaxCampaignElems = 60000``, ``kMaxCampaignDof = 80000``) put the order-2 ceiling
#: near 19k tetrahedra, so the finest honest reference is per-part, not global: slender
#: parts (F = bbox_volume / diag^3 ~ 0.012) reach h_rel 0.024 while the boxy ones
#: (F ~ 0.13) top out around 0.05.
#:
#: So the campaign sweeps three rungs and ``scripts/advisor/promote_truth.py`` promotes
#: the health-ok row with the largest ``n_dof`` per part -- each case automatically gets
#: the finest resolution its geometry can afford, and rungs that bust the budget are
#: skipped (they fail before meshing, so they cost almost nothing). Every rung is still
#: 2.5-8x finer than the training grid's 0.10-0.20.
TRUTH_H_REL_LADDER = (0.060, 0.038, 0.024)


def internal_truth_eligible(family: str, archetype_index: int) -> bool:
    """May this case's truth come from OUR OWN overkill solve?

    Only for cases that predate the portable-cost retrain. The internal ladder
    promotes this engine's finest mesh into the reference the same engine is then
    scored against, which is self-assessment; the corpus's value is that 88 of its
    references do not work that way. Rather than widen that hole, every case added
    by the retrain is excluded here and labelled exclusively by the independent
    Gmsh -> CalculiX chain. This predicate is the single place that rule lives.
    """
    return family not in RETRAIN_FAMILIES and archetype_index not in RETRAIN_ARCHETYPES


def truth_campaign(case_paths: list[str]) -> dict[str, Any]:
    n_runs = len(case_paths) * len(TRUTH_H_REL_LADDER)
    return {
        "name": "advisor-truth-0",
        "comment": (
            f"Overkill reference solves for the LEGACY procedural primitive corpus: "
            f"{len(case_paths)} cases x {len(TRUTH_H_REL_LADDER)} resolution rungs x 1 "
            f"tier = {n_runs} runs. Ground truth for every legacy case without a closed "
            "form. Cases added by the portable-cost retrain are absent by construction "
            "(internal_truth_eligible): their truth comes only from the independent "
            "Gmsh + CalculiX chain, never from this engine. Promoted into "
            "bench/reference/corpus/*.json by "
            "scripts/advisor/promote_truth.py (finest health-ok row per part wins) and "
            "never recomputed. The ladder replaces a single h_rel=0.005 because testlab "
            "refuses any run predicting more than 120000 elements, which h_rel=0.005 "
            "does for every corpus part; see TRUTH_H_REL_LADDER in "
            "scripts/gen_primitive_corpus.py. Regenerate this file with that script."
        ),
        "warehouse": True,
        "parts": case_paths,
        "tiers": [{"h_scale": 1.0, "keep_frac": 1.0}],
        "grid": {
            "mesher": ["graded_tet"],
            "h_rel": list(TRUTH_H_REL_LADDER),
            "order": [2],
            "adapt_passes": [3],
            "eta_target": [0.01],
            "element_tendency": [0.0],
            "feature_refine": [True],
            "bc_grading": [True],
            "skin_layers": [2],
            "p_elevate": [False],
            "adapt_leb_waves": [2],
        },
        "score": {"weights": {"accuracy": 1.0, "solve_ms": 0.0, "mesh_ms": 0.0}},
        "resources": {
            "max_run_wall_s": 1800,
            "max_pack_wall_s": 172800,
            "max_threads": 0,
            "max_mem_gb": 0,
        },
    }


def generate(families: list[str], regimes: list[int],
             *, force_overwrite_external: bool = False) -> tuple[int, int, list[str]]:
    STEP_DIR.mkdir(parents=True, exist_ok=True)
    REFERENCE_DIR.mkdir(parents=True, exist_ok=True)
    case_paths: list[str] = []
    n_parts = 0
    n_cases = 0
    protected_skipped: list[str] = []
    external_only: list[str] = []
    for family in FAMILIES:
        if family not in families:
            continue
        for regime in regimes:
            geom = build_geometry(family, regime)
            write_step(geom.shape, STEP_DIR / f"{geom.name}.step", name=geom.name)
            n_parts += 1
            for spec in case_specs(geom):
                case = case_json(geom, spec)
                name = case["part"]
                write_json(CASE_DIR / f"{name}.case.json", case)
                # Geometry and cases are always regenerated (they are derived from
                # the committed seed table), but a reference truth may have been
                # replaced by an INDEPENDENT source since this corpus was seeded.
                # Re-seeding it would silently discard a Gmsh+CalculiX or
                # closed-form answer and restore a first-order surrogate, so the
                # same allowlist that guards promote_truth.py applies here.
                reference_path = REFERENCE_DIR / f"{name}.json"
                blocked: list[tuple[str, str]] = []
                if reference_path.is_file():
                    try:
                        existing = json.loads(reference_path.read_text(encoding="utf-8"))
                    except (OSError, json.JSONDecodeError):
                        existing = {}
                    blocked = protected_metrics(existing)
                if blocked and not force_overwrite_external:
                    detail = ", ".join(f"{m} [{s}]" for m, s in blocked)
                    protected_skipped.append(f"{name}: {detail}")
                else:
                    write_json(reference_path, reference_json(geom, spec))
                if internal_truth_eligible(geom.family, spec["index"]):
                    case_paths.append(
                        f"bench/geometries/corpus/primitives/{name}.case.json"
                    )
                else:
                    external_only.append(name)
                n_cases += 1
    if set(families) == set(FAMILIES) and set(regimes) == set(range(len(REGIME_SCALE))):
        write_json(TRUTH_CAMPAIGN_DIR / "campaign.json", truth_campaign(case_paths))
        print(f"wrote {(TRUTH_CAMPAIGN_DIR / 'campaign.json').relative_to(ROOT)}"
              f"  ({len(case_paths)} parts)")
    else:
        print("partial selection: bench/campaigns/advisor-truth-0/campaign.json "
              "left untouched (regenerate with the full corpus)")
    if external_only:
        manifest = {
            "comment": (
                "Cases whose reference truth may come ONLY from the independent "
                "Gmsh + CalculiX chain: the four reinforcement/proximity families and "
                "archetypes c3/c4 on every family. They are absent from "
                "bench/campaigns/advisor-truth-0/campaign.json by construction, so "
                "scripts/advisor/promote_truth.py can never reach them. Scored probes "
                "are strain_energy and tip_deflection only. Regenerate this file with "
                "scripts/gen_primitive_corpus.py."
            ),
            "generator": "scripts/gen_primitive_corpus.py",
            "truth_chain": "bench/reference/external_truth.py "
                           "(gmsh 4.13.1 mesh -> calculix 2.23 solve)",
            "command": ("python bench/reference/external_truth.py --stage generate "
                        "$(jq -r '.cases[] | \"--case \" + .' "
                        "bench/reference/external/pending-external-truth.json)"),
            "n_cases": len(external_only),
            "cases": sorted(external_only),
        }
        write_json(EXTERNAL_PENDING_JSON, manifest)
        print(f"  {len(external_only)} case(s) are EXCLUDED from the internal "
              "overkill campaign and carry a provisional seed until the independent "
              "chain labels them:")
        print(f"    wrote {EXTERNAL_PENDING_JSON.relative_to(ROOT)}")
        print(f"    {manifest['command']}")
    return n_parts, n_cases, protected_skipped


# ── --check ─────────────────────────────────────────────────────────────────


def _boxes_intersect(box: list[list[float]],
                     lo: tuple[float, float, float],
                     hi: tuple[float, float, float]) -> bool:
    return all(box[0][i] <= hi[i] and box[1][i] >= lo[i] for i in range(3))


ACCEPTED_PROBE_KINDS = frozenset({
    # apps/testlab/main.cpp evaluate_probe(), lines 1582-1631.
    "mean_vm", "mean_von_mises", "face_mean_vm",
    "mean_vm_over_nominal", "scf_mean", "scf",
    "max_von_mises", "max_vm", "max_vm_over_nominal",
    "sigma_p99", "p99_vm",
    "strain_energy", "energy",
    "max_displacement", "tip_deflection",
    "mean_ux_on_face", "mean_uz_on_face",
    # Box-local nodal peak (`sigma_box_max`). evaluate_probe has accepted both
    # since it gained them, but this list did not, so `--check` reported the four
    # box_hole Howland SCF references as unusable for 4 corpus generations. They
    # are usable: a nodal peak is only prohibited as a score where it DIVERGES
    # under refinement, and the maximum beside a smooth circular hole in tension
    # is finite (ADR-0023; contrast the clamped-edge singularity that made
    # smoke_bar's max_von_mises reference punish the advisor for refining, which
    # is why `load_metrics` rejects the unrestricted max_* kinds outright).
    # Authoring one of these against a re-entrant corner or a clamped edge is
    # still a defect this list cannot catch.
    "peak_vm", "peak_vm_over_nominal",
})


#: Probe kinds a case added by the portable-cost retrain may be scored on. Global
#: quantities only: a nodal peak beside a rib root, a gusset toe or a
#: near-touching bore pair is a stress SINGULARITY, so refining the candidate mesh
#: drives it away from any finite reference and the score would punish the advisor
#: for meshing better (ADR-0032 section 4.1, and the smoke_bar max_von_mises
#: lesson). Enforced here rather than left to the reference author.
RETRAIN_PROBE_KINDS = frozenset({
    "strain_energy", "energy", "max_displacement", "tip_deflection",
})

#: Archetype index -> the number of ``loads[]`` entries the archetype must emit.
ARCHETYPE_LOAD_REGIONS = {0: 1, 1: 1, 2: 1, 3: 2, 4: 1}


def check() -> int:
    problems: list[str] = []
    steps = sorted(STEP_DIR.glob("*.step"))
    cases = sorted(STEP_DIR.glob("*.case.json"))
    if not steps:
        print(f"error: no STEP files under {STEP_DIR.relative_to(ROOT)}; "
              "run the generator first", file=sys.stderr)
        return 1

    bounds: dict[str, tuple[tuple[float, float, float], tuple[float, float, float]]] = {}
    for path in steps:
        shape = read_step(path)
        try:
            _require_solid(shape, path.stem)
        except RuntimeError as exc:
            problems.append(f"{path.name}: {exc}")
            continue
        volume = _volume(shape)
        if not volume > 0.0:
            problems.append(f"{path.name}: non-positive volume {volume}")
        bounds[path.stem] = occ_bbox(shape)
    print(f"checked {len(steps)} STEP solids (valid, one solid, volume > 0)")

    for path in cases:
        try:
            case = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            problems.append(f"{path.name}: {exc}")
            continue
        stem = Path(case["geometry"]).stem
        if stem not in bounds:
            problems.append(f"{path.name}: geometry {case['geometry']} not generated")
            continue
        lo, hi = bounds[stem]
        for group, key in (("bcs", "fix"), ("loads", "traction")):
            for i, region in enumerate(case.get(group, [])):
                box = region["select"]["box"]
                if not _boxes_intersect(box, lo, hi):
                    problems.append(
                        f"{path.name}: {group}[{i}].select.box {box} misses the "
                        f"{stem} bbox {lo}..{hi}"
                    )
                if key not in region:
                    problems.append(f"{path.name}: {group}[{i}] missing '{key}'")
        for i, region in enumerate(case["loads"]):
            if not any(abs(v) > 0.0 for v in region["traction"]):
                problems.append(f"{path.name}: loads[{i}] has zero traction")
        boxes = [region["select"]["box"] for region in case["loads"]]
        if len(boxes) != len({json.dumps(box) for box in boxes}):
            problems.append(f"{path.name}: two load regions share the same select box")

        corpus = case.get("corpus", {})
        family = corpus.get("family", "")
        index = int(str(case["part"]).rsplit("_c", 1)[-1])
        expected_regions = ARCHETYPE_LOAD_REGIONS.get(index)
        if expected_regions is not None and len(case["loads"]) != expected_regions:
            problems.append(f"{path.name}: archetype c{index} must emit "
                            f"{expected_regions} load region(s), found "
                            f"{len(case['loads'])}")
        if index == 4:
            normal = corpus.get("pressure_face_outward_normal")
            pressure = corpus.get("pressure_Pa")
            traction = case["loads"][0]["traction"]
            if not isinstance(normal, list) or len(normal) != 3 or not pressure:
                problems.append(f"{path.name}: c4 must declare pressure_Pa and "
                                "pressure_face_outward_normal")
            else:
                want = [-float(pressure) * float(v) for v in normal]
                if max(abs(a - b) for a, b in zip(traction, want)) > 1e-6 * float(pressure):
                    problems.append(
                        f"{path.name}: c4 traction {traction} is not the declared "
                        f"pressure along -normal {want}")

        ref_path = ROOT / case["reference"]
        if not ref_path.is_file():
            problems.append(f"{path.name}: missing reference {case['reference']}")
            continue
        reference = json.loads(ref_path.read_text(encoding="utf-8"))
        metrics = reference.get("metrics")
        if not isinstance(metrics, list) or not metrics:
            problems.append(f"{ref_path.name}: metrics[] missing or empty")
            continue
        if reference.get("part") != case["part"]:
            problems.append(f"{ref_path.name}: part mismatch with {path.name}")
        for metric in metrics:
            kind = metric.get("probe", {}).get("kind")
            if kind not in ACCEPTED_PROBE_KINDS:
                problems.append(f"{ref_path.name}: probe kind '{kind}' is not accepted "
                                "by testlab evaluate_probe")
            if not internal_truth_eligible(family, index) \
                    and kind not in RETRAIN_PROBE_KINDS:
                problems.append(
                    f"{ref_path.name}: probe kind '{kind}' is not allowed for a case "
                    "added by the portable-cost retrain (global quantities only, "
                    f"one of {sorted(RETRAIN_PROBE_KINDS)})")
            if not isinstance(metric.get("value"), (int, float)):
                problems.append(f"{ref_path.name}: metric '{metric.get('name')}' "
                                "has no numeric value")
            if kind in {"mean_vm_over_nominal", "scf", "scf_mean",
                        "max_vm_over_nominal"} \
                    and not abs(float(metric["probe"].get("nominal", 0.0))) > 0.0:
                problems.append(f"{ref_path.name}: metric '{metric.get('name')}' "
                                "needs probe.nominal != 0")
            select = metric.get("probe", {}).get("select")
            if select and not _boxes_intersect(select["box"], lo, hi):
                problems.append(f"{ref_path.name}: probe select box misses the bbox")
    print(f"checked {len(cases)} case JSONs and their references "
          "(parse, bbox intersection, probe kinds)")

    campaign_path = TRUTH_CAMPAIGN_DIR / "campaign.json"
    if campaign_path.is_file():
        campaign = json.loads(campaign_path.read_text(encoding="utf-8"))
        missing = [p for p in campaign["parts"] if not (ROOT / p).is_file()]
        if missing:
            problems.append(f"campaign.json references {len(missing)} missing cases: "
                            f"{missing[:3]}")
        print(f"checked {campaign_path.relative_to(ROOT)} "
              f"({len(campaign['parts'])} parts)")
        ineligible = [p for p in campaign["parts"]
                      if not internal_truth_eligible(
                          Path(p).name.split(".case.json")[0].rsplit("_s", 1)[0],
                          int(Path(p).name.split(".case.json")[0].rsplit("_c", 1)[-1]))]
        if ineligible:
            problems.append(
                f"campaign.json offers the internal overkill ladder to "
                f"{len(ineligible)} case(s) whose truth must be independent: "
                f"{ineligible[:3]}")
    else:
        problems.append(f"missing {campaign_path.relative_to(ROOT)}")

    if problems:
        print(f"\n{len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("check: OK")
    return 0


# ── --check-coverage ────────────────────────────────────────────────────────


def _coverage_module():
    """Import the descriptor machinery lazily.

    Lazily because ``advisor.corpus_evidence`` pulls in numpy and the dataset
    loader, and neither generation nor ``--check`` needs either; a generator that
    cannot emit a STEP because a training-side import moved would be a silly way
    to break the corpus.
    """
    import importlib

    return importlib.import_module("advisor.corpus_evidence")


def check_coverage(new_families: tuple[str, ...] = RETRAIN_FAMILIES, *,
                   duplicate_of: str | None = None,
                   csv_path: Path | None = None) -> int:
    """Is every new family at least as far from the corpus as the corpus is wide?

    The gate the `perforated_plate` lesson earned: that family landed CLOSER to an
    existing one than the existing families were to each other, which makes it a
    weaker test of transfer, and nothing in the generator noticed. Now the
    generator refuses to call a family a widening until its standardized
    descriptor centroid clears the corpus's own minimum pairwise distance.

    ``duplicate_of`` is the negative test: it injects a near-duplicate of an
    existing family (its own descriptor rows, nudged by one part in a million) and
    passes only if the gate REJECTS it. A gate that has never been shown to fail is
    not evidence that anything passed it.
    """
    import numpy as np

    evidence = _coverage_module()
    csv_path = csv_path or evidence.FEATURES_CSV
    names, raw, families = evidence.load_descriptors(csv_path)
    under_test = list(new_families)
    if duplicate_of is not None:
        if duplicate_of not in set(families.tolist()):
            print(f"error: no descriptor rows for '{duplicate_of}' to clone",
                  file=sys.stderr)
            return 2
        source = raw[families == duplicate_of]
        clone_name = f"{duplicate_of}_nearduplicate"
        raw = np.vstack([raw, source * (1.0 + 1.0e-6)])
        families = np.concatenate([families, np.full(source.shape[0], clone_name)])
        under_test = [clone_name]
    report = evidence.coverage_report(raw, families, under_test)
    print(f"descriptor space: {len(names)} columns, {raw.shape[0]} geometries, "
          f"{len(report['reference_families'])} reference families")
    print(f"reference minimum pairwise centroid distance: "
          f"{report['reference_min_pairwise_distance']:.4f} "
          f"over {report['n_reference_pairs']} pairs")
    for family, entry in sorted(report["families_under_test"].items()):
        verdict = "REJECT" if entry["below_reference_minimum"] else "ok"
        print(f"  {family:<24} d={entry['distance_to_nearest_reference']:.4f} "
              f"to {entry['nearest_reference_family']:<18} "
              f"margin={entry['margin_over_threshold']:+.4f}  {verdict}")
    for family in report["missing_descriptors"]:
        print(f"  {family:<24} NO DESCRIPTOR ROWS -- cannot be certified distinct; "
              f"regenerate {csv_path.name} first")

    if duplicate_of is not None:
        rejected = all(entry["below_reference_minimum"]
                       for entry in report["families_under_test"].values())
        if not rejected or not report["families_under_test"]:
            print("negative test FAILED: the gate accepted a near-duplicate family",
                  file=sys.stderr)
            return 1
        print("negative test passed: the near-duplicate was rejected")
        return 0
    if not report["ok"]:
        print("coverage FAILED: see the rejected families above", file=sys.stderr)
        return 1
    print("coverage: OK")
    return 0


# ── --self-test ─────────────────────────────────────────────────────────────


#: SHA-256 of the comma-joined seed table as it stood before the portable-cost
#: retrain appended to it. Every campaign row, every reference truth and every
#: committed STEP in the legacy corpus is a function of these 44 integers in this
#: order, so a reordering is a silent corpus swap. A checksum pins the order
#: without a second copy of the table to drift from.
LEGACY_SEED_DIGEST = "e4f066278c07721a2ce7655ec2fc64daf8960d47082762f655337144c28e85c8"
LEGACY_SEED_COUNT = 44


def _seed_digest(seeds: tuple[int, ...]) -> str:
    import hashlib

    return hashlib.sha256(",".join(str(seed) for seed in seeds).encode()).hexdigest()


def _selftest_geometry(name: str = "selftest_s0") -> Geometry:
    """A plain 3:1:0.5 plate, so the archetype contracts can be checked without CAD."""
    shape = _box((0.0, -0.05, 0.0), (0.30, 0.10, 0.02))
    return Geometry(
        name=name, family="selftest", regime=0, seed=12345, shape=shape,
        lo=(0.0, -0.05, 0.0), hi=(0.30, 0.05, 0.02),
        axis=0, transverse=2,
        end_area=0.002, guard_end_area=True, load_region="end_slab",
        fix_axis=0, fix_side="lo", fix_char_len=0.02, load_char_len=0.02,
        span=0.30, params={}, analytic=None,
    )


def run_self_test() -> int:
    """The invariants the corpus's reproducibility and truth discipline rest on."""
    failures: list[str] = []

    def check_that(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    print("self-test: tables")
    check_that(len(FAMILIES) == 15, f"expected 15 families, got {len(FAMILIES)}")
    check_that(len(SEEDS) == len(FAMILIES) * len(REGIME_SCALE),
               f"seed table has {len(SEEDS)} entries for "
               f"{len(FAMILIES) * len(REGIME_SCALE)} parts")
    check_that(len(set(SEEDS)) == len(SEEDS), "seed table repeats a seed")
    check_that(set(BUILDERS) == set(FAMILIES), "BUILDERS and FAMILIES disagree")
    check_that(_seed_digest(SEEDS[:LEGACY_SEED_COUNT]) == LEGACY_SEED_DIGEST,
               "the first 44 seeds changed: the legacy corpus is no longer "
               "reproducible from this table")
    check_that(FAMILIES[len(FAMILIES) - len(RETRAIN_FAMILIES):] == RETRAIN_FAMILIES,
               "the retrain families are not the last entries in FAMILIES")

    print("self-test: archetype contracts")
    geom = _selftest_geometry()
    specs = case_specs(geom)
    check_that([spec["index"] for spec in specs] == [0, 1, 2, 3, 4],
               "case_specs must emit archetypes c0..c4 in order")
    cases = [case_json(geom, spec) for spec in specs]
    for spec, case in zip(specs, cases):
        want = ARCHETYPE_LOAD_REGIONS[spec["index"]]
        check_that(len(case["loads"]) == want,
                   f"c{spec['index']} emitted {len(case['loads'])} load regions, "
                   f"want {want}")
    c3 = cases[3]
    first = c3["loads"][0]["traction"]
    second = c3["loads"][1]["traction"]
    dot = sum(a * b for a, b in zip(first, second))
    check_that(abs(dot) < 1e-9,
               f"c3's two regions must pull in independent directions, dot={dot}")
    check_that(c3["loads"][0]["select"]["box"] != c3["loads"][1]["select"]["box"],
               "c3's two regions must be distinct boxes")
    band = c3["loads"][1]["select"]["box"]
    check_that(band[0][0] > geom.lo[0] and band[1][0] < geom.hi[0],
               "c3's band must stay clear of both ends")
    c4 = cases[4]
    normal = c4["corpus"]["pressure_face_outward_normal"]
    traction = c4["loads"][0]["traction"]
    check_that(all(abs(t + PRESSURE_NORMAL * n) < 1e-9 * PRESSURE_NORMAL
                   for t, n in zip(traction, normal)),
               f"c4 traction {traction} must be the pressure along -{normal}")
    check_that(abs(abs(sum(n * n for n in normal)) - 1.0) < 1e-9,
               f"c4 face normal {normal} is not a unit axis direction")
    check_that(c4["loads"][0]["select"]["box"] != cases[0]["loads"][0]["select"]["box"],
               "c4 must select its own face, not the c0 end slab")

    print("self-test: truth discipline")
    for spec, case in zip(specs, cases):
        reference = reference_json(geom, spec)
        kinds = {metric["probe"]["kind"] for metric in reference["metrics"]}
        if spec["index"] in RETRAIN_ARCHETYPES:
            check_that(kinds <= RETRAIN_PROBE_KINDS,
                       f"c{spec['index']} reference uses non-global probes {kinds}")
            check_that(all(metric["source"] in SELF_GENERATED_SOURCES
                           for metric in reference["metrics"]),
                       f"c{spec['index']} seed must be overwritable by the external "
                       "chain")
        check_that(all(metric["value"] > 0.0 for metric in reference["metrics"]),
                   f"c{spec['index']} reference carries a non-positive value")
    for family in RETRAIN_FAMILIES:
        check_that(not internal_truth_eligible(family, 0),
                   f"{family} must never be offered the internal overkill ladder")
    for index in RETRAIN_ARCHETYPES:
        check_that(not internal_truth_eligible(FAMILIES[0], index),
                   f"archetype c{index} must never be offered the internal ladder")
    check_that(internal_truth_eligible("box_hole", 0),
               "legacy cases must stay eligible for the internal ladder")

    print("self-test: coverage gate")
    import numpy as np

    rng = np.random.default_rng(11)
    base = rng.normal(size=(24, 6)) + np.repeat(np.arange(6.0), 4)[:, None] * 3.0
    families = np.repeat([f"fam{i}" for i in range(6)], 4)
    evidence = _coverage_module()
    distinct = evidence.coverage_report(
        np.vstack([base, base[:4] + 40.0]),
        np.concatenate([families, np.full(4, "distinct")]), ["distinct"])
    check_that(distinct["ok"], "the gate rejected a genuinely distant family")
    duplicate = evidence.coverage_report(
        np.vstack([base, base[:4] * (1.0 + 1e-6)]),
        np.concatenate([families, np.full(4, "clone")]), ["clone"])
    check_that(not duplicate["ok"], "the gate accepted a near-duplicate family")
    absent = evidence.coverage_report(base, families, ["never_generated"])
    check_that(not absent["ok"], "the gate passed a family with no descriptor rows")

    if failures:
        print(f"\nself-test FAILED: {len(failures)} check(s)", file=sys.stderr)
        for message in failures:
            print(f"  - {message}", file=sys.stderr)
        return 1
    print("\nself-test passed")
    return 0


# ── CLI ─────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="re-read every generated STEP and case/reference JSON "
                             "and validate them; write nothing")
    parser.add_argument("--family", action="append", choices=list(FAMILIES),
                        help="generate only this family (repeatable)")
    parser.add_argument("--regime", action="append", type=int,
                        choices=list(range(len(REGIME_SCALE))),
                        help="generate only this size regime (repeatable)")
    parser.add_argument("--force-overwrite-external", action="store_true",
                        help="DESTRUCTIVE: also re-seed reference truths whose source "
                             "is protected (analytic / external-*), discarding an "
                             "independent answer for a first-order surrogate")
    parser.add_argument("--check-coverage", action="store_true",
                        help="measure each retrain family's standardized descriptor "
                             "centroid against the corpus's own minimum pairwise "
                             "distance and fail if it is closer; writes nothing")
    parser.add_argument("--coverage-negative-test", metavar="FAMILY",
                        help="inject a near-duplicate of FAMILY and require "
                             "--check-coverage to reject it (gate self-check)")
    parser.add_argument("--coverage-csv", type=Path, default=None,
                        help="descriptor CSV to measure coverage against; defaults "
                             "to bench/advisor/geometry_features.csv. Point it at a "
                             "freshly regenerated file to gate BEFORE committing it")
    parser.add_argument("--self-test", action="store_true",
                        help="check the table, archetype and truth-discipline "
                             "invariants; touches no artifact")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    if args.coverage_negative_test:
        return check_coverage(duplicate_of=args.coverage_negative_test,
                              csv_path=args.coverage_csv)
    if args.check_coverage:
        return check_coverage(csv_path=args.coverage_csv)
    if args.check:
        return check()

    families = args.family or list(FAMILIES)
    regimes = sorted(set(args.regime or range(len(REGIME_SCALE))))
    if args.force_overwrite_external:
        print("WARNING: --force-overwrite-external will re-seed protected reference "
              "truths, replacing independently sourced answers with first-order "
              "surrogates", file=sys.stderr)
    n_parts, n_cases, protected_skipped = generate(
        families, regimes, force_overwrite_external=args.force_overwrite_external)
    n_written = n_cases - len(protected_skipped)
    print(f"corpus: {n_parts} STEP parts, {n_cases} case JSONs, "
          f"{n_written} reference truths written")
    print(f"  geometry + cases: {STEP_DIR.relative_to(ROOT)}")
    print(f"  truths:           {REFERENCE_DIR.relative_to(ROOT)}")
    if protected_skipped:
        print(f"  REFUSED to re-seed {len(protected_skipped)} protected reference "
              "truth(s) -- not this repo's truth to rewrite:")
        for entry in protected_skipped:
            print(f"    {entry}")
        print("  (may only re-seed "
              + "/".join(sorted(SELF_GENERATED_SOURCES))
              + "; pass --force-overwrite-external to override)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
