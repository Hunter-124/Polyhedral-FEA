// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Closest-point projection onto a triangle surface, boundary conformity
// metrics, and Jacobian-safe limited surface snap for Cartesian fills.

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <vector>

namespace polymesh::mesh {

struct ClosestPoint {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    double distance = 0.0;
    std::size_t triangle = 0;
};

/// Closest point on the surface to `p` (brute-force; fine for product STLs).
ClosestPoint closest_on_surface(const geom::TriSurface& surface, const Eigen::Vector3d& p);

/// Max / mean distance of `points` to the surface (metres).
struct ConformityStats {
    double max_distance = 0.0;
    double mean_distance = 0.0;
    std::size_t count = 0;
};

ConformityStats surface_conformity(const geom::TriSurface& surface,
                                   const std::vector<Eigen::Vector3d>& points,
                                   const std::vector<std::uint32_t>& point_indices);

/// Result of a Jacobian-safe boundary snap.
struct SnapStats {
    std::size_t n_candidates = 0;
    std::size_t n_moved = 0;
    std::size_t n_unsnapped = 0;
    double max_residual = 0.0; // metres, after snap/unsnap
};

/// Collect node indices that participate in inverted / non-positive elements.
using CollectOffendersFn = std::function<void(std::set<std::uint32_t>& offenders)>;

/// Pull boundary lattice nodes toward the STL in multi-pass steps, then unsnap
/// any node that participates in an inverted element (B3 / ADR-0015).
///
/// @param h Characteristic lattice size (metres) — caps travel.
/// @param max_move_frac Max |Δ| / h per node across all passes (default 0.75;
///        product paths often pass 1.0–1.15 so LEB mid-edges leave the stair).
/// @param passes Number of incremental projection passes (default 4).
/// @param feature_edges Optional sharp CAD edges: true crease nodes (as close
///        to a feature as to the surface) project to the edge first; free-face
///        / hole-wall nodes still project to the surface.
SnapStats snap_boundary_nodes(const geom::TriSurface& surface,
                              std::vector<Eigen::Vector3d>& nodes,
                              const std::vector<std::uint32_t>& boundary_nodes, double h,
                              const CollectOffendersFn& collect_offenders,
                              double max_move_frac = 0.75, int passes = 4,
                              std::span<const geom::SharpEdge> feature_edges = {});

/// Result of a tangential boundary smoothing pass.
struct SmoothStats {
    std::size_t n_moved = 0;    // nodes moved in the final accepted state
    std::size_t n_reverted = 0; // nodes reverted by the inversion guard
    double max_residual = 0.0;  // metres, after smoothing
};

/// Constrained Laplacian smoothing of free-surface nodes: each boundary node
/// relaxes toward the centroid of its boundary neighbors, then re-projects to
/// the STL, so travel is tangential (residual stays ~0) while stair/sawtooth
/// spacing evens out. Crease nodes (within ~0.1 h of a sharp feature edge and
/// forming a 2-neighbor crease chain) relax along the crease and re-project to
/// it; corners/junctions and near-crease wall nodes are left untouched so
/// sharp edges stay sharp. Nodes whose move inverts an element are reverted
/// via `collect_offenders` (B3-safe like the snap).
SmoothStats smooth_boundary_nodes(const geom::TriSurface& surface,
                                  std::vector<Eigen::Vector3d>& nodes,
                                  std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                                  double h, const CollectOffendersFn& collect_offenders,
                                  int passes = 2, double relax = 0.5,
                                  std::span<const geom::SharpEdge> feature_edges = {});

} // namespace polymesh::mesh

