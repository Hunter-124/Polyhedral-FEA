// SPDX-License-Identifier: AGPL-3.0-or-later
#include "geom/tri_surface.hpp"

#include <Eigen/Geometry>

#include <format>

namespace polymesh::geom {

void TriSurface::validate() const {
    constexpr double kAreaTol = 1e-20; // m^2
    const auto n = static_cast<std::uint32_t>(vertices.size());
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const auto& idx = triangles[t];
        for (const auto i : idx) {
            if (i >= n) {
                throw GeomError(
                    std::format("triangle {} references out-of-range vertex {}", t, i));
            }
        }
        const Eigen::Vector3d ab = vertices[idx[1]] - vertices[idx[0]];
        const Eigen::Vector3d ac = vertices[idx[2]] - vertices[idx[0]];
        const double area = 0.5 * ab.cross(ac).norm();
        if (area < kAreaTol) {
            throw GeomError(std::format(
                "triangle {} is degenerate (area {:.3e} m^2 below tolerance)", t, area));
        }
    }
}

} // namespace polymesh::geom
