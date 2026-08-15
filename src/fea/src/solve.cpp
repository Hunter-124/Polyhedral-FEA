// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"

#include "fea/backend.hpp"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::fea {

namespace {

// Eigen exposes IncompleteCholesky::shift() only from 5.0 on; 3.4 (Ubuntu LTS,
// and the version Chudware pins) keeps the escalated shift private. The value is
// diagnostic only, so report the exact final shift where the accessor exists and
// the requested initial shift otherwise, labelled so a log is never misread.
template <class T>
concept HasShiftAccessor = requires(const T& ic) {
    { ic.shift() } -> std::convertible_to<double>;
};

template <class T>
std::string ichol_shift_text(const T& ic, double requested_initial_shift) {
    if constexpr (HasShiftAccessor<T>) {
        return std::format("{}", ic.shift());
    } else {
        return std::format("{} (initial; final not exposed by this Eigen)",
                           requested_initial_shift);
    }
}

} // namespace

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
constexpr int kCgAutoIterCap = 30000;

int cg_iteration_budget(Eigen::Index nfree, const SolveOptions& options) {
    if (options.cg_max_iters > 0) {
        return options.cg_max_iters;
    }
    return static_cast<int>(
        std::clamp<Eigen::Index>(2 * nfree, Eigen::Index{1000}, Eigen::Index{kCgAutoIterCap}));
}

/// Why a PCG attempt stopped.
enum class CgStop {
    kConverged,
    kIterationLimit,
    kAttainableAccuracy,
    kBreakdown,
};

struct CgAttempt {
    Eigen::VectorXd x;
    int iterations = 0;
    int reliable_restarts = 0;
    double true_relative_residual = std::numeric_limits<double>::infinity();
    CgStop stop = CgStop::kBreakdown;
};

std::string_view cg_stop_text(CgStop stop) {
    switch (stop) {
    case CgStop::kConverged:
        return "converged";
    case CgStop::kIterationLimit:
        return "iteration limit";
    case CgStop::kAttainableAccuracy:
        return "attainable-accuracy limit";
    case CgStop::kBreakdown:
        return "breakdown";
    }
    return "unknown";
}

std::string join_attempts(const std::vector<std::string>& attempts) {
    std::string joined;
    for (const auto& attempt : attempts) {
        if (!joined.empty()) {
            joined += "; ";
        }
        joined += attempt;
    }
    return joined;
}

/// Preconditioned conjugate gradient that reports progress from inside one
/// uninterrupted recurrence. Before accepting recursive convergence it
/// recomputes b-A*x. If round-off has let the recurrence drift, that true
/// residual restarts the recurrence instead of returning a false success.
template <typename Precond>
CgAttempt run_cg(const Eigen::SparseMatrix<double>& a, const Eigen::VectorXd& rhs,
                 const Precond& precond, double tol, int max_iters, int report_every,
                 const std::function<void(int, int, double)>& on_progress) {
    CgAttempt result;
    result.x = Eigen::VectorXd::Zero(rhs.size());
    const double rhs_norm2 = rhs.squaredNorm();
    if (!(rhs_norm2 > 0.0)) {
        result.true_relative_residual = 0.0;
        result.stop = CgStop::kConverged;
        return result;
    }
    const double threshold = tol * tol * rhs_norm2;
    // A reliable replacement is useful after the recursive residual has fallen
    // substantially from the last independently measured residual. Limiting
    // replacement frequency prevents a requested tolerance below attainable
    // accuracy from repeatedly discarding the Krylov space, while allowing a
    // long ill-conditioned solve several updates to repair accumulated drift.
    constexpr double kReliableUpdateDelta = 1e-2;
    constexpr int kMaxReliableRestarts = 4;

    Eigen::VectorXd r = rhs; // b-A*x0 with x0=0
    Eigen::VectorXd z = precond.solve(r);
    Eigen::VectorXd p = z;
    Eigen::VectorXd ap(rhs.size());
    double r_norm2 = rhs_norm2;
    double last_reliable_norm2 = rhs_norm2;
    double rz = r.dot(z);
    bool breakdown = !(rz > 0.0);
    bool attainable_accuracy = false;

    while (!breakdown && result.iterations < max_iters && r_norm2 > threshold) {
        ap.noalias() = a * p;
        const double pap = p.dot(ap);
        if (!(pap > 0.0)) {
            breakdown = true; // K_ff is not positive definite along p
            break;
        }
        const double alpha = rz / pap;
        result.x += alpha * p;
        r -= alpha * ap;
        r_norm2 = r.squaredNorm();
        ++result.iterations;

        if (r_norm2 <= threshold) {
            // Recursive residuals lose their connection to b-A*x on long,
            // ill-conditioned solves. Never report success without measuring
            // the same residual that callers and campaign health gates see.
            const double recursive_norm2 = r_norm2;
            r = rhs;
            r.noalias() -= a * result.x;
            r_norm2 = r.squaredNorm();
            if (r_norm2 <= threshold) {
                break;
            }

            const double update_bound2 =
                kReliableUpdateDelta * kReliableUpdateDelta * last_reliable_norm2;
            if (recursive_norm2 > update_bound2 ||
                result.reliable_restarts >= kMaxReliableRestarts) {
                // The recurrence has not earned another replacement, or the
                // bounded reliable-update allowance is exhausted. At this
                // precision another restart would cycle at the same floor.
                attainable_accuracy = true;
                break;
            }

            last_reliable_norm2 = r_norm2;
            ++result.reliable_restarts;
            z = precond.solve(r);
            rz = r.dot(z);
            if (!(rz > 0.0)) {
                breakdown = true;
                break;
            }
            p = z;
            if (on_progress) {
                on_progress(result.iterations, max_iters, std::sqrt(r_norm2 / rhs_norm2));
            }
            continue;
        }

        z = precond.solve(r);
        const double rz_next = r.dot(z);
        if (!(rz_next > 0.0)) {
            breakdown = true; // preconditioner lost positive definiteness
            break;
        }
        p = (rz_next / rz) * p + z;
        rz = rz_next;
        if (on_progress && report_every > 0 && result.iterations % report_every == 0) {
            on_progress(result.iterations, max_iters, std::sqrt(r_norm2 / rhs_norm2));
        }
    }

    // The error/result contract is always based on the independently
    // recomputed residual, including iteration-limit and breakdown exits.
    r = rhs;
    r.noalias() -= a * result.x;
    result.true_relative_residual = std::sqrt(r.squaredNorm() / rhs_norm2);
    if (result.true_relative_residual <= tol) {
        result.stop = CgStop::kConverged;
    } else if (attainable_accuracy) {
        result.stop = CgStop::kAttainableAccuracy;
    } else if (!breakdown && result.iterations >= max_iters) {
        result.stop = CgStop::kIterationLimit;
    } else {
        result.stop = CgStop::kBreakdown;
    }
    return result;
}

Eigen::VectorXd solve_reduced(const Eigen::SparseMatrix<double>& kff,
                              const Eigen::VectorXd& rhs, const SolveOptions& options) {
    const Eigen::Index nfree = kff.rows();
    const SolveMethod method = select_solve_method(nfree, options);

    if (method == SolveMethod::kCG) {
        const int max_iters = cg_iteration_budget(nfree, options);
        const int report_every = std::max(options.cg_progress_chunk, 0);
        constexpr double kIcRetryInitialShift = 0.5;

        auto emit_note = [&](std::string note) {
            if (options.on_note) {
                options.on_note(note);
            }
        };
        auto attempt_summary = [](std::string_view preconditioner, const CgAttempt& attempt) {
            return std::format("{}: {}, {} iterations, true relative residual={}, "
                               "reliable restarts={}",
                               preconditioner, cg_stop_text(attempt.stop), attempt.iterations,
                               attempt.true_relative_residual, attempt.reliable_restarts);
        };

        std::vector<std::string> attempts;
        attempts.reserve(3);
        CgAttempt selected_attempt;
        std::string selected_preconditioner;
        int total_iterations = 0;
        bool have_attempt = false;
        bool target_met = false;

        auto run_attempt = [&](std::string_view name, const auto& preconditioner) {
            emit_note(std::format("CG using {} (target tol={}, acceptance tol={}, "
                                  "max iterations={})",
                                  name, options.cg_tol, options.cg_accept_tol, max_iters));
            CgAttempt attempt =
                run_cg(kff, rhs, preconditioner, options.cg_tol, max_iters, report_every,
                       options.on_progress);
            total_iterations += attempt.iterations;
            attempts.push_back(attempt_summary(name, attempt));
            const bool converged = attempt.stop == CgStop::kConverged;
            if (!have_attempt || converged ||
                attempt.true_relative_residual < selected_attempt.true_relative_residual) {
                selected_attempt = std::move(attempt);
                selected_preconditioner = name;
                have_attempt = true;
            }
            target_met = converged;
        };
        // Eigen's modified incomplete Cholesky is the primary preconditioner.
        // Its default initial shift (1e-3) makes ten attempts ending near
        // 0.256 on the scaled matrix. Continue at the next shift scale before
        // giving up on IC and falling back to Jacobi.
        constexpr double kIcDefaultInitialShift = 1e-3;
        Eigen::IncompleteCholesky<double> ichol;
        ichol.compute(kff);
        if (ichol.info() == Eigen::Success) {
            const std::string name = std::format(
                "incomplete Cholesky (shift={})", ichol_shift_text(ichol, kIcDefaultInitialShift));
            run_attempt(name, ichol);
        } else {
            const std::string failed_shift = ichol_shift_text(ichol, kIcDefaultInitialShift);
            attempts.push_back(std::format(
                "incomplete Cholesky: factorization failed after shift {}", failed_shift));
            emit_note(std::format(
                "CG incomplete Cholesky factorization failed after shift {}; "
                "retrying with initial shift {}",
                failed_shift, kIcRetryInitialShift));

            ichol.setInitialShift(kIcRetryInitialShift);
            ichol.factorize(kff); // reuse the already-computed AMD ordering
            if (ichol.info() == Eigen::Success) {
                const std::string name = std::format(
                    "shifted incomplete Cholesky (initial shift={}, final shift={})",
                    kIcRetryInitialShift, ichol_shift_text(ichol, kIcRetryInitialShift));
                run_attempt(name, ichol);
            } else {
                const std::string retry_shift = ichol_shift_text(ichol, kIcRetryInitialShift);
                attempts.push_back(std::format(
                    "shifted incomplete Cholesky: factorization failed after shift {}",
                    retry_shift));
                emit_note(std::format(
                    "CG shifted incomplete Cholesky factorization failed after shift {}; "
                    "using Jacobi",
                    retry_shift));
            }
        }

        if (!target_met) {
            if (!attempts.empty() && ichol.info() == Eigen::Success) {
                emit_note("CG incomplete Cholesky target not met; using Jacobi");
            }
            const Eigen::DiagonalPreconditioner<double> jacobi(kff);
            run_attempt("Jacobi", jacobi);
        }

        const std::string provenance = join_attempts(attempts);
        if (!target_met &&
            (!have_attempt ||
             selected_attempt.true_relative_residual > options.cg_accept_tol)) {
            throw FeaError(std::format(
                "solve_elastostatics: CG failed (target tol={}, acceptance tol={}, "
                "max iterations per attempt={}, total iterations={}, best preconditioner={}, "
                "best true relative residual={}, preconditioner attempts=[{}])",
                options.cg_tol, options.cg_accept_tol, max_iters, total_iterations,
                selected_preconditioner, selected_attempt.true_relative_residual, provenance));
        }
        if (options.on_progress) {
            options.on_progress(selected_attempt.iterations, max_iters,
                                selected_attempt.true_relative_residual);
        }
        if (target_met) {
            emit_note(std::format(
                "CG converged with {} after {} iterations ({} total; "
                "true relative residual={}; reliable restarts={}); attempts=[{}]",
                selected_preconditioner, selected_attempt.iterations, total_iterations,
                selected_attempt.true_relative_residual,
                selected_attempt.reliable_restarts, provenance));
        } else {
            emit_note(std::format(
                "CG TARGET NOT MET: accepted {} after {} iterations ({} total; "
                "target tol={}; acceptance tol={}; achieved true relative residual={}; "
                "reliable restarts={}); attempts=[{}]",
                selected_preconditioner, selected_attempt.iterations, total_iterations,
                options.cg_tol, options.cg_accept_tol,
                selected_attempt.true_relative_residual,
                selected_attempt.reliable_restarts, provenance));
        }
        return std::move(selected_attempt.x);
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

Eigen::VectorXd solve_elastostatics(
    const NodalMesh& mesh, const Material& material, const Dirichlet& dirichlet,
    const Eigen::VectorXd& loads, const SolveOptions& options,
    const LinearConstraints* constraints) {
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    if (loads.size() != ndof) {
        throw FeaError(std::format("solve_elastostatics: load vector size {} != 3N = {}",
                                   loads.size(), ndof));
    }
    if (!std::isfinite(options.cg_tol) || options.cg_tol <= 0.0) {
        throw FeaError("solve_elastostatics: cg_tol must be finite and positive");
    }
    if (!std::isfinite(options.cg_accept_tol) || options.cg_accept_tol <= 0.0) {
        throw FeaError("solve_elastostatics: cg_accept_tol must be finite and positive");
    }
    if (!loads.allFinite()) {
        throw FeaError("solve_elastostatics: load vector contains a non-finite value");
    }
    const bool has_linear_constraints = constraints != nullptr && !constraints->empty();
    if (has_linear_constraints) {
        constraints->validate(ndof);
    }
    const auto is_slave = [&](Eigen::Index dof) {
        return has_linear_constraints && dof >= 0 &&
               static_cast<std::uint64_t>(dof) <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) &&
               constraints->is_slave(static_cast<std::uint32_t>(dof));
    };
    for (const auto& [dof, value] : dirichlet.dof_values) {
        if (dof < 0 || dof >= ndof) {
            throw FeaError(
                std::format("solve_elastostatics: constrained DOF {} out of range", dof));
        }
        if (!std::isfinite(value)) {
            throw FeaError(std::format(
                "solve_elastostatics: constrained DOF {} has a non-finite value", dof));
        }
        if (is_slave(dof)) {
            throw FeaError(std::format(
                "solve_elastostatics: Dirichlet-prescribed DOF {} is a linear-constraint "
                "slave; prescribe its unconstrained masters instead",
                dof));
        }
    }

    const Eigen::Index constrained_dofs =
        has_linear_constraints ? static_cast<Eigen::Index>(constraints->size()) : 0;
    const Eigen::Index system_dofs = ndof - constrained_dofs;
    const Eigen::Index nfree =
        system_dofs - static_cast<Eigen::Index>(dirichlet.dof_values.size());
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

    // Map original unconstrained DOFs to T columns in ascending original order.
    std::vector<Eigen::Index> original_to_system(static_cast<std::size_t>(ndof), -1);
    Eigen::Index system_index = 0;
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        if (!is_slave(dof)) {
            original_to_system[static_cast<std::size_t>(dof)] = system_index++;
        }
    }
    std::map<Eigen::Index, double> system_dirichlet;
    for (const auto& [dof, value] : dirichlet.dof_values) {
        system_dirichlet.emplace(original_to_system[static_cast<std::size_t>(dof)], value);
    }

    auto k = assemble_stiffness(mesh, material);
    Eigen::SparseMatrix<double> k_system;
    Eigen::VectorXd f_system;
    if (has_linear_constraints) {
        const auto t = constraints->transform(ndof);
        Eigen::SparseMatrix<double> left = t.transpose() * k;
        k_system = left * t;
        f_system = t.transpose() * loads;
    } else {
        k_system = std::move(k);
        f_system = loads;
    }

    // Map constrained-system DOFs to Dirichlet-free indices; -1 is prescribed.
    std::vector<Eigen::Index> reduced(static_cast<std::size_t>(system_dofs), -1);
    Eigen::Index reduced_count = 0;
    for (Eigen::Index dof = 0; dof < system_dofs; ++dof) {
        if (!system_dirichlet.contains(dof)) {
            reduced[static_cast<std::size_t>(dof)] = reduced_count++;
        }
    }

    // Reduced system: K_ff u_f = f_f - K_fc u_c.
    Eigen::VectorXd rhs(reduced_count);
    for (Eigen::Index dof = 0; dof < system_dofs; ++dof) {
        const auto r = reduced[static_cast<std::size_t>(dof)];
        if (r >= 0) {
            rhs[r] = f_system[dof];
        }
    }
    std::vector<Eigen::Triplet<double>> triplets;
    for (int outer = 0; outer < k_system.outerSize(); ++outer) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(k_system, outer); it; ++it) {
            const auto row = reduced[static_cast<std::size_t>(it.row())];
            const auto col = reduced[static_cast<std::size_t>(it.col())];
            if (row >= 0 && col >= 0) {
                triplets.emplace_back(row, col, it.value());
            } else if (row >= 0 && col < 0) {
                rhs[row] -= it.value() * system_dirichlet.at(it.col());
            }
        }
    }
    Eigen::SparseMatrix<double> kff(reduced_count, reduced_count);
    kff.setFromTriplets(triplets.begin(), triplets.end());

    // The allocation-free preflight above uses the post-constraint free count;
    // kAuto may downgrade LDLT to CG when only the iterative footprint fits.
    const Eigen::VectorXd uf = solve_reduced(kff, rhs, selected_options);

    Eigen::VectorXd u_system(system_dofs);
    for (Eigen::Index dof = 0; dof < system_dofs; ++dof) {
        const auto r = reduced[static_cast<std::size_t>(dof)];
        u_system[dof] = r >= 0 ? uf[r] : system_dirichlet.at(dof);
    }
    return has_linear_constraints ? constraints->recover(u_system, ndof) : u_system;
}

double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u) {
    const auto k = assemble_stiffness(mesh, material);
    return 0.5 * u.dot(k * u);
}

} // namespace polymesh::fea
