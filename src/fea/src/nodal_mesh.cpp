// SPDX-License-Identifier: BSD-3-Clause
#include "fea/nodal_mesh.hpp"

#include <format>

namespace polymesh::fea {

void NodalMesh::check_validity() const {
    const auto n = static_cast<std::uint32_t>(nodes.size());
    for (std::size_t e = 0; e < elements.size(); ++e) {
        const auto& element = elements[e];
        if (element.type == ElementType::kPolyVem) {
            if (element.nodes.size() < 4) {
                throw FeaError(std::format("poly VEM element {} has fewer than 4 nodes", e));
            }
            if (element.faces.size() < 4) {
                throw FeaError(std::format("poly VEM element {} has fewer than 4 faces", e));
            }
        } else {
            const auto expected = static_cast<std::size_t>(element_num_nodes(element.type));
            if (element.nodes.size() != expected) {
                throw FeaError(std::format("element {} has {} nodes, expected {}", e,
                                           element.nodes.size(), expected));
            }
        }
        for (const auto node : element.nodes) {
            if (node >= n) {
                throw FeaError(
                    std::format("element {} references out-of-range node {}", e, node));
            }
        }
        for (const auto& face : element.faces) {
            for (const auto li : face) {
                if (li >= element.nodes.size()) {
                    throw FeaError(std::format(
                        "element {} face references local index {} out of range", e, li));
                }
            }
        }
    }
}

} // namespace polymesh::fea
