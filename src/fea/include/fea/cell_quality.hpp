// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Per-cell shape quality for the whole element zoo — one *measured* number per
// cell, never a fabricated constant. Consumers: mesher diagnostics (`diag`),
// VTU cell arrays, and the stress-sample quality floor that keeps sliver cells
// out of scored stress numbers.
//
// Convention for every measure below:
//   1     = ideal (regular) cell of that type,
//   → 0   = degenerating (flat / sliver / collapsed),
//   < 0   = inverted (negative Jacobian),
//   NaN   = NOT MEASURED (connectivity too poor to define the measure).
// NaN is deliberate: an unmeasured cell must be skipped by the caller, never
// silently averaged in as "perfect". Anything that reports a quality for a cell
// it never measured is a fabricated number.
//
// Measures:
//  - kTet4 / kTet10 — normalized volume/edge³ aspect 6√2·V/l_max³ over the four
//    corner nodes (`mesh::tet4_aspect_quality`); 1 for the regular tet. Same
//    number tet meshes always reported, so tet diagnostics do not move.
//  - kHex8 / kHex20 / kPrism6 / kPyramid5 — the **worse of two** measures, both
//    normalized so the regular cell of the type scores 1. A cell must be sound
//    in its angles *and* in its thickness; neither term subsumes the other.
//    1. Minimum corner **scaled Jacobian**: at each corner,
//       det(e₁,e₂,e₃)/(|e₁||e₂||e₃|) of the three incident edge vectors
//       (right-handed order), which is the scaled determinant of the parametric
//       Jacobian there for straight-edged cells. Normalized by the value the
//       regular cell attains: hex/tet corners are already 1, prism corners are
//       60° (÷ sin60 = √3/2) and regular-pyramid base corners are 1/√2 (× √2).
//       Hex20/tet10 use their corner nodes (mid-side nodes are edge midpoints by
//       construction). The pyramid apex is a collapsed point of the reference
//       map, so no Jacobian is defined there — only the 4 base corners are
//       measured, which is where a squashed pyramid actually shows up.
//    2. **Volume collapse**: signed cell volume (divergence theorem over the
//       outward faces, each fanned from its own centroid) per mean-edge-length
//       cubed, over the value the regular cell attains (cube 1, equilateral
//       prism √3/4, all-edges-equal pyramid 1/(3√2)). Term 1 structurally
//       cannot see this: every corner of a 1×1×1e-4 pancake hex is a perfect
//       right angle, so the corner measure alone rated that cell exactly 1.0 —
//       a fabricated perfect score for a cell whose FE map is conditioned 1e-4,
//       and precisely the sliver the stress-sample quality floor exists to
//       exclude. Measured 2026-08-08: pancake hex 1.0 → 3.4e-4, 45°-sheared
//       cube 0.707 → 0.678, 1:2 box 1.0 → 0.844, cube/equilateral prism/regular
//       pyramid unchanged at 1.0.
//  - kPolyVem — a polyhedral cell has no parametric map, so quality is the
//    minimum face-corner quality (`mesh::polygon_corner_quality`) over the
//    cell's boundary faces: 1 only when every face is a regular polygon, → 0 for
//    stretched or collapsed faces, and exactly 0 when the signed volume is
//    non-positive (inverted / degenerate cell). Documented as a boundary-shape
//    proxy, not a Jacobian.

#include "fea/nodal_mesh.hpp"

#include <cstddef>
#include <limits>
#include <vector>

namespace polymesh::fea {

/// Shape quality of one cell (see header comment for per-type measures).
/// NaN when the cell's connectivity does not support any measure.
double cell_quality(const NodalMesh& mesh, const NodalElement& element);

/// Per-element quality, aligned with `mesh.elements`.
std::vector<double> cell_quality(const NodalMesh& mesh);

/// Summary over the cells that could actually be measured.
struct CellQualityStats {
    /// Min / mean over measured cells; NaN when nothing was measurable.
    double min = std::numeric_limits<double>::quiet_NaN();
    double mean = std::numeric_limits<double>::quiet_NaN();
    std::size_t n_measured = 0;
    std::size_t n_unmeasured = 0;
};

/// Min/mean over measured cells only; unmeasured cells are counted, not faked.
CellQualityStats summarize_cell_quality(const NodalMesh& mesh);

/// Volume of one cell in m³, integrated with the element's OWN shape functions
/// over their own parametric domain (`sum w·|det J|`); kPolyVem cells use the
/// divergence theorem on their face loops instead, since they have no
/// isoparametric map. Curved cells are measured with a rule one order above
/// the stiffness rule, so the number is insensitive to the solver's
/// integration shortcut.
///
/// This is the ONE definition of "how much solid is this cell". The fill-volume
/// guard and the advisor's per-element samples both come through here. They did
/// not, once: `stress.cpp` used a bare |det J| at a single reference point,
/// which drops the reference-domain measure and reports 0.125x the true volume
/// of a hex and ~0.09x that of a pyramid.
double element_volume(const NodalMesh& mesh, const NodalElement& element);

/// Sum of `element_volume` over every cell, in m³.
double mesh_volume(const NodalMesh& mesh);

} // namespace polymesh::fea
