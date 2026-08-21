// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Linear homogeneous multi-point constraints on displacement DOFs.
// Contract: u[slave] = sum(weight * u[master]); displacement units are metres.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace polymesh::fea {

struct LinearConstraint {
    std::uint32_t slave_dof = 0;
    std::vector<std::pair<std::uint32_t, double>> masters;
};

/// Deterministic homogeneous multi-point constraints.
/// Masters must be unconstrained DOFs; chained and cyclic constraints are rejected.
class LinearConstraints {
  public:
    LinearConstraints() = default;
    explicit LinearConstraints(std::vector<LinearConstraint> constraints);

    /// Adds one constraint. A slave DOF may appear only once.
    void add(LinearConstraint constraint);

    [[nodiscard]] bool empty() const noexcept { return constraints_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return constraints_.size(); }
    [[nodiscard]] bool is_slave(std::uint32_t dof) const noexcept;
    [[nodiscard]] const std::vector<LinearConstraint>& entries() const noexcept {
        return constraints_;
    }

    /// Validates ranges and rejects chained or cyclic master relationships.
    void validate(Eigen::Index n_dof) const;

    /// Builds u_full = T u_free. Free columns follow ascending original DOF index.
    [[nodiscard]] Eigen::SparseMatrix<double> transform(Eigen::Index n_dof) const;

    /// Expands reduced unconstrained values to all original DOFs.
    [[nodiscard]] Eigen::VectorXd recover(const Eigen::VectorXd& u_reduced,
                                          Eigen::Index n_dof) const;

  private:
    std::vector<LinearConstraint> constraints_;
    std::map<std::uint32_t, std::size_t> slave_indices_;
};

} // namespace polymesh::fea
