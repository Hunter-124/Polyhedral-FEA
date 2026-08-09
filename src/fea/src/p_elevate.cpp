// SPDX-License-Identifier: BSD-3-Clause
#include "fea/p_elevate.hpp"

#include <array>
#include <format>
#include <map>
#include <utility>

namespace polymesh::fea {
namespace {

using Edge = std::pair<std::uint32_t, std::uint32_t>;

// Canonical mid-edge order — must match fea/nodal_mesh.hpp documentation.
constexpr std::array<std::array<int, 2>, 6> kTetEdges{
    {{{0, 1}}, {{1, 2}}, {{0, 2}}, {{0, 3}}, {{1, 3}}, {{2, 3}}}};
constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{{0, 1}},
                                                        {{1, 2}},
                                                        {{2, 3}},
                                                        {{3, 0}},
                                                        {{4, 5}},
                                                        {{5, 6}},
                                                        {{6, 7}},
                                                        {{7, 4}},
                                                        {{0, 4}},
                                                        {{1, 5}},
                                                        {{2, 6}},
                                                        {{3, 7}}}};
constexpr std::array<std::array<int, 2>, 9> kPrismEdges{
    {{{0, 1}}, {{1, 2}}, {{2, 0}}, {{3, 4}}, {{4, 5}}, {{5, 3}}, {{0, 3}}, {{1, 4}}, {{2, 5}}}};
constexpr std::array<std::array<int, 2>, 8> kPyramidEdges{
    {{{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}, {{0, 4}}, {{1, 4}}, {{2, 4}}, {{3, 4}}}};

Edge canonical_edge(std::uint32_t a, std::uint32_t b) {
    return std::minmax(a, b);
}

template <std::size_t N, typename Fn>
void visit_fixed_edges(const NodalElement& element,
                       const std::array<std::array<int, 2>, N>& edges, int n_corner,
                       Fn&& fn) {
    if (element.nodes.size() < static_cast<std::size_t>(n_corner)) {
        throw FeaError("p_elevate: element node count invalid");
    }
    for (const auto& edge : edges) {
        fn(canonical_edge(element.nodes[static_cast<std::size_t>(edge[0])],
                          element.nodes[static_cast<std::size_t>(edge[1])]));
    }
}

template <typename Fn>
void visit_corner_edges(const NodalElement& element, Fn&& fn) {
    switch (element.type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        visit_fixed_edges(element, kTetEdges, 4, std::forward<Fn>(fn));
        return;
    case ElementType::kHex8:
    case ElementType::kHex20:
        visit_fixed_edges(element, kHexEdges, 8, std::forward<Fn>(fn));
        return;
    case ElementType::kPrism6:
        visit_fixed_edges(element, kPrismEdges, 6, std::forward<Fn>(fn));
        return;
    case ElementType::kPyramid5:
        visit_fixed_edges(element, kPyramidEdges, 5, std::forward<Fn>(fn));
        return;
    case ElementType::kPolyVem:
        for (const auto& face : element.faces) {
            for (std::size_t i = 0; i < face.size(); ++i) {
                const auto a_local = face[i];
                const auto b_local = face[(i + 1) % face.size()];
                if (a_local >= element.nodes.size() || b_local >= element.nodes.size()) {
                    throw FeaError("p_elevate: poly face index out of range");
                }
                fn(canonical_edge(element.nodes[a_local], element.nodes[b_local]));
            }
        }
        return;
    }
}

bool promotable(ElementType type) {
    return type == ElementType::kTet4 || type == ElementType::kHex8;
}

bool quadratic(ElementType type) {
    return type == ElementType::kTet10 || type == ElementType::kHex20;
}

void seed_existing_midpoints(const NodalElement& element, std::map<Edge, std::uint32_t>& mids) {
    const auto seed = [&](const auto& edges, std::size_t n_corner) {
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto& edge = edges[i];
            const auto key = canonical_edge(
                element.nodes[static_cast<std::size_t>(edge[0])],
                element.nodes[static_cast<std::size_t>(edge[1])]);
            const auto mid = element.nodes[n_corner + i];
            const auto [it, inserted] = mids.try_emplace(key, mid);
            if (!inserted && it->second != mid) {
                throw FeaError(std::format(
                    "p_elevate: quadratic edge ({},{}) has inconsistent midside nodes", key.first,
                    key.second));
            }
        }
    };
    if (element.type == ElementType::kTet10) {
        if (element.nodes.size() != 10) {
            throw FeaError("p_elevate: tet10 node count invalid");
        }
        seed(kTetEdges, 4);
    } else if (element.type == ElementType::kHex20) {
        if (element.nodes.size() != 20) {
            throw FeaError("p_elevate: hex20 node count invalid");
        }
        seed(kHexEdges, 8);
    }
}

PElevateResult p_elevate_impl(const NodalMesh& mesh, const std::vector<bool>& elevate) {
    if (elevate.size() != mesh.elements.size()) {
        throw FeaError("p_elevate: elevate mask size mismatch");
    }

    struct EdgeIncidence {
        bool has_linear = false;
    };
    std::map<Edge, EdgeIncidence> incidence;
    std::map<Edge, std::uint32_t> midpoints;
    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        const auto& element = mesh.elements[e];
        seed_existing_midpoints(element, midpoints);
        const bool final_quadratic = quadratic(element.type) || (elevate[e] && promotable(element.type));
        visit_corner_edges(element, [&](const Edge& edge) {
            incidence[edge].has_linear = incidence[edge].has_linear || !final_quadratic;
        });
    }

    PElevateResult result;
    result.mesh.nodes = mesh.nodes;
    result.mesh.elements.reserve(mesh.elements.size());
    const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto key = canonical_edge(a, b);
        const auto [it, inserted] =
            midpoints.try_emplace(key, static_cast<std::uint32_t>(result.mesh.nodes.size()));
        if (inserted) {
            result.mesh.nodes.push_back(0.5 * (result.mesh.nodes[a] + result.mesh.nodes[b]));
        }
        return it->second;
    };

    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        const auto& element = mesh.elements[e];
        if (!elevate[e] || !promotable(element.type)) {
            result.mesh.elements.push_back(element);
            continue;
        }
        NodalElement promoted;
        promoted.nodes = element.nodes;
        promoted.faces = element.faces;
        if (element.type == ElementType::kTet4) {
            if (element.nodes.size() != 4) {
                throw FeaError("p_elevate: tet4 node count invalid");
            }
            promoted.type = ElementType::kTet10;
            for (const auto& edge : kTetEdges) {
                promoted.nodes.push_back(
                    midpoint(element.nodes[static_cast<std::size_t>(edge[0])],
                             element.nodes[static_cast<std::size_t>(edge[1])]));
            }
        } else {
            if (element.nodes.size() != 8) {
                throw FeaError("p_elevate: hex8 node count invalid");
            }
            promoted.type = ElementType::kHex20;
            for (const auto& edge : kHexEdges) {
                promoted.nodes.push_back(
                    midpoint(element.nodes[static_cast<std::size_t>(edge[0])],
                             element.nodes[static_cast<std::size_t>(edge[1])]));
            }
        }
        result.mesh.elements.push_back(std::move(promoted));
        ++result.n_promoted;
    }

    for (const auto& [edge, mid] : midpoints) {
        const auto it = incidence.find(edge);
        if (it == incidence.end() || !it->second.has_linear) {
            continue;
        }
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            result.constraints.add(LinearConstraint{
                .slave_dof = 3u * mid + axis,
                .masters = {{3u * edge.first + axis, 0.5},
                            {3u * edge.second + axis, 0.5}}});
        }
        ++result.n_constrained_midside;
    }
    return result;
}

} // namespace

NodalMesh promote_to_quadratic(const NodalMesh& mesh) {
    std::vector<bool> all(mesh.elements.size(), true);
    return p_elevate_impl(mesh, all).mesh;
}

PElevateResult p_elevate_with_constraints(
    const NodalMesh& mesh, std::span<const std::size_t> elevate_indices) {
    std::vector<bool> mask(mesh.elements.size(), false);
    for (const auto idx : elevate_indices) {
        if (idx >= mesh.elements.size()) {
            throw FeaError("p_elevate: element index out of range");
        }
        mask[idx] = true;
    }
    return p_elevate_impl(mesh, mask);
}

NodalMesh p_elevate(const NodalMesh& mesh, std::span<const std::size_t> elevate_indices) {
    return p_elevate_with_constraints(mesh, elevate_indices).mesh;
}

PElevateResult p_elevate_with_constraints(const NodalMesh& mesh,
                                           std::span<const bool> elevate_mask) {
    if (elevate_mask.size() != mesh.elements.size()) {
        throw FeaError("p_elevate: elevate mask size mismatch");
    }
    return p_elevate_impl(mesh, std::vector<bool>(elevate_mask.begin(), elevate_mask.end()));
}

NodalMesh p_elevate(const NodalMesh& mesh, std::span<const bool> elevate_mask) {
    return p_elevate_with_constraints(mesh, elevate_mask).mesh;
}

ElementTypeCounts count_element_types(const NodalMesh& mesh) {
    ElementTypeCounts c;
    for (const auto& el : mesh.elements) {
        switch (el.type) {
        case ElementType::kTet4:
            ++c.tet4;
            break;
        case ElementType::kTet10:
            ++c.tet10;
            break;
        case ElementType::kHex8:
            ++c.hex8;
            break;
        case ElementType::kHex20:
            ++c.hex20;
            break;
        default:
            ++c.other;
            break;
        }
    }
    return c;
}

} // namespace polymesh::fea
