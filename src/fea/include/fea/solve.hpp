// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Linear elastostatics solve: assemble, apply Dirichlet constraints by
// partitioning, factorize with sparse LDLT.

#include "fea/assembly.hpp"

#include <map>

namespace polymesh::fea {

/// Prescribed displacements, keyed by global DOF index (3*node + axis),
/// values in metres.
struct Dirichlet {
    std::map<Eigen::Index, double> dof_values;

    /// Prescribes all three displacement components of a node.
    void fix_node(std::uint32_t node, const Eigen::Vector3d& u = Eigen::Vector3d::Zero()) {
        for (int axis = 0; axis < 3; ++axis) {
            dof_values[3 * static_cast<Eigen::Index>(node) + axis] = u[axis];
        }
    }
};

/// Solves K u = f with the given constraints. `loads` is the full-size global
/// load vector (3N); returns the full-size displacement vector with
/// prescribed values in place. Throws FeaError if the reduced system is
/// singular (insufficient constraints leave rigid-body modes).
Eigen::VectorXd solve_elastostatics(const NodalMesh& mesh, const Material& material,
                                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads);

/// Strain energy 1/2 u^T K u, joules.
double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u);

} // namespace polymesh::fea
