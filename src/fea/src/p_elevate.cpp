// SPDX-License-Identifier: BSD-3-Clause
#include "fea/p_elevate.hpp"

#include <array>
#include <map>
#include <utility>

namespace polymesh::fea {
namespace {

// Canonical mid-edge order — must match fea/nodal_mesh.hpp documentation.
constexpr std::array<std::array<int, 2>, 6> kTetEdges{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
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

NodalMesh p_elevate_impl(const NodalMesh& mesh, const std::vector<bool>& elevate) {
    if (elevate.size() != mesh.elements.size()) {
        throw FeaError("p_elevate: elevate mask size mismatch");
    }
    NodalMesh out;
    out.nodes = mesh.nodes;
    out.elements.reserve(mesh.elements.size());
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
    const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto key = std::minmax(a, b);
        const auto [it, inserted] =
            midpoints.try_emplace(key, static_cast<std::uint32_t>(out.nodes.size()));
        if (inserted) {
            out.nodes.push_back(0.5 * (out.nodes[a] + out.nodes[b]));
        }
        return it->second;
    };

    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        const auto& element = mesh.elements[e];
        if (!elevate[e]) {
            out.elements.push_back(element);
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
        } else if (element.type == ElementType::kHex8) {
            if (element.nodes.size() != 8) {
                throw FeaError("p_elevate: hex8 node count invalid");
            }
            promoted.type = ElementType::kHex20;
            for (const auto& edge : kHexEdges) {
                promoted.nodes.push_back(
                    midpoint(element.nodes[static_cast<std::size_t>(edge[0])],
                             element.nodes[static_cast<std::size_t>(edge[1])]));
            }
        } else {
            // Already quadratic or unsupported type: leave as-is.
            out.elements.push_back(element);
            continue;
        }
        out.elements.push_back(std::move(promoted));
    }
    return out;
}

} // namespace

NodalMesh promote_to_quadratic(const NodalMesh& mesh) {
    std::vector<bool> all(mesh.elements.size(), true);
    return p_elevate_impl(mesh, all);
}

NodalMesh p_elevate(const NodalMesh& mesh, std::span<const std::size_t> elevate_indices) {
    std::vector<bool> mask(mesh.elements.size(), false);
    for (const auto idx : elevate_indices) {
        if (idx >= mesh.elements.size()) {
            throw FeaError("p_elevate: element index out of range");
        }
        mask[idx] = true;
    }
    return p_elevate_impl(mesh, mask);
}

NodalMesh p_elevate(const NodalMesh& mesh, std::span<const bool> elevate_mask) {
    if (elevate_mask.size() != mesh.elements.size()) {
        throw FeaError("p_elevate: elevate mask size mismatch");
    }
    return p_elevate_impl(mesh, std::vector<bool>(elevate_mask.begin(), elevate_mask.end()));
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
