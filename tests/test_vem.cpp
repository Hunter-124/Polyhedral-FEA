// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {
const Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

NodalMesh hex_as_vem_mesh(int nx, int ny, int nz) {
    auto mesh = box_hex_mesh(nx, ny, nz, {1.0, 1.0, 1.0});
    for (auto& el : mesh.elements) {
        auto poly = hex8_as_poly(el);
        el.type = ElementType::kPolyVem;
        el.faces = std::move(poly.faces);
    }
    mesh.check_validity();
    return mesh;
}
} // namespace

TEST_CASE("VEM poly volume of unit hex is 1") {
    std::vector<Eigen::Vector3d> c{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const auto faces =
        hex8_as_poly(NodalElement{ElementType::kHex8, {0, 1, 2, 3, 4, 5, 6, 7}}).faces;
    REQUIRE_THAT(poly_volume(c, faces), Catch::Matchers::WithinAbs(1.0, 1e-12));
}

TEST_CASE("VEM hex stiffness has six near-zero eigenvalues (RBM)") {
    NodalMesh mesh;
    mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    NodalElement el{ElementType::kHex8, {0, 1, 2, 3, 4, 5, 6, 7}};
    auto poly = hex8_as_poly(el);
    el.type = ElementType::kPolyVem;
    el.faces = poly.faces;
    mesh.elements = {el};
    const auto k = element_stiffness(mesh, el, kSteel);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
    REQUIRE(es.info() == Eigen::Success);
    // Six rigid-body modes → six eigenvalues ~0
    int zeros = 0;
    for (Eigen::Index i = 0; i < k.rows(); ++i) {
        if (std::abs(es.eigenvalues()[i]) < 1e-6 * kSteel.youngs_modulus) {
            ++zeros;
        }
    }
    REQUIRE(zeros == 6);
}

TEST_CASE("VEM patch test: constant strain on hex-as-poly mesh") {
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, 3e-4, -8e-4, 5e-4, -6e-4, 2e-4, 7e-4;
    auto mesh = hex_as_vem_mesh(2, 2, 2);
    Eigen::Vector3d lo = mesh.nodes.front(), hi = mesh.nodes.front();
    for (const auto& p : mesh.nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& p = mesh.nodes[i];
        const bool boundary =
            (p - lo).cwiseAbs().minCoeff() < 1e-9 || (hi - p).cwiseAbs().minCoeff() < 1e-9;
        if (boundary) {
            bc.fix_node(static_cast<std::uint32_t>(i), g * p);
        }
    }
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = solve_elastostatics(mesh, kSteel, bc, loads);
    double max_err = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d ue = g * mesh.nodes[i];
        max_err =
            std::max(max_err, (u.segment<3>(3 * static_cast<Eigen::Index>(i)) - ue).norm());
    }
    REQUIRE(max_err < 1e-9);
}
