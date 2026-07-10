// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Isoparametric element stiffness and global assembly for linear
// elastostatics. DOF layout: node i owns global DOFs (3i, 3i+1, 3i+2) for
// (u_x, u_y, u_z).

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include <functional>

namespace polymesh::fea {

/// A body-force density field, N/m^3, evaluated at a physical point.
using BodyForce = std::function<Eigen::Vector3d(const Eigen::Vector3d&)>;

/// Element stiffness K_e = integral of B^T D B dV over the element,
/// size 3n x 3n, N/m. Throws FeaError on non-positive Jacobian.
Eigen::MatrixXd element_stiffness(const NodalMesh& mesh, const NodalElement& element,
                                  const Material& material);

/// Global stiffness matrix, size 3N x 3N (N = node count), symmetric.
Eigen::SparseMatrix<double> assemble_stiffness(const NodalMesh& mesh,
                                               const Material& material);

/// Consistent nodal load vector for a body-force field:
/// f = integral of N^T b dV, size 3N, newtons.
Eigen::VectorXd assemble_body_load(const NodalMesh& mesh, const BodyForce& body_force);

} // namespace polymesh::fea
