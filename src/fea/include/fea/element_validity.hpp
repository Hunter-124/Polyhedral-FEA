// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// The assembly's own definition of "this element can be integrated".
//
// `fea::cell_quality` answers a different question — how well shaped is this
// cell — and the two disagree exactly where it matters. A mesh repair pass that
// accepts a node move on `cell_quality >= floor` can still ship an element the
// assembly refuses with `element_stiffness: non-positive Jacobian`, because the
// quality measure is a normalized corner/volume ratio while the assembly needs
// det J > 0 at every quadrature point of the element's own rule (and, for a
// pyramid, in both tets of the conformity-safe split it is actually integrated
// as). Measured while building the ADR-0035 exterior gate: a quality-accepted
// move shipped det J = -6.085e-09 on icecream_cone/graded and -3.564e-10 on
// plate_hole/varyhedron.
//
// So the predicate is defined once, next to the rule it mirrors, and repair
// passes gate on it instead of on a proxy.

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
/// map; they are reported valid here and are gated by their own positive-volume
/// test (`fea::poly_volume`).
[[nodiscard]] bool element_jacobians_positive(const NodalMesh& mesh, const NodalElement& element);

/// `element_jacobians_positive` for every element incident to `node`, given a
/// precomputed incidence list.
[[nodiscard]] bool star_jacobians_positive(const NodalMesh& mesh,
                                           std::span<const std::uint32_t> incident_elements);

} // namespace polymesh::fea
