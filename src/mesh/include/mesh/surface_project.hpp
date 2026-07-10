// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Closest-point projection onto a triangle surface, boundary conformity
// metrics, and Jacobian-safe limited surface snap for Cartesian fills.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <functional>
#include <set>
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
///        raised from 0.55 so curved walls/cylinders can leave the stair-case).
/// @param passes Number of incremental projection passes (default 4).
SnapStats snap_boundary_nodes(const geom::TriSurface& surface,
                              std::vector<Eigen::Vector3d>& nodes,
                              const std::vector<std::uint32_t>& boundary_nodes, double h,
                              const CollectOffendersFn& collect_offenders,
                              double max_move_frac = 0.75, int passes = 4);

} // namespace polymesh::mesh
