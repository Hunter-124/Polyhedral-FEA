// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Phases of `graded_tet_fill_surface` (mesh-private). hybrid_fill.cpp owns the
// phase order; every phase reads and mutates the one GradedFillState.

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh::detail {

/// Inputs and mesh shared by the graded-fill phases once the lattice is refined.
struct GradedFillState {
    const geom::TriSurface& surface;
    std::span<const geom::SharpEdge> features;
    const BoundaryFit* fit;
    BoundaryProjectionContext* projection;
    const MirrorFrame* mirror;
    FillProgressScope& progress;
    GradedTetFillOutput& out;
    /// Coarse lattice spacing (metres).
    double hc;
    /// Per node, the shortest incident edge of the refined lattice before any
    /// CAD projection (metres).
    std::vector<double> original_spacing;
    /// Projection-resistant boundary nodes, captured once after the first snap.
    std::unordered_set<std::uint32_t> initial_juts;
};

/// Distance from `p` to the surface, answered in the canonical octant
/// (reflection is an isometry, so this is the same distance).
double surface_distance(const geom::TriSurface& surface, const MirrorFrame* mirror,
                        const Eigen::Vector3d& p);

/// Nodes on exterior triangular faces (faces used by one tet), in the
/// mirror-canonical order the snap/smooth rounds must visit them.
std::vector<std::uint32_t>
tet_boundary_nodes(const std::vector<std::array<std::uint32_t, 4>>& tets,
                   const std::vector<Eigen::Vector3d>& nodes);

/// Edges of the free-face shell that are used a number of times other than two.
///
/// A conforming tet complex has none. An edge collapse that violates the link
/// condition, or a carve that strands a neighbour with two exposed faces meeting
/// at their shared edge, slits the skin along a line without changing the volume
/// or invalidating a single element: the two torn patches coincide and the
/// divergence volume cancels, so no volume or validity check can find it.
struct TetShellTopology {
    std::size_t n_torn_edges = 0;
    /// Packed (min << 32) | max, for the torn edges only.
    std::unordered_set<std::uint64_t> torn;
};

TetShellTopology tet_shell_topology(const std::vector<std::array<std::uint32_t, 4>>& tets);

/// Restrict a proposed set of tet deletions to those that keep the boundary as
/// intact as it already is (no torn edge beyond those already present).
///
/// Every deletion site decides tet-by-tet and cannot see that removing one tet
/// may strand a neighbour with two exposed faces. Each site proposes; this
/// disposes. There are two ways to close a slit and they are not equally good:
///
///  - EXTEND: delete the stranded neighbour too. At a void carve that neighbour
///    is a one-cell spike poking into the hole, so removing it moves the
///    boundary toward the true surface.
///  - REVIVE: put a deleted tet back. That reconnects the survivors through
///    solid, but at a void carve it backfills material INTO the hole.
///
/// Extending is tried first and revival is the fallback, because backfilling
/// measurably degrades the surface. Extension is capped so a runaway cannot eat
/// the solid, and if neither converges the whole proposal is dropped (returns
/// false, `kill` cleared) — a surviving flake is a quality problem, a torn skin
/// is a correctness one, and the two are not tradeable.
bool restrict_kill_to_shell(const std::vector<std::array<std::uint32_t, 4>>& tets,
                            std::vector<char>& kill, int max_rounds = 8);

/// Snap the current free surface onto the CAD/tessellation (bulk snap, orbit-locked
/// straggler rescue, per-node re-project), then flip any negative tet positive.
/// Boundary nodes are recollected from unpaired tet faces on every call — after
/// LEB, so rim mid-edge nodes get snapped, and after each carve, whose freshly
/// exposed faces were never snapped. Stale pre-LEB lattice quads are never used:
/// after refinement they can include interior corners.
void snap_round(GradedFillState& s);

/// S4 sliver-cap collapse: snapping all four corners of a skin tet onto a curved
/// surface leaves a near-flat cap that unsnap cannot cure without reopening the
/// residual. Void juts, then worst-aspect caps, merge along an edge (conforming;
/// the dead node merges into the survivor) when every incident tet stays valid,
/// for whole reflection orbits; a pass is reverted on any new torn edge or >0.5%
/// volume loss. Then S5 void/flake carve. `collisions_only` runs only when some
/// tet has two corners closer than 5% of their original spacing, collapses only
/// such caps, and skips the jut phase and S5.
void repair_round(GradedFillState& s, bool collisions_only = false);

/// S6 tangential smoothing: snap places nodes *on* the surface but keeps their
/// lattice-stair spacing, which reads as sawtooth on curved walls and hole rims.
/// Relax boundary nodes toward their boundary-neighbour centroid and re-project
/// (crease nodes relax along the crease), reverting any move that inverts a tet;
/// then hard-pin CAD vertices and sharp edges when `fit` can pin (ADR-0035).
void smooth_boundary_and_pin_features(GradedFillState& s);

/// S7 overlapped-sheet carve. Snap gives every node its exact CAD owner, so at a
/// concave crease two sheets project onto their own face patches and can legally
/// interpenetrate — every tet positive, every edge manifold, and free faces
/// buried strictly inside other cells. The overlap is born in the first snap
/// round, so no downstream smoothing prevents it. A buried face's owner tet is
/// doubly-counted volume, so the remedy is deletion under the shell guard,
/// alternated with re-snaps of the newly exposed layer; node-pulling alone is
/// the rejected variant (it strands or grows the tangle). Ends with a repair
/// round, a final carve, a bounded pull and a collision-only repair; throws
/// ValidityError when buried faces remain.
void carve_overlapped_sheets(GradedFillState& s);

/// Interior sliver relaxation.
///
/// S4 collapses sliver caps and S5 peels the flakes that gain a free face, so
/// both are boundary-facing by construction. A sliver wedged in the INTERIOR
/// survives them, and a cell decades below the shape floor conditions the
/// stiffness matrix out of CG's reach (ADR-0033), so the mesher owns this.
///
/// The cure is room, exactly as in `hex_fill_surface`: a sliver's non-boundary
/// nodes relax toward the centroid of their edge neighbours, and a move is kept
/// only when the worst aspect over the node's whole incident star strictly
/// improves. Boundary nodes are frozen, so this cannot cost boundary fidelity,
/// and monotone acceptance means it cannot make any cell worse than it found it.
void relax_interior_slivers(GradedFillState& s);

} // namespace polymesh::mesh::detail
