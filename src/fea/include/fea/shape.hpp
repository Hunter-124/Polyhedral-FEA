// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Shape functions and their reference-coordinate derivatives for the standard
// isoparametric element zoo (node ordering documented in nodal_mesh.hpp).
//
// Verified properties (test suite): partition of unity, zero derivative sum,
// and the nodal delta property N_i(xi_j) = delta_ij.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

namespace polymesh::fea {

struct ShapeEval {
    /// Shape function values, one per node.
    Eigen::VectorXd n;
    /// Derivatives w.r.t. reference coordinates, row i = dN_i/d(xi,eta,zeta).
    Eigen::Matrix<double, Eigen::Dynamic, 3> dn;
};

/// Evaluates shape functions at reference coordinates `xi`.
ShapeEval eval_shape(ElementType type, const Eigen::Vector3d& xi);

/// Reference coordinates of each node, in canonical order.
std::vector<Eigen::Vector3d> reference_nodes(ElementType type);

} // namespace polymesh::fea
