// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// The assembly's own definition of "this element can be integrated".
//
// `fea::cell_quality` answers a different question — how well shaped is this
// cell — and a quality-accepted node move can still ship an element the
// assembly refuses with `element_stiffness: non-positive Jacobian`: quality is a
// normalized corner/volume ratio, while the assembly needs det J > 0 at every
// quadrature point of the element's own rule (for a pyramid, in both tets of
// the conformity-safe split). Repair passes gate on this predicate (ADR-0035).

#include "fea/nodal_mesh.hpp"

#include <cstdint>
#include <span>

namespace polymesh::fea {

/// True when every quadrature point the assembly will integrate this element at
/// has a strictly positive Jacobian determinant.
///
/// Mirrors `element_stiffness`: pyramids are tested through the two tets of the
/// shared-face-consistent split, every other straight element through
/// `default_rule(element.type)`. Polyhedral VEM cells carry no isoparametric
/// map; they are valid when they have faces and a positive divergence-theorem
/// volume (`fea::poly_volume`).
[[nodiscard]] bool element_jacobians_positive(const NodalMesh& mesh,
                                              const NodalElement& element);

/// `element_jacobians_positive` for every element incident to `node`, given a
/// precomputed incidence list.
[[nodiscard]] bool star_jacobians_positive(const NodalMesh& mesh,
                                           std::span<const std::uint32_t> incident_elements);

} // namespace polymesh::fea
