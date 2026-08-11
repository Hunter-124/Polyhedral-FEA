// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// p-elevation: promote linear nodal elements to quadratic by inserting
// shared mid-edge nodes (tet4→tet10, hex8→hex20). Selective promotion also
// reports displacement constraints that keep p=1/p=2 traces conforming.

#include "fea/constraints.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::fea {

/// Mesh and interface constraints produced by selective p-elevation.
struct PElevateResult {
    NodalMesh mesh;
    LinearConstraints constraints;
    std::size_t n_promoted = 0;
    /// Requested promotions rejected because inherited curved edge nodes would
    /// make the promoted element invalid at a stiffness quadrature point.
    std::size_t n_rejected = 0;
    std::size_t n_constrained_midside = 0;
};

/// Promote every validity-safe promotable linear element (tet4→tet10,
/// hex8→hex20). Already-quadratic, prism, pyramid, and VEM elements are left
/// unchanged. New mid-edge nodes sit at straight edge midpoints; mids inherited
/// from an existing quadratic neighbour retain their curved positions.
NodalMesh promote_to_quadratic(const NodalMesh& mesh);

/// Selective p-elevation: promote only the listed element indices.
/// Indices must be valid; duplicates are ignored. Shared edges across
/// promoted elements reuse one midpoint. Unlisted linear elements stay p=1.
NodalMesh p_elevate(const NodalMesh& mesh, std::span<const std::size_t> elevate_indices);

/// Selective p-elevation with the p=1/p=2 interface constraints required by
/// `solve_elastostatics`. Incidence is computed on the input connectivity.
PElevateResult p_elevate_with_constraints(
    const NodalMesh& mesh, std::span<const std::size_t> elevate_indices);

/// Convenience: promote where `elevate_mask[e]` is true (size = mesh.elements).
NodalMesh p_elevate(const NodalMesh& mesh, std::span<const bool> elevate_mask);

/// Mask overload of `p_elevate_with_constraints`.
PElevateResult p_elevate_with_constraints(const NodalMesh& mesh,
                                           std::span<const bool> elevate_mask);

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
