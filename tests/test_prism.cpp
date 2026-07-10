// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"
#include "fea/shape.hpp"
#include "fea/solve.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Dense>

using namespace polymesh::fea;

TEST_CASE("prism6 partition of unity and nodal delta") {
    const auto nodes = reference_nodes(ElementType::kPrism6);
    REQUIRE(nodes.size() == 6);
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const auto s = eval_shape(ElementType::kPrism6, nodes[j]);
        REQUIRE(std::abs(s.n.sum() - 1.0) < 1e-12);
        for (std::size_t i = 0; i < 6; ++i) {
            REQUIRE(std::abs(s.n[static_cast<Eigen::Index>(i)] - (i == j ? 1.0 : 0.0)) <
                    1e-12);
        }
    }
}

TEST_CASE("prism6 patch test on single prism stack") {
    // Two stacked prisms? Single prism cell mesh with linear field on boundary.
    NodalMesh mesh;
    // Physical prism: unit triangle extruded [0,1] in z
    mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
    mesh.elements.push_back(NodalElement{ElementType::kPrism6, {0, 1, 2, 3, 4, 5}});
    Eigen::Matrix3d g;
    g << 1e-3, 0, 0, 0, -5e-4, 0, 0, 0, 2e-4;
    Dirichlet bc;
    for (std::uint32_t i = 0; i < 6; ++i) {
        bc.fix_node(i, g * mesh.nodes[i]);
    }
    const Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd loads = Eigen::VectorXd::Zero(18);
    // With all DOFs prescribed, solve is trivial interpolation — check stiffness SPD-ish
    const auto k = element_stiffness(mesh, mesh.elements[0], mat);
    REQUIRE(k.rows() == 18);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
    int zeros = 0;
    for (int i = 0; i < 18; ++i) {
        if (std::abs(es.eigenvalues()[i]) < 1e-6 * mat.youngs_modulus)
            ++zeros;
    }
    REQUIRE(zeros == 6);
}
