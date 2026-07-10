// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "support/mms.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

#include <cmath>

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

/// Hex mesh with mid-edge nodes → VEM k=2 (ADR-0017).
NodalMesh hex_as_vem_k2_mesh(int nx, int ny, int nz) {
    auto mesh = promote_to_quadratic(box_hex_mesh(nx, ny, nz, {1.0, 1.0, 1.0}));
    for (auto& el : mesh.elements) {
        auto poly = hex20_as_poly(el);
        el.type = ElementType::kPolyVem;
        el.nodes = std::move(poly.nodes);
        el.faces = std::move(poly.faces);
    }
    mesh.check_validity();
    return mesh;
}

int count_near_zero_eigs(const Eigen::MatrixXd& k, double scale) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
    REQUIRE(es.info() == Eigen::Success);
    int zeros = 0;
    for (Eigen::Index i = 0; i < k.rows(); ++i) {
        if (std::abs(es.eigenvalues()[i]) < 1e-6 * scale) {
            ++zeros;
        }
    }
    return zeros;
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
    REQUIRE(count_near_zero_eigs(k, kSteel.youngs_modulus) == 6);
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

TEST_CASE("VEM k=2 hex20-as-poly: infer order, 6 RBM, 60 DOFs") {
    auto mesh = promote_to_quadratic(box_hex_mesh(1, 1, 1, {1.0, 1.0, 1.0}));
    REQUIRE(mesh.elements.size() == 1);
    auto poly = hex20_as_poly(mesh.elements[0]);
    REQUIRE(poly.nodes.size() == 20);
    REQUIRE(vem_infer_order(poly.nodes.size(), poly.faces) == 2);
    NodalElement el{ElementType::kPolyVem, poly.nodes, poly.faces};
    mesh.elements = {el};
    const auto k = element_stiffness(mesh, el, kSteel);
    REQUIRE(k.rows() == 60);
    REQUIRE(count_near_zero_eigs(k, kSteel.youngs_modulus) == 6);
}

TEST_CASE("VEM k=2 patch test: constant strain on hex mid-edge mesh") {
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, 3e-4, -8e-4, 5e-4, -6e-4, 2e-4, 7e-4;
    auto mesh = hex_as_vem_k2_mesh(2, 2, 2);
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

TEST_CASE("VEM k=2 exact quadratic: degree-2 MMS nearly zero energy error") {
    // [P₂]³ lies in the k=2 projector range → discrete solution exact up to
    // solver/load integration noise (guards projector + body-load dual).
    const auto mms = ManufacturedSolution::random(/*degree=*/2, /*seed=*/11, kSteel);
    auto mesh = hex_as_vem_k2_mesh(2, 2, 2);
    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& p = mesh.nodes[i];
        const bool boundary = p.minCoeff() < 1e-12 || p.maxCoeff() > 1.0 - 1e-12;
        if (boundary) {
            bc.fix_node(static_cast<std::uint32_t>(i), mms.displacement(p));
        }
    }
    const auto loads =
        assemble_body_load(mesh, [&](const Eigen::Vector3d& p) { return mms.body_force(p); });
    const auto u = solve_elastostatics(mesh, kSteel, bc, loads);
    const double error = energy_norm_error(mesh, kSteel, u, mms);
    const auto zero = Eigen::VectorXd::Zero(u.size());
    const double scale = energy_norm_error(mesh, kSteel, zero, mms);
    INFO("error " << error << " scale " << scale);
    REQUIRE(error < 1e-8 * scale);
}

TEST_CASE("VEM k=2 MMS energy-norm order ~ 2 on hex path") {
    // Cubic field outside [P₂]³; theory for k=2 is O(h²) in energy norm.
    const auto mms = ManufacturedSolution::random(/*degree=*/3, /*seed=*/2026, kSteel);
    auto solve_err = [&](int n) {
        auto mesh = hex_as_vem_k2_mesh(n, n, n);
        Dirichlet bc;
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            const auto& p = mesh.nodes[i];
            const bool boundary = p.minCoeff() < 1e-12 || p.maxCoeff() > 1.0 - 1e-12;
            if (boundary) {
                bc.fix_node(static_cast<std::uint32_t>(i), mms.displacement(p));
            }
        }
        const auto loads = assemble_body_load(
            mesh, [&](const Eigen::Vector3d& p) { return mms.body_force(p); });
        const auto u = solve_elastostatics(mesh, kSteel, bc, loads);
        return energy_norm_error(mesh, kSteel, u, mms);
    };
    const double e_coarse = solve_err(2);
    const double e_fine = solve_err(4);
    const double observed = std::log(e_coarse / e_fine) / std::log(2.0);
    INFO("VEM k=2: errors " << e_coarse << " -> " << e_fine << ", observed order "
                            << observed);
    REQUIRE(std::abs(observed - 2.0) < 0.2);
}
