// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Closest-point projection onto a triangle surface and boundary conformity
// metrics (how far stair-cased nodes sit from the true STL).

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <vector>

namespace polymesh::mesh {

struct ClosestPoint {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    double distance = 0.0;
    std::size_t triangle = 0;
};

/// Closest point on the surface to `p` (brute-force; fine for draft meshes).
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

} // namespace polymesh::mesh
