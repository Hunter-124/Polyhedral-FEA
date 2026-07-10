// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Discrete feature analysis on triangle surfaces (P3 a priori path).
// Sharp edges via dihedral angle; used to seed graded sizing fields.
// Curvature / thin-wall indicators live in geom/indicators.hpp (C2).

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::geom {

struct SharpEdge {
    std::uint32_t v0 = 0;
    std::uint32_t v1 = 0;
    /// Dihedral angle in radians (π = flat, smaller = sharper crease).
    double dihedral = 0.0;
};

/// Edges whose dihedral is sharper than `sharp_angle_deg` (default 30° from flat).
/// Boundary edges (one incident triangle) are always included.
std::vector<SharpEdge> detect_sharp_edges(const TriSurface& surface,
                                          double sharp_angle_deg = 30.0);

/// Distance from point to closest sharp edge segment (metres). Large if none.
double distance_to_features(const Eigen::Vector3d& p, const TriSurface& surface,
                            std::span<const SharpEdge> edges);

/// Closest point on any sharp-edge segment (for edge-aware snap).
struct ClosestOnFeature {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    double distance = std::numeric_limits<double>::infinity();
};

ClosestOnFeature closest_on_features(const Eigen::Vector3d& p, const TriSurface& surface,
                                     std::span<const SharpEdge> edges);

} // namespace polymesh::geom
