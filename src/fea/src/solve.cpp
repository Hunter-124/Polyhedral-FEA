// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"

#include "fea/backend.hpp"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
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

SolveDecision decide_solve_method(Eigen::Index nfree, const SolveOptions& options,
                                  const SolveResourceEstimate& estimate,
                                  std::uint64_t effective_cap_bytes) {
    SolveDecision decision;
    decision.method = select_solve_method(nfree, options);
    if (options.method == SolveMethod::kAuto && decision.method == SolveMethod::kDirect &&
        estimate.direct_peak_bytes > effective_cap_bytes &&
        estimate.cg_peak_bytes <= effective_cap_bytes) {
        decision.method = SolveMethod::kCG;
        decision.note =
            std::format("memory budget: LDLT estimate {} exceeds cap {}; using CG estimate {}",
                        format_memory_bytes(estimate.direct_peak_bytes),
                        format_memory_bytes(effective_cap_bytes),
                        format_memory_bytes(estimate.cg_peak_bytes));
    }
    decision.estimated_bytes = decision.method == SolveMethod::kDirect
                                   ? estimate.direct_peak_bytes
                                   : estimate.cg_peak_bytes;
    return decision;
}

namespace {

/// Iteration cap `kAuto` applies when the caller leaves `cg_max_iters` at 0.
/// Even at ~10 ms per iteration this bounds a hopeless solve to minutes rather
/// than letting `2 * nfree` grind for hours on a system that will not converge.
constexpr int kCgAutoIterCap = 20000;

int cg_iteration_budget(Eigen::Index nfree, const SolveOptions& options) {
    if (options.cg_max_iters > 0) {
        return options.cg_max_iters;
    }
    return static_cast<int>(
        std::clamp<Eigen::Index>(2 * nfree, Eigen::Index{1000}, Eigen::Index{kCgAutoIterCap}));
}

/// Preconditioned conjugate gradient that reports progress from inside the
/// recurrence.
///
/// Hand-rolled instead of `Eigen::ConjugateGradient` because progress used to
/// be produced by restarting Eigen's solver every `cg_progress_chunk`
/// iterations through `solveWithGuess`. A restart discards the Krylov space, so
/// the callback-driven path converged far slower than the plain one — the GUI
/// and the CLI were effectively running different solvers. Reporting from one
/// uninterrupted recurrence removes that split. `iters` and `rel_error` report
/// what actually happened; the caller decides whether that counts as success.
template <typename Precond>
Eigen::VectorXd run_cg(const Eigen::SparseMatrix<double>& a, const Eigen::VectorXd& rhs,
                       const Precond& precond, double tol, int max_iters, int report_every,
                       const std::function<void(int, int, double)>& on_progress, int& iters,
                       double& rel_error) {
    Eigen::VectorXd x = Eigen::VectorXd::Zero(rhs.size());
    iters = 0;
    const double rhs_norm2 = rhs.squaredNorm();
    if (!(rhs_norm2 > 0.0)) {
        rel_error = 0.0;
        return x;
    }
    const double threshold = tol * tol * rhs_norm2;

    Eigen::VectorXd r = rhs; // b − A·x0 with x0 = 0
    Eigen::VectorXd z = precond.solve(r);
    Eigen::VectorXd p = z;
    Eigen::VectorXd ap(rhs.size());
    double r_norm2 = rhs_norm2;
    double rz = r.dot(z);

    while (iters < max_iters && r_norm2 > threshold) {
        ap.noalias() = a * p;
        const double pap = p.dot(ap);
        if (!(pap > 0.0)) {
            break; // breakdown: K_ff is not positive definite along p
        }
        const double alpha = rz / pap;
        x += alpha * p;
        r -= alpha * ap;
        r_norm2 = r.squaredNorm();
        ++iters;
        if (r_norm2 <= threshold) {
            break;
        }
        z = precond.solve(r);
        const double rz_next = r.dot(z);
        if (!(rz_next > 0.0)) {
            break; // preconditioner lost positive definiteness
        }
        p = (rz_next / rz) * p + z;
        rz = rz_next;
        if (on_progress && report_every > 0 && iters % report_every == 0) {
            on_progress(iters, max_iters, std::sqrt(r_norm2 / rhs_norm2));
        }
    }
    rel_error = std::sqrt(r_norm2 / rhs_norm2);
    return x;
}

Eigen::VectorXd solve_reduced(const Eigen::SparseMatrix<double>& kff,
                              const Eigen::VectorXd& rhs, const SolveOptions& options) {
    const Eigen::Index nfree = kff.rows();
    const SolveMethod method = select_solve_method(nfree, options);

    if (method == SolveMethod::kCG) {
        const int max_iters = cg_iteration_budget(nfree, options);
        const int report_every = std::max(options.cg_progress_chunk, 0);
        int iters = 0;
        double rel_error = std::numeric_limits<double>::infinity();
        Eigen::VectorXd uf;

        // K_ff is SPD, so the preconditioner must be too: an incomplete
        // *Cholesky* keeps the preconditioned operator symmetric, which is what
        // CG's convergence argument rests on. The previous IncompleteLUT is not
        // symmetric on a general SPD matrix, which is a large part of why CG
        // used to crawl here. Eigen's IncompleteCholesky auto-shifts until the
        // factorisation succeeds; fall back to Jacobi if even that fails.
        Eigen::IncompleteCholesky<double> ichol;
        ichol.compute(kff);
        if (ichol.info() == Eigen::Success) {
            uf = run_cg(kff, rhs, ichol, options.cg_tol, max_iters, report_every,
                        options.on_progress, iters, rel_error);
        }
        if (rel_error > options.cg_tol) {
            const Eigen::DiagonalPreconditioner<double> jacobi(kff);
            uf = run_cg(kff, rhs, jacobi, options.cg_tol, max_iters, report_every,
                        options.on_progress, iters, rel_error);
        }
        if (rel_error > options.cg_tol) {
            throw FeaError(
                std::format("solve_elastostatics: CG failed to converge after {} iterations "
                            "(tol={}, relative residual={})",
                            iters, options.cg_tol, rel_error));
        }
        if (options.on_progress) {
            options.on_progress(iters, max_iters, rel_error);
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
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    if (loads.size() != ndof) {
        throw FeaError(std::format("solve_elastostatics: load vector size {} != 3N = {}",
                                   loads.size(), ndof));
    }
    if (!std::isfinite(options.cg_tol) || options.cg_tol <= 0.0) {
        throw FeaError("solve_elastostatics: cg_tol must be finite and positive");
    }
    if (!loads.allFinite()) {
        throw FeaError("solve_elastostatics: load vector contains a non-finite value");
    }
    for (const auto& [dof, value] : dirichlet.dof_values) {
        if (dof < 0 || dof >= ndof) {
            throw FeaError(
                std::format("solve_elastostatics: constrained DOF {} out of range", dof));
        }
        if (!std::isfinite(value)) {
            throw FeaError(std::format(
                "solve_elastostatics: constrained DOF {} has a non-finite value", dof));
        }
    }

    const Eigen::Index nfree = ndof - static_cast<Eigen::Index>(dirichlet.dof_values.size());
    const auto estimate = estimate_solve_resources(mesh, nfree);
    const auto budget = effective_memory_budget(options.max_mem_gb);
    const auto decision =
        decide_solve_method(nfree, options, estimate, budget.effective_cap_bytes);
    if (decision.estimated_bytes > budget.effective_cap_bytes) {
        const bool direct = decision.method == SolveMethod::kDirect;
        throw FeaError(
            std::format("solve_elastostatics: estimated {} solve footprint {} exceeds "
                        "effective memory cap "
                        "{} (limiting term: {}); raise --max-mem <GB> or free system memory",
                        direct ? "LDLT" : "CG", format_memory_bytes(decision.estimated_bytes),
                        format_memory_bytes(budget.effective_cap_bytes),
                        limiting_resource_term(estimate, direct)));
    }
    if (!decision.note.empty() && options.on_note) {
        options.on_note(decision.note);
    }
    init_runtime_performance();

    SolveOptions selected_options = options;
    selected_options.method = decision.method;

    // Map global DOFs to reduced (free) indices; -1 marks constrained.
    std::vector<Eigen::Index> reduced(static_cast<std::size_t>(ndof), -1);
    Eigen::Index reduced_count = 0;
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        if (!dirichlet.dof_values.contains(dof)) {
            reduced[static_cast<std::size_t>(dof)] = reduced_count++;
        }
    }

    const auto k = assemble_stiffness(mesh, material);

    // Reduced system: K_ff u_f = f_f - K_fc u_c.
    Eigen::VectorXd rhs(reduced_count);
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
    Eigen::SparseMatrix<double> kff(reduced_count, reduced_count);
    kff.setFromTriplets(triplets.begin(), triplets.end());

    // The allocation-free preflight above preserves the normal cell-independent
    // threshold policy, except that kAuto may downgrade LDLT to CG when only the
    // iterative footprint fits. Explicit method choices remain authoritative.
    const Eigen::VectorXd uf = solve_reduced(kff, rhs, selected_options);

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
