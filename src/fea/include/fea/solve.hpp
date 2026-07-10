// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Linear elastostatics solve: assemble, apply Dirichlet constraints by
// partitioning, then sparse direct LDLT or iterative CG.

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

/// Linear solver for the reduced free-DOF system K_ff u_f = rhs.
enum class SolveMethod {
    /// SimplicialLDLT when nfree ≤ cg_threshold; ConjugateGradient otherwise.
    kAuto,
    /// Always sparse Cholesky (SimplicialLDLT). Exact for SPD within roundoff.
    kDirect,
    /// ConjugateGradient with DiagonalPreconditioner (SPD iterative).
    kCG,
};

/// Options for `solve_elastostatics`. Defaults keep LDLT for small/medium free
/// systems so Tier-0 patch tests stay machine-exact on the direct path.
struct SolveOptions {
    SolveMethod method = SolveMethod::kAuto;

    /// Free-DOF count above which `kAuto` selects CG. Default 8000 avoids O(N³)
    /// memory growth of sparse direct factorisation on large meshes while leaving
    /// typical verification meshes on LDLT.
    Eigen::Index cg_threshold = 8000;

    /// CG relative residual tolerance: stop when ‖r‖ / ‖b‖ ≤ cg_tol.
    double cg_tol = 1e-10;

    /// CG iteration cap. 0 means max(2 * nfree, 1000).
    int cg_max_iters = 0;
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
/// Default `options` use sparse LDLT for nfree ≤ 8000 and CG above that.
/// Force `SolveMethod::kDirect` for exact patch-test path; force `kCG` to
/// exercise the iterative solver on small systems.
Eigen::VectorXd solve_elastostatics(const NodalMesh& mesh, const Material& material,
                                    const Dirichlet& dirichlet, const Eigen::VectorXd& loads,
                                    const SolveOptions& options = {});

/// Strain energy 1/2 u^T K u, joules.
double strain_energy(const NodalMesh& mesh, const Material& material,
                     const Eigen::VectorXd& u);

} // namespace polymesh::fea
