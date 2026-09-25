// SPDX-License-Identifier: BSD-3-Clause
// Opposing-partition suppression: a coarse face tiled from the other side by
// several finer faces is interior, and all of them leave the boundary.
#include "boundary_loops.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace polymesh::fea::detail::boundary {
namespace {

bool point_on_segment(const Eigen::Vector2d& point, const Eigen::Vector2d& a,
                      const Eigen::Vector2d& b, double tolerance) {
    if (std::abs(cross_2d(a, b, point)) > tolerance * std::max(1.0, (b - a).norm())) {
        return false;
    }
    return point.x() >= std::min(a.x(), b.x()) - tolerance &&
           point.x() <= std::max(a.x(), b.x()) + tolerance &&
           point.y() >= std::min(a.y(), b.y()) - tolerance &&
           point.y() <= std::max(a.y(), b.y()) + tolerance;
}

bool point_in_or_on_polygon(const Eigen::Vector2d& point,
                            const std::vector<Eigen::Vector2d>& polygon, double tolerance) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Eigen::Vector2d& a = polygon[j];
        const Eigen::Vector2d& b = polygon[i];
        if (point_on_segment(point, a, b, tolerance)) {
            return true;
        }
        const bool crosses = (a.y() > point.y()) != (b.y() > point.y());
        if (crosses) {
            const double crossing_x =
                a.x() + (point.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            if (crossing_x > point.x()) {
                inside = !inside;
            }
        }
    }
    return inside;
}

bool point_strictly_inside_polygon(const Eigen::Vector2d& point,
                                   const std::vector<Eigen::Vector2d>& polygon,
                                   double tolerance) {
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        if (point_on_segment(point, polygon[i], polygon[(i + 1) % polygon.size()],
                             tolerance)) {
            return false;
        }
    }
    return point_in_or_on_polygon(point, polygon, tolerance);
}

bool lies_in_target(const NodalMesh& mesh, const Loop& loop, const Projection& target,
                    std::vector<Eigen::Vector2d>& projected) {
    projected = project_points(mesh, loop, target);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Eigen::Vector3d delta = mesh.nodes[loop[i]] - target.origin;
        if (std::abs(delta.dot(target.normal)) > target.length_tolerance ||
            !point_in_or_on_polygon(projected[i], target.points, target.length_tolerance)) {
            return false;
        }
        const Eigen::Vector2d midpoint =
            0.5 * (projected[i] + projected[(i + 1) % projected.size()]);
        if (!point_in_or_on_polygon(midpoint, target.points, target.length_tolerance)) {
            return false;
        }
    }
    return true;
}

struct AtomicEdge {
    Edge key;
    int direction = 0;
    std::uint32_t from = 0;
    std::uint32_t to = 0;
};

Eigen::Vector2d project_node(const NodalMesh& mesh, std::uint32_t node,
                             const Projection& projection) {
    const Eigen::Vector3d delta = mesh.nodes[node] - projection.origin;
    return {delta.dot(projection.u), delta.dot(projection.v)};
}

bool atomize_loop_edges(const NodalMesh& mesh, const Loop& loop, const Projection& projection,
                        const std::set<std::uint32_t>& participating_nodes,
                        std::vector<AtomicEdge>& atoms) {
    atoms.clear();
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const std::uint32_t from = loop[i];
        const std::uint32_t to = loop[(i + 1) % loop.size()];
        const Eigen::Vector2d a = project_node(mesh, from, projection);
        const Eigen::Vector2d b = project_node(mesh, to, projection);
        const Eigen::Vector2d segment = b - a;
        const double length_squared = segment.squaredNorm();
        if (!(length_squared > projection.length_tolerance * projection.length_tolerance)) {
            return false;
        }
        std::vector<std::pair<double, std::uint32_t>> cuts;
        for (const std::uint32_t node : participating_nodes) {
            const Eigen::Vector2d point = project_node(mesh, node, projection);
            if (point_on_segment(point, a, b, projection.length_tolerance)) {
                cuts.emplace_back((point - a).dot(segment) / length_squared, node);
            }
        }
        std::sort(cuts.begin(), cuts.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        if (cuts.size() < 2) {
            return false;
        }
        for (std::size_t cut = 0; cut + 1 < cuts.size(); ++cut) {
            const std::uint32_t atom_from = cuts[cut].second;
            const std::uint32_t atom_to = cuts[cut + 1].second;
            const Eigen::Vector2d atom_delta = project_node(mesh, atom_to, projection) -
                                               project_node(mesh, atom_from, projection);
            if (!(atom_delta.norm() > projection.length_tolerance)) {
                return false;
            }
            const Edge key = edge(atom_from, atom_to);
            atoms.push_back({key, atom_from == key.a ? 1 : -1, atom_from, atom_to});
        }
    }
    return true;
}

bool proper_segments_cross(const Eigen::Vector2d& a, const Eigen::Vector2d& b,
                           const Eigen::Vector2d& c, const Eigen::Vector2d& d,
                           double tolerance) {
    const double epsilon = tolerance * std::max({(b - a).norm(), (d - c).norm(), tolerance});
    const double abc = cross_2d(a, b, c);
    const double abd = cross_2d(a, b, d);
    const double cda = cross_2d(c, d, a);
    const double cdb = cross_2d(c, d, b);
    return ((abc > epsilon && abd < -epsilon) || (abc < -epsilon && abd > epsilon)) &&
           ((cda > epsilon && cdb < -epsilon) || (cda < -epsilon && cdb > epsilon));
}

bool polygon_self_crosses(const std::vector<Eigen::Vector2d>& polygon, double tolerance) {
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const std::size_t i_next = (i + 1) % polygon.size();
        for (std::size_t j = i + 1; j < polygon.size(); ++j) {
            const std::size_t j_next = (j + 1) % polygon.size();
            if (i == j || i_next == j || j_next == i) {
                continue;
            }
            if (proper_segments_cross(polygon[i], polygon[i_next], polygon[j], polygon[j_next],
                                      tolerance)) {
                return true;
            }
        }
    }
    return false;
}

struct PartitionPiece {
    std::size_t loop_index = 0;
    double area = 0.0;
    double signed_area = 0.0;
    std::vector<Eigen::Vector2d> projected;
    std::set<Edge> preliminary_atoms;
};

bool edge_has_other_owner(const EdgeOwners& owners, const Edge& candidate,
                          std::size_t target_index) {
    const auto found = owners.find(candidate);
    if (found == owners.end()) {
        return false;
    }
    return std::any_of(found->second.begin(), found->second.end(),
                       [&](std::size_t owner) { return owner != target_index; });
}

} // namespace

bool suppress_opposing_partition(const NodalMesh& mesh, std::size_t target_index,
                                 const std::vector<Loop>& loops, std::vector<bool>& active) {
    const Loop& target_loop = loops[target_index];
    if (!active[target_index] || target_loop.size() < 3) {
        return false;
    }
    Projection target;
    if (!project_loop(mesh, target_loop, target) ||
        polygon_self_crosses(target.points, target.length_tolerance)) {
        return false;
    }

    std::vector<PartitionPiece> pool;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (i == target_index || !active[i]) {
            continue;
        }
        std::vector<Eigen::Vector2d> projected;
        if (!lies_in_target(mesh, loops[i], target, projected) ||
            polygon_self_crosses(projected, target.length_tolerance)) {
            continue;
        }
        const double piece_signed_area = signed_area(projected);
        const double piece_area = std::abs(piece_signed_area);
        if (!(piece_area > target.area_epsilon) ||
            piece_area > target.area + target.area_tolerance ||
            (piece_signed_area > 0.0) == (target.signed_area > 0.0)) {
            continue;
        }
        pool.push_back({i, piece_area, piece_signed_area, std::move(projected), {}});
    }
    if (pool.empty()) {
        return false;
    }

    std::set<std::uint32_t> preliminary_nodes(target_loop.begin(), target_loop.end());
    for (const PartitionPiece& piece : pool) {
        preliminary_nodes.insert(loops[piece.loop_index].begin(),
                                 loops[piece.loop_index].end());
    }
    std::vector<AtomicEdge> target_atoms;
    if (!atomize_loop_edges(mesh, target_loop, target, preliminary_nodes, target_atoms)) {
        return false;
    }
    std::set<Edge> reached_edges;
    for (const AtomicEdge& atom : target_atoms) {
        reached_edges.insert(atom.key);
    }
    for (PartitionPiece& piece : pool) {
        std::vector<AtomicEdge> atoms;
        if (!atomize_loop_edges(mesh, loops[piece.loop_index], target, preliminary_nodes,
                                atoms)) {
            return false;
        }
        for (const AtomicEdge& atom : atoms) {
            piece.preliminary_atoms.insert(atom.key);
        }
    }

    std::vector<bool> selected(pool.size(), false);
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < pool.size(); ++i) {
            if (selected[i]) {
                continue;
            }
            bool connected = false;
            for (const Edge& candidate_edge : pool[i].preliminary_atoms) {
                if (reached_edges.contains(candidate_edge)) {
                    connected = true;
                    break;
                }
            }
            if (connected) {
                selected[i] = true;
                reached_edges.insert(pool[i].preliminary_atoms.begin(),
                                     pool[i].preliminary_atoms.end());
                changed = true;
            }
        }
    }

    std::set<std::uint32_t> participating_nodes(target_loop.begin(), target_loop.end());
    double covered_area = 0.0;
    std::size_t piece_count = 0;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (selected[i]) {
            ++piece_count;
            covered_area += pool[i].area;
            participating_nodes.insert(loops[pool[i].loop_index].begin(),
                                       loops[pool[i].loop_index].end());
        }
    }
    if (piece_count == 0 || std::abs(covered_area - target.area) > target.area_tolerance) {
        return false;
    }

    if (!atomize_loop_edges(mesh, target_loop, target, participating_nodes, target_atoms)) {
        return false;
    }
    std::map<Edge, int> target_directions;
    for (const AtomicEdge& atom : target_atoms) {
        if (!target_directions.try_emplace(atom.key, atom.direction).second) {
            return false;
        }
    }

    struct Incidence {
        int count = 0;
        int direction_balance = 0;
    };
    std::map<Edge, Incidence> incidence;
    std::vector<std::vector<AtomicEdge>> piece_atoms(pool.size());
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (!selected[i]) {
            continue;
        }
        if (!atomize_loop_edges(mesh, loops[pool[i].loop_index], target, participating_nodes,
                                piece_atoms[i])) {
            return false;
        }
        for (const AtomicEdge& atom : piece_atoms[i]) {
            Incidence& record = incidence[atom.key];
            ++record.count;
            record.direction_balance += atom.direction;
        }
    }
    for (const auto& [boundary_edge, target_direction] : target_directions) {
        const auto it = incidence.find(boundary_edge);
        if (it == incidence.end() || it->second.count != 1 ||
            it->second.direction_balance != -target_direction) {
            return false;
        }
    }
    for (const auto& [piece_edge, record] : incidence) {
        const auto boundary = target_directions.find(piece_edge);
        if (boundary != target_directions.end()) {
            if (record.count != 1 || record.direction_balance != -boundary->second) {
                return false;
            }
        } else if (record.count != 2 || record.direction_balance != 0) {
            return false;
        }
    }

    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (!selected[i]) {
            continue;
        }
        for (std::size_t j = i + 1; j < pool.size(); ++j) {
            if (!selected[j]) {
                continue;
            }
            for (const AtomicEdge& a : piece_atoms[i]) {
                for (const AtomicEdge& b : piece_atoms[j]) {
                    if (a.from == b.from || a.from == b.to || a.to == b.from || a.to == b.to) {
                        continue;
                    }
                    if (proper_segments_cross(project_node(mesh, a.from, target),
                                              project_node(mesh, a.to, target),
                                              project_node(mesh, b.from, target),
                                              project_node(mesh, b.to, target),
                                              target.length_tolerance)) {
                        return false;
                    }
                }
            }
            for (const Eigen::Vector2d& point : pool[i].projected) {
                if (point_strictly_inside_polygon(point, pool[j].projected,
                                                  target.length_tolerance)) {
                    return false;
                }
            }
            for (const Eigen::Vector2d& point : pool[j].projected) {
                if (point_strictly_inside_polygon(point, pool[i].projected,
                                                  target.length_tolerance)) {
                    return false;
                }
            }
        }
    }

    active[target_index] = false;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (selected[i]) {
            active[pool[i].loop_index] = false;
        }
    }
    return true;
}

bool triangle_has_partition_hint(const NodalMesh& mesh, std::size_t target_index,
                                 const std::vector<Loop>& loops, const EdgeOwners& edge_owners,
                                 const NodeNeighbors& neighbors) {
    const Loop& target_loop = loops[target_index];
    Projection target;
    if (target_loop.size() != 3 || !project_loop(mesh, target_loop, target)) {
        return false;
    }
    for (std::size_t i = 0; i < target_loop.size(); ++i) {
        const std::uint32_t a = target_loop[i];
        const std::uint32_t b = target_loop[(i + 1) % target_loop.size()];
        const Eigen::Vector2d projected_a = project_node(mesh, a, target);
        const Eigen::Vector2d projected_b = project_node(mesh, b, target);

        const auto adjacent = neighbors.find(a);
        if (adjacent != neighbors.end()) {
            for (const std::uint32_t middle : adjacent->second) {
                if (middle == a || middle == b ||
                    !edge_has_other_owner(edge_owners, edge(a, middle), target_index) ||
                    !edge_has_other_owner(edge_owners, edge(middle, b), target_index)) {
                    continue;
                }
                const Eigen::Vector3d delta = mesh.nodes[middle] - target.origin;
                const Eigen::Vector2d projected_middle = project_node(mesh, middle, target);
                if (std::abs(delta.dot(target.normal)) <= target.length_tolerance &&
                    point_on_segment(projected_middle, projected_a, projected_b,
                                     target.length_tolerance) &&
                    (projected_middle - projected_a).norm() > target.length_tolerance &&
                    (projected_middle - projected_b).norm() > target.length_tolerance) {
                    return true;
                }
            }
        }

        const auto exact_edge_owners = edge_owners.find(edge(a, b));
        if (exact_edge_owners == edge_owners.end()) {
            continue;
        }
        for (const std::size_t owner : exact_edge_owners->second) {
            if (owner == target_index) {
                continue;
            }
            for (const std::uint32_t node : loops[owner]) {
                if (node == a || node == b) {
                    continue;
                }
                const Eigen::Vector3d delta = mesh.nodes[node] - target.origin;
                if (std::abs(delta.dot(target.normal)) <= target.length_tolerance &&
                    point_strictly_inside_polygon(project_node(mesh, node, target),
                                                  target.points, target.length_tolerance)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace polymesh::fea::detail::boundary
