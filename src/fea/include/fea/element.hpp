// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "fea/material.hpp"

#include <Eigen/Core>

#include <cstdint>

namespace polymesh::fea {

/// A single element's contribution to the global system.
///
/// Every implementation — isoparametric or VEM, any order — must satisfy the
/// Tier-0 gates in bench: exact constant-strain patch test on distorted
/// meshes, zero strain energy for rigid-body modes, and no spurious
/// zero-energy modes (see BENCHMARKS.md).
class Element {
  public:
    virtual ~Element() = default;

    /// Polynomial order p this instance is configured for.
    virtual std::uint8_t order() const = 0;

    /// Number of nodes (3 DOFs each).
    virtual std::size_t num_nodes() const = 0;

    /// Element stiffness matrix, size 3*num_nodes() square, in N/m.
    virtual Eigen::MatrixXd stiffness(const Material& material) const = 0;
};

} // namespace polymesh::fea
