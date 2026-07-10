// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"

#include "fea/backend.hpp"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <format>
#include <vector>

namespace polymesh::fea {

SolveMethod select_solve_method(Eigen::Index nfree, const SolveOptions& options) {
    switch (options.method) {
    case SolveMethod::kDirect:
        return SolveMethod::kDirect;
    case SolveMethod::kCG:
        return SolveMethod::kCG;
    case SolveMethod::kAuto:
        return (nfree > options.cg_threshold) ? SolveMethod::kCG : SolveMethod::kDirect;
    }
    return SolveMethod::kDirect;
}

namespace {

Eigen::VectorXd solve_reduced(const Eigen::SparseMatrix<double>& kff,
                              const Eigen::VectorXd& rhs, const SolveOptions& options) {
    const Eigen::Index nfree = kff.rows();
    const SolveMethod method = select_solve_method(nfree, options);

    if (method == SolveMethod::kCG) {
        // V1: IncompleteLUT preconditioner (far better than diagonal on FE
        // systems). Fall back to diagonal if ILUT setup fails.
        using CG_ILUT =
            Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                                     Eigen::IncompleteLUT<double>>;
        using CG_Diag =
            Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper,
                                     Eigen::DiagonalPreconditioner<double>>;
        const int max_iters =
            options.cg_max_iters > 0
                ? options.cg_max_iters
                : static_cast<int>(std::max<Eigen::Index>(2 * nfree, Eigen::Index{1000}));

        CG_ILUT cg;
        cg.setTolerance(options.cg_tol);
        cg.setMaxIterations(max_iters);
        // Drop fill / drop tol: modest fill keeps setup cheap on mid-size systems.
        cg.preconditioner().setDroptol(1e-4);
        cg.preconditioner().setFillfactor(10);
        cg.compute(kff);
        if (cg.info() == Eigen::Success) {
            const Eigen::VectorXd uf = cg.solve(rhs);
            if (cg.info() == Eigen::Success) {
                return uf;
            }
        }

        CG_Diag cg_d;
        cg_d.setTolerance(options.cg_tol);
        cg_d.setMaxIterations(max_iters);
        cg_d.compute(kff);
        if (cg_d.info() != Eigen::Success) {
            throw FeaError("solve_elastostatics: CG setup failed — system is singular "
                           "(insufficient constraints?)");
        }
        const Eigen::VectorXd uf = cg_d.solve(rhs);
        if (cg_d.info() != Eigen::Success) {
            throw FeaError(
                std::format("solve_elastostatics: CG failed to converge after {} iterations "
                            "(tol={}, estimated error={})",
                            cg_d.iterations(), options.cg_tol, cg_d.error()));
        }
        return uf;
    }

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(kff);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("solve_elastostatics: factorization failed — system is singular "
                       "(insufficient constraints?)");
    }
    const Eigen::VectorXd uf = ldlt.solve(rhs);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("solve_elastostatics: back-substitution failed");
    }
    return uf;
}

} // namespace

Eigen::VectorXd solve_elastostatics(const NodalMesh& mesh, const Material& material,
                                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads,
                                    const SolveOptions& options) {
    init_runtime_performance();
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

    const Eigen::VectorXd uf = solve_reduced(kff, rhs, options);

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
