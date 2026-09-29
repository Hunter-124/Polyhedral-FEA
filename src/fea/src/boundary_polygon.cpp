// SPDX-License-Identifier: BSD-3-Clause
// Planar projection of a boundary loop and its area-preserving triangulation.
#include "boundary_loops.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace polymesh::fea::detail::boundary {

double signed_area(const std::vector<Eigen::Vector2d>& points) {
    double twice_area = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Eigen::Vector2d& a = points[i];
        const Eigen::Vector2d& b = points[(i + 1) % points.size()];
        twice_area += a.x() * b.y() - a.y() * b.x();
    }
    return 0.5 * twice_area;
}

bool project_loop(const NodalMesh& mesh, const Loop& loop, Projection& projection) {
    projection = {};
    projection.origin = mesh.nodes[loop[0]];
    Eigen::Vector3d twice_area_normal = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Eigen::Vector3d a = mesh.nodes[loop[i]] - projection.origin;
        const Eigen::Vector3d b = mesh.nodes[loop[(i + 1) % loop.size()]] - projection.origin;
        twice_area_normal += a.cross(b);
        projection.scale = std::max(projection.scale, a.norm());
    }
    const double scale_squared = projection.scale * projection.scale;
    projection.area_epsilon = 128.0 * std::numeric_limits<double>::epsilon() * scale_squared;
    if (!(twice_area_normal.norm() > projection.area_epsilon)) {
        return false;
    }
    projection.normal = twice_area_normal.normalized();
    Eigen::Vector3d axis = Eigen::Vector3d::UnitX();
    if (std::abs(projection.normal.y()) <= std::abs(projection.normal.x()) &&
        std::abs(projection.normal.y()) <= std::abs(projection.normal.z())) {
        axis = Eigen::Vector3d::UnitY();
    } else if (std::abs(projection.normal.z()) <= std::abs(projection.normal.x()) &&
               std::abs(projection.normal.z()) <= std::abs(projection.normal.y())) {
        axis = Eigen::Vector3d::UnitZ();
    }
    projection.u = projection.normal.cross(axis).normalized();
    projection.v = projection.normal.cross(projection.u);
    projection.points.reserve(loop.size());
    for (const std::uint32_t node : loop) {
        const Eigen::Vector3d delta = mesh.nodes[node] - projection.origin;
        projection.points.emplace_back(delta.dot(projection.u), delta.dot(projection.v));
    }
    projection.signed_area = signed_area(projection.points);
    projection.area = std::abs(projection.signed_area);
    projection.area_tolerance =
        std::max(1e-10 * projection.area,
                 256.0 * std::numeric_limits<double>::epsilon() * scale_squared);
    projection.length_tolerance =
        1e-9 * std::max(projection.scale, std::numeric_limits<double>::min());
    return projection.area > projection.area_epsilon;
}

std::vector<Eigen::Vector2d> project_points(const NodalMesh& mesh, const Loop& loop,
                                            const Projection& projection) {
    std::vector<Eigen::Vector2d> points;
    points.reserve(loop.size());
    for (const std::uint32_t node : loop) {
        const Eigen::Vector3d delta = mesh.nodes[node] - projection.origin;
        points.emplace_back(delta.dot(projection.u), delta.dot(projection.v));
    }
    return points;
}

namespace {

double triangle_area(const Eigen::Vector2d& a, const Eigen::Vector2d& b,
                     const Eigen::Vector2d& c) {
    return 0.5 * std::abs(cross_2d(a, b, c));
}

bool preserves_area(const std::vector<std::array<std::uint32_t, 3>>& triangles,
                    const NodalMesh& mesh, const Projection& projection) {
    double area = 0.0;
    for (const auto& triangle : triangles) {
        const Loop loop{triangle[0], triangle[1], triangle[2]};
        const auto points = project_points(mesh, loop, projection);
        const double piece_area = triangle_area(points[0], points[1], points[2]);
        if (!(piece_area > projection.area_epsilon)) {
            return false;
        }
        area += piece_area;
    }
    return std::abs(area - projection.area) <= projection.area_tolerance;
}

bool triangulate_star(const NodalMesh& mesh, const Loop& loop, const Projection& projection,
                      std::vector<std::array<std::uint32_t, 3>>& triangles) {
    for (std::size_t root = 0; root < loop.size(); ++root) {
        std::vector<std::array<std::uint32_t, 3>> candidate;
        candidate.reserve(loop.size() - 2);
        for (std::size_t offset = 1; offset + 1 < loop.size(); ++offset) {
            candidate.push_back({loop[root], loop[(root + offset) % loop.size()],
                                 loop[(root + offset + 1) % loop.size()]});
        }
        if (preserves_area(candidate, mesh, projection)) {
            triangles = std::move(candidate);
            return true;
        }
    }
    return false;
}

bool point_strictly_inside_triangle(const Eigen::Vector2d& point, const Eigen::Vector2d& a,
                                    const Eigen::Vector2d& b, const Eigen::Vector2d& c,
                                    double orientation, double epsilon) {
    return orientation * cross_2d(a, b, point) > epsilon &&
           orientation * cross_2d(b, c, point) > epsilon &&
           orientation * cross_2d(c, a, point) > epsilon;
}

bool triangulate_ears(const NodalMesh& mesh, const Loop& loop, const Projection& projection,
                      std::vector<std::array<std::uint32_t, 3>>& triangles) {
    std::vector<std::size_t> remaining(loop.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        remaining[i] = i;
    }
    const double orientation = projection.signed_area > 0.0 ? 1.0 : -1.0;
    const double cross_epsilon = 2.0 * projection.area_epsilon;
    std::vector<std::array<std::uint32_t, 3>> candidate;
    candidate.reserve(loop.size() - 2);
    while (remaining.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::size_t previous =
                remaining[(i + remaining.size() - 1) % remaining.size()];
            const std::size_t current = remaining[i];
            const std::size_t next = remaining[(i + 1) % remaining.size()];
            const Eigen::Vector2d& a = projection.points[previous];
            const Eigen::Vector2d& b = projection.points[current];
            const Eigen::Vector2d& c = projection.points[next];
            if (orientation * cross_2d(a, b, c) <= cross_epsilon) {
                continue;
            }
            bool contains_vertex = false;
            for (const std::size_t vertex : remaining) {
                if (vertex == previous || vertex == current || vertex == next) {
                    continue;
                }
                if (point_strictly_inside_triangle(projection.points[vertex], a, b, c,
                                                   orientation, cross_epsilon)) {
                    contains_vertex = true;
                    break;
                }
            }
            if (contains_vertex) {
                continue;
            }
            candidate.push_back({loop[previous], loop[current], loop[next]});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            return false;
        }
    }
    candidate.push_back({loop[remaining[0]], loop[remaining[1]], loop[remaining[2]]});
    if (!preserves_area(candidate, mesh, projection)) {
        return false;
    }
    triangles = std::move(candidate);
    return true;
}

} // namespace

bool triangulate_loop(const NodalMesh& mesh, const Loop& loop,
                      std::vector<std::array<std::uint32_t, 3>>& triangles) {
    Projection projection;
    if (!project_loop(mesh, loop, projection)) {
        return false;
    }
    return triangulate_star(mesh, loop, projection, triangles) ||
           triangulate_ears(mesh, loop, projection, triangles);
}

} // namespace polymesh::fea::detail::boundary
