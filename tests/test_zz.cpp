// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "fea/zz.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_CASE("ZZ recovery produces finite nodal stress and eta") {
    auto mesh = polymesh::test_support::box_hex_mesh(3, 3, 3, {1.0, 0.2, 0.2});
    polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    polymesh::fea::Dirichlet bc;
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] < 1e-9) {
            bc.fix_node(i);
        }
    }
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] > 1.0 - 1e-9) {
            loads(3 * static_cast<Eigen::Index>(i) + 1) = 1.0;
        }
    }
    const auto u = polymesh::fea::solve_elastostatics(mesh, mat, bc, loads);
    const auto zz = polymesh::fea::recover_zz(mesh, mat, u);
    REQUIRE(zz.nodal_stress.size() == mesh.nodes.size());
    REQUIRE(zz.element_eta.size() == mesh.elements.size());
    REQUIRE(std::isfinite(zz.global_eta));
    REQUIRE(zz.global_eta >= 0.0);
}
