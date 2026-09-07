// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// True local h-refine for tet4 meshes (ADR-0016 / ROADMAP D4).
//
// Strategy: Rivara longest-edge bisection (LEB) with longest-edge propagation
// path (LEPP) closure. Every edge that is split is bisected in *all* tets that
// share it, so the output is conforming tet4 with no hanging nodes and no
// multipoint constraints in assembly.
//
// Units: node coordinates in metres.

#include "geom/tri_surface.hpp"
#include "mesh/mirror.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// Counters for tests / mesher notes (optional out-parameter).
struct LocalRefineStats {
    std::size_t n_input_tets = 0;
    std::size_t n_output_tets = 0;
    std::size_t n_marked = 0;
    /// Number of parent tet → 2 children replacements.
    std::size_t n_bisections = 0;
    std::size_t n_new_nodes = 0;
    /// Free-surface midpoints whose projection onto `surface` was ACCEPTED.
    std::size_t n_surface_mids = 0;
    /// Free-surface midpoints returned to the Euclidean chord because the
    /// projection would have folded a child or because its chord sag exceeded a
    /// fifth of the child size being created (the closest point had stopped
    /// describing this edge's own curve — a bore across a clearance, a hole rim).
    /// The surface residual on these is the caller's to recover, by sliding the
    /// node back onto the face under a per-node quality test.
    std::size_t n_chord_mids = 0;
    /// Terminal edges skipped because no midpoint kept all children positive
    /// (sliver-safe: region stays coarser instead of aborting the mesh).
    std::size_t n_skipped_slivers = 0;
};

/// Longest-edge bisection of a pure tet4 mesh.
///
/// @param nodes Vertex positions (metres); copied then extended with midpoints.
/// @param tets  Each entry four node indices, preferably positive orientation.
/// @param marked Element indices into `tets` to refine (duplicates ignored).
/// @param stats Optional statistics.
/// @param surface Optional CAD/STL: free-surface edge midpoints are projected
///        onto the surface instead of Euclidean chords (reduces hole/void
///        residual after LEB). Nullptr = pure geometric mids (default).
/// @param mirror Optional verified reflection symmetry: the free-surface
///        midpoint projection is answered in the canonical octant and reflected
///        back, so a mid-edge node and its mirror image land on mirrored points
///        of the surface rather than on whatever the local facet row happens to
///        offer (mesh/mirror.hpp).
/// @param max_sag_fraction Curvature gate on the surface projection, as a
///        fraction of the CHILD edge length being created. `|projected - chord|`
///        is the chord sag of the surface the endpoints sit on (L^2*k/8 for an
///        arc), so a projection whose sag exceeds this fraction of L/2 is
///        declined and the node goes on the Euclidean chord instead: past that
///        the closest point has stopped describing this edge's own curve (a bore
///        across a clearance, a hole rim, the far side of a thin wall) and
///        following it folds a child. 0 disables the gate, which is what the
///        Cartesian fills want — their parent edges are a whole lattice cell
///        long, so the sag on a coarse curved wall is legitimately large and
///        their own pre/post boundary-snap passes own the residual. A caller
///        that refines an ALREADY-fine mesh toward a sub-millimetre target and
///        has no snap pass of its own wants ~0.2 plus a smoothing pass over the
///        declined nodes.
TetFillOutput local_refine_tets(std::vector<Eigen::Vector3d> nodes,
                                std::vector<std::array<std::uint32_t, 4>> tets,
                                std::span<const std::size_t> marked,
                                LocalRefineStats* stats = nullptr,
                                const geom::TriSurface* surface = nullptr,
                                const MirrorFrame* mirror = nullptr,
                                double max_sag_fraction = 0.0);

/// Same as above, taking a `TetFillOutput` (nodes + tets). Boundary quads from
/// the input are **not** preserved.
TetFillOutput local_refine_tets(const TetFillOutput& mesh, std::span<const std::size_t> marked,
                                LocalRefineStats* stats = nullptr,
                                const geom::TriSurface* surface = nullptr,
                                const MirrorFrame* mirror = nullptr,
                                double max_sag_fraction = 0.0);

} // namespace polymesh::mesh
