// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Zienkiewicz–Zhu superconvergent patch recovery (nodal) and a *relative*
// energy-norm error indicator per element. Used for visualization quality and
// adapt marking (P5). Double precision only.
//
// Energy norm: ‖σ‖²_e = ∫_e σᵀ D⁻¹ σ dV (units J — the element's complementary
// strain energy). The indicator compares the recovered stress σ* to the FE
// stress σ_h in that norm and divides by ‖σ_h‖ over the whole mesh, so both the
// per-element and the global number are dimensionless and mesh-size independent
// (they do NOT grow as sqrt(N) the way a raw Pa-valued root-sum-square does).

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/stress.hpp"

#include <Eigen/Core>

#include <vector>

namespace polymesh::fea {

struct ZzRecovery {
    /// Recovered (smoothed) nodal stress, Pa.
    std::vector<Stress> nodal_stress;
    /// Per-element relative energy-norm error indicator
    /// η_e = sqrt(∫_e (σ*−σ_h)ᵀ D⁻¹ (σ*−σ_h) dV / Σ_f ∫_f σ_hᵀ D⁻¹ σ_h dV).
    /// Dimensionless, volume-weighted (a big bad element outranks a small one),
    /// and Σ η_e² = global_eta². Dörfler marking consumes these squared shares.
    std::vector<double> element_eta;
    /// Global relative energy-norm error η = sqrt(Σ η_e²): dimensionless, 0 =
    /// exact recovery, 0.05 ≈ 5% energy-norm error. This is the quantity an
    /// `eta_target` stop compares against.
    double global_eta = 0.0;
};

/// Patch recovery: fit linear stress over element patches around each node,
/// evaluate recovered stress at nodes; compare to raw Gauss-centroid stress
/// for element indicators.
ZzRecovery recover_zz(const NodalMesh& mesh, const Material& material,
                      const Eigen::VectorXd& u);

} // namespace polymesh::fea
