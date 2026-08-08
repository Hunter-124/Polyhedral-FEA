// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/zz.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
