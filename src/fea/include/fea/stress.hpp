// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Stress recovery from a displacement solution.
//
// v0 recovery: element stress evaluated at each node's reference position,
// averaged over all elements sharing the node. Zienkiewicz-Zhu
// superconvergent patch recovery replaces this as the error-estimation
// workhorse in Phase P5 (adapt module); this simple average is fine for
// visualization and peak-stress benchmarks.

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <vector>

namespace polymesh::fea {

/// Stress in Voigt order (xx, yy, zz, yz, xz, xy), Pa.
using Stress = Eigen::Matrix<double, 6, 1>;

/// Nodal-averaged stress for every node, Pa.
std::vector<Stress> recover_nodal_stress(const NodalMesh& mesh, const Material& material,
                                         const Eigen::VectorXd& u);

/// Von Mises equivalent stress, Pa.
double von_mises(const Stress& s);

} // namespace polymesh::fea
