// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/solve.hpp"

#include <Eigen/SparseCholesky>

#include <format>
#include <vector>

namespace polymesh::fea {

Eigen::VectorXd solve_elastostatics(const NodalMesh& mesh, const Material& material,
                                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads) {
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    if (loads.size() != ndof) {
        throw FeaError(std::format("solve_elastostatics: load vector size {} != 3N = {}",
                                   loads.size(), ndof));
    }
    for (const auto& [dof, value] : dirichlet.dof_values) {
        if (dof < 0 || dof >= ndof) {
            throw FeaError(
                std::format("solve_elastostatics: constrained DOF {} out of range", dof));
        }
        (void)value;
    }

    // Map global DOFs to reduced (free) indices; -1 marks constrained.
    std::vector<Eigen::Index> reduced(static_cast<std::size_t>(ndof), -1);
    Eigen::Index nfree = 0;
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        if (!dirichlet.dof_values.contains(dof)) {
            reduced[static_cast<std::size_t>(dof)] = nfree++;
        }
    }

    const auto k = assemble_stiffness(mesh, material);

    // Reduced system: K_ff u_f = f_f - K_fc u_c.
    Eigen::VectorXd rhs(nfree);
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        const auto r = reduced[static_cast<std::size_t>(dof)];
        if (r >= 0) {
            rhs[r] = loads[dof];
        }
    }
    std::vector<Eigen::Triplet<double>> triplets;
    for (int outer = 0; outer < k.outerSize(); ++outer) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(k, outer); it; ++it) {
            const auto row = reduced[static_cast<std::size_t>(it.row())];
            const auto col = reduced[static_cast<std::size_t>(it.col())];
            if (row >= 0 && col >= 0) {
                triplets.emplace_back(row, col, it.value());
            } else if (row >= 0 && col < 0) {
                rhs[row] -= it.value() * dirichlet.dof_values.at(it.col());
            }
        }
    }
    Eigen::SparseMatrix<double> kff(nfree, nfree);
    kff.setFromTriplets(triplets.begin(), triplets.end());

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(kff);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("solve_elastostatics: factorization failed — system is singular "
                       "(insufficient constraints?)");
    }
    const Eigen::VectorXd uf = ldlt.solve(rhs);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("solve_elastostatics: back-substitution failed");
    }

    Eigen::VectorXd u(ndof);
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        const auto r = reduced[static_cast<std::size_t>(dof)];
        u[dof] = r >= 0 ? uf[r] : dirichlet.dof_values.at(dof);
    }
    return u;
}

double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u) {
    const auto k = assemble_stiffness(mesh, material);
    return 0.5 * u.dot(k * u);
}

} // namespace polymesh::fea
