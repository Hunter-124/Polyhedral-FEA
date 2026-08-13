#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate the procedural primitive corpus for the learned mesh advisor (ADR-0027).

Six families x four size regimes = 24 manifold STEP solids, three boundary-condition
load cases each = 72 ``*.case.json`` files, and one ``bench/reference/corpus/*.json``
truth file per case.

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
* Every other case gets a first-order beam surrogate marked ``"source": "provisional"``
  with a 100 % tolerance; ``scripts/advisor/promote_truth.py`` replaces those values with
  the ``advisor-truth-0`` overkill reference answers, once, and never recomputes them.

Run from the repo root::

    python scripts/gen_primitive_corpus.py
    python scripts/gen_primitive_corpus.py --check
    python scripts/gen_primitive_corpus.py --family stepped_shaft
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
    from OCP.BRepPrimAPI import (
        BRepPrimAPI_MakeBox,
        BRepPrimAPI_MakeCylinder,
        BRepPrimAPI_MakeSphere,
    )
    from OCP.gp import gp_Ax2, gp_Dir, gp_Pnt
    from OCP.IFSelect import IFSelect_RetDone
    from OCP.ShapeFix import ShapeFix_Shape
    from OCP.STEPControl import STEPControl_Reader
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
)

MATERIAL = {"E": 2.1e11, "nu": 0.3, "rho": 7850}

#: Traction magnitudes (Pa) per case archetype. Chosen so peak strain stays ~1e-4.
TRACTION_AXIAL = 1.0e6
TRACTION_TRANSVERSE = 1.0e5
TRACTION_OBLIQUE = 2.0e5

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
    """Round to 10 significant digits so emitted JSON is stable and readable."""
    return float(f"{float(value):.10g}")


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


BUILDERS = {
    "box_hole": build_box_hole,
    "l_bracket": build_l_bracket,
    "plate_notch": build_plate_notch,
    "stepped_shaft": build_stepped_shaft,
    "channel": build_channel,
    "sphere_box": build_sphere_box,
    "tube": build_tube,
    "perforated_plate": build_perforated_plate,
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


def oblique_direction(geom: Geometry, rng: random.Random) -> tuple[float, float, float]:
    """Unit vector off every axis (each |component| >= 0.26), axial part positive."""
    raw = [jitter(rng, 0.45, 1.0) for _ in range(3)]
    for i in range(3):
        if i != geom.axis and rng.random() < 0.5:
            raw[i] = -raw[i]
    norm = math.sqrt(sum(v * v for v in raw))
    return tuple(r10(v / norm) for v in raw)  # type: ignore[return-value]


def case_specs(geom: Geometry) -> list[dict[str, Any]]:
    """Three load cases per part: axial, transverse, oblique."""
    rng = random.Random(geom.seed + 1)
    axial = [0.0, 0.0, 0.0]
    axial[geom.axis] = TRACTION_AXIAL
    transverse = [0.0, 0.0, 0.0]
    transverse[geom.transverse] = -TRACTION_TRANSVERSE
    direction = oblique_direction(geom, rng)
    oblique = [r10(TRACTION_OBLIQUE * component) for component in direction]

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
    ]


def case_json(geom: Geometry, spec: dict[str, Any]) -> dict[str, Any]:
    name = f"{geom.name}_c{spec['index']}"
    select: dict[str, Any] = {"box": load_box(geom)}
    if geom.guard_end_area:
        select["expected_area"] = geom.end_area
    select["normal_min_dot"] = spec["normal_min_dot"]
    return {
        "part": name,
        "geometry": f"bench/geometries/corpus/primitives/{geom.name}.step",
        "material": dict(MATERIAL),
        "bcs": [{"select": {"box": fix_box(geom)}, "fix": [True, True, True]}],
        "loads": [{"select": select, "traction": spec["traction"]}],
        "reference": f"bench/reference/corpus/{name}.json",
        "corpus": {
            "family": geom.family,
            "regime": geom.regime,
            "seed": geom.seed,
            "archetype": spec["archetype"],
            "fixed_face": f"{AXIS_NAMES[geom.fix_axis]}_{geom.fix_side}",
            "loaded_face": f"{AXIS_NAMES[geom.axis]}_hi",
            "load_face_boundary": geom.load_face_boundary,
            "generator": "scripts/gen_primitive_corpus.py",
        },
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
    # Radius of gyration of an equivalent solid rectangular section.
    inertia = area * transverse_extent * transverse_extent / 12.0

    axial_t = abs(traction[geom.axis])
    perp_sq = sum(traction[i] ** 2 for i in range(3) if i != geom.axis)
    perp_t = math.sqrt(perp_sq)

    delta_axial = axial_t * span / young
    load_perp = perp_t * area
    delta_perp = load_perp * span ** 3 / (3.0 * young * inertia)
    deflection = math.sqrt(delta_axial ** 2 + delta_perp ** 2)
    energy = 0.5 * (axial_t * area * delta_axial + load_perp * delta_perp)
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


def reference_json(geom: Geometry, spec: dict[str, Any]) -> dict[str, Any]:
    case_name = f"{geom.name}_c{spec['index']}"
    if spec["analytic"] == "kirsch":
        return kirsch_reference(geom, case_name)
    if spec["analytic"] == "stepped_cantilever":
        return stepped_cantilever_reference(geom, case_name, spec["traction"])
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


def truth_campaign(case_paths: list[str]) -> dict[str, Any]:
    n_runs = len(case_paths) * len(TRUTH_H_REL_LADDER)
    return {
        "name": "advisor-truth-0",
        "comment": (
            f"Overkill reference solves for the procedural primitive corpus: "
            f"{len(case_paths)} cases x {len(TRUTH_H_REL_LADDER)} resolution rungs x 1 "
            f"tier = {n_runs} runs. Ground truth for every case without a closed form; "
            "promoted into bench/reference/corpus/*.json by "
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
                case_paths.append(
                    f"bench/geometries/corpus/primitives/{name}.case.json"
                )
                n_cases += 1
    if set(families) == set(FAMILIES) and set(regimes) == set(range(len(REGIME_SCALE))):
        write_json(TRUTH_CAMPAIGN_DIR / "campaign.json", truth_campaign(case_paths))
        print(f"wrote {(TRUTH_CAMPAIGN_DIR / 'campaign.json').relative_to(ROOT)}"
              f"  ({len(case_paths)} parts)")
    else:
        print("partial selection: bench/campaigns/advisor-truth-0/campaign.json "
              "left untouched (regenerate with the full corpus)")
    return n_parts, n_cases, protected_skipped


# ── --check ─────────────────────────────────────────────────────────────────


def _boxes_intersect(box: list[list[float]],
                     lo: tuple[float, float, float],
                     hi: tuple[float, float, float]) -> bool:
    return all(box[0][i] <= hi[i] and box[1][i] >= lo[i] for i in range(3))


ACCEPTED_PROBE_KINDS = frozenset({
    # apps/testlab/main.cpp evaluate_probe(), lines 1281-1318.
    "mean_vm", "mean_von_mises", "face_mean_vm",
    "mean_vm_over_nominal", "scf_mean", "scf",
    "max_von_mises", "max_vm", "max_vm_over_nominal",
    "sigma_p99", "p99_vm",
    "strain_energy", "energy",
    "max_displacement", "tip_deflection",
    "mean_ux_on_face", "mean_uz_on_face",
})


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
        if not any(abs(v) > 0.0 for v in case["loads"][0]["traction"]):
            problems.append(f"{path.name}: zero traction")

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
    else:
        problems.append(f"missing {campaign_path.relative_to(ROOT)}")

    if problems:
        print(f"\n{len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("check: OK")
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
    args = parser.parse_args(argv)

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
