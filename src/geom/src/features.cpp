// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <utility>

namespace polymesh::geom {
namespace {

Eigen::Vector3d tri_normal(const TriSurface& s, std::size_t t) {
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

double point_segment_distance(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                              const Eigen::Vector3d& b) {
    const Eigen::Vector3d ab = b - a;
    const double denom = ab.squaredNorm();
    if (denom == 0.0) {
        return (p - a).norm();
    }
    const double t = std::clamp((p - a).dot(ab) / denom, 0.0, 1.0);
    return (p - (a + t * ab)).norm();
}

} // namespace

std::vector<SharpEdge> detect_sharp_edges(const TriSurface& surface, double sharp_angle_deg) {
    surface.validate();
    using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<EdgeKey, std::vector<std::size_t>> edge_tris;
    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const auto& tri = surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            auto a = tri[static_cast<std::size_t>(e)];
            auto b = tri[static_cast<std::size_t>((e + 1) % 3)];
            if (a > b) {
                std::swap(a, b);
            }
            edge_tris[{a, b}].push_back(t);
        }
    }

    const double cos_thresh = std::cos(sharp_angle_deg * 3.14159265358979323846 / 180.0);
    std::vector<SharpEdge> out;
    for (const auto& [key, tris] : edge_tris) {
        if (tris.size() == 1) {
            out.push_back(SharpEdge{key.first, key.second, 0.0});
            continue;
        }
        if (tris.size() != 2) {
            continue; // non-manifold — treat as feature
        }
        const Eigen::Vector3d n0 = tri_normal(surface, tris[0]);
        const Eigen::Vector3d n1 = tri_normal(surface, tris[1]);
        const double c = std::clamp(n0.dot(n1), -1.0, 1.0);
        // Flat ⇒ cos≈1. Sharp crease ⇒ cos smaller.
        if (c < cos_thresh) {
            out.push_back(SharpEdge{key.first, key.second, std::acos(c)});
        }
    }
    return out;
}

double distance_to_features(const Eigen::Vector3d& p, const TriSurface& surface,
                            std::span<const SharpEdge> edges) {
    double best = std::numeric_limits<double>::infinity();
    for (const auto& e : edges) {
        best = std::min(
            best, point_segment_distance(p, surface.vertices[e.v0], surface.vertices[e.v1]));
    }
    return best;
}

} // namespace polymesh::geom
