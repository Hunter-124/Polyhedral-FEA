// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// p-elevation: promote linear nodal elements to quadratic by inserting
// shared mid-edge nodes (tet4→tet10, hex8→hex20). Midpoints on a global
// undirected edge map so adjacent elements share the same mid-edge node.

#include "fea/nodal_mesh.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::fea {

/// Promote every promotable linear element (tet4→tet10, hex8→hex20).
/// Already-quadratic, prism, pyramid, and VEM elements are left unchanged.
/// Mid-edge nodes sit at straight edge midpoints (isoparametric p=2).
NodalMesh promote_to_quadratic(const NodalMesh& mesh);

/// Selective p-elevation: promote only the listed element indices.
/// Indices must be valid; duplicates are ignored. Shared edges across
/// promoted elements reuse one midpoint. Unlisted linear elements stay p=1.
NodalMesh p_elevate(const NodalMesh& mesh, std::span<const std::size_t> elevate_indices);

/// Convenience: promote where `elevate_mask[e]` is true (size = mesh.elements).
NodalMesh p_elevate(const NodalMesh& mesh, std::span<const bool> elevate_mask);

/// Count elements of each promotable / quadratic type (for notes / tests).
struct ElementTypeCounts {
    std::size_t tet4 = 0;
    std::size_t tet10 = 0;
    std::size_t hex8 = 0;
    std::size_t hex20 = 0;
    std::size_t other = 0;
};

ElementTypeCounts count_element_types(const NodalMesh& mesh);

} // namespace polymesh::fea
