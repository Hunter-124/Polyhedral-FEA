// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/poly_mesh.hpp"

#include "poly_mesh_geometry.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <vector>

namespace polymesh::mesh {

using detail::coplanar_polygons_overlap;
using detail::face_geometry;
using detail::FaceGeometry;
using detail::point_in_cell_strict;
using detail::polygon_is_simple;
using detail::segment_hits_face_interior;
using detail::triangulate_simple_polygon;

void PolyMesh::check_validity() const {
    const auto nv = static_cast<std::uint32_t>(vertices.size());
    const auto nc = static_cast<std::uint32_t>(cells.size());
    std::vector<int> face_cell_references(faces.size(), 0);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.vertices.size() < 3) {
            throw ValidityError(std::format("face {} has fewer than 3 vertices", f));
        }
        for (const auto v : face.vertices) {
            if (v >= nv) {
                throw ValidityError(
                    std::format("face {} references out-of-range vertex {}", f, v));
            }
        }
        if (face.owner >= nc) {
            throw ValidityError(
                std::format("face {} references out-of-range owner cell {}", f, face.owner));
        }
        if (face.neighbour && *face.neighbour >= nc) {
            throw ValidityError(std::format(
                "face {} references out-of-range neighbour cell {}", f, *face.neighbour));
        }
        if (face.neighbour && *face.neighbour == face.owner) {
            throw ValidityError(std::format("face {} has the same owner and neighbour cell {}",
                                            f, face.owner));
        }
    }
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const Cell& cell = cells[c];
        if (cell.faces.size() < 4) {
            throw ValidityError(
                std::format("cell {} has fewer than 4 faces (cannot bound a volume)", c));
        }
        std::set<FaceId> unique_faces;
        for (const auto f : cell.faces) {
            if (f >= faces.size()) {
                throw ValidityError(
                    std::format("cell {} references out-of-range face {}", c, f));
            }
            if (!unique_faces.insert(f).second) {
                throw ValidityError(std::format("cell {} lists face {} more than once", c, f));
            }
            const Face& face = faces[f];
            const auto cid = static_cast<CellId>(c);
            if (face.owner != cid && face.neighbour != cid) {
                throw ValidityError(std::format(
                    "cell {} lists face {} which does not reference it back", c, f));
            }
            ++face_cell_references[f];
        }
    }
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const int expected = faces[f].neighbour ? 2 : 1;
        if (face_cell_references[f] != expected) {
            throw ValidityError(
                std::format("face {} is listed by {} cells but ownership requires {}", f,
                            face_cell_references[f], expected));
        }
    }
}

void PolyMesh::triangulate_boundary_incident_faces() {
    check_validity();
    std::vector<char> boundary_vertex(vertices.size(), 0);
    for (const Face& face : faces) {
        if (face.neighbour) {
            continue;
        }
        for (const VertexId vertex : face.vertices) {
            boundary_vertex[vertex] = 1;
        }
    }

    std::vector<Face> triangulated;
    triangulated.reserve(faces.size());
    std::vector<std::vector<FaceId>> cell_faces(cells.size());
    const auto append = [&](Face face) {
        const FaceId id = static_cast<FaceId>(triangulated.size());
        cell_faces[face.owner].push_back(id);
        if (face.neighbour) {
            cell_faces[*face.neighbour].push_back(id);
        }
        triangulated.push_back(std::move(face));
    };
    for (const Face& face : faces) {
        const bool touches_boundary =
            std::any_of(face.vertices.begin(), face.vertices.end(),
                        [&](VertexId vertex) { return boundary_vertex[vertex] != 0; });
        if (!touches_boundary || face.vertices.size() <= 3) {
            append(face);
            continue;
        }
        const auto triangles = triangulate_simple_polygon(*this, face);
        if (triangles.size() + 2 != face.vertices.size()) {
            throw ValidityError(
                "cannot triangulate a boundary-incident polygon without degeneracy");
        }
        for (const auto& triangle : triangles) {
            append(Face{.vertices = {triangle[0], triangle[1], triangle[2]},
                        .owner = face.owner,
                        .neighbour = face.neighbour});
        }
    }
    faces = std::move(triangulated);
    for (std::size_t c = 0; c < cells.size(); ++c) {
        cells[c].faces = std::move(cell_faces[c]);
    }
}

void PolyMesh::check_geometry() const {
    check_validity();
    if (vertices.empty()) {
        return;
    }

    Eigen::Vector3d mesh_min = vertices.front();
    Eigen::Vector3d mesh_max = vertices.front();
    for (const Eigen::Vector3d& vertex : vertices) {
        mesh_min = mesh_min.cwiseMin(vertex);
        mesh_max = mesh_max.cwiseMax(vertex);
    }
    const double mesh_scale = std::max((mesh_max - mesh_min).norm(), 1e-30);
    const double mesh_linear_tol = 2e-9 * mesh_scale;

    std::vector<FaceGeometry> geometry;
    geometry.reserve(faces.size());
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        std::set<VertexId> unique_vertices(face.vertices.begin(), face.vertices.end());
        if (unique_vertices.size() != face.vertices.size()) {
            throw ValidityError(
                std::format("face {} repeats a vertex in its polygon loop", f));
        }
        FaceGeometry fg = face_geometry(*this, face);
        const double area_tol =
            1e-14 * std::max(fg.diameter * fg.diameter, mesh_scale * mesh_scale * 1e-24);
        if (!(fg.diameter > 0.0) || !(fg.area.norm() > area_tol)) {
            throw ValidityError(std::format("face {} has zero geometric area", f));
        }
        const Eigen::Vector3d normal = fg.area.normalized();
        const double planarity_tol = 1e-8 * fg.diameter + mesh_linear_tol;
        for (const VertexId vertex : face.vertices) {
            const double offset = std::abs((vertices[vertex] - fg.centroid).dot(normal));
            if (offset > planarity_tol) {
                throw ValidityError(
                    std::format("face {} is nonplanar by {:.3e} m (tolerance {:.3e} m)", f,
                                offset, planarity_tol));
            }
        }
        if (!polygon_is_simple(*this, face, fg)) {
            throw ValidityError(
                std::format("face {} has a self-intersecting polygon loop", f));
        }
        geometry.push_back(fg);
    }

    // Closed manifold exterior: every undirected edge of the boundary
    // triangulation appears in exactly two exterior faces.
    std::map<std::pair<VertexId, VertexId>, int> boundary_edge_count;
    for (const Face& face : faces) {
        if (face.neighbour) {
            continue;
        }
        for (std::size_t i = 0; i < face.vertices.size(); ++i) {
            VertexId a = face.vertices[i];
            VertexId b = face.vertices[(i + 1) % face.vertices.size()];
            if (a > b) {
                std::swap(a, b);
            }
            ++boundary_edge_count[{a, b}];
        }
    }
    for (const auto& [edge, count] : boundary_edge_count) {
        if (count != 2) {
            throw ValidityError(std::format(
                "boundary edge ({},{}) appears {} times (want 2 for closed manifold)",
                edge.first, edge.second, count));
        }
    }

    struct EdgeUse {
        int count = 0;
        int direction_balance = 0;
        std::vector<std::size_t> incident_faces;
    };
    std::vector<Eigen::Vector3d> cell_mins(cells.size());
    std::vector<Eigen::Vector3d> cell_maxs(cells.size());
    std::vector<std::vector<VertexId>> cell_vertex_ids(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const CellId cid = static_cast<CellId>(c);
        const Cell& cell = cells[c];
        std::map<std::pair<VertexId, VertexId>, EdgeUse> edge_uses;
        std::set<VertexId> cell_vertices;
        for (std::size_t local_face = 0; local_face < cell.faces.size(); ++local_face) {
            const Face& face = faces[cell.faces[local_face]];
            const bool owner = face.owner == cid;
            for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                const std::size_t next = (i + 1) % face.vertices.size();
                const VertexId a = owner ? face.vertices[i] : face.vertices[next];
                const VertexId b = owner ? face.vertices[next] : face.vertices[i];
                const auto key = std::minmax(a, b);
                EdgeUse& use = edge_uses[{key.first, key.second}];
                ++use.count;
                use.direction_balance += a < b ? 1 : -1;
                use.incident_faces.push_back(local_face);
                cell_vertices.insert(a);
                cell_vertices.insert(b);
            }
        }
        if (cell_vertices.size() < 4) {
            throw ValidityError(std::format("cell {} has fewer than 4 geometric vertices", c));
        }
        std::vector<std::vector<std::size_t>> adjacency(cell.faces.size());
        for (const auto& [edge, use] : edge_uses) {
            if (use.count != 2 || use.direction_balance != 0 ||
                use.incident_faces.size() != 2) {
                throw ValidityError(
                    std::format("cell {} edge ({},{}) has {} uses with direction balance {}",
                                c, edge.first, edge.second, use.count, use.direction_balance));
            }
            const std::size_t a = use.incident_faces[0];
            const std::size_t b = use.incident_faces[1];
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
        std::vector<char> visited(cell.faces.size(), 0);
        std::vector<std::size_t> stack{0};
        visited[0] = 1;
        std::size_t n_visited = 0;
        while (!stack.empty()) {
            const std::size_t current = stack.back();
            stack.pop_back();
            ++n_visited;
            for (const std::size_t next : adjacency[current]) {
                if (!visited[next]) {
                    visited[next] = 1;
                    stack.push_back(next);
                }
            }
        }
        if (n_visited != cell.faces.size()) {
            throw ValidityError(
                std::format("cell {} has disconnected closed-shell components", c));
        }

        for (std::size_t i = 0; i < cell.faces.size(); ++i) {
            const FaceId fa_id = cell.faces[i];
            const Face& fa = faces[fa_id];
            for (std::size_t j = i + 1; j < cell.faces.size(); ++j) {
                const FaceId fb_id = cell.faces[j];
                const Face& fb = faces[fb_id];
                const Eigen::Vector3d face_overlap =
                    geometry[fa_id].max.cwiseMin(geometry[fb_id].max) -
                    geometry[fa_id].min.cwiseMax(geometry[fb_id].min);
                if ((face_overlap.array() < -mesh_linear_tol).any()) {
                    continue;
                }
                if (coplanar_polygons_overlap(*this, fa, geometry[fa_id], fb, geometry[fb_id],
                                              mesh_linear_tol)) {
                    throw ValidityError(std::format(
                        "cell {} faces {} and {} overlap in one plane", c, fa_id, fb_id));
                }
                bool intersects = false;
                for (std::size_t e = 0; e < fa.vertices.size() && !intersects; ++e) {
                    intersects = segment_hits_face_interior(
                        vertices[fa.vertices[e]],
                        vertices[fa.vertices[(e + 1) % fa.vertices.size()]], *this, fb,
                        geometry[fb_id], mesh_linear_tol);
                }
                for (std::size_t e = 0; e < fb.vertices.size() && !intersects; ++e) {
                    intersects = segment_hits_face_interior(
                        vertices[fb.vertices[e]],
                        vertices[fb.vertices[(e + 1) % fb.vertices.size()]], *this, fa,
                        geometry[fa_id], mesh_linear_tol);
                }
                if (intersects) {
                    throw ValidityError(
                        std::format("cell {} faces {} and {} intersect away from shared edges",
                                    c, fa_id, fb_id));
                }
            }
        }

        Eigen::Vector3d cell_min = vertices[*cell_vertices.begin()];
        Eigen::Vector3d cell_max = cell_min;
        for (const VertexId vertex : cell_vertices) {
            cell_min = cell_min.cwiseMin(vertices[vertex]);
            cell_max = cell_max.cwiseMax(vertices[vertex]);
        }
        cell_mins[c] = cell_min;
        cell_maxs[c] = cell_max;
        cell_vertex_ids[c].assign(cell_vertices.begin(), cell_vertices.end());
        double volume = 0.0;
        for (const FaceId face_id : cell.faces) {
            const double sign = faces[face_id].owner == cid ? 1.0 : -1.0;
            volume += sign *
                      (geometry[face_id].centroid - cell_min).dot(geometry[face_id].area) /
                      3.0;
        }
        const double cell_scale = std::max((cell_max - cell_min).norm(), 1e-30);
        const double volume_tol = 1e-14 * cell_scale * cell_scale * cell_scale;
        if (!(volume > volume_tol)) {
            throw ValidityError(std::format(
                "cell {} has non-positive or collapsed volume {:.3e} m^3", c, volume));
        }
        if (cell.kind == CellKind::kTet && cell_vertices.size() != 4) {
            throw ValidityError(
                std::format("tet cell {} does not reference exactly 4 unique vertices", c));
        }
    }

    // A valid cell shell says nothing about how distinct cells occupy space.
    // Prune disjoint (and merely touching) cell boxes, then reject positive-area
    // face overlap, transverse face crossings, and strict containment.
    std::vector<std::size_t> cell_order(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) {
        cell_order[c] = c;
    }
    std::sort(cell_order.begin(), cell_order.end(), [&](std::size_t a, std::size_t b) {
        return cell_mins[a].x() < cell_mins[b].x();
    });
    for (std::size_t ai = 0; ai < cell_order.size(); ++ai) {
        const std::size_t a = cell_order[ai];
        for (std::size_t bi = ai + 1; bi < cell_order.size(); ++bi) {
            const std::size_t b = cell_order[bi];
            if (cell_mins[b].x() >= cell_maxs[a].x()) {
                break;
            }
            const Eigen::Vector3d overlap =
                cell_maxs[a].cwiseMin(cell_maxs[b]) - cell_mins[a].cwiseMax(cell_mins[b]);
            const double pair_scale = std::max((cell_maxs[a] - cell_mins[a]).norm(),
                                               (cell_maxs[b] - cell_mins[b]).norm());
            const double pair_linear_tol =
                std::max(mesh_linear_tol, 2e-9 * std::max(pair_scale, 1e-30));
            if ((overlap.array() <= pair_linear_tol).any()) {
                continue;
            }

            const bool topological_neighbours =
                std::any_of(cells[a].faces.begin(), cells[a].faces.end(), [&](FaceId face_id) {
                    return std::find(cells[b].faces.begin(), cells[b].faces.end(), face_id) !=
                           cells[b].faces.end();
                });
            for (const FaceId fa_id : cells[a].faces) {
                const Face& fa = faces[fa_id];
                for (const FaceId fb_id : cells[b].faces) {
                    if (fa_id == fb_id) {
                        continue; // one topological face shared by legitimate neighbours
                    }
                    const Face& fb = faces[fb_id];
                    const Eigen::Vector3d face_overlap =
                        geometry[fa_id].max.cwiseMin(geometry[fb_id].max) -
                        geometry[fa_id].min.cwiseMax(geometry[fb_id].min);
                    if ((face_overlap.array() < -pair_linear_tol).any()) {
                        continue;
                    }
                    if (coplanar_polygons_overlap(*this, fa, geometry[fa_id], fb,
                                                  geometry[fb_id], pair_linear_tol)) {
                        throw ValidityError(
                            std::format("cells {} and {} have overlapping faces {} and {}", a,
                                        b, fa_id, fb_id));
                    }
                    bool intersects = false;
                    for (std::size_t e = 0; e < fa.vertices.size() && !intersects; ++e) {
                        intersects = segment_hits_face_interior(
                            vertices[fa.vertices[e]],
                            vertices[fa.vertices[(e + 1) % fa.vertices.size()]], *this, fb,
                            geometry[fb_id], pair_linear_tol);
                    }
                    for (std::size_t e = 0; e < fb.vertices.size() && !intersects; ++e) {
                        intersects = segment_hits_face_interior(
                            vertices[fb.vertices[e]],
                            vertices[fb.vertices[(e + 1) % fb.vertices.size()]], *this, fa,
                            geometry[fa_id], pair_linear_tol);
                    }
                    if (intersects) {
                        std::size_t shared_vertices = 0;
                        for (const VertexId va : fa.vertices) {
                            shared_vertices += static_cast<std::size_t>(
                                std::find(fb.vertices.begin(), fb.vertices.end(), va) !=
                                fb.vertices.end());
                        }
                        throw ValidityError(std::format(
                            "cells {} and {} have crossing faces {} and {} "
                            "(topological_neighbours={}, shared_vertices={})",
                            a, b, fa_id, fb_id, topological_neighbours, shared_vertices));
                    }
                }
            }

            // After all face pairs have been proven noncrossing, two connected
            // closed shells are either disjoint or one contains the other.
            // One representative vertex from each shell therefore decides
            // containment. Topological neighbours already share their legal
            // contact face and cannot contain one another without another face
            // crossing, so they need no solid-angle evaluation.
            if (!topological_neighbours) {
                const CellId aid = static_cast<CellId>(a);
                const CellId bid = static_cast<CellId>(b);
                const VertexId a_vertex = cell_vertex_ids[a].front();
                if (point_in_cell_strict(vertices[a_vertex], *this, cells[b], bid, geometry,
                                         pair_linear_tol)) {
                    throw ValidityError(
                        std::format("cell {} contains a vertex of cell {}", b, a));
                }
                const VertexId b_vertex = cell_vertex_ids[b].front();
                if (point_in_cell_strict(vertices[b_vertex], *this, cells[a], aid, geometry,
                                         pair_linear_tol)) {
                    throw ValidityError(
                        std::format("cell {} contains a vertex of cell {}", a, b));
                }
            }
        }
    }
}

} // namespace polymesh::mesh
