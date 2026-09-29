// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve_health.hpp"

#include "fea/assembly.hpp"
#include "fea/solve.hpp"

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include <algorithm>
#include <cmath>

namespace polymesh::fea {

SolveHealth solve_health(const NodalMesh& mesh, const Material& material, const Dirichlet& bc,
                         const Eigen::VectorXd& f, const Eigen::VectorXd& u) {
    const auto K = assemble_stiffness(mesh, material);
    const Eigen::VectorXd r = K * u - f;

    double free_r2 = 0.0;
    double f2 = 0.0;
    Eigen::Vector3d F_sum = Eigen::Vector3d::Zero();
    Eigen::Vector3d R_sum = Eigen::Vector3d::Zero();
    const Eigen::Index n_dof = u.size();
    for (Eigen::Index dof = 0; dof < n_dof; ++dof) {
        f2 += f[dof] * f[dof];
        const int axis = static_cast<int>(dof % 3);
        if (bc.dof_values.contains(dof)) {
            R_sum[axis] += r[dof];
        } else {
            free_r2 += r[dof] * r[dof];
            F_sum[axis] += f[dof];
        }
    }
    constexpr double kEps = 1e-30;
    SolveHealth health;
    health.free_residual_rel = std::sqrt(free_r2) / std::max(std::sqrt(f2), kEps);
    // Equilibrium: sum(F_applied on free) + sum(reactions on constrained) ≈ 0.
    health.reaction_sum_err = (F_sum + R_sum).norm() / std::max(F_sum.norm(), kEps);
    return health;
}

} // namespace polymesh::fea
