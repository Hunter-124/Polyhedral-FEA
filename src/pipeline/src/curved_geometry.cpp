// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "fea/cell_quality.hpp"
#include "fea/element_validity.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/p_elevate.hpp"
#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {

struct QuadraticBoundaryMid {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t mid = 0;
};

std::vector<QuadraticBoundaryMid> quadratic_boundary_mids(const fea::NodalMesh& nodal_mesh) {
    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
                                                                   {1, 2},
                                                                   {2, 3},
                                                                   {3, 0},
                                                                   {4, 5},
                                                                   {5, 6},
                                                                   {6, 7},
                                                                   {7, 4},
                                                                   {0, 4},
                                                                   {1, 5},
                                                                   {2, 6},
                                                                   {3, 7}}};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edge_mids;
    for (const auto& element : nodal_mesh.elements) {
        const bool tet10 = element.type == fea::ElementType::kTet10;
        const bool hex20 = element.type == fea::ElementType::kHex20;
        if (!tet10 && !hex20) {
            continue;
        }
        const std::size_t n_corner = tet10 ? 4 : 8;
        const std::size_t n_mid = tet10 ? 6 : 12;
        if (element.nodes.size() < n_corner + n_mid) {
            continue;
        }
        for (std::size_t edge_index = 0; edge_index < n_mid; ++edge_index) {
            const auto& edge = tet10 ? kTetEdges[edge_index] : kHexEdges[edge_index];
            const auto a = element.nodes[static_cast<std::size_t>(edge[0])];
            const auto b = element.nodes[static_cast<std::size_t>(edge[1])];
            edge_mids[std::minmax(a, b)] = element.nodes[n_corner + edge_index];
        }
    }

    static constexpr std::array<std::array<int, 2>, 3> kTriEdges{{{0, 1}, {1, 2}, {0, 2}}};
    static constexpr std::array<std::array<int, 2>, 4> kQuadEdges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
    std::map<std::uint32_t, QuadraticBoundaryMid> by_mid;
    for (const auto& face : fea::boundary_surface_faces(nodal_mesh)) {
        const bool tri6 = face.type == fea::FaceType::kTri6;
        const bool quad8 = face.type == fea::FaceType::kQuad8;
        if (!tri6 && !quad8) {
            continue;
        }
        const std::size_t n_edges = tri6 ? kTriEdges.size() : kQuadEdges.size();
        for (std::size_t edge_index = 0; edge_index < n_edges; ++edge_index) {
            const auto& edge = tri6 ? kTriEdges[edge_index] : kQuadEdges[edge_index];
            const auto a = face.nodes[static_cast<std::size_t>(edge[0])];
            const auto b = face.nodes[static_cast<std::size_t>(edge[1])];
            const auto it = edge_mids.find(std::minmax(a, b));
            if (it != edge_mids.end()) {
                by_mid.try_emplace(it->second, QuadraticBoundaryMid{a, b, it->second});
            }
        }
    }

    std::vector<QuadraticBoundaryMid> result;
    result.reserve(by_mid.size());
    for (const auto& [mid, edge] : by_mid) {
        (void)mid;
        result.push_back(edge);
    }
    return result;
}

bool quadratic_incident_valid(const fea::NodalMesh& nodal_mesh,
                              const fea::NodalElement& element, std::uint32_t moved_node,
                              const Eigen::Vector3d& saved_position, double volume_epsilon,
                              double jacobian_epsilon) {
    const bool tet10 = element.type == fea::ElementType::kTet10;
    const bool hex20 = element.type == fea::ElementType::kHex20;
    if (!tet10 && !hex20) {
        return false;
    }
    if (element.nodes.size() != (tet10 ? 10 : 20)) {
        return false;
    }

    if (tet10) {
        const auto& a = nodal_mesh.nodes[element.nodes[0]];
        const auto& b = nodal_mesh.nodes[element.nodes[1]];
        const auto& c = nodal_mesh.nodes[element.nodes[2]];
        const auto& d = nodal_mesh.nodes[element.nodes[3]];
        if (!(mesh::validity::tet_signed_volume(a, b, c, d) > volume_epsilon)) {
            return false;
        }
    } else {
        std::array<Eigen::Vector3d, 8> corners{};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            corners[i] = nodal_mesh.nodes[element.nodes[i]];
        }
        for (const auto& corner : mesh::validity::kHexCornerTriples) {
            if (!(mesh::validity::tet_signed_volume(
                      corners[static_cast<std::size_t>(corner[0])],
                      corners[static_cast<std::size_t>(corner[1])],
                      corners[static_cast<std::size_t>(corner[2])],
                      corners[static_cast<std::size_t>(corner[3])]) > volume_epsilon)) {
                return false;
            }
        }
    }

    Eigen::Matrix<double, Eigen::Dynamic, 3> after(element.nodes.size(), 3);
    Eigen::Matrix<double, Eigen::Dynamic, 3> before(element.nodes.size(), 3);
    for (std::size_t i = 0; i < element.nodes.size(); ++i) {
        const auto node = element.nodes[i];
        after.row(static_cast<Eigen::Index>(i)) = nodal_mesh.nodes[node].transpose();
        before.row(static_cast<Eigen::Index>(i)) =
            (node == moved_node ? saved_position : nodal_mesh.nodes[node]).transpose();
    }

    // Use exactly the integration points consumed by element_stiffness. Keeping
    // the rule selection in fea::default_rule makes a future quadrature change
    // update both the acceptance gate and the solver together.
    static const auto tet10_rule = fea::default_rule(fea::ElementType::kTet10);
    static const auto hex20_rule = fea::default_rule(fea::ElementType::kHex20);
    const auto& rule = tet10 ? tet10_rule : hex20_rule;
    for (const auto& qp : rule) {
        const auto shape = fea::eval_shape(element.type, qp.xi);
        const double det_before = (shape.dn.transpose() * before).determinant();
        const double det_after = (shape.dn.transpose() * after).determinant();
        if (!std::isfinite(det_before) || !std::isfinite(det_after) || !(det_before > 0.0) ||
            !(det_after > jacobian_epsilon)) {
            return false;
        }
    }
    const double quality = fea::cell_quality(nodal_mesh, element);
    if (!std::isfinite(quality) || quality < mesh::validity::kCellShapeFloor) {
        return false;
    }
    return true;
}

} // namespace

bool make_boundary_projection(const geom::CadModel& cad, double h,
                              mesh::BoundaryProjectionContext* ctx,
                              std::vector<mesh::BoundarySupport>* provenance,
                              std::shared_ptr<const geom::CadTopology>* topology_out) {
    if (ctx == nullptr || provenance == nullptr || cad.empty()) {
        if (ctx != nullptr) {
            *ctx = {};
        }
        return false;
    }

    // 32 interior samples per edge, not 10: the polyline is the capture test
    // and the arclength parameterization for the feature pin (ADR-0035), and
    // a 10-sample circle has a 4.9%·R chord sag — a fifth of a cell on a small
    // bore, enough to mis-capture a crease node. The pin target itself is
    // always the exact OCC curve projection, so this only sharpens the
    // classification, never the geometry.
    auto topology = std::make_shared<geom::CadTopology>(geom::extract_topology(cad, 32));
    if (topology_out != nullptr) {
        *topology_out = topology;
    }
    const geom::CadModel* cad_ptr = &cad;
    ctx->provenance = provenance;
    ctx->topology = topology;
    ctx->target = [cad_ptr, topology = std::move(topology),
                   h](const Eigen::Vector3d& p,
                      mesh::BoundarySupport& owner) -> std::optional<mesh::BoundaryTarget> {
        std::optional<geom::ProjectResult> exact;
        if (owner.kind == mesh::BoundarySupportKind::kCadVertex) {
            exact = geom::project_point_on_vertex(*cad_ptr, owner.id, p);
        } else if (owner.kind == mesh::BoundarySupportKind::kCadEdge) {
            exact = geom::project_point_on_edge(*cad_ptr, owner.id, p);
        } else if (owner.kind == mesh::BoundarySupportKind::kCadFace) {
            exact = geom::project_point_on_face(*cad_ptr, owner.id, p);
            // Ownership exists so a node cannot drift across a trimmed face or
            // a sharp edge, not so a misclassification can pin it in mid-air.
            // A node that latched a face it cannot reach — classified at its
            // raw lattice site, then found on the far side of a bore rim —
            // would be projected onto that face's nearest trimmed point and
            // ship O(h) off the solid. When the owned projection is more than
            // half a cell away and the free whole-shape projection is
            // materially closer, the classification was wrong: adopt the better
            // face and record it. Edges and vertices stay immutable — those
            // owners are features, not conveniences.
            if (exact && exact->distance > 0.5 * h) {
                if (auto free_target = geom::project_point_on_surface(*cad_ptr, p);
                    free_target && free_target->distance < 0.5 * exact->distance &&
                    free_target->face_id != geom::kInvalidCadSupportId) {
                    owner = {mesh::BoundarySupportKind::kCadFace, free_target->face_id};
                    exact = std::move(free_target);
                }
            }
        } else {
            exact = geom::project_point_on_surface(*cad_ptr, p);
            if (!exact) {
                return std::nullopt;
            }

            const double feature_slack = 0.08 * h;
            // The crease preference exists to capture lattice stair nodes that
            // sit O(h) off the surface near a sharp rim. It must not claim a
            // node whose home face is decisively closer than the crease. The
            // absolute slack alone cannot separate "on the face next to the
            // rim" from "stair node of the rim"; the ratio can — a genuine
            // crease node has the two distances within a small multiple of
            // each other.
            const auto crease_competes = [&](double crease_distance) {
                return crease_distance <= exact->distance + feature_slack &&
                       crease_distance <= 4.0 * exact->distance + 1e-3 * h;
            };
            bool chose_vertex = false;
            const geom::CadVertex* nearest_vertex = nullptr;
            double vertex_distance = std::numeric_limits<double>::infinity();
            for (const auto& vertex : topology->vertices) {
                const double distance = (p - vertex.position).norm();
                if (distance < vertex_distance) {
                    nearest_vertex = &vertex;
                    vertex_distance = distance;
                }
            }
            if (nearest_vertex != nullptr && vertex_distance <= 0.40 * h &&
                crease_competes(vertex_distance)) {
                auto vertex_exact =
                    geom::project_point_on_vertex(*cad_ptr, nearest_vertex->id, p);
                if (vertex_exact) {
                    exact = std::move(vertex_exact);
                    chose_vertex = true;
                    owner = {mesh::BoundarySupportKind::kCadVertex, nearest_vertex->id};
                }
            }

            if (!chose_vertex && owner.kind == mesh::BoundarySupportKind::kUnknown) {
                const auto nearest_edge = geom::closest_edge(*topology, p, true);
                if (nearest_edge && nearest_edge->distance <= 0.55 * h &&
                    crease_competes(nearest_edge->distance)) {
                    auto edge_exact =
                        geom::project_point_on_edge(*cad_ptr, nearest_edge->edge_id, p);
                    if (edge_exact) {
                        exact = std::move(edge_exact);
                        owner = {mesh::BoundarySupportKind::kCadEdge, nearest_edge->edge_id};
                    }
                }
            }

            if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                exact->support_kind == geom::CadSupportKind::kVertex) {
                owner = {mesh::BoundarySupportKind::kCadVertex, exact->support_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                       exact->support_kind == geom::CadSupportKind::kEdge &&
                       exact->support_id < topology->edges.size() &&
                       topology->edges[exact->support_id].feature ==
                           geom::CadEdgeFeature::kSharp) {
                owner = {mesh::BoundarySupportKind::kCadEdge, exact->support_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                       exact->face_id != geom::kInvalidCadSupportId) {
                owner = {mesh::BoundarySupportKind::kCadFace, exact->face_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown) {
                return std::nullopt;
            }
        }
        if (!exact) {
            return std::nullopt;
        }
        return mesh::BoundaryTarget{exact->point, exact->distance};
    };
    return true;
}

std::size_t project_quadratic_boundary_mids(fea::NodalMesh& nodal_mesh,
                                            const geom::CadModel& cad,
                                            mesh::BoundaryProjectionContext* projection,
                                            double h,
                                            std::vector<std::uint32_t>* reverted_nodes,
                                            std::vector<std::uint32_t>* partial_nodes,
                                            const mesh::MirrorFrame* mirror) {
    if (reverted_nodes != nullptr) {
        reverted_nodes->clear();
    }
    if (partial_nodes != nullptr) {
        partial_nodes->clear();
    }
    if (projection == nullptr || !projection->target || cad.empty() || !(h > 0.0)) {
        return 0;
    }
    mesh::fill_progress_phase("quadratic_boundary_census");
    auto boundary_mids = quadratic_boundary_mids(nodal_mesh);
    if (boundary_mids.empty()) {
        return 0;
    }
    // Mirror-canonical visit order. Each mid is projected and then line-searched
    // back against its own incident cells, and those cells are shared, so the
    // order decides which mids keep how much of their projection. Ascending id
    // does not mirror (ADR-0036 Section 9).
    {
        const mesh::MirrorKeyFrame mkey = mesh::mirror_key_frame(nodal_mesh.nodes);
        std::stable_sort(
            boundary_mids.begin(), boundary_mids.end(), [&](const auto& x, const auto& y) {
                if (x.mid >= nodal_mesh.nodes.size() || y.mid >= nodal_mesh.nodes.size()) {
                    return x.mid < y.mid;
                }
                const auto kx = mkey.key(nodal_mesh.nodes[x.mid]);
                const auto ky = mkey.key(nodal_mesh.nodes[y.mid]);
                return kx != ky ? kx < ky : x.mid < y.mid;
            });
    }
    std::unordered_map<std::uint32_t, Eigen::Vector3d> saved_positions;
    saved_positions.reserve(boundary_mids.size());
    for (const auto& edge : boundary_mids) {
        if (edge.mid < nodal_mesh.nodes.size()) {
            saved_positions.try_emplace(edge.mid, nodal_mesh.nodes[edge.mid]);
        }
    }

    std::unordered_map<std::uint32_t, std::vector<std::size_t>> incident;
    incident.reserve(boundary_mids.size());
    for (const auto& edge : boundary_mids) {
        incident.try_emplace(edge.mid);
    }
    for (std::size_t element_index = 0; element_index < nodal_mesh.elements.size();
         ++element_index) {
        mesh::fill_progress_poll(element_index, nodal_mesh.elements.size());
        for (const auto node : nodal_mesh.elements[element_index].nodes) {
            if (auto it = incident.find(node); it != incident.end()) {
                it->second.push_back(element_index);
            }
        }
    }

    const auto owner_of = [&](std::uint32_t node) {
        if (projection->provenance != nullptr && node < projection->provenance->size()) {
            return (*projection->provenance)[node];
        }
        return mesh::BoundarySupport{};
    };
    const auto commit_owner = [&](std::uint32_t node, mesh::BoundarySupport owner) {
        if (projection->provenance == nullptr) {
            return;
        }
        if (projection->provenance->size() <= node) {
            projection->provenance->resize(static_cast<std::size_t>(node) + 1);
        }
        (*projection->provenance)[node] = owner;
    };

    const double volume_epsilon = 1e-14 * h * h * h;
    const double jacobian_epsilon = 1e-8 * h * h * h;
    std::size_t projected = 0;
    mesh::fill_progress_phase("quadratic_boundary_projection");
    std::size_t mids_done = 0;
    for (const auto& edge : boundary_mids) {
        mesh::fill_progress_poll(mids_done++, boundary_mids.size());
        if (edge.a >= nodal_mesh.nodes.size() || edge.b >= nodal_mesh.nodes.size() ||
            edge.mid >= nodal_mesh.nodes.size()) {
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(edge.mid);
            }
            continue;
        }
        const Eigen::Vector3d saved = nodal_mesh.nodes[edge.mid];
        const auto owner_a = owner_of(edge.a);
        const auto owner_b = owner_of(edge.b);
        std::optional<geom::ProjectResult> exact;
        mesh::BoundarySupport direct_owner;
        bool direct = owner_a.kind != mesh::BoundarySupportKind::kUnknown &&
                      owner_a.kind == owner_b.kind && owner_a.id == owner_b.id;
        if (direct) {
            direct_owner = owner_a;
            // Folded query, unfolded answer: the owner id these corners agree on
            // is the canonical-octant entity (ADR-0036 Section 9.2).
            const Eigen::Vector3d query = mesh::mirror_fold(mirror, saved);
            if (owner_a.kind == mesh::BoundarySupportKind::kCadEdge) {
                exact = geom::project_point_on_edge(cad, owner_a.id, query);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadFace) {
                exact = geom::project_point_on_face(cad, owner_a.id, query);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadVertex) {
                exact = geom::project_point_on_vertex(cad, owner_a.id, query);
            } else {
                direct = false;
            }
            if (exact) {
                exact->point = mesh::mirror_unfold(mirror, exact->point, saved);
                exact->distance = (exact->point - saved).norm();
            }
        }

        std::optional<mesh::BoundaryTarget> target;
        if (direct) {
            if (exact) {
                commit_owner(edge.mid, direct_owner);
                target = mesh::BoundaryTarget{exact->point, exact->distance};
            }
        } else {
            // Endpoints that both sit on the same sharp edge own the mid
            // jointly even when their provenance KINDS differ: a rim node is
            // legitimately on the edge and on a face at once, so kind
            // disagreement says nothing. Otherwise the chord mid of a rim edge
            // free-classifies to the face (the chord lies in the face plane)
            // and bows off the rim.
            if (projection->topology != nullptr) {
                const double tol = 1e-3 * h;
                const Eigen::Vector3d qa = mesh::mirror_fold(mirror, nodal_mesh.nodes[edge.a]);
                const Eigen::Vector3d qb = mesh::mirror_fold(mirror, nodal_mesh.nodes[edge.b]);
                double best = std::numeric_limits<double>::infinity();
                std::uint32_t best_id = 0;
                bool found = false;
                for (const auto& topo_edge : projection->topology->edges) {
                    if (topo_edge.feature != geom::CadEdgeFeature::kSharp) {
                        continue;
                    }
                    const auto pa = geom::project_point_on_edge(cad, topo_edge.id, qa);
                    const auto pb = geom::project_point_on_edge(cad, topo_edge.id, qb);
                    if (!pa || !pb) {
                        continue;
                    }
                    const double both = std::max(pa->distance, pb->distance);
                    if (both <= tol && both < best) {
                        best = both;
                        best_id = topo_edge.id;
                        found = true;
                    }
                }
                if (found) {
                    const Eigen::Vector3d query = mesh::mirror_fold(mirror, saved);
                    if (auto on_edge = geom::project_point_on_edge(cad, best_id, query)) {
                        on_edge->point = mesh::mirror_unfold(mirror, on_edge->point, saved);
                        on_edge->distance = (on_edge->point - saved).norm();
                        commit_owner(edge.mid,
                                     mesh::BoundarySupport{mesh::BoundarySupportKind::kCadEdge,
                                                           best_id});
                        target = mesh::BoundaryTarget{on_edge->point, on_edge->distance};
                    }
                }
            }
            if (!target) {
                target = mesh::owned_boundary_projection_target(saved, edge.mid, projection,
                                                                mirror);
            }
        }
        if (!target) {
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(edge.mid);
            }
            continue;
        }

        const auto incident_valid = [&]() {
            if (const auto it = incident.find(edge.mid); it != incident.end()) {
                for (const auto element_index : it->second) {
                    if (!quadratic_incident_valid(nodal_mesh,
                                                  nodal_mesh.elements[element_index], edge.mid,
                                                  saved, volume_epsilon, jacobian_epsilon)) {
                        return false;
                    }
                }
            }
            return true;
        };

        nodal_mesh.nodes[edge.mid] = target->point;
        if (incident_valid()) {
            ++projected;
            continue;
        }

        double valid_fraction = 0.0;
        double invalid_fraction = 1.0;
        const Eigen::Vector3d displacement = target->point - saved;
        for (int step = 0; step < 6; ++step) {
            const double fraction = 0.5 * (valid_fraction + invalid_fraction);
            nodal_mesh.nodes[edge.mid] = saved + fraction * displacement;
            if (incident_valid()) {
                valid_fraction = fraction;
            } else {
                invalid_fraction = fraction;
            }
        }
        if (valid_fraction > 0.0) {
            nodal_mesh.nodes[edge.mid] = saved + valid_fraction * displacement;
            const auto surface_projection =
                geom::project_point_on_surface(cad, nodal_mesh.nodes[edge.mid]);
            if (surface_projection && surface_projection->distance <= 0.02 * h) {
                ++projected;
            } else if (partial_nodes != nullptr) {
                partial_nodes->push_back(edge.mid);
            }
            continue;
        }

        nodal_mesh.nodes[edge.mid] = saved;
        if (reverted_nodes != nullptr) {
            reverted_nodes->push_back(edge.mid);
        }
        continue;
    }
    // Midpoint moves share cells. A sequence can pass every local line search
    // and still make their combined curved mapping unacceptable, so close with
    // a whole-cell rollback and feed those edges to the caller's h-refinement
    // fallback.
    for (int round = 0; round < 4; ++round) {
        mesh::fill_progress_phase("quadratic_quality_pass", round + 1, 4);
        std::size_t checked = 0;
        std::set<std::uint32_t> rollback;
        for (const auto& element : nodal_mesh.elements) {
            mesh::fill_progress_poll(checked++, nodal_mesh.elements.size());
            const double quality = fea::cell_quality(nodal_mesh, element);
            if (fea::element_jacobians_positive(nodal_mesh, element) &&
                std::isfinite(quality) && quality >= mesh::validity::kCellShapeFloor) {
                continue;
            }
            for (const auto node : element.nodes) {
                if (saved_positions.contains(node)) {
                    rollback.insert(node);
                }
            }
        }
        if (rollback.empty()) {
            break;
        }
        bool changed = false;
        for (const auto node : rollback) {
            const auto& saved = saved_positions.at(node);
            if ((nodal_mesh.nodes[node] - saved).squaredNorm() > 0.0) {
                nodal_mesh.nodes[node] = saved;
                changed = true;
            }
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(node);
            }
        }
        if (!changed) {
            break;
        }
    }
    if (reverted_nodes != nullptr) {
        std::sort(reverted_nodes->begin(), reverted_nodes->end());
        reverted_nodes->erase(std::unique(reverted_nodes->begin(), reverted_nodes->end()),
                              reverted_nodes->end());
    }
    if (partial_nodes != nullptr) {
        std::sort(partial_nodes->begin(), partial_nodes->end());
        partial_nodes->erase(std::unique(partial_nodes->begin(), partial_nodes->end()),
                             partial_nodes->end());
    }
    return projected;
}

CurvedGeometryResult curve_volume_geometry(const Model& model, const fea::NodalMesh& source,
                                           double h) {
    CurvedGeometryResult result;
    result.mesh.nodes = source.nodes;
    result.mesh.elements.reserve(source.elements.size());
    for (const auto& element : source.elements) {
        if (element.type != fea::ElementType::kPyramid5 || element.nodes.size() != 5) {
            result.mesh.elements.push_back(element);
            continue;
        }
        const auto& p = element.nodes;
        const int diagonal = mesh::validity::pyramid_split_diagonal(
            source.nodes[p[0]], source.nodes[p[1]], source.nodes[p[2]], source.nodes[p[3]]);
        const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            result.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {a, b, c, p[4]}});
        };
        if (diagonal == 1) {
            emit(p[1], p[2], p[3]);
            emit(p[1], p[3], p[0]);
        } else {
            emit(p[0], p[1], p[2]);
            emit(p[0], p[2], p[3]);
        }
        ++result.n_pyramids_split;
    }

    fea::NodalMesh linear_mesh = result.mesh;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<std::size_t> promote;
        promote.reserve(linear_mesh.elements.size());
        for (std::size_t i = 0; i < linear_mesh.elements.size(); ++i) {
            const auto type = linear_mesh.elements[i].type;
            if (type == fea::ElementType::kTet4 || type == fea::ElementType::kHex8) {
                promote.push_back(i);
            }
        }
        auto elevated = fea::p_elevate_with_constraints(linear_mesh, promote);
        result.mesh = std::move(elevated.mesh);
        const fea::NodalMesh straight_mesh = result.mesh;
        result.constraints = std::move(elevated.constraints);
        result.n_promoted = elevated.n_promoted;

        std::vector<std::uint32_t> reverted;
        std::vector<std::uint32_t> partial;
        std::map<std::uint32_t, std::array<std::uint32_t, 2>> parents;
        for (const auto& edge : quadratic_boundary_mids(result.mesh)) {
            parents.try_emplace(edge.mid, std::array{edge.a, edge.b});
        }
        if (model.cad && h > 0.0) {
            std::vector<mesh::BoundarySupport> provenance;
            mesh::BoundaryProjectionContext projection;
            // The linear mesh handed here is exactly mirror-symmetric on a
            // symmetric part (ADR-0036 §9); the curved promotion must not undo
            // that, so the mid projection is folded like every other one.
            const mesh::MirrorFrame* mirror = model.mirror.any() ? &model.mirror : nullptr;
            if (make_boundary_projection(*model.cad, h, &projection, &provenance)) {
                result.n_projected = project_quadratic_boundary_mids(
                    result.mesh, *model.cad, &projection, h, &reverted, &partial, mirror);
            }
            const auto topology = geom::extract_topology(*model.cad);
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> edge_mid_incidence;
            edge_mid_incidence.reserve(parents.size());
            for (std::size_t ei = 0; ei < result.mesh.elements.size(); ++ei) {
                for (const auto node : result.mesh.elements[ei].nodes) {
                    if (parents.contains(node)) {
                        edge_mid_incidence[node].push_back(ei);
                    }
                }
            }
            // One topology query and one OCC projection per boundary corner —
            // not per quadratic edge. A corner is shared by ~6 boundary edges,
            // so caching its exact sharp-edge owner is what keeps this pass off
            // the mesher's critical path.
            std::unordered_map<std::uint32_t, std::optional<std::uint32_t>> sharp_owner;
            const auto exact_sharp_owner =
                [&](std::uint32_t node) -> std::optional<std::uint32_t> {
                const auto cached = sharp_owner.find(node);
                if (cached != sharp_owner.end()) {
                    return cached->second;
                }
                std::optional<std::uint32_t> owner;
                // Folded, so a corner and its mirror image latch the SAME
                // canonical sharp edge and the mid projection below reproduces
                // mirrored points from it (ADR-0036 Section 9).
                const Eigen::Vector3d query =
                    mesh::mirror_fold(mirror, result.mesh.nodes[node]);
                if (const auto near = geom::closest_edge(topology, query, true)) {
                    if (near->distance <= h) {
                        if (const auto exact = geom::project_point_on_edge(
                                *model.cad, near->edge_id, query)) {
                            if (exact->distance <= 1e-6 * h) {
                                owner = near->edge_id;
                            }
                        }
                    }
                }
                sharp_owner.emplace(node, owner);
                return owner;
            };
            // Mirror-canonical visit order: each pin is accepted against the
            // shared node array, so the order decides which ones survive.
            auto sharp_mids = quadratic_boundary_mids(result.mesh);
            {
                const mesh::MirrorKeyFrame mkey = mesh::mirror_key_frame(result.mesh.nodes);
                std::stable_sort(sharp_mids.begin(), sharp_mids.end(),
                                 [&](const auto& x, const auto& y) {
                                     const auto kx = mkey.key(result.mesh.nodes[x.mid]);
                                     const auto ky = mkey.key(result.mesh.nodes[y.mid]);
                                     return kx != ky ? kx < ky : x.mid < y.mid;
                                 });
            }
            for (const auto& edge : sharp_mids) {
                const auto owner_a = exact_sharp_owner(edge.a);
                if (!owner_a) {
                    continue;
                }
                const auto owner_b = exact_sharp_owner(edge.b);
                if (!owner_b || *owner_a != *owner_b) {
                    continue;
                }
                const Eigen::Vector3d mid_query =
                    mesh::mirror_fold(mirror, result.mesh.nodes[edge.mid]);
                const auto exact =
                    geom::project_point_on_edge(*model.cad, *owner_a, mid_query);
                if (!exact) {
                    continue;
                }
                const Eigen::Vector3d saved_mid = result.mesh.nodes[edge.mid];
                result.mesh.nodes[edge.mid] =
                    mesh::mirror_unfold(mirror, exact->point, saved_mid);
                bool valid = true;
                for (const auto ei : edge_mid_incidence[edge.mid]) {
                    const auto& element = result.mesh.elements[ei];
                    const double quality = fea::cell_quality(result.mesh, element);
                    const double target =
                        std::min(mesh::validity::kCellShapeFloor,
                                 fea::cell_quality(straight_mesh, straight_mesh.elements[ei]));
                    valid = valid && fea::element_jacobians_positive(result.mesh, element) &&
                            std::isfinite(quality) && quality >= target;
                }
                if (!valid) {
                    result.mesh.nodes[edge.mid] = saved_mid;
                    continue;
                }
                ++result.n_projected;
            }
        }
        // Curvature must never make a cell worse, but it also must not be held
        // to a floor the straight promotion already misses: `cell_quality` on a
        // p2 cell is not the corner ratio the mesher gates on, so a cell the
        // mesher shipped at 0.0201 can read 0.0182 straight. Compare each curved
        // cell against its own straight baseline and the shared floor.
        std::vector<double> straight_quality(straight_mesh.elements.size(), 0.0);
        for (std::size_t i = 0; i < straight_mesh.elements.size(); ++i) {
            straight_quality[i] = fea::cell_quality(straight_mesh, straight_mesh.elements[i]);
        }
        bool curved_mesh_valid = true;
        for (std::size_t i = 0; i < result.mesh.elements.size(); ++i) {
            const auto& element = result.mesh.elements[i];
            const double quality = fea::cell_quality(result.mesh, element);
            const double target =
                std::min(mesh::validity::kCellShapeFloor,
                         i < straight_quality.size() ? straight_quality[i] : 0.0);
            curved_mesh_valid = curved_mesh_valid &&
                                fea::element_jacobians_positive(result.mesh, element) &&
                                std::isfinite(quality) && quality >= target;
        }
        if (!curved_mesh_valid) {
            result.mesh = straight_mesh;
            partial.clear();
            reverted.clear();
            for (const auto& [mid, edge] : parents) {
                (void)edge;
                reverted.push_back(mid);
            }
        }
        result.n_partial = partial.size();
        result.n_reverted = reverted.size();
        if ((partial.empty() && reverted.empty()) || attempt == 3) {
            break;
        }

        bool pure_linear_tet = true;
        for (const auto& element : linear_mesh.elements) {
            pure_linear_tet = pure_linear_tet && element.type == fea::ElementType::kTet4;
        }
        if (!pure_linear_tet) {
            break;
        }
        std::set<std::array<std::uint32_t, 2>> offending_edges;
        for (const auto node : partial) {
            if (const auto found = parents.find(node); found != parents.end()) {
                auto edge = found->second;
                std::sort(edge.begin(), edge.end());
                offending_edges.insert(edge);
            }
        }
        for (const auto node : reverted) {
            if (const auto found = parents.find(node); found != parents.end()) {
                auto edge = found->second;
                std::sort(edge.begin(), edge.end());
                offending_edges.insert(edge);
            }
        }
        mesh::TetFillOutput linear;
        linear.nodes = linear_mesh.nodes;
        linear.tets.reserve(linear_mesh.elements.size());
        std::vector<std::size_t> marked;
        for (std::size_t i = 0; i < linear_mesh.elements.size(); ++i) {
            const auto& nodes = linear_mesh.elements[i].nodes;
            std::array<std::uint32_t, 4> tet{nodes[0], nodes[1], nodes[2], nodes[3]};
            linear.tets.push_back(tet);
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    auto edge = std::array{tet[static_cast<std::size_t>(a)],
                                           tet[static_cast<std::size_t>(b)]};
                    std::sort(edge.begin(), edge.end());
                    if (offending_edges.contains(edge)) {
                        marked.push_back(i);
                    }
                }
            }
        }
        std::sort(marked.begin(), marked.end());
        marked.erase(std::unique(marked.begin(), marked.end()), marked.end());
        if (marked.empty()) {
            break;
        }
        // The fallback is opportunistic: if conformal refinement cannot clear
        // these marks (LEB closure can fail on an already-graded 2:1 front), the
        // straight-edged promotion above is still a correct mesh, so abandon the
        // retry instead of failing the whole mesh/solve.
        mesh::LocalRefineStats refine_stats;
        mesh::TetFillOutput refined;
        try {
            refined = mesh::local_refine_tets(linear, marked, &refine_stats, &model.surface);
        } catch (const std::exception&) {
            break;
        }
        if (refine_stats.n_new_nodes == 0) {
            break;
        }
        bool refined_shape_ok = true;
        for (const auto& tet : refined.tets) {
            refined_shape_ok =
                refined_shape_ok &&
                mesh::validity::tet_shape_quality(
                    refined.nodes[tet[0]], refined.nodes[tet[1]], refined.nodes[tet[2]],
                    refined.nodes[tet[3]]) >= mesh::validity::kCellShapeFloor;
        }
        if (!refined_shape_ok) {
            break;
        }
        result.n_h_refined += refine_stats.n_bisections;
        linear_mesh.nodes = std::move(refined.nodes);
        linear_mesh.elements.clear();
        linear_mesh.elements.reserve(refined.tets.size());
        for (const auto& tet : refined.tets) {
            linear_mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
    }
    result.mesh.check_validity();
    // Integrability is absolute: a non-integrable cell here would be a bug in
    // the promotion/projection above, not a mesh the caller can use.
    for (const auto& element : result.mesh.elements) {
        if (!fea::element_jacobians_positive(result.mesh, element)) {
            throw std::runtime_error(
                std::format("curve_volume_geometry: {} cell is not integrable after curving",
                            fea::element_type_name(element.type)));
        }
    }
    return result;
}

} // namespace polymesh::pipeline
