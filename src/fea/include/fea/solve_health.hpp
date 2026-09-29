// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

namespace polymesh::fea {

struct Dirichlet;

/// Independent a-posteriori check of a linear-elastic solve K u = f.
struct SolveHealth {
    /// ||r_free|| / max(||f||, 1e-30) with r = K u - f over unconstrained DOFs.
    double free_residual_rel = 0.0;
    /// |sum f_free + sum r_constrained| / max(|sum f_free|, 1e-30), per-axis
    /// force sums: equilibrium of applied loads against support reactions.
    double reaction_sum_err = 0.0;
};

/// Reassembles K from `mesh`/`material` (never trusts the solver's matrix) and
/// measures `u` against `f`. DOF d is axis d % 3; a DOF is constrained iff it is
/// a key of `bc.dof_values`. `u` and `f` must be 3N long for N mesh nodes.
[[nodiscard]] SolveHealth solve_health(const NodalMesh& mesh, const Material& material,
                                       const Dirichlet& bc, const Eigen::VectorXd& f,
                                       const Eigen::VectorXd& u);

} // namespace polymesh::fea
