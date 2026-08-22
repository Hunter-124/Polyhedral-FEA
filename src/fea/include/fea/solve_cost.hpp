// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "fea/constraints.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/SparseCore>

#include <cstdint>

namespace polymesh::fea {

struct Dirichlet;

/// Hardware-portable work implied by a reduced stiffness sparsity pattern.
/// Counts are independent of coefficient values and do not run a numeric solve.
struct SolveCostEstimate {
    Eigen::Index nfree = 0;
    std::uint64_t pattern_nnz = 0;
    /// Stored strict-lower entries in Eigen's unit-lower SimplicialLDLT factor.
    std::uint64_t factor_nnz = 0;
    double factor_flops = 0.0;
    double cg_flops_per_iter = 0.0;
    double cg_bytes_per_iter = 0.0;
};

/// Compulsory structural byte traffic for direct factorization plus two
/// triangular solves. This is host-independent and intentionally cache-agnostic.
[[nodiscard]] double estimate_direct_solve_bytes(const SolveCostEstimate& estimate);

/// Apply Eigen's default AMD ordering, build the elimination forest, and run
/// Gilbert-Ng-Peyton column counting without allocating the numeric factor.
[[nodiscard]] SolveCostEstimate
analyze_solve_cost(const Eigen::SparseMatrix<double>& pattern_only_kff);

/// Construct the exact reduced free-DOF pattern from element connectivity,
/// prescribed DOFs, and optional homogeneous linear constraints, then analyze
/// it without stiffness assembly or a numeric solve.
[[nodiscard]] SolveCostEstimate
analyze_solve_cost(const NodalMesh& mesh, const Dirichlet& dirichlet,
                   const LinearConstraints* constraints = nullptr);

} // namespace polymesh::fea
