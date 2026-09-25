// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <span>

namespace polymesh::fea::detail {

/// One-dimensional Gauss-Legendre rule on [-1, 1], nodes in ascending order.
struct Gauss1d {
    std::span<const double> nodes;   // on [-1, 1]
    std::span<const double> weights; // sum to 2
};

/// Tabulated n-point rule for n in 1..6 (exact to degree 2n-1). Throws
/// FeaError for any other n.
Gauss1d gauss_1d(int n);

} // namespace polymesh::fea::detail
