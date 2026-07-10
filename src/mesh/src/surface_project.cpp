// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace polymesh::mesh {
namespace {

// Ericson closest-point on triangle.
Eigen::Vector3d closest_on_triangle(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                                    const Eigen::Vector3d& b, const Eigen::Vector3d& c) {
    const Eigen::Vector3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return a;
    }
    const Eigen::Vector3d bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Eigen::Vector3d cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

ClosestPoint closest_on_surface(const geom::TriSurface& surface, const Eigen::Vector3d& p) {
    ClosestPoint best;
    best.distance = std::numeric_limits<double>::infinity();
    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const auto& tri = surface.triangles[t];
        const Eigen::Vector3d q = closest_on_triangle(
            p, surface.vertices[tri[0]], surface.vertices[tri[1]], surface.vertices[tri[2]]);
        const double d = (p - q).norm();
        if (d < best.distance) {
            best.distance = d;
            best.point = q;
            best.triangle = t;
        }
    }
    return best;
}

ConformityStats surface_conformity(const geom::TriSurface& surface,
                                   const std::vector<Eigen::Vector3d>& points,
                                   const std::vector<std::uint32_t>& point_indices) {
    ConformityStats s;
    if (point_indices.empty()) {
        return s;
    }
    double sum = 0.0;
    for (auto i : point_indices) {
        const double d = closest_on_surface(surface, points[i]).distance;
        s.max_distance = std::max(s.max_distance, d);
        sum += d;
        ++s.count;
    }
    s.mean_distance = sum / static_cast<double>(s.count);
    return s;
}

} // namespace polymesh::mesh
