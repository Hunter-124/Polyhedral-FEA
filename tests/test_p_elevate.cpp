// SPDX-License-Identifier: BSD-3-Clause

// D3: p-elevation API — promote tet4→tet10 / hex8→hex20 with shared mid-edge
// nodes; smooth marking; constant-strain patch still exact after full promote.

#include "adapt/error.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace fea = polymesh::fea;
using polymesh::test_support::box_hex_mesh;
using polymesh::test_support::box_tet_mesh;

namespace {

const fea::Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

double patch_max_error(const fea::NodalMesh& mesh) {
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;

    Eigen::Vector3d lo = mesh.nodes.front();
    Eigen::Vector3d hi = mesh.nodes.front();
    for (const auto& p : mesh.nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const double tol = 1e-9;
    fea::Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& p = mesh.nodes[i];
        const bool boundary =
            (p - lo).cwiseAbs().minCoeff() < tol || (hi - p).cwiseAbs().minCoeff() < tol;
        if (boundary) {
            bc.fix_node(static_cast<std::uint32_t>(i), g * p);
        }
    }
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = fea::solve_elastostatics(mesh, kSteel, bc, loads);
    double max_error = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d exact = g * mesh.nodes[i];
        const Eigen::Vector3d fem = u.segment<3>(3 * static_cast<Eigen::Index>(i));
        max_error = std::max(max_error, (fem - exact).norm());
    }
    return max_error;
}

} // namespace

TEST_CASE("D3: promote pure tet4 lattice → all tet10; nodes grow; patch < 1e-12") {
    auto mesh = box_tet_mesh(2, 2, 2, {1.0, 0.8, 1.2});
    const auto n_lin = mesh.nodes.size();
    const auto n_el = mesh.elements.size();
    REQUIRE(n_el == 6 * 8); // 6 tets × 2³ cells
    for (const auto& el : mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kTet4);
    }

    mesh = fea::promote_to_quadratic(mesh);
    REQUIRE_NOTHROW(mesh.check_validity());
    REQUIRE(mesh.elements.size() == n_el);
    REQUIRE(mesh.nodes.size() > n_lin);

    const auto counts = fea::count_element_types(mesh);
    REQUIRE(counts.tet4 == 0);
    REQUIRE(counts.tet10 == n_el);
    REQUIRE(counts.hex8 == 0);
    REQUIRE(counts.hex20 == 0);

    for (const auto& el : mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kTet10);
        REQUIRE(el.nodes.size() == 10);
    }

    // Shared mid-edge nodes: no duplicate geometry for same undirected edge.
    // Count unique midpoints via set of node ids used as mid-edge.
    std::set<std::uint32_t> mid_ids;
    for (const auto& el : mesh.elements) {
        for (std::size_t k = 4; k < 10; ++k) {
            mid_ids.insert(el.nodes[k]);
        }
    }
    REQUIRE(mid_ids.size() == mesh.nodes.size() - n_lin);

    CHECK(patch_max_error(mesh) < 1e-12);
}

TEST_CASE("D3: promote pure hex8 lattice → all hex20; patch < 1e-12") {
    auto mesh = box_hex_mesh(2, 2, 2, {1.0, 0.8, 1.2});
    const auto n_lin = mesh.nodes.size();
    const auto n_el = mesh.elements.size();
    mesh = fea::promote_to_quadratic(mesh);
    REQUIRE_NOTHROW(mesh.check_validity());
    REQUIRE(mesh.nodes.size() > n_lin);
    const auto counts = fea::count_element_types(mesh);
    REQUIRE(counts.hex8 == 0);
    REQUIRE(counts.hex20 == n_el);
    for (const auto& el : mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kHex20);
        REQUIRE(el.nodes.size() == 20);
    }
    CHECK(patch_max_error(mesh) < 1e-12);
}

TEST_CASE("D3: selective p_elevate leaves unlisted tet4 linear") {
    auto mesh = box_tet_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    const std::vector<std::size_t> elevate{0, 1, 2};
    mesh = fea::p_elevate(mesh, elevate);
    REQUIRE_NOTHROW(mesh.check_validity());
    const auto counts = fea::count_element_types(mesh);
    REQUIRE(counts.tet10 == 3);
    REQUIRE(counts.tet4 == mesh.elements.size() - 3);
    REQUIRE(mesh.elements[0].type == fea::ElementType::kTet10);
    REQUIRE(mesh.elements[3].type == fea::ElementType::kTet4);
}

TEST_CASE("D3: mark_smooth is complement of Dörfler") {
    // Synthetic η: a few large, many small.
    std::vector<double> eta{1.0, 0.9, 0.1, 0.05, 0.02, 0.01};
    const auto high = polymesh::adapt::dorfler_mark(eta, 0.3);
    const auto smooth = polymesh::adapt::mark_smooth(eta, 0.3);
    REQUIRE_FALSE(high.empty());
    REQUIRE_FALSE(smooth.empty());
    REQUIRE(high.size() + smooth.size() == eta.size());
    std::set<std::size_t> all;
    for (auto i : high) {
        all.insert(i);
    }
    for (auto i : smooth) {
        REQUIRE_FALSE(all.contains(i)); // no overlap
        all.insert(i);
    }
    REQUIRE(all.size() == eta.size());
}

TEST_CASE("D3: mark_smooth all when total η² is zero") {
    const std::vector<double> eta{0.0, 0.0, 0.0};
    const auto smooth = polymesh::adapt::mark_smooth(eta, 0.3);
    REQUIRE(smooth.size() == 3);
}

TEST_CASE("D3: mid-edge node is geometric midpoint of corners") {
    auto mesh = box_hex_mesh(1, 1, 1, {2.0, 2.0, 2.0});
    mesh = fea::promote_to_quadratic(mesh);
    REQUIRE(mesh.elements.size() == 1);
    const auto& el = mesh.elements[0];
    // Edge (0,1) → mid node 8
    const Eigen::Vector3d mid_expected =
        0.5 * (mesh.nodes[el.nodes[0]] + mesh.nodes[el.nodes[1]]);
    REQUIRE((mesh.nodes[el.nodes[8]] - mid_expected).norm() < 1e-15);
}
