// SPDX-License-Identifier: BSD-3-Clause
#include "fea/boundary_faces.hpp"
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace polymesh::fea {
namespace {

using Loop = std::vector<std::uint32_t>;

struct Edge {
    std::uint32_t a = 0;
    std::uint32_t b = 0;

    friend bool operator<(const Edge& lhs, const Edge& rhs) {
        return lhs.a < rhs.a || (lhs.a == rhs.a && lhs.b < rhs.b);
    }
};

Edge edge(std::uint32_t a, std::uint32_t b) { return a < b ? Edge{a, b} : Edge{b, a}; }

void collect_element_loops(const NodalElement& el, std::vector<Loop>& loops) {
    const auto& n = el.nodes;
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        loops.push_back({a, b, c, d});
    };
    auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        loops.push_back({a, b, c});
    };
    switch (el.type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        if (n.size() < 4) {
            return;
        }
        tri(n[0], n[2], n[1]);
        tri(n[0], n[1], n[3]);
        tri(n[0], n[3], n[2]);
        tri(n[1], n[2], n[3]);
        break;
    case ElementType::kHex8:
    case ElementType::kHex20:
        if (n.size() < 8) {
            return;
        }
        quad(n[0], n[3], n[2], n[1]);
        quad(n[4], n[5], n[6], n[7]);
        quad(n[0], n[1], n[5], n[4]);
        quad(n[1], n[2], n[6], n[5]);
        quad(n[2], n[3], n[7], n[6]);
        quad(n[3], n[0], n[4], n[7]);
        break;
    case ElementType::kPrism6:
        if (n.size() < 6) {
            return;
        }
        tri(n[0], n[2], n[1]);
        tri(n[3], n[4], n[5]);
        quad(n[0], n[1], n[4], n[3]);
        quad(n[1], n[2], n[5], n[4]);
        quad(n[2], n[0], n[3], n[5]);
        break;
    case ElementType::kPyramid5:
        if (n.size() < 5) {
            return;
        }
        quad(n[0], n[1], n[2], n[3]);
        tri(n[0], n[1], n[4]);
        tri(n[1], n[2], n[4]);
        tri(n[2], n[3], n[4]);
        tri(n[3], n[0], n[4]);
        break;
    case ElementType::kPolyVem:
        for (const auto& face : el.faces) {
            if (face.size() < 3) {
                continue;
            }
            Loop loop;
            loop.reserve(face.size());
            bool valid = true;
            for (const std::uint32_t local : face) {
                if (local >= n.size()) {
                    valid = false;
                    break;
                }
                loop.push_back(n[local]);
            }
            if (valid) {
                loops.push_back(std::move(loop));
            }
        }
        break;
    }
}

bool sanitize_loop(const NodalMesh& mesh, Loop& loop) {
    Loop clean;
    clean.reserve(loop.size());
    for (const std::uint32_t node : loop) {
        if (node >= mesh.nodes.size()) {
            return false;
        }
        if (clean.empty() || clean.back() != node) {
            clean.push_back(node);
        }
    }
    if (clean.size() > 1 && clean.front() == clean.back()) {
        clean.pop_back();
    }
    if (clean.size() < 3) {
        return false;
    }
    Loop key = clean;
    std::sort(key.begin(), key.end());
    key.erase(std::unique(key.begin(), key.end()), key.end());
    if (key.size() != clean.size()) {
        return false;
    }
    loop = std::move(clean);
    return true;
}

Loop loop_key(const Loop& loop) {
    Loop key = loop;
    std::sort(key.begin(), key.end());
    return key;
}

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

double cross_2d(const Eigen::Vector2d& a, const Eigen::Vector2d& b, const Eigen::Vector2d& c) {
    const Eigen::Vector2d ab = b - a;
    const Eigen::Vector2d ac = c - a;
    return ab.x() * ac.y() - ab.y() * ac.x();
}

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

using EdgeOwners = std::map<Edge, std::vector<std::size_t>>;
using NodeNeighbors = std::map<std::uint32_t, std::set<std::uint32_t>>;

bool edge_has_other_owner(const EdgeOwners& owners, const Edge& candidate,
                          std::size_t target_index) {
    const auto found = owners.find(candidate);
    if (found == owners.end()) {
        return false;
    }
    return std::any_of(found->second.begin(), found->second.end(),
                       [&](std::size_t owner) { return owner != target_index; });
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

std::vector<Loop> resolve_boundary_loops(const NodalMesh& mesh) {
    std::vector<Loop> loops;
    std::vector<Loop> element_loops;
    for (const NodalElement& element : mesh.elements) {
        element_loops.clear();
        collect_element_loops(element, element_loops);
        for (Loop& loop : element_loops) {
            if (sanitize_loop(mesh, loop)) {
                loops.push_back(std::move(loop));
            }
        }
    }

    std::vector<bool> active(loops.size(), true);
    std::map<Loop, std::vector<std::size_t>> exact_owners;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        exact_owners[loop_key(loops[i])].push_back(i);
    }
    for (const auto& [key, owners] : exact_owners) {
        (void)key;
        if (owners.size() > 1) {
            for (const std::size_t owner : owners) {
                active[owner] = false;
            }
        }
    }

    EdgeOwners edge_owners;
    NodeNeighbors neighbors;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (!active[i]) {
            continue;
        }
        for (std::size_t corner = 0; corner < loops[i].size(); ++corner) {
            const std::uint32_t a = loops[i][corner];
            const std::uint32_t b = loops[i][(corner + 1) % loops[i].size()];
            edge_owners[edge(a, b)].push_back(i);
            neighbors[a].insert(b);
            neighbors[b].insert(a);
        }
    }

    // A loop deactivated above is a genuinely interior face: it was matched
    // exactly by another element's face, so both copies leave. What follows is
    // different -- `suppress_opposing_partition` cancels a coarse face against
    // the several finer faces that tile it from the other side, which is a
    // GEOMETRIC judgement and can therefore be wrong. When it is, a real
    // exterior facet disappears and the shell has a hole in it.
    std::vector<bool> paired_out(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        paired_out[i] = !active[i];
    }

    std::vector<std::size_t> partition_targets;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (active[i] &&
            (loops[i].size() > 3 ||
             triangle_has_partition_hint(mesh, i, loops, edge_owners, neighbors))) {
            partition_targets.push_back(i);
        }
    }
    std::sort(partition_targets.begin(), partition_targets.end(),
              [&](std::size_t a, std::size_t b) { return loops[a].size() > loops[b].size(); });
    for (const std::size_t target : partition_targets) {
        suppress_opposing_partition(mesh, target, loops, active);
    }

    // So the suppression proposes and the shell disposes. Revive the minimum
    // that closes it: only loops this step removed are eligible, never the
    // exactly-paired interior faces, and reviving strictly reduces the number
    // of once-used edges, so this terminates. Measured on sphere_box_s0 at
    // h=0.0036 m with --mesher graded, where the tet complex is watertight
    // (0 torn edges, no face used more than twice, 11036 free faces over 88612
    // tets) and this function still reported 6 open edges.
    for (int repair = 0; repair < 8; ++repair) {
        std::map<Edge, int> edge_use;
        for (std::size_t i = 0; i < loops.size(); ++i) {
            if (!active[i]) {
                continue;
            }
            for (std::size_t c = 0; c < loops[i].size(); ++c) {
                ++edge_use[edge(loops[i][c], loops[i][(c + 1) % loops[i].size()])];
            }
        }
        std::set<Edge> torn;
        for (const auto& [e, count] : edge_use) {
            if (count != 2) {
                torn.insert(e);
            }
        }
        if (torn.empty()) {
            break;
        }
        std::size_t revived = 0;
        for (std::size_t i = 0; i < loops.size(); ++i) {
            if (active[i] || paired_out[i]) {
                continue;
            }
            bool touches = false;
            for (std::size_t c = 0; c < loops[i].size() && !touches; ++c) {
                touches = torn.count(edge(loops[i][c], loops[i][(c + 1) % loops[i].size()])) > 0;
            }
            if (touches) {
                active[i] = true;
                ++revived;
            }
        }
        if (revived == 0) {
            break;
        }
    }

    std::vector<Loop> boundary;
    boundary.reserve(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (active[i]) {
            boundary.push_back(std::move(loops[i]));
        }
    }
    return boundary;
}

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

bool triangulate_loop(const NodalMesh& mesh, const Loop& loop,
                      std::vector<std::array<std::uint32_t, 3>>& triangles) {
    Projection projection;
    if (!project_loop(mesh, loop, projection)) {
        return false;
    }
    return triangulate_star(mesh, loop, projection, triangles) ||
           triangulate_ears(mesh, loop, projection, triangles);
}

/// Mid-edge node of every quadratic element edge, keyed by its corner pair.
/// Node orders are the canonical ones documented on `NodalElement`.
std::map<Edge, std::uint32_t> quadratic_edge_mids(const NodalMesh& mesh) {
    static constexpr std::array<std::array<std::size_t, 2>, 6> kTet10Edges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    static constexpr std::array<std::array<std::size_t, 2>, 12> kHex20Edges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5},
         {2, 6}, {3, 7}}};
    std::map<Edge, std::uint32_t> mids;
    for (const auto& el : mesh.elements) {
        if (el.type == ElementType::kTet10 && el.nodes.size() >= 10) {
            for (std::size_t e = 0; e < kTet10Edges.size(); ++e) {
                mids.emplace(edge(el.nodes[kTet10Edges[e][0]], el.nodes[kTet10Edges[e][1]]),
                             el.nodes[4 + e]);
            }
        } else if (el.type == ElementType::kHex20 && el.nodes.size() >= 20) {
            for (std::size_t e = 0; e < kHex20Edges.size(); ++e) {
                mids.emplace(edge(el.nodes[kHex20Edges[e][0]], el.nodes[kHex20Edges[e][1]]),
                             el.nodes[8 + e]);
            }
        }
    }
    return mids;
}

/// Split one boundary loop through its mid-edge nodes, so a curved face draws
/// curved. Emits only nodes that already exist and already sit on the exact
/// B-rep (`pipeline::project_quadratic_boundary_mids` put them there), so no
/// interpolated vertex is invented and every sub-facet still carries real
/// nodal data for result colouring.
///
///   triangle a-b-c   -> 4 triangles through (ab, bc, ca)
///   quad a-b-c-d     -> 4 corner triangles + the central quad (ab, bc, cd, da)
///
/// Returns false when the loop is not a quadratic face, leaving it untouched.
bool subdivide_curved_loop(const Loop& loop, const std::map<Edge, std::uint32_t>& mids,
                           std::vector<Loop>& out) {
    if (loop.size() != 3 && loop.size() != 4) {
        return false;
    }
    std::array<std::uint32_t, 4> mid{};
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto found = mids.find(edge(loop[i], loop[(i + 1) % loop.size()]));
        if (found == mids.end()) {
            // A mixed-p mesh has linear cells next to quadratic ones. Refining
            // only some edges of a face would tear it, so leave the face whole.
            return false;
        }
        mid[i] = found->second;
    }
    if (loop.size() == 3) {
        out.push_back({loop[0], mid[0], mid[2]});
        out.push_back({mid[0], loop[1], mid[1]});
        out.push_back({mid[2], mid[1], loop[2]});
        out.push_back({mid[0], mid[1], mid[2]});
        return true;
    }
    out.push_back({loop[0], mid[0], mid[3]});
    out.push_back({mid[0], loop[1], mid[1]});
    out.push_back({mid[1], loop[2], mid[2]});
    out.push_back({mid[2], loop[3], mid[3]});
    out.push_back({mid[0], mid[1], mid[2], mid[3]});
    return true;
}

} // namespace

std::vector<std::array<std::uint32_t, 4>> extract_boundary_faces(const NodalMesh& mesh) {
    std::vector<std::array<std::uint32_t, 4>> boundary;
    for (const Loop& loop : resolve_boundary_loops(mesh)) {
        // A 3- or 4-node loop IS the face; there is nothing to decide. The
        // projection used to be computed here, thrown away, and its success
        // used as a filter -- so a free face that happened to be degenerate
        // (collinear, zero area) was silently dropped, punching a hole in the
        // shell that the traction integral, the fidelity sampler and the
        // renderer all consume. Measured on sphere_box_s0 at h=0.0036 m with
        // --mesher graded: the tet complex was watertight and this function
        // reported 6 open edges. Nothing is lost by keeping the face: a
        // zero-area face integrates to zero (`traction.cpp` guards the
        // zero-length normal), and the topology stays complete.
        if (loop.size() == 3) {
            boundary.push_back({loop[0], loop[1], loop[2], loop[2]});
        } else if (loop.size() == 4) {
            boundary.push_back({loop[0], loop[1], loop[2], loop[3]});
        } else {
            std::vector<std::array<std::uint32_t, 3>> triangles;
            if (!triangulate_loop(mesh, loop, triangles)) {
                // Star and ear clipping both failed, which means the polygon is
                // degenerate rather than merely awkward. Fan it anyway: a
                // degenerate fan is zero-area and harmless, a missing facet is
                // a hole.
                triangles.clear();
                for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
                    triangles.push_back({loop[0], loop[k], loop[k + 1]});
                }
            }
            for (const auto& triangle : triangles) {
                boundary.push_back({triangle[0], triangle[1], triangle[2], triangle[2]});
            }
        }
    }
    return boundary;
}

std::vector<std::vector<std::uint32_t>> extract_boundary_polys(const NodalMesh& mesh) {
    auto loops = resolve_boundary_loops(mesh);
    // Quadratic cells used to be faceted from their CORNER nodes alone here, so
    // a rim whose mid-edge nodes had been projected exactly onto the B-rep still
    // drew as the straight chord — the curvature was computed, paid for, and
    // then thrown away one layer before the screen. Split those faces through
    // the mid-edge nodes they already own.
    const auto mids = quadratic_edge_mids(mesh);
    if (mids.empty()) {
        return loops;
    }
    std::vector<Loop> curved;
    curved.reserve(loops.size() * 4);
    for (const auto& loop : loops) {
        if (!subdivide_curved_loop(loop, mids, curved)) {
            curved.push_back(loop);
        }
    }
    return curved;
}

} // namespace polymesh::fea
