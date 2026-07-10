// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"
#include "fea/shape.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

using namespace polymesh::fea;

TEST_CASE("pyramid5 partition of unity at nodes") {
    const auto nodes = reference_nodes(ElementType::kPyramid5);
    REQUIRE(nodes.size() == 5);
    for (std::size_t j = 0; j < nodes.size(); ++j) {
        const auto s = eval_shape(ElementType::kPyramid5, nodes[j]);
        REQUIRE(std::abs(s.n.sum() - 1.0) < 1e-12);
        for (std::size_t i = 0; i < 5; ++i) {
            REQUIRE(std::abs(s.n[static_cast<Eigen::Index>(i)] - (i == j ? 1.0 : 0.0)) <
                    1e-10);
        }
    }
}

TEST_CASE("pyramid5 stiffness has six rigid-body modes") {
    NodalMesh mesh;
    // Unit-ish pyramid: base [0,1]^2 z=0, apex (0.5,0.5,1)
    mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5, 0.5, 1}};
    mesh.elements.push_back(NodalElement{ElementType::kPyramid5, {0, 1, 2, 3, 4}});
    const Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto k = element_stiffness(mesh, mesh.elements[0], mat);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
    REQUIRE(es.info() == Eigen::Success);
    int zeros = 0;
    for (int i = 0; i < k.rows(); ++i) {
        if (std::abs(es.eigenvalues()[i]) < 1e-6 * mat.youngs_modulus)
            ++zeros;
    }
    REQUIRE(zeros == 6);
}
