// SPDX-License-Identifier: BSD-3-Clause
#include "fea/constraints.hpp"

#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace polymesh::fea {

LinearConstraints::LinearConstraints(std::vector<LinearConstraint> constraints) {
    constraints_.reserve(constraints.size());
    for (auto& constraint : constraints) {
        add(std::move(constraint));
    }
}

void LinearConstraints::add(LinearConstraint constraint) {
    if (slave_indices_.contains(constraint.slave_dof)) {
        throw FeaError(std::format("LinearConstraints: slave DOF {} appears more than once",
                                   constraint.slave_dof));
    }
    if (constraint.masters.empty()) {
        throw FeaError(std::format("LinearConstraints: slave DOF {} has no masters",
                                   constraint.slave_dof));
    }
    std::set<std::uint32_t> seen_masters;
    for (const auto& [master, weight] : constraint.masters) {
        if (master == constraint.slave_dof) {
            throw FeaError(
                std::format("LinearConstraints: slave DOF {} directly depends on itself",
                            constraint.slave_dof));
        }
        if (slave_indices_.contains(master)) {
            throw FeaError(std::format(
                "LinearConstraints: chained constraint: master DOF {} is itself a slave",
                master));
        }
        if (!seen_masters.insert(master).second) {
            throw FeaError(
                std::format("LinearConstraints: master DOF {} is repeated for slave DOF {}",
                            master, constraint.slave_dof));
        }
        if (!std::isfinite(weight)) {
            throw FeaError(
                std::format("LinearConstraints: slave DOF {} has a non-finite weight",
                            constraint.slave_dof));
        }
    }
    slave_indices_.emplace(constraint.slave_dof, constraints_.size());
    constraints_.push_back(std::move(constraint));
}

bool LinearConstraints::is_slave(std::uint32_t dof) const noexcept {
    return slave_indices_.contains(dof);
}

void LinearConstraints::validate(Eigen::Index n_dof) const {
    if (n_dof < 0 ||
        static_cast<std::uint64_t>(n_dof) >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ull) {
        throw FeaError(std::format("LinearConstraints: invalid DOF count {}", n_dof));
    }
    if (static_cast<Eigen::Index>(constraints_.size()) > n_dof) {
        throw FeaError("LinearConstraints: more slave DOFs than total DOFs");
    }
    for (const auto& constraint : constraints_) {
        if (static_cast<Eigen::Index>(constraint.slave_dof) >= n_dof) {
            throw FeaError(
                std::format("LinearConstraints: slave DOF {} out of range for {} DOFs",
                            constraint.slave_dof, n_dof));
        }
        for (const auto& [master, weight] : constraint.masters) {
            (void)weight;
            if (static_cast<Eigen::Index>(master) >= n_dof) {
                throw FeaError(
                    std::format("LinearConstraints: master DOF {} out of range for {} DOFs",
                                master, n_dof));
            }
            if (slave_indices_.contains(master)) {
                throw FeaError(std::format(
                    "LinearConstraints: chained or cyclic constraint: master DOF {} is "
                    "itself a slave",
                    master));
            }
        }
    }
}

Eigen::SparseMatrix<double> LinearConstraints::transform(Eigen::Index n_dof) const {
    validate(n_dof);
    const Eigen::Index n_free = n_dof - static_cast<Eigen::Index>(constraints_.size());
    std::vector<Eigen::Index> free_column(static_cast<std::size_t>(n_dof), -1);
    Eigen::Index column = 0;
    for (Eigen::Index dof = 0; dof < n_dof; ++dof) {
        if (!is_slave(static_cast<std::uint32_t>(dof))) {
            free_column[static_cast<std::size_t>(dof)] = column++;
        }
    }

    std::size_t nnz = static_cast<std::size_t>(n_free);
    for (const auto& constraint : constraints_) {
        nnz += constraint.masters.size();
    }
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(nnz);
    for (Eigen::Index dof = 0; dof < n_dof; ++dof) {
        const auto col = free_column[static_cast<std::size_t>(dof)];
        if (col >= 0) {
            triplets.emplace_back(dof, col, 1.0);
        }
    }
    for (const auto& constraint : constraints_) {
        for (const auto& [master, weight] : constraint.masters) {
            triplets.emplace_back(static_cast<Eigen::Index>(constraint.slave_dof),
                                  free_column[static_cast<std::size_t>(master)], weight);
        }
    }
    Eigen::SparseMatrix<double> result(n_dof, n_free);
    result.setFromTriplets(triplets.begin(), triplets.end());
    return result;
}

Eigen::VectorXd LinearConstraints::recover(const Eigen::VectorXd& u_reduced,
                                           Eigen::Index n_dof) const {
    validate(n_dof);
    const Eigen::Index expected = n_dof - static_cast<Eigen::Index>(constraints_.size());
    if (u_reduced.size() != expected) {
        throw FeaError(
            std::format("LinearConstraints: reduced vector size {} does not match expected {}",
                        u_reduced.size(), expected));
    }
    Eigen::VectorXd result(n_dof);
    Eigen::Index column = 0;
    for (Eigen::Index dof = 0; dof < n_dof; ++dof) {
        if (!is_slave(static_cast<std::uint32_t>(dof))) {
            result[dof] = u_reduced[column++];
        }
    }
    for (const auto& constraint : constraints_) {
        double value = 0.0;
        for (const auto& [master, weight] : constraint.masters) {
            value += weight * result[static_cast<Eigen::Index>(master)];
        }
        result[static_cast<Eigen::Index>(constraint.slave_dof)] = value;
    }
    return result;
}

} // namespace polymesh::fea
