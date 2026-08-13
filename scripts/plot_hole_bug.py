#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""The classification aliasing bug, before and after, as a matched render grid.

Feature classification used to sample cell-centre parity on the coarse lattice.
A feature narrower than a cell therefore fell between samples and was aliased
away: on ``box_hole_s0_c0`` the bore was over-cut into a rectangular void at
h_rel 0.20, vanished completely at 0.12, and came back as a ragged remnant at
0.08 -- refinement made the hole disappear, which is the opposite of what
refinement is for.

Every panel is drawn from a mesh that is already on disk under
``bench/results/hole-diagnosis/``: nothing here re-meshes or re-solves. The
number under each panel is measured from that mesh, not quoted from a report:

  material volume   sum of tet volumes
  void volume       bounding-box volume minus material volume, i.e. how much
                    of the plate the mesher actually removed
  recovery          void volume as a percentage of the CAD bore volume
                    pi * (d/2)^2 * t, with d/W taken from the external truth
                    findings and W, t measured from the mesh bounding box

Run from anywhere:

    python scripts/plot_hole_bug.py
    python scripts/plot_hole_bug.py --diagnosis-dir bench/results/hole-diagnosis \
        --out-dir docs/validation/figures
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402
import vtu_wire_png as wire  # noqa: E402

from matplotlib.collections import LineCollection  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DIAGNOSIS_DIR = ROOT / "bench" / "results" / "hole-diagnosis"
FINDINGS = ROOT / "bench" / "reference" / "external" / "external-truth-findings.json"
OUT_DIR = ROOT / "docs" / "validation" / "figures"

#: The case the diagnosis meshes were cut for.
CASE = "box_hole_s0_c0"
#: Resolution rungs, coarse to fine. The story is that quality moves the wrong
#: way along this axis, so the order matters.
RUNGS = ("0.20", "0.12", "0.08")
#: (row label, filename prefix). "before" is the aliased classifier.
VARIANTS = (("before the fix", ""), ("after the fix", "fixed-"))
#: A void this small is not a bore, it is mesh noise on the plate surface.
ABSENT_FRACTION = 0.02


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--diagnosis-dir", type=Path, default=DIAGNOSIS_DIR,
                        help="directory holding the before/after VTUs "
                             "(default: bench/results/hole-diagnosis)")
    parser.add_argument("--out-dir", type=Path, default=OUT_DIR,
                        help="figure output directory (default: "
                             "docs/validation/figures)")
    parser.add_argument("--mesher", default="hybrid",
                        help="mesher suffix in the VTU names (default: hybrid)")
    parser.add_argument("--meshers", default="hybrid,graded",
                        help="comma-separated meshers to measure; the one that "
                             "loses the bore on the most rungs is drawn, the "
                             "rest are reported in the footer "
                             "(default: hybrid,graded)")
    return parser.parse_args()


@dataclass
class Mesh:
    """One diagnosis mesh, parsed once and measured."""
    path: Path
    h_rel: str
    variant: str
    points: np.ndarray
    edges: np.ndarray  # (n, 2, 2) top-view segments
    n_cells: int
    material_mm3: float
    bbox_mm: np.ndarray

    @property
    def void_mm3(self) -> float:
        return float(np.prod(self.bbox_mm) - self.material_mm3)



def mesh_volume(points: np.ndarray, cells: Sequence[Sequence[int]],
                types: Sequence[int], face_stream: Any,
                faceoffsets: Any) -> float:
    """Volume of any cell zoo: tet, hex, wedge, pyramid and polyhedron.

    Each cell is fanned from its own centroid to a triangulation of its faces
    and the tet magnitudes are summed. That is exact for a convex cell whatever
    its face count, which matters here because the hybrid meshes are hex-bulk
    with transition cells -- a tet-only formula silently reports the whole
    bounding box as material and would have turned a measurement into a
    fabrication.
    """
    if types and all(t == 10 for t in types) and all(len(c) == 4 for c in cells):
        # All-tet mesh: one vectorised determinant instead of a per-face fan.
        # The graded rungs run to 69k cells and the general path is minutes.
        idx = np.asarray(cells, dtype=np.int64)
        a = points[idx[:, 1]] - points[idx[:, 0]]
        b = points[idx[:, 2]] - points[idx[:, 0]]
        c = points[idx[:, 3]] - points[idx[:, 0]]
        return float(np.abs(np.einsum("ij,ij->i", np.cross(a, b), c)).sum() / 6.0)

    blocks = wire._polyhedron_face_blocks(face_stream, faceoffsets, len(cells))
    listed = points.tolist()
    total = 0.0
    for index, ids in enumerate(cells):
        vtk_type = types[index] if index < len(types) else None
        faces = wire.cell_faces(list(ids), vtk_type, blocks[index], listed)
        if not faces:
            continue
        centroid = points[list(ids)].mean(axis=0)
        for face in faces:
            corners = [points[i] for i in face]
            for j in range(1, len(corners) - 1):
                a = corners[0] - centroid
                b = corners[j] - centroid
                c = corners[j + 1] - centroid
                total += abs(float(np.dot(np.cross(a, b), c))) / 6.0
    return total


def load_mesh(path: Path, h_rel: str, variant: str) -> Mesh | None:
    if not path.is_file():
        return None
    pts, cells, types, face_stream, faceoffsets = wire.parse_vtu_ascii(path)
    points = np.asarray(pts, dtype=float)
    edge_set, _ = wire.boundary_edges(pts, cells, types, face_stream, faceoffsets)
    segments = np.array([[points[a][:2], points[b][:2]] for a, b in sorted(edge_set)],
                        dtype=float)
    bbox = (points.max(axis=0) - points.min(axis=0)) * 1000.0  # mm
    volume = mesh_volume(points, cells, types, face_stream, faceoffsets)
    return Mesh(path=path, h_rel=h_rel, variant=variant, points=points,
                edges=segments * 1000.0, n_cells=len(cells),
                material_mm3=volume * 1e9, bbox_mm=bbox)


def cad_bore_volume(bbox_mm: np.ndarray) -> tuple[float, float] | None:
    """CAD bore volume from the published d/W ratio and the measured plate.

    d/W is read from the external-truth findings rather than assumed, so this
    number moves with the reference set instead of being frozen here.
    """
    if not FINDINGS.is_file():
        return None
    payload = json.loads(FINDINGS.read_text(encoding="utf-8"))
    for finding in payload.get("findings", []):
        for entry in (finding.get("evidence") or {}).get("per_case", []) or []:
            if entry.get("case") != CASE:
                continue
            d_over_w = float(entry["d_over_W"])
            width = float(min(bbox_mm[0], bbox_mm[1]))
            thickness = float(min(bbox_mm))
            diameter = d_over_w * width
            return math.pi * (diameter / 2.0) ** 2 * thickness, diameter
    return None


def classify(recovery: float) -> tuple[str, str]:
    """Verdict for a measured void, as a fraction of the ideal bore volume.

    The ideal is a smooth cylinder; a tet mesh cuts a faceted one, so a
    correctly resolved bore lands slightly above 100% rather than on it. The
    bands are therefore asymmetric and the residual is called out in the
    footer instead of being dressed up as agreement.
    """
    theme = fs.theme()
    if not math.isfinite(recovery):
        return "no reference", theme.muted
    if recovery < ABSENT_FRACTION * 100.0:
        # The lattice still carries an imprint of the feature at the surface,
        # which is why a ring can be visible in a panel that removed nothing.
        return "bore absent — surface imprint only", theme.bad
    if recovery > 200.0:
        return f"over-cut — {recovery:.0f}% of the bore", theme.bad
    if recovery < 85.0:
        return f"partial — {recovery:.0f}% of the bore", theme.warn
    return f"resolved — {recovery:.0f}% of the bore", theme.ok


def draw_panel(ax: Any, mesh: Mesh, bore_mm3: float | None) -> str:
    """One matched top-down wireframe. Returns the panel's measured caption."""
    theme = fs.theme()
    ax.add_collection(LineCollection(mesh.edges, colors=theme.ink, linewidths=0.28,
                                     antialiaseds=True))
    recovery = (100.0 * mesh.void_mm3 / bore_mm3) if bore_mm3 else math.nan
    verdict, colour = classify(recovery)
    ax.text(0.5, -0.055, verdict, transform=ax.transAxes, ha="center", va="top",
            fontsize=fs.FONT_PT["annot"], color=colour, weight="bold")
    detail = f"{mesh.n_cells:,} cells · void {mesh.void_mm3:.1f} mm³"
    fs.assert_glyphs(detail, verdict)
    ax.text(0.5, -0.135, detail, transform=ax.transAxes, ha="center", va="top",
            fontsize=fs.FONT_PT["annot"] - 1.0, color=theme.muted)
    return f"{verdict} ({detail})"


def main() -> int:
    args = parse_args()
    fs.use("light")

    def load_set(mesher: str) -> dict[tuple[str, str], Mesh]:
        found: dict[tuple[str, str], Mesh] = {}
        for label, prefix in VARIANTS:
            for rung in RUNGS:
                path = args.diagnosis_dir / f"{prefix}{CASE}-h{rung}-{mesher}.vtu"
                mesh = load_mesh(path, rung, label)
                if mesh is not None:
                    found[(label, rung)] = mesh
        return found

    candidates = args.meshers.split(",") if args.meshers else [args.mesher]
    sets = {name: load_set(name) for name in candidates}
    sets = {name: found for name, found in sets.items() if found}
    if not sets:
        print(f"no data yet — no diagnosis meshes under {args.diagnosis_dir}; "
              "skipping hole_aliasing.png")
        return 0

    any_mesh = next(iter(next(iter(sets.values())).values()))
    bore = cad_bore_volume(any_mesh.bbox_mm)
    bore_mm3, diameter_mm = bore if bore else (None, float("nan"))

    def recovery_of(mesh: Mesh) -> float:
        return 100.0 * mesh.void_mm3 / bore_mm3 if bore_mm3 else math.nan

    def vanished_rungs(found: dict[tuple[str, str], Mesh]) -> list[str]:
        return [r for r in RUNGS
                if ("before the fix", r) in found
                and math.isfinite(recovery_of(found[("before the fix", r)]))
                and recovery_of(found[("before the fix", r)])
                < ABSENT_FRACTION * 100.0]

    print(f"\nhole_aliasing.png — {CASE}, meshers: {', '.join(sets)}")
    print(f"  plate bounding box: {any_mesh.bbox_mm[0]:.2f} x "
          f"{any_mesh.bbox_mm[1]:.2f} x {any_mesh.bbox_mm[2]:.2f} mm")
    if bore_mm3:
        print(f"  CAD bore: d = {diameter_mm:.2f} mm, ideal volume "
              f"{bore_mm3:.1f} mm³ (d/W from external-truth-findings.json, "
              "plate measured from the mesh)")

    # Headline row-pair = the mesher whose before-fix meshes lose the bore on
    # the most rungs. Chosen from the measurements, not asserted here, so a
    # rerun that changes which mesher aliases changes the figure.
    for name, found in sets.items():
        for (label, rung), mesh in sorted(found.items()):
            print(f"  {name:<8} {label:<16} h_rel {rung}: "
                  f"{classify(recovery_of(mesh))[0]}  "
                  f"({mesh.n_cells:,} cells, void {mesh.void_mm3:.1f} mm³)")
    mesher = max(sets, key=lambda name: (len(vanished_rungs(sets[name])), name))
    meshes = sets[mesher]
    vanished = vanished_rungs(meshes)
    coarse = recovery_of(meshes[("before the fix", RUNGS[0])]) \
        if ("before the fix", RUNGS[0]) in meshes else math.nan

    if vanished:
        finding = (f"refinement made the bore vanish — over-cut to "
                   f"{coarse:.0f}% of its volume at h_rel {RUNGS[0]}, then "
                   f"gone at h_rel {', '.join(vanished)}")
    else:
        finding = ("no before-fix rung loses the bore — the aliasing pathology "
                   "is not reproduced by these meshes")
    others = []
    for name, found in sets.items():
        if name == mesher:
            continue
        gone = vanished_rungs(found)
        others.append(f"{name}: bore lost at h_rel {', '.join(gone)}" if gone
                      else f"{name}: bore survives every rung")
    print(f"  headline mesher: {mesher} · {finding}")
    for line in others:
        print(f"  other mesher — {line}")

    facet_note = ""
    after = [recovery_of(meshes[("after the fix", r)]) for r in RUNGS
             if ("after the fix", r) in meshes]
    if after and all(math.isfinite(v) for v in after):
        facet_note = (f"; after the fix the void settles at {min(after):.0f}–"
                      f"{max(after):.0f}% of the ideal cylinder, the excess "
                      "being the faceting of a tet bore against a smooth CAD one")

    fig, axes = fs.figure(
        "Classification sampled cell-centre parity, so a sub-cell bore aliased away",
        subtitle=f"{CASE} · {mesher} mesher, one part, three resolutions · {finding}",
        footer=fs.footer_source(args.diagnosis_dir, FINDINGS, n=len(meshes),
                                note="top view, identical framing in every panel; "
                                     "volumes measured from the meshes"
                                     + (" · " + "; ".join(others) if others else "")
                                     + facet_note),
        size=(12.0, 6.8), nrows=len(VARIANTS), ncols=len(RUNGS),
        share_y_axis=False)

    # One framing for every panel: a before/after pair drawn at different
    # scales is not a comparison.
    span = any_mesh.bbox_mm[:2]
    pad = 0.04 * span
    origin = any_mesh.points.min(axis=0)[:2] * 1000.0

    for row, (label, _prefix) in enumerate(VARIANTS):
        for col, rung in enumerate(RUNGS):
            ax = axes[row][col]
            ax.set_xticks([])
            ax.set_yticks([])
            ax.set_aspect("equal")
            ax.set_xlim(origin[0] - pad[0], origin[0] + span[0] + pad[0])
            ax.set_ylim(origin[1] - pad[1], origin[1] + span[1] + pad[1])
            for side in ax.spines.values():
                side.set_visible(True)
                side.set_color(fs.theme().rule)
            mesh = meshes.get((label, rung))
            if mesh is None:
                ax.text(0.5, 0.5, "mesh missing", ha="center", va="center",
                        transform=ax.transAxes, color=fs.theme().muted)
                continue
            caption = draw_panel(ax, mesh, bore_mm3)
            if row == 0:
                fs.panel_title(ax, f"h_rel {rung}")
            print(f"  {label:<16} h_rel {rung}: {caption}")
        axes[row][0].set_ylabel(label, fontsize=fs.FONT_PT["label"], weight="bold",
                                labelpad=10)

    fs.finish(fig, args.out_dir / "hole_aliasing.png", max_bytes=700 * 1024)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
