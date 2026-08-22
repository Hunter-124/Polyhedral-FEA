#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""PolyMesh advisor cinema renderer -- records the GUI's cinema surface to video.

Produces every file in ``docs/assets/cinema/``: the full take as h264, a
palette-optimised full-take GIF for inline README use, a poster still, and a
``manifest.json`` recording the exact command that made them.

The frames are not drawn here. They are the GUI's own framebuffer, captured by
the ``record`` verb of ``polymesh-gui --auto``: one PNG per frame at a fixed
1/60 s virtual timestep, so the take is deterministic in frame content and
independent of how fast the machine renders it. This script's whole job is to
drive that, refuse to publish a partial capture, and encode what came out.

Design notes
------------
* **Nothing in the pixels is fabricated.** The advisor panel draws
  ``Advisor::explain()`` -- the deployed ONNX graph's own trunk taps. The
  spectral panel calls the production refinement-plan/FFT path, the cell
  microscope measures captured ``NodalMesh`` snapshots, and the result acts draw
  completed solve fields. This script never computes an activation, spectrum,
  quality value, progress fraction or element count. Manifest numbers are either
  file measurements or tokens printed by the GUI; missing data stays missing.
* **Provenance comes from figstyle**, the same ``git_revision`` and ``digest``
  every generated figure in the repo is stamped with, so the footer burned into
  the video and the JSON beside it agree with the still figures by construction.
  ``POLYMESH_CINEMA_STAMP`` carries it into the GUI, which draws it verbatim.
* **A partial render is an error, never a shorter video.** The frame set is
  verified as exactly N contiguous ``frame_%05d.png`` of nonzero size before
  ffmpeg is invoked; a missing or truncated frame exits nonzero with the index.
* **Frame geometry is measured, not asserted.** ``--size`` sets both the Xvfb
  screen and, through ``POLYMESH_GUI_SIZE``, the GUI window; the encoded
  resolution is read out of the first PNG's IHDR and recorded, so a window that
  did not honour the request is visible in the manifest rather than assumed away.
* **Face ids are GUI smooth-region ids, discovered per part at load time.** The
  complex default uses the ice-cream cone's planar foot (region 1) and connected
  curved exterior (region 0), recorded in ``icecream_cone.case.json``. The run
  clamps the foot and applies a conserved -z 1000 N resultant to the curved
  body. An invalid id fails the take instead of silently recording no load.
* **The GIF is the whole 60 s take, budgeted, and it reports what it cost.**
  GitHub inlines a GIF rather than a repo-relative ``<video>``. The film pays
  for readable holds in duration, then the encoder ladder trades frame rate
  before width until the result fits ``--gif-max-bytes``; content is never
  silently cut. ``--gif-start``/``--gif-duration`` remain explicit overrides.

Usage
-----
    python3 scripts/render_cinema.py --all                # capture + encode
    python3 scripts/render_cinema.py --only mp4 --only gif # re-encode on disk
    python3 scripts/render_cinema.py --list
    python3 scripts/render_cinema.py --frames 900 --size 1920x1080
    python3 scripts/render_cinema.py --all --part box_hole_s0_c0
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[0]))
import figstyle as fs  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
GUI = REPO / "build/apps/gui/polymesh-gui"
MODEL_DIR = REPO / "bench/advisor"
OUT_DIR = REPO / "docs/assets/cinema"
#: Gitignored by the repo-root ``/build*/`` rule, like build/showcase.
FRAMES_DIR = REPO / "build/cinema/frames"

#: The cinema's virtual clock is 1/60 s per frame, so this is both the capture
#: rate and the playback rate: one recorded frame is one displayed frame and the
#: video's duration is the take's own virtual duration.
FPS = 60
#: 3600 frames = 60.0 s of virtual time. The take is paced as a professional
#: presentation rather than a loop: 7.8 s for exact-CAD/spectral sizing, 9.0 s
#: for the advisor (including a 3.15 s final-state hold), 10.8 s for
#: construction/cell inspection, 5.4 s for the mesh hold, and 27.0 s for the
#: two-pass solved-field/refinement sequence. Shorter recordings scale every beat.
DEFAULT_FRAMES = 3600
#: The Xvfb screen AND, via ``POLYMESH_GUI_SIZE``, the GUI window itself: the
#: window otherwise opens at the interactive default it has always had
#: (``kDefaultWindowW``/``H`` = 1600x1000 in ``apps/gui/main.cpp``) whatever the
#: screen is, which shows up as a 1600x1000 film rather than an error. 1080p
#: without paying for a 4K framebuffer.
DEFAULT_SCREEN = (1920, 1080)
#: Complex quadratic solve plus 3600-frame capture. The published h=10 mm
#: graded ice-cream-cone solve has historically completed in about two minutes;
#: 30 minutes leaves headroom for software rendering and encoding.
DEFAULT_TIMEOUT = 1800

# h264 at CRF 18 is visually lossless on flat UI panels and thin lines, which is
# what this take mostly is. yuv420p + faststart because GitHub and every browser
# want both.
CRF = 18
#: h264 encoders in preference order, with the rate control each one actually
#: understands. libx264 is the reference path and the one the committed asset is
#: encoded with; the fallbacks exist because a distribution ffmpeg may ship
#: without it -- Fedora's `ffmpeg-free` has libopenh264 and the NVENC wrappers
#: and no libx264, and silently producing nothing on such a machine is worse
#: than producing a file whose encoder is named in the manifest. CRF is an x264
#: concept, so each fallback gets the nearest thing it has: NVENC's constant
#: quantiser, and openh264's quality-first rate control at a bitrate generous
#: enough for flat UI content at this size.
H264_LADDER: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("libx264", ("-crf", str(CRF), "-preset", "slow")),
    ("h264_nvenc", ("-preset", "p5", "-rc", "vbr", "-cq", str(CRF), "-b:v", "0")),
    ("libopenh264", ("-rc_mode", "quality", "-b:v", "8M",
                     "-maxrate", "12M", "-bufsize", "24M")),
)
#: GIF ladder, tried in order until one lands under the byte budget: width and
#: frame rate only, never content.
#:
#: It starts wider than the 960 px this used to open at, because the film's text
#: is what the ladder is really trading against. The composition sets its
#: headline at 40 px and its numbers at 27 px in a 1080-line frame, so a 1100 px
#: GIF delivers them at 23 and 15 px -- readable at the ~870 CSS px GitHub gives
#: a full-width README image, and readable again on a 2x display. At the old
#: 960/15 the same rows arrived at 20 and 13 px, which was the top of the range
#: where they were still legible; below 720 px the numbers stop being readable at
#: all, which is why the ladder ends there rather than continuing down.
#:
#: Frame rate is spent before width for the same reason: the take is mostly
#: still holds and slow sweeps, so 10 fps costs almost nothing visually while
#: halving the frame count, and a hold compresses to nearly nothing in a GIF
#: whatever the rate.
GIF_LADDER = ((1100, 12), (1100, 10), (960, 10), (860, 10), (720, 10), (720, 8))
GIF_MAX_BYTES = 8 * 1024 * 1024
#: The whole take, unless asked otherwise. The film's payload is spread across
#: all of it -- the mesh completing and being held, stress arriving, the stress
#: gradient, the refinement, the load ramp, the final freeze -- and any slice
#: short enough to be a "loop" drops most of them. This is a change of policy
#: from the act-derived window that used to live here: that window existed
#: because a 20 s take could not be shown whole inside 8 MB, and it showed the
#: deliberation and the start of the fill. It cannot show a result the film did
#: not used to have.
GIF_START_FRACTION = 0.0
GIF_DURATION_FRACTION = 1.0


def rel(path: Path) -> str:
    """Repo-relative string when possible, absolute otherwise."""
    try:
        return str(path.resolve().relative_to(REPO))
    except ValueError:
        return str(path)


# ---------------------------------------------------------------------------
# The case
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class Case:
    """One part plus one load case, spelled as the GUI's own --auto verbs.

    ``fix_face`` and ``load_face`` are **GUI** face ids -- the ids the viewport
    assigns when it loads the part -- not the case JSON's selection boxes. They
    are recorded here because they were measured (see the module docstring), and
    they stay overridable from the command line because a change to face
    discovery would renumber them.
    """

    name: str
    step: str
    case_json: str
    h_mm: float
    fix_face: int
    load_face: int
    load: tuple[float, float, float]
    why: str
    load_note: str
    youngs_gpa: float = 200.0
    poisson: float = 0.3
    # None preserves an accepted advisor action. The default OOD hero explicitly
    # requests one measured pass after the advisor has abstained.
    adapt_passes: int | None = None
    eta_target: float = 0.0


CASES: dict[str, Case] = {
    # The hero case is deliberately more demanding than the advisor corpus:
    # a watertight Boolean union of a truncated cone and an intersecting sphere,
    # with a sharp circular join, a curved scoop and a small planar foot. The
    # shipped OOD gate refuses it instead of extrapolating; that refusal is part
    # of the film, then the configured product fallback runs unchanged.
    #
    # h=12 mm / graded / quadratic begins at 30,496 cells. The configured real
    # adaptive pass finishes at 35,951 tet10 cells with measured shape-quality
    # minimum 0.03378 (above the 0.02 ship floor) and preserves 27,808 cells
    # while replacing only the changed topology. Spectral sizing is explicitly
    # enabled and its real report is printed into the cinema manifest.
    "icecream_cone": Case(
        name="icecream_cone",
        step="tests/fixtures/parts/icecream_cone.step",
        case_json="docs/assets/cinema/icecream_cone.case.json",
        h_mm=12.0,
        fix_face=1,
        load_face=0,
        load=(0.0, 0.0, -1000.0),
        why="complex cone+sphere Boolean with exact curved CAD, FFT sizing, "
            "quadratic geometry, positive quality margin and a measured OOD "
            "refusal before the verified fallback",
        load_note="GUI region 1 (planar foot) fixed; conserved -z 1000 N "
                  "resultant on GUI region 0 (connected cone+scoop exterior)",
        youngs_gpa=200.0,
        poisson=0.3,
        adapt_passes=1,
        eta_target=0.0,
    ),
    # Earlier advised take, retained as `--part sphere_box_s0_c0`. Of the 44
    # primitives in the corpus only 23 are advised at all. Of those, sphere_box_s0
    # gives by far the most elements because its curved wall drives curvature and
    # feature grading: 11,692 elements and 13,146 DOF.
    #
    # It advises hybrid_zoo at h_rel 0.08, order 1, one adapt pass and eta 0.02,
    # at Mahalanobis distance 3.34 against the 5.034 operating point.
    "sphere_box_s0_c0": Case(
        name="sphere_box_s0_c0",
        step="bench/geometries/corpus/primitives/sphere_box_s0.step",
        case_json="bench/geometries/corpus/primitives/sphere_box_s0_c0.case.json",
        # FALLBACK ONLY, and not the mesh size the video shows: when the advisor
        # advises it sets h itself from h_rel. 6.888 mm is 0.08 of this part's
        # 86.10 mm bbox diagonal (x [0, 0.0729] m, y and z [-0.0162, 0.0162] m),
        # i.e. the same size the advisor's own h_rel resolves to here, so a
        # refusal falls back to a comparable mesh rather than to a different film.
        h_mm=6.888,
        fix_face=0,
        load_face=5,
        # 1e6 Pa along +x over the x_hi face, and that face's area is a
        # measurement rather than an authored number: this case's loaded face is
        # curved (`"load_face_boundary": "curved_surface"`) and
        # scripts/gen_primitive_corpus.py emits `select.expected_area` only for
        # planar ends, so there is none in the case JSON to read. The CLI
        # reported the exact CAD area of that face as 0.000528197958 m2, which at
        # 1e6 Pa is a resultant of 528.197958 N; the `loadface` verb takes
        # newtons, so that is the faithful translation of the case.
        load=(528.197958, 0.0, 0.0),
        why="the advisor advises it (ood 3.34, not vetoed) and of the 23 advised "
            "primitives it gives the most elements: 11,692 elements, 13,146 DOF",
        load_note="1e6 Pa over the exact CAD area 0.000528197958 m2 of the "
                  "curved x_hi face = 528.197958 N",
    ),
    # Retained, and still selectable with `--part box_hole_s0_c0`. It was the
    # film's original case for one reason that has not expired: it is the exact
    # input the retired `activation_map.png` was computed on (its title read
    # "run 30 on box_hole_s0_c0 - cfg-116b3958"), so the video supersedes that
    # figure on the figure's own input rather than on a case chosen to flatter
    # the replacement. Measured with `polymesh solve <part> --advisor
    # bench/advisor`, box_hole_s0 scores an out-of-distribution distance of 3.57
    # against the shipped 5.034 operating point and is advised (hybrid_zoo,
    # h_rel 0.2, order 2, 0 adapt passes, failure_prob 5.3e-06).
    #
    # It is 568 elements, which is why it is no longer the default: the fill lane
    # runs out of geometry to build long before the pass lane runs out of
    # candidates to score.
    "box_hole_s0_c0": Case(
        name="box_hole_s0_c0",
        step="bench/geometries/corpus/primitives/box_hole_s0.step",
        case_json="bench/geometries/corpus/primitives/box_hole_s0_c0.case.json",
        # FALLBACK ONLY, and not the mesh size the video shows: when the advisor
        # advises it sets h itself from h_rel (0.2 of the bbox diagonal, 8.02 mm
        # on this part). This value governs only the case where the advisor
        # refuses and the GUI falls back to the study's own setting.
        h_mm=8.0,
        fix_face=0,
        load_face=5,
        # The case JSON specifies a 1e6 Pa traction over an `expected_area` of
        # 8.925720996e-05 m2 on the x_hi face. The GUI's `loadface` verb takes
        # newtons, so the faithful translation of that case is the equivalent
        # resultant, 89.257 N -- not a round number chosen for the caption.
        load=(89.257, 0.0, 0.0),
        why="the advisor advises it (ood 3.57, not vetoed); the case the "
            "retired activation_map.png was computed on",
        load_note="1e6 Pa over 8.925720996e-05 m2 of the x_hi face = 89.257 N",
    ),
}
DEFAULT_CASE = "icecream_cone"


# ---------------------------------------------------------------------------
# Provenance
# ---------------------------------------------------------------------------
def model_onnx(model_dir: Path) -> Path:
    return model_dir / "model.onnx"


def stamp_text(model_dir: Path) -> str:
    """The footer line the GUI draws verbatim, in figstyle's stamp grammar.

    Same two facts every generated figure in the repo carries -- the revision of
    the code and the digest of the data -- so a frame grabbed out of the video
    can be matched against a still figure without guessing.
    """
    onnx = model_onnx(model_dir)
    sha, _ = fs.digest(onnx)
    if not sha:
        raise SystemExit(f"no model to stamp: {rel(onnx)} does not exist")
    return f"git {fs.git_revision()} · model.onnx sha256 {sha[:fs.DIGEST_CHARS]}"


# ---------------------------------------------------------------------------
# Driving the GUI
# ---------------------------------------------------------------------------
def auto_spec(case: Case, part: Path, model_dir: Path, frames_dir: Path,
              frames: int) -> str:
    """The --auto script, in the order the cinema needs it run.

    ``cinema on`` before ``cinema advisor`` so the layout exists when the
    explanation lands in it, and ``solve`` after both so the mesh-stage sink is
    already installed when the fill runs -- the stages are observed as they are
    built and cannot be recovered afterwards.
    """
    fx, fy, fz = case.load
    actions = [
        f"load {rel(part)}",
        f"h {case.h_mm:g}",
        f"material {case.youngs_gpa:.9g} {case.poisson:.9g}",
        "spectral on",
        f"fix {case.fix_face}",
        # `.9g`, not `g`: preserve measured resultants instead of rounding the
        # physical case for display.
        f"loadface {case.load_face} {fx:.9g} {fy:.9g} {fz:.9g}",
        "cinema on",
        f"cinema advisor {rel(model_dir)}",
    ]
    if case.adapt_passes is not None:
        # Deliberately after `cinema advisor`: an accepted action normally owns
        # adapt settings, while this configured OOD fallback owns its real pass.
        actions.append(f"adapt {case.adapt_passes} {case.eta_target:.9g}")
    actions.extend([
        "wire off",
        "solve",
        f"record {frames_dir} {frames}",
        "quit",
    ])
    return "; ".join(actions)


def gui_argv(gui: Path, spec: str, screen: tuple[int, int]) -> list[str]:
    w, h = screen
    return [
        "xvfb-run", "-a", "-s", f"-screen 0 {w}x{h}x24",
        str(gui), "--auto", spec,
    ]


def _shquote(arg: str) -> str:
    if re.fullmatch(r"[\w./=+-]+", arg):
        return arg
    return "'" + arg.replace("'", "'\\''") + "'"


def run_gui(argv: list[str], env_stamp: str, timeout: int,
            window: tuple[int, int]) -> tuple[str, float]:
    """Run the GUI, streaming its output. Returns (stdout, wall seconds).

    Streamed rather than collected at exit because the capture is minutes long
    and its only progress signal is what the GUI prints.

    `POLYMESH_GUI_SIZE` sizes the window itself. Without it the GUI opens at its
    interactive 1600x1000 whatever the Xvfb screen is, so a 1920x1080 screen
    would record a 1600x1000 film in a 1080p-shaped void; the encoded size is
    still measured from the frames rather than assumed from either number.
    """
    size = f"{window[0]}x{window[1]}"
    print("    $ POLYMESH_CINEMA_STAMP=" + _shquote(env_stamp)
          + f" POLYMESH_GUI_SIZE={size} \\\n      "
          + " ".join(_shquote(a) for a in argv), flush=True)
    env = dict(os.environ, POLYMESH_CINEMA_STAMP=env_stamp,
               POLYMESH_GUI_SIZE=size)
    started = time.monotonic()
    proc = subprocess.Popen(argv, cwd=REPO, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    lines: list[str] = []
    assert proc.stdout is not None
    try:
        for line in proc.stdout:
            lines.append(line)
            print("      " + line.rstrip(), flush=True)
        code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        raise SystemExit(f"polymesh-gui exceeded {timeout} s; capture abandoned")
    wall = time.monotonic() - started
    if code != 0:
        raise SystemExit(f"polymesh-gui exited {code} after {wall:.1f} s; "
                         "no frames will be encoded")
    return "".join(lines), wall


# ---------------------------------------------------------------------------
# What the GUI reported
# ---------------------------------------------------------------------------
#: The five lines below are the GUI's stdout contract for a take, agreed with
#: the cinema panel and matched token for token. Extra fields are tolerated so a
#: future field does not break this script, and unknown keys are dropped rather
#: than guessed at.
#:
#: The take announcement, printed when recording starts. Deliberately a
#: different verb from ``record`` so it cannot collide with the summary parse.
_TAKE_RE = re.compile(r"^cinema:\s+take\b(?P<rest>.*)$")
#: The act table, one line per act: name, frame span, virtual time span.
_ACT_RE = re.compile(
    r"^cinema:\s+act\s+(?P<name>\S+)\s+frames\s+(?P<f0>\d+)\.\.(?P<f1>\d+)"
    r"\s+t\s+(?P<t0>[\d.]+)\.\.(?P<t1>[\d.]+)")
#: The record summary, parsed as loose ``key value`` pairs plus one word-valued
#: field, ``solver``.
_SUMMARY_RE = re.compile(r"^cinema:\s+record\b(?P<rest>.*)$")
#: Which linear solver the take's solves actually ran through. Word-valued, so
#: ``_PAIR_RE`` cannot see it: the method is a choice `fea::SolveMethod::kAuto`
#: makes from the free DOF count, and the film states it rather than letting a
#: reader assume iterations were animated on a case that was factorised.
_SOLVER_RE = re.compile(r"\bsolver\s+(?P<solver>\S+)")
#: The advisor line, same pair grammar plus one word-valued field.
_ADVISOR_RE = re.compile(r"^cinema:\s+advisor\s+(?!unavailable\b)(?P<rest>.*)$")
_DECISION_RE = re.compile(r"\bdecision\s+(?P<decision>\S+)")
#: Spectral refinement-plan report, computed by the same pipeline function the
#: solve uses and printed before the worker starts.
_SPECTRAL_RE = re.compile(r"^cinema:\s+spectral\b(?P<rest>.*)$")
#: No trunk taps, no model, or the advisor would not construct: there are no
#: activations to draw and the panel says so on screen.
_UNAVAILABLE_RE = re.compile(
    r"^cinema:\s+advisor\s+unavailable\s+(?P<rest>.*)$")
#: Per-frame progress. Streamed to the console as it arrives, but kept out of
#: the manifest -- progress chatter is not provenance.
_PROGRESS_RE = re.compile(r"^cinema:\s+frame\s+\d+/\d+\b")

_PAIR_RE = re.compile(r"(?P<key>[a-z_]+)\s+(?P<value>-?[\d.eE+-]*[\d.])")

#: Keys lifted out of the record summary into the manifest. ``stages`` counts the
#: mesher's construction stages and ``solve_stages`` the completed adaptive
#: passes: two different real sequences, so two different keys. ``skipped`` is the
#: elements the viewport could not triangulate, kept because an element the film
#: did not draw is exactly the kind of number a reader should not have to take on
#: trust.
_SUMMARY_KEYS = ("frames", "fps", "candidates", "stages", "solve_stages",
                 "elements", "nodes", "dof", "quality_min", "quality_mean",
                 "youngs_pa", "poisson", "max_von_mises_pa", "global_eta",
                 "max_displacement_m", "deform_scale", "visible_displacement_m",
                 "visible_fraction", "unchanged", "removed", "added", "skipped", "poster",
                 "width", "height")
#: Keys lifted out of the advisor line. ``candidates`` is the enumerated grid
#: without the final re-score pass; ``frames`` counts every forward pass
#: including it, so both are kept and named apart.
_ADVISOR_KEYS = ("candidates", "frames", "gate_threshold", "h_rel", "order",
                 "adapt_passes", "eta_target", "vetoed", "ood_distance",
                 "applied")
_SPECTRAL_KEYS = ("applied", "modes_total", "modes_kept", "energy_kept",
                  "edge_seeds", "predicted_before", "predicted_after",
                  "geometry_seeds", "bc_seeds", "brep_curvature",
                  "field_samples", "h_before_min", "h_before_max",
                  "h_after_min", "h_after_max")
#: Keys lifted out of the take announcement.
_TAKE_KEYS = ("frames", "fps", "duration")


@dataclass
class GuiReport:
    """What the GUI said about the take. Absent fields stay None, never 0."""
    acts: list[dict]
    counts: dict[str, float]
    advisor: dict[str, float]
    spectral: dict[str, float]
    take: dict[str, float]
    decision: str | None
    solver: str | None
    unavailable: str | None
    raw: list[str]

    @property
    def poster_frame(self) -> int | None:
        value = self.counts.get("poster")
        return int(value) if value is not None else None

    @property
    def reported_size(self) -> tuple[int, int] | None:
        width, height = self.counts.get("width"), self.counts.get("height")
        if width is None or height is None:
            return None
        return int(width), int(height)

    def act(self, name: str) -> dict | None:
        return next((a for a in self.acts if a["act"] == name), None)


def _pairs(rest: str, keys: tuple[str, ...]) -> dict[str, float]:
    out: dict[str, float] = {}
    for pair in _PAIR_RE.finditer(rest):
        if pair["key"] in keys:
            try:
                out[pair["key"]] = float(pair["value"])
            except ValueError:
                continue
    return out


def parse_report(stdout: str) -> GuiReport:
    acts: list[dict] = []
    counts: dict[str, float] = {}
    advisor: dict[str, float] = {}
    spectral: dict[str, float] = {}
    take: dict[str, float] = {}
    decision: str | None = None
    solver: str | None = None
    unavailable: str | None = None
    raw: list[str] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line.startswith("cinema:") or _PROGRESS_RE.match(line):
            continue
        raw.append(line)
        gone = _UNAVAILABLE_RE.match(line)
        if gone:
            unavailable = gone["rest"].strip()
            continue
        started = _TAKE_RE.match(line)
        if started:
            take.update(_pairs(started["rest"], _TAKE_KEYS))
            continue
        act = _ACT_RE.match(line)
        if act:
            t0, t1 = float(act["t0"]), float(act["t1"])
            acts.append({
                "act": act["name"],
                "first_frame": int(act["f0"]),
                "last_frame": int(act["f1"]),
                "start_s": t0,
                "end_s": t1,
                "duration_s": round(t1 - t0, 6),
            })
            continue
        summary = _SUMMARY_RE.match(line)
        if summary:
            counts.update(_pairs(summary["rest"], _SUMMARY_KEYS))
            named = _SOLVER_RE.search(summary["rest"])
            if named:
                solver = named["solver"]
            continue
        sized = _SPECTRAL_RE.match(line)
        if sized:
            spectral.update(_pairs(sized["rest"], _SPECTRAL_KEYS))
            continue
        advised = _ADVISOR_RE.match(line)
        if advised:
            advisor.update(_pairs(advised["rest"], _ADVISOR_KEYS))
            found = _DECISION_RE.search(advised["rest"])
            if found:
                decision = found["decision"]
    return GuiReport(acts=acts, counts=counts, advisor=advisor, spectral=spectral,
                     take=take, decision=decision, solver=solver,
                     unavailable=unavailable, raw=raw)


# ---------------------------------------------------------------------------
# Frame verification
# ---------------------------------------------------------------------------
def png_size(path: Path) -> tuple[int, int]:
    """(width, height) from a PNG's IHDR. No image library, no decode."""
    with path.open("rb") as stream:
        head = stream.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{rel(path)} is not a PNG; the capture is unusable")
    return (int.from_bytes(head[16:20], "big"),
            int.from_bytes(head[20:24], "big"))


def frame_path(frames_dir: Path, index: int) -> Path:
    return frames_dir / f"frame_{index:05d}.png"


def verify_frames(frames_dir: Path, expected: int) -> tuple[int, int]:
    """Exactly `expected` contiguous nonzero frames, or exit nonzero.

    A capture that stopped early, a frame the GUI failed to write, a frame it
    opened but never filled, and frames left behind by a longer earlier take are
    four different defects with one symptom: a video that is not the take. All
    four are named here rather than silently encoded.
    """
    present = sorted(frames_dir.glob("frame_*.png"))
    if not present:
        raise SystemExit(f"no frames in {rel(frames_dir)}; expected {expected}")
    missing = [i for i in range(expected)
               if not frame_path(frames_dir, i).is_file()]
    if missing:
        shown = ", ".join(str(i) for i in missing[:8])
        more = f" (+{len(missing) - 8} more)" if len(missing) > 8 else ""
        raise SystemExit(
            f"{len(missing)} of {expected} frames missing from "
            f"{rel(frames_dir)}: {shown}{more}")
    wanted = {frame_path(frames_dir, i) for i in range(expected)}
    extra = sorted(p.name for p in present if p not in wanted)
    if extra:
        raise SystemExit(
            f"{len(extra)} file(s) beyond the expected {expected} frames in "
            f"{rel(frames_dir)} (first: {extra[0]}); frames from an earlier "
            "take would be encoded into this one")
    empty = [i for i in range(expected)
             if frame_path(frames_dir, i).stat().st_size == 0]
    if empty:
        raise SystemExit(
            f"{len(empty)} zero-byte frame(s) in {rel(frames_dir)}, first at "
            f"index {empty[0]}; the capture is incomplete")
    size = png_size(frame_path(frames_dir, 0))
    if size[0] % 2 or size[1] % 2:
        raise SystemExit(
            f"frame size {size[0]}x{size[1]} has an odd dimension; yuv420p "
            "cannot encode it and scaling would resample every UI hairline")
    last = png_size(frame_path(frames_dir, expected - 1))
    if last != size:
        raise SystemExit(
            f"frame {expected - 1} is {last[0]}x{last[1]}, not "
            f"{size[0]}x{size[1]}; the window was resized mid-capture")
    total = sum(frame_path(frames_dir, i).stat().st_size for i in range(expected))
    print(f"    verified {expected} frames at {size[0]}x{size[1]}, "
          f"{total / 1e6:.1f} MB on disk")
    return size


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------
def ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if exe is None:
        raise SystemExit("ffmpeg is not on PATH; cannot encode the take")
    return exe


def ffmpeg_version() -> str:
    out = subprocess.run([ffmpeg(), "-version"], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    return out[0].strip() if out else "unknown"


def ffmpeg_encoders() -> set[str]:
    """Encoder names this ffmpeg build actually has, from ``-encoders``."""
    out = subprocess.run([ffmpeg(), "-hide_banner", "-encoders"],
                         capture_output=True, text=True, check=True).stdout
    return set(re.findall(r"^\s*[VAS][\w.]*\s+(\S+)", out, re.MULTILINE))


def pick_encoder(requested: str | None) -> tuple[str, tuple[str, ...]]:
    """The h264 encoder to use, and its rate-control arguments.

    An explicit ``--encoder`` is honoured if the build has it and refused if it
    does not, rather than quietly falling back to something else and putting a
    different codec behind the same filename.
    """
    available = ffmpeg_encoders()
    if requested:
        for name, rate in H264_LADDER:
            if name == requested:
                if name not in available:
                    raise SystemExit(
                        f"this ffmpeg has no '{name}' encoder; it offers "
                        + ", ".join(n for n, _ in H264_LADDER if n in available))
                return name, rate
        raise SystemExit(
            f"unknown encoder '{requested}'; known: "
            + ", ".join(n for n, _ in H264_LADDER))
    for name, rate in H264_LADDER:
        if name in available:
            return name, rate
    raise SystemExit(
        "this ffmpeg build has none of "
        + ", ".join(n for n, _ in H264_LADDER)
        + "; install one (Fedora: ffmpeg from RPM Fusion carries libx264)")


def run_ffmpeg(args: list[str]) -> None:
    argv = [ffmpeg(), "-hide_banner", "-loglevel", "error", "-y", *args]
    print("    $ " + " ".join(_shquote(a) for a in argv), flush=True)
    proc = subprocess.run(argv, cwd=REPO, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"ffmpeg exited {proc.returncode}:\n{proc.stderr.strip()}")
    if proc.stderr.strip():
        print("      " + proc.stderr.strip())


def encode_mp4(frames_dir: Path, out: Path, fps: int,
               encoder: tuple[str, tuple[str, ...]]) -> dict:
    name, rate = encoder
    if name != "libx264":
        print(f"    note: encoding with {name}; the committed asset's reference "
              "path is libx264 at CRF "
              f"{CRF}, and the manifest records which was used")
    run_ffmpeg([
        "-framerate", str(fps),
        "-start_number", "0",
        "-i", str(frames_dir / "frame_%05d.png"),
        "-c:v", name,
        *rate,
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        str(out),
    ])
    sha, _ = fs.digest(out)
    info = {"file": out.name, "codec": "h264", "encoder": name,
            "rate_control": list(rate), "pix_fmt": "yuv420p",
            "faststart": True, "fps": fps,
            "bytes": out.stat().st_size, "sha256": sha}
    print(f"    wrote {rel(out)}  ({info['bytes'] / 1e6:.1f} MB, {name})")
    return info


def gif_window(report: GuiReport, total_s: float, start_arg: float | None,
               duration_arg: float | None) -> tuple[float, float, str]:
    """Which slice of the take the inline GIF shows, and where that came from.

    The default is all of it. The act-table rules that used to live here picked
    a four-second window around the deliberation, and they were the right answer
    to a different film: one where the payload was the network deciding and the
    fill starting, and where showing the whole 20 s inside 8 MB was not
    affordable. This film's payload is the sequence -- the mesh finishing and
    being held, the stress arriving, the gradient of it, the refinement, the
    ramp, the final freeze -- and no window narrow enough to be a loop contains
    more than one of those. The byte budget is met by the ladder instead, in
    width and frame rate, which costs sharpness rather than content.

    ``--gif-start`` / ``--gif-duration`` still cut a slice, and the source string
    records which rule produced the window so the manifest never carries two
    unexplained numbers.
    """
    start = GIF_START_FRACTION * total_s
    end = start + GIF_DURATION_FRACTION * total_s
    acts = ", ".join(a["act"] for a in report.acts) if report.acts else "none reported"
    source = (f"the whole {total_s:g} s take (acts: {acts}); the GIF shows every "
              f"beat and pays for it in the ladder rather than in content")
    if start_arg is not None:
        start, source = start_arg, "--gif-start"
        end = start + (duration_arg if duration_arg is not None
                       else GIF_DURATION_FRACTION * total_s)
    if duration_arg is not None:
        end = start + duration_arg
        source = "--gif-duration" if start_arg is None else "--gif-start/--gif-duration"
    end = min(end, total_s)
    if end <= start:
        raise SystemExit(
            f"the GIF window {start:g}..{end:g} s is empty within a "
            f"{total_s:g} s take")
    return start, end - start, source


def encode_gif(frames_dir: Path, out: Path, *, fps: int, start_s: float,
               duration_s: float, max_bytes: int) -> dict:
    """Palette-optimised GIF of a time subset, first ladder rung under budget."""
    attempts: list[dict] = []
    palette = out.with_suffix(".palette.png")
    for width, gif_fps in GIF_LADDER:
        chain = f"fps={gif_fps},scale={width}:-1:flags=lanczos"
        run_ffmpeg([
            "-framerate", str(fps), "-start_number", "0",
            "-ss", f"{start_s:g}", "-t", f"{duration_s:g}",
            "-i", str(frames_dir / "frame_%05d.png"),
            "-vf", f"{chain},palettegen=stats_mode=diff",
            str(palette),
        ])
        run_ffmpeg([
            "-framerate", str(fps), "-start_number", "0",
            "-ss", f"{start_s:g}", "-t", f"{duration_s:g}",
            "-i", str(frames_dir / "frame_%05d.png"),
            "-i", str(palette),
            "-lavfi", f"{chain}[x];[x][1:v]paletteuse=dither=sierra2_4a",
            "-loop", "0",
            str(out),
        ])
        size = out.stat().st_size
        attempts.append({"width": width, "fps": gif_fps, "bytes": size})
        print(f"    {rel(out)} at {width} px {gif_fps} fps: {size / 1e6:.2f} MB "
              f"(budget {max_bytes / 1e6:.1f} MB)")
        if size <= max_bytes:
            palette.unlink(missing_ok=True)
            sha, _ = fs.digest(out)
            return {"file": out.name, "width": width, "fps": gif_fps,
                    "start_s": start_s, "duration_s": duration_s,
                    "bytes": size, "sha256": sha, "attempts": attempts,
                    "max_bytes": max_bytes}
    palette.unlink(missing_ok=True)
    raise SystemExit(
        "no GIF setting reached the byte budget: "
        + ", ".join(f"{a['width']}px/{a['fps']}fps={a['bytes'] / 1e6:.2f}MB"
                    for a in attempts)
        + f"; all exceed {max_bytes / 1e6:.1f} MB. Shorten --gif-duration or "
          "raise --gif-max-bytes rather than shipping an oversized inline loop.")


def write_poster(frames_dir: Path, out: Path, index: int) -> dict:
    """Re-encode one captured frame as a compressed PNG.

    Not a copy. The GUI's in-process PNG writer emits stored (uncompressed)
    DEFLATE blocks, which is the right trade for a capture path that must not
    stall the render loop but makes every frame exactly width*height*4 bytes --
    8.3 MB at 1080p. Re-encoding the same pixels losslessly at ffmpeg's highest
    PNG compression measured 65 kB, a 127x saving on a committed asset for no
    change to a single pixel.
    """
    src = frame_path(frames_dir, index)
    if not src.is_file():
        raise SystemExit(f"poster frame {index} ({rel(src)}) does not exist")
    run_ffmpeg(["-i", str(src), "-compression_level", "100", str(out)])
    sha, _ = fs.digest(out)
    width, height = png_size(out)
    src_bytes = src.stat().st_size
    out_bytes = out.stat().st_size
    if (width, height) != png_size(src):
        raise SystemExit(f"poster re-encode changed geometry: {rel(src)} is "
                         f"{png_size(src)}, {rel(out)} is {(width, height)}")
    print(f"    wrote {rel(out)}  (frame {index}, {width}x{height}, "
          f"{out_bytes / 1e3:.0f} kB from {src_bytes / 1e6:.1f} MB uncompressed)")
    return {"file": out.name, "source_frame": index, "width": width,
            "height": height, "bytes": out_bytes,
            "source_bytes": src_bytes, "sha256": sha}


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
STAGES = ("frames", "mp4", "gif", "poster")
STAGE_HELP = {
    "frames": "drive the GUI under xvfb-run and capture build/cinema/frames",
    "mp4": "encode advisor_cinema.mp4 (h264, yuv420p, +faststart)",
    "gif": "encode advisor_cinema.gif (palette-optimised inline loop)",
    "poster": "re-encode the first fully-composed frame as poster.png",
}


def _as_int(value: float | None) -> int | None:
    return int(value) if value is not None else None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Render the PolyMesh advisor activation cinema.")
    ap.add_argument("--all", action="store_true",
                    help="capture and encode everything")
    ap.add_argument("--only", action="append", default=[],
                    help=f"stage to run; repeatable ({', '.join(STAGES)})")
    ap.add_argument("--list", action="store_true",
                    help="print the stage and case tables and exit")
    ap.add_argument("--part", default=DEFAULT_CASE,
                    help=f"case to record (default: {DEFAULT_CASE})")
    ap.add_argument("--step", type=Path, default=None,
                    help="override the STEP file of the selected case")
    ap.add_argument("--fix-face", type=int, default=None,
                    help="GUI face id to clamp; default is the case's measured "
                         "id, which a change to face discovery would renumber")
    ap.add_argument("--load-face", type=int, default=None,
                    help="GUI face id to load; default is the case's measured id")
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES,
                    help=f"frames to record at 1/{FPS} s each "
                         f"(default: {DEFAULT_FRAMES} = "
                         f"{DEFAULT_FRAMES / FPS:g} s)")
    ap.add_argument("--gui", type=Path, default=GUI,
                    help="polymesh-gui binary "
                         "(default: build/apps/gui/polymesh-gui)")
    ap.add_argument("--model-dir", type=Path, default=MODEL_DIR,
                    help="advisor model directory (default: bench/advisor)")
    ap.add_argument("--out-dir", type=Path, default=OUT_DIR,
                    help="output directory (default: docs/assets/cinema)")
    ap.add_argument("--frames-dir", type=Path, default=FRAMES_DIR,
                    help="frame scratch directory (default: build/cinema/frames)")
    ap.add_argument("--size", default="{}x{}".format(*DEFAULT_SCREEN),
                    help="Xvfb screen and GUI window geometry, WxH (default: "
                         "{}x{}); the window is sized with POLYMESH_GUI_SIZE and "
                         "the encoded size is measured from the frames rather "
                         "than assumed".format(*DEFAULT_SCREEN))
    ap.add_argument("--poster-frame", type=int, default=None,
                    help="frame index for poster.png; default is the index the "
                         "GUI reports as the first fully-composed frame, or 0")
    ap.add_argument("--gif-start", type=float, default=None,
                    help="GIF subset start in seconds; default is derived from "
                         "the act table the GUI reported -- centred on its "
                         "longest act, or straddling the advisor/mesh cut when "
                         "the take still has one")
    ap.add_argument("--gif-duration", type=float, default=None,
                    help=f"GIF subset length in seconds; default is "
                         f"{GIF_DURATION_FRACTION:g} of the take, clamped to the "
                         f"act it is centred on")
    ap.add_argument("--gif-max-bytes", type=int, default=GIF_MAX_BYTES,
                    help=f"inline GIF byte budget (default: {GIF_MAX_BYTES})")
    ap.add_argument("--encoder", default=None,
                    help="h264 encoder; default is the first this ffmpeg has "
                         "of " + ", ".join(n for n, _ in H264_LADDER))
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                    help="GUI wall-clock limit in seconds "
                         f"(default: {DEFAULT_TIMEOUT})")
    ap.add_argument("--allow-no-activations", action="store_true",
                    help="publish even if the GUI reports the advisor has no "
                         "trunk taps, so the network panel has no real data")
    args = ap.parse_args(argv)

    if args.list:
        print("stages (--only NAME):")
        for stage in STAGES:
            print(f"  {stage:8s} {STAGE_HELP[stage]}")
        print("\ncases (--part NAME):")
        for case in CASES.values():
            fx, fy, fz = case.load
            mark = "  (default)" if case.name == DEFAULT_CASE else ""
            print(f"  {case.name}{mark}")
            print(f"    part      {case.step}")
            print(f"    case      {case.case_json}")
            adapt = (f"; adapt {case.adapt_passes} {case.eta_target:.9g}"
                     if case.adapt_passes is not None else "")
            print(f"    verbs     h {case.h_mm:g}; "
                  f"material {case.youngs_gpa:.9g} {case.poisson:.9g}{adapt}; "
                  f"fix {case.fix_face}; "
                  f"loadface {case.load_face} {fx:.9g} {fy:.9g} {fz:.9g}")
            print(f"    load      {case.load_note}")
            print(f"    h         configured before inference; accepted advice "
                  f"overrides it, refusal keeps it")
            print(f"    why       {case.why}")
        print(f"\noutputs land in {rel(OUT_DIR)}; frames in "
              f"{rel(FRAMES_DIR)} (gitignored by the repo-root /build*/ rule)")
        return 0

    unknown = [name for name in args.only if name not in STAGES]
    if unknown:
        ap.error(f"unknown stage(s): {', '.join(unknown)}; "
                 f"expected {', '.join(STAGES)}")
    if not args.only and not args.all:
        ap.error("pass --all, --only STAGE, or --list")
    wanted = set(STAGES) if args.all else set(args.only)

    base = CASES.get(args.part)
    if base is None:
        ap.error(f"unknown part '{args.part}'; known: {', '.join(CASES)}")
    case = Case(
        name=base.name,
        step=str(args.step) if args.step else base.step,
        case_json=base.case_json,
        h_mm=base.h_mm,
        fix_face=base.fix_face if args.fix_face is None else args.fix_face,
        load_face=base.load_face if args.load_face is None else args.load_face,
        load=base.load,
        why=base.why,
        load_note=base.load_note,
        youngs_gpa=base.youngs_gpa,
        poisson=base.poisson,
        adapt_passes=base.adapt_passes,
        eta_target=base.eta_target,
    )
    match = re.fullmatch(r"(\d+)x(\d+)", args.size)
    if not match:
        ap.error(f"--size wants WxH, got '{args.size}'")
    screen = (int(match[1]), int(match[2]))

    part = Path(case.step)
    if not part.is_absolute():
        part = REPO / part
    out_dir: Path = args.out_dir
    frames_dir: Path = args.frames_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    frames_dir.mkdir(parents=True, exist_ok=True)

    stamp = stamp_text(args.model_dir)
    print(f"stamp: {stamp}")

    spec = auto_spec(case, part, args.model_dir, frames_dir.resolve(), args.frames)
    argv_gui = gui_argv(args.gui, spec, screen)
    report = GuiReport(acts=[], counts={}, advisor={}, spectral={}, take={},
                       decision=None, solver=None, unavailable=None, raw=[])
    wall = None

    # ---- capture ------------------------------------------------------------
    if "frames" in wanted:
        if not args.gui.is_file():
            raise SystemExit(
                f"{rel(args.gui)} does not exist; build the GUI first "
                "(cmake --build build --target polymesh-gui)")
        if not part.is_file():
            # The primitive corpus is generated, not committed (.gitignore keeps
            # /bench/geometries/corpus/ out of the tree), so a missing part is a
            # missing generation step rather than a broken checkout.
            raise SystemExit(
                f"{rel(part)} does not exist; generate the primitive corpus "
                "first (python scripts/gen_primitive_corpus.py)")
        if shutil.which("xvfb-run") is None:
            raise SystemExit("xvfb-run is not installed; cannot drive the GUI "
                             "headlessly")
        stale = sorted(frames_dir.glob("frame_*.png"))
        if stale:
            print(f"[frames] clearing {len(stale)} frame(s) from an earlier take")
            for path in stale:
                path.unlink()
        print(f"[frames] {case.name}, {args.frames} frames at {FPS} fps "
              f"({args.frames / FPS:g} s), Xvfb screen "
              f"{screen[0]}x{screen[1]}")
        stdout, wall = run_gui(argv_gui, stamp, args.timeout, screen)
        report = parse_report(stdout)
        print(f"    GUI wall time {wall:.1f} s")
        if report.unavailable is not None and not args.allow_no_activations:
            raise SystemExit(
                f"the GUI reports the advisor is unavailable ({report.unavailable}), "
                "so the network panel has no real activations to draw. Re-export "
                "the model with the trunk taps (python "
                "scripts/advisor/export_onnx.py) or pass --allow-no-activations "
                "to publish the surface as it is.")
        if report.advisor.get("vetoed"):
            print("    note: the advisor vetoed this part as out of "
                  "distribution, so the panel draws a refusal rather than an "
                  "explanation")
        if report.solver is not None:
            print(f"    solver {report.solver} (as the GUI reported it; the "
                  "method is chosen from the free DOF count, not requested "
                  "here)")
        elif report.raw:
            print("    note: the GUI printed no solver token, so the manifest "
                  "records which linear solver ran as unknown")
        if not report.acts:
            print("    note: the GUI printed no act table, so per-act durations "
                  "are unknown and the manifest will say so")
        reported = report.counts.get("frames")
        if reported is not None and int(reported) != args.frames:
            raise SystemExit(
                f"the GUI reports {int(reported)} recorded frames, not the "
                f"{args.frames} requested")
        size = report.reported_size
        if size is not None and (size[0] % 2 or size[1] % 2):
            raise SystemExit(
                f"the GUI reports a {size[0]}x{size[1]} framebuffer, which "
                "yuv420p cannot encode without resampling every UI hairline")

    manifest_path = out_dir / "manifest.json"
    # A `--only gif` run drives no GUI, so `report` is empty and the act rule
    # would fall through to the scheduled fractions. The frames on disk were
    # recorded by an earlier run whose act table is in the manifest beside them,
    # and that table is the truth about those exact frames -- so read it back
    # rather than guessing at fractions. The window's recorded source says it came
    # from the manifest, so a reader can tell a re-encode from a fresh capture.
    acts_from_manifest = False
    if not report.acts and manifest_path.is_file():
        try:
            prior = json.loads(manifest_path.read_text()).get("report") or {}
        except json.JSONDecodeError:
            prior = {}
        prior_acts = prior.get("acts")
        if isinstance(prior_acts, list) and prior_acts:
            report.acts = prior_acts
            acts_from_manifest = True
            print(f"    read {len(prior_acts)} acts back from "
                  f"{rel(manifest_path)} (this run drove no GUI)")

    # ---- verify, then encode -----------------------------------------------
    frame_size = None
    if wanted & {"mp4", "gif", "poster"}:
        frame_size = verify_frames(frames_dir, args.frames)
        reported_size = report.reported_size
        if reported_size is not None and reported_size != frame_size:
            raise SystemExit(
                f"the GUI reports a {reported_size[0]}x{reported_size[1]} "
                f"framebuffer but the frames are "
                f"{frame_size[0]}x{frame_size[1]}; the window manager resized "
                "the window, so the video would not be the surface the GUI drew")

    outputs: dict[str, dict] = {}
    if "mp4" in wanted:
        print("[mp4]")
        outputs["mp4"] = encode_mp4(frames_dir, out_dir / "advisor_cinema.mp4",
                                    FPS, pick_encoder(args.encoder))
    if "gif" in wanted:
        print("[gif]")
        gif_start, gif_duration, gif_source = gif_window(
            report, args.frames / FPS, args.gif_start, args.gif_duration)
        if acts_from_manifest and args.gif_start is None and args.gif_duration is None:
            gif_source += ", from the act table this manifest already carried"
        print(f"    window {gif_start:g}..{gif_start + gif_duration:g} s of "
              f"{args.frames / FPS:g} s ({gif_source})")
        outputs["gif"] = encode_gif(
            frames_dir, out_dir / "advisor_cinema.gif", fps=FPS,
            start_s=gif_start, duration_s=gif_duration,
            max_bytes=args.gif_max_bytes)
        outputs["gif"]["window_source"] = gif_source
    if "poster" in wanted:
        print("[poster]")
        index = args.poster_frame
        source = "--poster-frame"
        if index is None:
            index = report.poster_frame
            source = "reported by the GUI as the first fully-composed frame"
        if index is None:
            index = 0
            source = ("frame 0; the GUI reported no fully-composed frame index "
                      "and none was given")
        outputs["poster"] = write_poster(frames_dir, out_dir / "poster.png",
                                         index)
        outputs["poster"]["frame_source"] = source

    # ---- manifest -----------------------------------------------------------
    onnx = model_onnx(args.model_dir)
    onnx_sha, _ = fs.digest(onnx)
    manifest = {
        "schema": "polymesh.cinema.manifest/1",
        "generated_utc": _dt.datetime.now(_dt.timezone.utc)
                            .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_rev": fs.git_revision(),
        "stamp": stamp,
        "stages_run": sorted(wanted),
        "case": {
            "part": case.name,
            "step": rel(part),
            "case_json": case.case_json,
            "fix_face": case.fix_face,
            "load_face": case.load_face,
            "load_n": list(case.load),
            "load_note": case.load_note,
            "h_mm_configured": case.h_mm,
            "youngs_modulus_gpa": case.youngs_gpa,
            "poissons_ratio": case.poisson,
            "adapt_passes_configured": case.adapt_passes,
            "eta_target_configured": case.eta_target,
            "h_note": "configured before inference; an accepted advisor action "
                      "overrides it, while a refusal leaves it unchanged",
            "why": case.why,
        },
        "model": {
            "dir": rel(args.model_dir),
            "onnx": rel(onnx),
            "onnx_sha256": onnx_sha,
        },
        "capture": {
            "gui": rel(args.gui),
            "command": argv_gui,
            "auto_spec": spec,
            "env": {"POLYMESH_CINEMA_STAMP": stamp},
            "xvfb_screen": f"{screen[0]}x{screen[1]}",
            "frame_size": (f"{frame_size[0]}x{frame_size[1]}"
                           if frame_size else None),
            "frames_dir": rel(frames_dir),
            "frame_count": args.frames,
            "fps": FPS,
            "duration_s": round(args.frames / FPS, 6),
            "gui_wall_s": round(wall, 3) if wall is not None else None,
            "ffmpeg": ffmpeg_version() if wanted & {"mp4", "gif"} else None,
        },
        # Everything below is what the GUI itself reported about the take. A
        # missing value is null with a stated reason, never a filled-in guess.
        "report": {
            "acts": report.acts,
            "take": {key: report.take[key] for key in sorted(report.take)},
            "acts_note": (None if report.acts else
                          "the GUI printed no act table during this run, so "
                          "per-act durations are unknown"),
            "candidates": _as_int(report.counts.get("candidates")),
            "forward_passes": _as_int(report.advisor.get("frames")),
            "mesh_stages": _as_int(report.counts.get("stages")),
            "solve_stages": _as_int(report.counts.get("solve_stages")),
            "elements": _as_int(report.counts.get("elements")),
            "nodes": _as_int(report.counts.get("nodes")),
            "dof": _as_int(report.counts.get("dof")),
            "quality_min": report.counts.get("quality_min"),
            "quality_mean": report.counts.get("quality_mean"),
            "youngs_modulus_pa": report.counts.get("youngs_pa"),
            "poissons_ratio": report.counts.get("poisson"),
            "max_von_mises_pa": report.counts.get("max_von_mises_pa"),
            "global_eta": report.counts.get("global_eta"),
            "max_displacement_m": report.counts.get("max_displacement_m"),
            "deformation_scale": report.counts.get("deform_scale"),
            "visible_displacement_m": report.counts.get("visible_displacement_m"),
            "visible_displacement_fraction": report.counts.get("visible_fraction"),
            "unchanged_cells": _as_int(report.counts.get("unchanged")),
            "removed_cells": _as_int(report.counts.get("removed")),
            "added_cells": _as_int(report.counts.get("added")),
            "elements_skipped": _as_int(report.counts.get("skipped")),
            "solver": report.solver,
            "solver_note": (None if report.solver else
                            "the GUI printed no solver token on its record "
                            "line, so which linear solver ran is unknown from "
                            "this run"),
            "poster_frame": report.poster_frame,
            "framebuffer": (f"{report.reported_size[0]}x"
                            f"{report.reported_size[1]}"
                            if report.reported_size else None),
            "advisor_activations": (None if not report.raw
                                    else report.unavailable is None),
            "advisor_unavailable": report.unavailable,
            "decision": report.decision,
            "advisor_fields": {key: report.advisor[key]
                               for key in sorted(report.advisor)},
            "spectral_fields": {key: report.spectral[key]
                                for key in sorted(report.spectral)},
            "lines": report.raw,
        },
        "outputs": outputs,
    }
    _merge_existing(manifest_path, manifest, out_dir)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"\nwrote {rel(manifest_path)}")
    return 0


def _merge_existing(manifest_path: Path, manifest: dict, out_dir: Path) -> None:
    """Carry forward what this run did not produce: output records, and the
    report.

    A ``--only gif`` run must not erase the mp4's digest, and must not claim the
    mp4 it did not make. The same holds for the GUI's own account of the take: a
    re-encode drives no GUI, so it has no act table, no advisor fields and no
    solver token, and writing that emptiness over the record of the run that
    actually captured the frames would destroy the provenance of the frames still
    sitting on disk. Anything carried over keeps the timestamp of the run that
    made it, under ``carried_over``.
    """
    if not manifest_path.is_file():
        return
    try:
        previous = json.loads(manifest_path.read_text())
    except json.JSONDecodeError:
        return
    for name, record in (previous.get("outputs") or {}).items():
        if name in manifest["outputs"] or not isinstance(record, dict):
            continue
        file_name = record.get("file")
        if not file_name or not (out_dir / str(file_name)).is_file():
            continue
        carried = dict(record)
        carried["carried_over"] = previous.get("generated_utc")
        manifest["outputs"][name] = carried
    if not (manifest.get("report") or {}).get("lines"):
        prior = previous.get("report")
        if isinstance(prior, dict) and prior.get("lines"):
            carried = dict(prior)
            carried["carried_over"] = previous.get("generated_utc")
            manifest["report"] = carried


if __name__ == "__main__":
    sys.exit(main())
