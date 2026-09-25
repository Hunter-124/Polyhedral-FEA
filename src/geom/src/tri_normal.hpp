// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private geom helper shared by features.cpp and indicators.cpp.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstddef>

namespace polymesh::geom::detail {

/// Unit normal of triangle `t` from its winding; +z for a zero-area triangle.
inline Eigen::Vector3d tri_normal(const TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    const Eigen::Vector3d n = ab.cross(ac);
    const double len = n.norm();
    if (len > 0.0) {
        return Eigen::Vector3d(n / len);
    }
    return Eigen::Vector3d(0.0, 0.0, 1.0);
}

} // namespace polymesh::geom::detail
