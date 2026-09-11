// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Linear elastostatics solve: assemble, apply optional homogeneous multi-point
// constraints, partition Dirichlet DOFs, then sparse direct LDLT or iterative CG.

#include "fea/assembly.hpp"
#include "fea/constraints.hpp"
#include "fea/resource_budget.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

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

/// Linear solver for the reduced free-DOF system K_ff u_f = rhs.
enum class SolveMethod {
    /// Direct ladder when nfree ≤ cg_threshold, ConjugateGradient above it.
    kAuto,
    /// Sparse direct: CholmodSupernodalLLT where SuiteSparse is compiled in,
    /// else SimplicialLDLT(AMD), with SparseLU for a non-positive pivot.
    /// Exact for SPD within roundoff.
    kDirect,
    /// ConjugateGradient with an incomplete-Cholesky preconditioner (SPD
    /// iterative), bounded by `cg_max_iters`.
    kCG,
};

/// Free-DOF count above which `kAuto` prefers CG, before the memory guard.
///
/// With SuiteSparse the direct rung is a supernodal (BLAS3) factorization,
/// which beats preconditioned CG on these sparsities by orders of magnitude at
/// every size that fits in memory — so the DOF number stops being the deciding
/// factor and `decide_solve_method`'s memory budget is. The threshold survives
/// only as a backstop: above it the factor estimate is far enough outside
/// anything measured that the bounded-memory iterative path is the safer
/// default. Without SuiteSparse the direct rung is a scalar simplicial
/// factorization, which really does fall off a cliff, so the historical
/// 50,000 stands.
#if defined(POLYMESH_WITH_CHOLMOD)
inline constexpr Eigen::Index kDefaultCgThreshold = 1'500'000;
#else
inline constexpr Eigen::Index kDefaultCgThreshold = 50'000;
#endif

/// Options for `solve_elastostatics`. Defaults keep the direct ladder for every
/// system that fits in the memory budget, so Tier-0 patch tests stay
/// machine-exact on the direct path.
struct SolveOptions {
    SolveMethod method = SolveMethod::kAuto;

    /// Maximum estimated solve footprint in decimal GB. 0 = automatic cap at
    /// 70% of the operating system's currently available memory.
    double max_mem_gb = 0.0;

    /// Free-DOF count above which `kAuto` selects CG; see `kDefaultCgThreshold`
    /// for why the number depends on which direct rung this build has.
    ///
    /// The selection is deliberately cell-type independent: what makes a system
    /// hard for CG is its conditioning, and every mesher this project ships
    /// produces systems bad enough that preconditioned CG loses to a sparse
    /// Cholesky factorisation by two orders of magnitude at these sizes
    /// (measured: 11040-DOF plate-with-hole hex, 179 s CG vs 0.9 s LDLT;
    /// 233,820-DOF curved tet10 plate, hours of CG that never reached 1e-8 vs
    /// 19.6 s of supernodal factorization).
    Eigen::Index cg_threshold = kDefaultCgThreshold;

    /// CG true relative residual tolerance: return only when
    /// ‖b-K*x‖ / ‖b‖ ≤ cg_tol. If the recursive residual has fallen 100× since
    /// the last reliable measurement, a drifted recurrence restarts from
    /// b-K*x. At most four reliable replacements are allowed per attempt; the
    /// reduction and count bounds prevent futile restart cycles at an
    /// unattainable-accuracy floor.
    /// 1e-8 is ~5 digits below any discretisation error these meshes carry;
    /// chasing 1e-10 only bought iterations.
    double cg_tol = 1e-8;

    /// Maximum independently recomputed true relative residual that may be
    /// returned when no preconditioner reaches `cg_tol`. This is an explicit
    /// degraded-acceptance contract, not the iteration target: CG still pursues
    /// `cg_tol` through every available preconditioner. The solver emits a loud
    /// note with achieved residual and provenance when this threshold is used.
    /// Callers may tighten it; Testlab independently remeasures its health gate.
    double cg_accept_tol = 1e-6;

    /// CG iteration cap per preconditioner attempt. 0 means
    /// clamp(2 * nfree, 1000, 30000): always bounded, so a pathological system
    /// fails in bounded time instead of grinding.
    int cg_max_iters = 0;

    /// When `on_progress` is set, CG invokes the callback every this many
    /// iterations, at a reliable-residual restart, and at completion.
    /// 0 = restart/completion callbacks only. Reporting never restarts the
    /// recurrence; only detected recursive-residual drift does. Four iterations
    /// keeps cooperative GUI cancellation comfortably below a second even on
    /// large systems; keep the callback cheap.
    int cg_progress_chunk = 4;

    /// Optional progress callback (CG path only). Empty = no callbacks.
    /// Args: (iteration within the current preconditioner attempt, per-attempt
    /// max iterations, relative residual). The completion value is the
    /// independently recomputed true residual.
    std::function<void(int, int, double)> on_progress;

    /// Optional solve note: method-selection decisions plus CG preconditioner
    /// attempts, failures, and final true-residual provenance. Preflight notes
    /// run before assembly; CG notes run during/after the iterative solve.
    std::function<void(std::string_view)> on_note;
};

/// Concrete method and memory estimate chosen during allocation-free preflight.
struct SolveDecision {
    SolveMethod method = SolveMethod::kDirect;
    std::uint64_t estimated_bytes = 0;
    std::string note;
};

/// Portable work account for the numeric solve selected at runtime.
struct SolveCostMeasured {
    std::string method;
    int cg_iterations = 0;
    int cg_restarts = 0;
    std::uint64_t factor_nnz = 0;
    double flops = 0.0;
    double bytes = 0.0;
};

/// Wall-clock breakdown of one `solve_elastostatics` call, milliseconds.
/// Measured inside the solve, so a caller reporting a phase table never has to
/// infer where the time went from a single total. `analyze` is the symbolic
/// factorization (direct) or the preconditioner build (CG); `factorize` is the
/// numeric factorization or the CG iteration loop; `backsolve` is the
/// triangular solves plus recovery of the full displacement vector.
struct SolvePhaseTimings {
    double preflight_ms = 0.0;
    double assemble_ms = 0.0;
    double reduce_ms = 0.0;
    double analyze_ms = 0.0;
    double factorize_ms = 0.0;
    double backsolve_ms = 0.0;
    double total_ms = 0.0;
};

struct LinearSolveResult {
    Eigen::VectorXd u;
    SolveCostMeasured cost;
    SolvePhaseTimings phases;
};

/// Symmetric diagonal (Jacobi) equilibration of an SPD sparse matrix: returns
/// s_i = 1/sqrt(a_ii). Throws FeaError if any diagonal entry is ≤ 0 (the
/// matrix is not SPD). With S = diag(s) the exact congruence S·A·S has unit
/// diagonal and identical eigenvector structure up to scaling. Used to
/// equilibrate K_ff before preconditioning: MPC transforms and graded meshes
/// spread the diagonal over orders of magnitude, which degrades incomplete
/// Cholesky.
[[nodiscard]] Eigen::VectorXd
symmetric_diagonal_scaling(const Eigen::SparseMatrix<double>& spd);

/// Returns the concrete method `kAuto` (or an explicit method) will use for the
/// given free-DOF count. Useful for tests and diagnostics.
[[nodiscard]] SolveMethod select_solve_method(Eigen::Index nfree,
                                              const SolveOptions& options = {});

/// Apply the normal DOF threshold plus the effective memory cap. Explicit
/// kDirect/kCG requests remain authoritative; only kAuto may downgrade LDLT to
/// CG when the direct footprint does not fit and CG does.
[[nodiscard]] SolveDecision decide_solve_method(Eigen::Index nfree,
                                                const SolveOptions& options,
                                                const SolveResourceEstimate& estimate,
                                                std::uint64_t effective_cap_bytes);

/// Solves K u = f with the given Dirichlet and optional linear constraints.
/// `loads` is the full-size global load vector (3N). The returned displacement
/// has prescribed and slave values in place; `cost` records portable work for
/// the concrete direct or iterative method. A prescribed Dirichlet DOF must not
/// also be a slave. Throws FeaError if the reduced system is singular
/// (insufficient constraints leave rigid-body modes) or CG fails.
///
/// Default `options` use sparse LDLT for nfree ≤ `cg_threshold` (50000) and
/// bounded CG above that; the choice never depends on element type.
/// Force `SolveMethod::kDirect` for exact patch-test path; force `kCG` to
/// exercise the iterative solver on small systems.
[[nodiscard]] LinearSolveResult
solve_elastostatics(const NodalMesh& mesh, const Material& material,
                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads,
                    const SolveOptions& options = {},
                    const LinearConstraints* constraints = nullptr);

/// Strain energy 1/2 u^T K u, joules.
double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u);

} // namespace polymesh::fea
