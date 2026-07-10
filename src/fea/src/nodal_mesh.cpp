// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/nodal_mesh.hpp"

#include <format>

namespace polymesh::fea {

void NodalMesh::check_validity() const {
    const auto n = static_cast<std::uint32_t>(nodes.size());
    for (std::size_t e = 0; e < elements.size(); ++e) {
        const auto& element = elements[e];
        const auto expected = static_cast<std::size_t>(element_num_nodes(element.type));
        if (element.nodes.size() != expected) {
            throw FeaError(std::format("element {} has {} nodes, expected {}", e,
                                       element.nodes.size(), expected));
        }
        for (const auto node : element.nodes) {
            if (node >= n) {
                throw FeaError(
                    std::format("element {} references out-of-range node {}", e, node));
            }
        }
    }
}

} // namespace polymesh::fea
