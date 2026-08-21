// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/zz.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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

namespace {

struct CantileverResult {
    polymesh::fea::NodalMesh mesh;
    Eigen::VectorXd u;
};

/// Same 3×3×3 hex cantilever as above, optionally translated bodily in space.
/// Translation changes nothing physical: the stiffness, the displacement field
/// and every element stress are identical, so a stress recovery must return the
/// identical nodal field.
CantileverResult hex_cantilever(const Eigen::Vector3d& origin) {
    auto mesh = polymesh::test_support::box_hex_mesh(3, 3, 3, {1.0, 0.2, 0.2});
    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    polymesh::fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] < 1e-9) {
            bc.fix_node(i);
        }
        if (mesh.nodes[i][0] > 1.0 - 1e-9) {
            loads(3 * static_cast<Eigen::Index>(i) + 1) = 1.0;
        }
    }
    for (auto& p : mesh.nodes) {
        p += origin;
    }
    return {mesh, polymesh::fea::solve_elastostatics(mesh, mat, bc, loads)};
}

double max_von_mises(const std::vector<polymesh::fea::Stress>& s) {
    double m = 0.0;
    for (const auto& v : s) {
        m = std::max(m, polymesh::fea::von_mises(v));
    }
    return m;
}

struct SparseMidsidePatch {
    polymesh::fea::NodalMesh mesh;
    std::uint32_t midpoint = 0;
};

/// Four Tet10 elements around one edge. Their centroids form a barely
/// well-conditioned tetrahedron near x=1 while the shared midside node is at
/// the origin, reproducing the high-leverage extrapolation seen on graded
/// box-hole meshes.
SparseMidsidePatch sparse_midside_patch() {
    constexpr double half_edge = 0.02;
    const std::array<Eigen::Vector3d, 4> centroids{
        Eigen::Vector3d{0.95620406, -0.02697512, -0.05756971},
        Eigen::Vector3d{1.04332250, -0.21396582, 0.26428934},
        Eigen::Vector3d{1.07359006, 0.12373944, 0.16931630},
        Eigen::Vector3d{0.90601703, 0.00703957, -0.62175001}};

    polymesh::fea::NodalMesh linear;
    linear.nodes = {{-half_edge, 0.0, 0.0}, {half_edge, 0.0, 0.0}};
    for (const auto& centroid : centroids) {
        const Eigen::Vector3d transverse{0.0, centroid[2], -centroid[1]};
        const auto c = static_cast<std::uint32_t>(linear.nodes.size());
        linear.nodes.push_back(2.0 * centroid + transverse);
        const auto d = static_cast<std::uint32_t>(linear.nodes.size());
        linear.nodes.push_back(2.0 * centroid - transverse);
        linear.elements.emplace_back(polymesh::fea::ElementType::kTet4,
                                     std::vector<std::uint32_t>{0, 1, c, d});
    }

    auto quadratic = polymesh::fea::promote_to_quadratic(linear);
    const auto midpoint = quadratic.elements.front().nodes[4];
    return {std::move(quadratic), midpoint};
}

} // namespace

// Regression: the per-node patch fit used to be posed in ABSOLUTE coordinates,
// so a patch of diameter h sitting at distance R from the origin had a design
// matrix whose columns were constant to within h/R, and every node on a flat
// lattice face (coplanar incident centroids) made it exactly rank deficient.
// The unpivoted LDLᵀ then returned an arbitrary null-space point that the
// evaluation at |x| ≈ R amplified by R/h. Measured on plate+hole at h = 8 mm:
// max von Mises 1.6e12 Pa and η 4790 on a conforming, well-shaped hex lattice
// whose element stresses peak at 1.7e7 Pa.
TEST_CASE("ZZ recovery is invariant to a rigid translation of the mesh") {
    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    const auto at_origin = hex_cantilever({0.0, 0.0, 0.0});
    const auto far_away = hex_cantilever({500.0, -300.0, 120.0});

    const auto zz0 = polymesh::fea::recover_zz(at_origin.mesh, mat, at_origin.u);
    const auto zz1 = polymesh::fea::recover_zz(far_away.mesh, mat, far_away.u);

    REQUIRE(zz0.nodal_stress.size() == zz1.nodal_stress.size());
    const double scale = max_von_mises(zz0.nodal_stress);
    REQUIRE(scale > 0.0);
    double worst = 0.0;
    for (std::size_t i = 0; i < zz0.nodal_stress.size(); ++i) {
        worst = std::max(worst, (zz0.nodal_stress[i] - zz1.nodal_stress[i]).norm());
    }
    REQUIRE(worst <= 1e-9 * scale);
    REQUIRE(std::abs(zz0.global_eta - zz1.global_eta) <= 1e-9 * (1.0 + zz0.global_eta));
}

// A superconvergent patch recovery interpolates its samples; it must never
// report a stress orders of magnitude outside the element stresses it was built
// from. This is the assertion that fails loudest when a patch is rank deficient
// and its unresolvable slope is kept.
TEST_CASE("ZZ recovered nodal stress stays within the sampled element range") {
    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    for (const Eigen::Vector3d& origin :
         {Eigen::Vector3d{0.0, 0.0, 0.0}, Eigen::Vector3d{500.0, -300.0, 120.0}}) {
        const auto c = hex_cantilever(origin);
        const auto zz = polymesh::fea::recover_zz(c.mesh, mat, c.u);
        const auto raw = polymesh::fea::recover_nodal_stress(c.mesh, mat, c.u);
        const double raw_max = max_von_mises(raw);
        REQUIRE(raw_max > 0.0);
        REQUIRE(max_von_mises(zz.nodal_stress) <= 3.0 * raw_max);
        REQUIRE(zz.global_eta <= 1.0);
    }
}

TEST_CASE("ZZ sparse Tet10 midside patch does not extrapolate an unsmoothed fit") {
    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    auto patch = sparse_midside_patch();
    REQUIRE(patch.mesh.elements.size() == 4);
    REQUIRE(std::count_if(patch.mesh.elements.begin(), patch.mesh.elements.end(),
                          [&](const auto& element) {
                              return std::find(element.nodes.begin(), element.nodes.end(),
                                               patch.midpoint) != element.nodes.end();
                          }) == 4);

    Eigen::VectorXd u =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(patch.mesh.nodes.size()));
    constexpr double strain_yy = 1e-3;
    for (const auto node : patch.mesh.elements.front().nodes) {
        u[3 * static_cast<Eigen::Index>(node) + 1] = strain_yy * patch.mesh.nodes[node][1];
    }

    polymesh::fea::Stress strain = polymesh::fea::Stress::Zero();
    strain[1] = strain_yy;
    const double sampled_max = polymesh::fea::von_mises(mat.d_matrix() * strain);
    const auto zz = polymesh::fea::recover_zz(patch.mesh, mat, u);
    const double recovered = polymesh::fea::von_mises(zz.nodal_stress[patch.midpoint]);

    REQUIRE(sampled_max > 0.0);
    REQUIRE(std::abs(recovered - 0.25 * sampled_max) <= 1e-12 * sampled_max);
}
