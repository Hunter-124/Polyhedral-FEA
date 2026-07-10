// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Zienkiewicz–Zhu superconvergent patch recovery (nodal) and energy-norm
// error indicator per element. Used for visualization quality and adapt
// marking (P5). Double precision only.

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/stress.hpp"

#include <Eigen/Core>

#include <vector>

namespace polymesh::fea {

struct ZzRecovery {
    /// Recovered (smoothed) nodal stress, Pa.
    std::vector<Stress> nodal_stress;
    /// Per-element energy-norm error indicator η_e (relative scale).
    std::vector<double> element_eta;
    /// Global η ≈ sqrt(sum η_e^2).
    double global_eta = 0.0;
};

/// Patch recovery: fit linear stress over element patches around each node,
/// evaluate recovered stress at nodes; compare to raw Gauss-centroid stress
/// for element indicators.
ZzRecovery recover_zz(const NodalMesh& mesh, const Material& material,
                      const Eigen::VectorXd& u);

} // namespace polymesh::fea
