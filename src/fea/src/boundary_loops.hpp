// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to the boundary-face extraction TUs: boundary_faces.cpp (public
// API), boundary_loops.cpp, boundary_partition.cpp, boundary_polygon.cpp.
// A Loop is a cycle of global node ids; a Projection maps it onto its own plane.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace polymesh::fea::detail::boundary {

using Loop = std::vector<std::uint32_t>;

struct Edge {
    std::uint32_t a = 0;
    std::uint32_t b = 0;

    friend bool operator<(const Edge& lhs, const Edge& rhs) {
        return lhs.a < rhs.a || (lhs.a == rhs.a && lhs.b < rhs.b);
    }
};

inline Edge edge(std::uint32_t a, std::uint32_t b) { return a < b ? Edge{a, b} : Edge{b, a}; }

struct Projection {
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector3d normal = Eigen::Vector3d::Zero();
    Eigen::Vector3d u = Eigen::Vector3d::Zero();
    Eigen::Vector3d v = Eigen::Vector3d::Zero();
    std::vector<Eigen::Vector2d> points;
    double signed_area = 0.0;
    double area = 0.0;
    double scale = 0.0;
    double area_epsilon = 0.0;
    double area_tolerance = 0.0;
    double length_tolerance = 0.0;
};

inline double cross_2d(const Eigen::Vector2d& a, const Eigen::Vector2d& b,
                       const Eigen::Vector2d& c) {
    const Eigen::Vector2d ab = b - a;
    const Eigen::Vector2d ac = c - a;
    return ab.x() * ac.y() - ab.y() * ac.x();
}

using EdgeOwners = std::map<Edge, std::vector<std::size_t>>;
using NodeNeighbors = std::map<std::uint32_t, std::set<std::uint32_t>>;

// --- boundary_polygon.cpp ---
double signed_area(const std::vector<Eigen::Vector2d>& points);
bool project_loop(const NodalMesh& mesh, const Loop& loop, Projection& projection);
std::vector<Eigen::Vector2d> project_points(const NodalMesh& mesh, const Loop& loop,
                                            const Projection& projection);
/// Area-preserving triangulation: a star from some root, else ear clipping.
/// False when the loop is degenerate or neither preserves the projected area.
bool triangulate_loop(const NodalMesh& mesh, const Loop& loop,
                      std::vector<std::array<std::uint32_t, 3>>& triangles);

// --- boundary_partition.cpp ---
/// Deactivate `loops[target_index]` together with the finer, oppositely wound
/// loops that exactly tile it. Returns false and changes nothing otherwise.
bool suppress_opposing_partition(const NodalMesh& mesh, std::size_t target_index,
                                 const std::vector<Loop>& loops, std::vector<bool>& active);
/// True when a triangle loop may be the coarse side of a partition: an edge is
/// split by a coplanar node of other loops, or another owner's node lies inside it.
bool triangle_has_partition_hint(const NodalMesh& mesh, std::size_t target_index,
                                 const std::vector<Loop>& loops, const EdgeOwners& edge_owners,
                                 const NodeNeighbors& neighbors);

// --- boundary_loops.cpp ---
/// Exterior face loops of `mesh`: exactly paired faces and opposing partitions
/// removed, then partition-suppressed loops revived where the shell tore.
std::vector<Loop> resolve_boundary_loops(const NodalMesh& mesh);

} // namespace polymesh::fea::detail::boundary
