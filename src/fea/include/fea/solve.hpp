// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Linear elastostatics solve: assemble, apply Dirichlet constraints by
// partitioning, then sparse direct LDLT or iterative CG.

#include "fea/assembly.hpp"

#include <functional>
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

/// Linear solver for the reduced free-DOF system K_ff u_f = rhs.
enum class SolveMethod {
    /// SimplicialLDLT when nfree ≤ cg_threshold; ConjugateGradient otherwise.
    kAuto,
    /// Always sparse Cholesky (SimplicialLDLT). Exact for SPD within roundoff.
    kDirect,
    /// ConjugateGradient with an incomplete-Cholesky preconditioner (SPD
    /// iterative), bounded by `cg_max_iters`.
    kCG,
};

/// Options for `solve_elastostatics`. Defaults keep LDLT for small/medium free
/// systems so Tier-0 patch tests stay machine-exact on the direct path.
struct SolveOptions {
    SolveMethod method = SolveMethod::kAuto;

    /// Free-DOF count above which `kAuto` selects CG.
    ///
    /// The selection is deliberately cell-type independent: what makes a system
    /// hard for CG is its conditioning, and every mesher this project ships
    /// produces systems bad enough that preconditioned CG loses to a sparse
    /// Cholesky factorisation by two orders of magnitude at these sizes
    /// (measured: 11040-DOF plate-with-hole hex, 179 s CG vs 0.9 s LDLT).
    /// 3-D elastic sparsities of ~50k free DOF still factorise in seconds and
    /// well under a gigabyte, so `kAuto` stays direct up to there and only
    /// switches to CG where the factor genuinely stops fitting.
    Eigen::Index cg_threshold = 50000;

    /// CG relative residual tolerance: stop when ‖r‖ / ‖b‖ ≤ cg_tol.
    /// 1e-8 is ~5 digits below any discretisation error these meshes carry;
    /// chasing 1e-10 only bought iterations.
    double cg_tol = 1e-8;

    /// CG iteration cap. 0 means clamp(2 * nfree, 1000, 20000): always bounded,
    /// so a pathological system fails in bounded time instead of grinding.
    int cg_max_iters = 0;

    /// When `on_progress` is set, CG invokes the callback every this many
    /// iterations (and at completion). 0 = completion callback only. The
    /// recurrence is never restarted, so this only affects reporting rate.
    /// Keep the callback cheap.
    int cg_progress_chunk = 64;

    /// Optional progress callback (CG path only). Empty = no callbacks.
    /// Args: (iter, max_iters, relative residual).
    std::function<void(int, int, double)> on_progress;
};

/// Returns the concrete method `kAuto` (or an explicit method) will use for the
/// given free-DOF count. Useful for tests and diagnostics.
[[nodiscard]] SolveMethod select_solve_method(Eigen::Index nfree,
                                              const SolveOptions& options = {});

/// Solves K u = f with the given constraints. `loads` is the full-size global
/// load vector (3N); returns the full-size displacement vector with
/// prescribed values in place. Throws FeaError if the reduced system is
/// singular (insufficient constraints leave rigid-body modes) or if CG fails
/// to converge.
///
/// Default `options` use sparse LDLT for nfree ≤ `cg_threshold` (50000) and
/// bounded CG above that; the choice never depends on element type.
/// Force `SolveMethod::kDirect` for exact patch-test path; force `kCG` to
/// exercise the iterative solver on small systems.
Eigen::VectorXd solve_elastostatics(const NodalMesh& mesh, const Material& material,
                                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads,
                                    const SolveOptions& options = {});

/// Strain energy 1/2 u^T K u, joules.
double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u);

} // namespace polymesh::fea
