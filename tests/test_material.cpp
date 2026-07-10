// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Dense>

#include <cmath>

using polymesh::fea::Material;

TEST_CASE("d matrix matches Lamé constants") {
    // Steel-ish: E = 200 GPa, nu = 0.3.
    const Material m{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto d = m.d_matrix();
    const double l = m.lambda();
    const double mu = m.mu();
    CHECK(std::abs(d(0, 0) - (l + 2.0 * mu)) < 1.0);
    CHECK(std::abs(d(0, 1) - l) < 1.0);
    CHECK(std::abs(d(3, 3) - mu) < 1.0);
    // Symmetry.
    CHECK(d.isApprox(d.transpose()));
}

TEST_CASE("uniaxial stress recovers Young's modulus") {
    // For uniaxial stress σ_xx with free lateral contraction,
    // σ_xx / ε_xx must equal E. Invert D and check compliance.
    const Material m{.youngs_modulus = 70e9, .poissons_ratio = 0.33};
    const Eigen::Matrix<double, 6, 6> c = m.d_matrix().inverse();
    const double e_recovered = 1.0 / c(0, 0);
    CHECK(std::abs(e_recovered - m.youngs_modulus) / m.youngs_modulus < 1e-12);
}
