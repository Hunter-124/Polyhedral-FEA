// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Numerical quadrature on the reference tetrahedron and reference hexahedron.
//
// Tet rules integrate over the unit reference tet {xi,eta,zeta >= 0,
// xi+eta+zeta <= 1} (volume 1/6); hex rules over [-1,1]^3 (volume 8).
// Rules are verified against exact monomial integrals in the test suite, so
// a rule claiming degree d must integrate every monomial of total degree <= d
// exactly.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <vector>

namespace polymesh::fea {

struct QuadraturePoint {
    Eigen::Vector3d xi;
    double weight = 0.0;
};

/// Tet rule exact for polynomials of total degree <= `degree`.
/// degree 1 -> 1 point, degree 2 -> 4 points; higher degrees use a
/// Duffy-transform (collapsed cube) construction, exact by design.
std::vector<QuadraturePoint> tet_rule(int degree);

/// Tensor-product Gauss-Legendre rule, exact for degree <= 2n-1 per axis.
/// Supports n in [1, 5].
std::vector<QuadraturePoint> hex_rule(int points_per_axis);

/// Default stiffness-integration rule for an element type. Assumes
/// straight-edged elements (mid-side nodes at edge midpoints); curved
/// elements will need higher-degree rules when P3 introduces them.
std::vector<QuadraturePoint> default_rule(ElementType type);

} // namespace polymesh::fea
