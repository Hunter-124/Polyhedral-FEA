// SPDX-License-Identifier: BSD-3-Clause
#include "fea/nodal_mesh.hpp"
#include "fea/stress.hpp"
#include "support/structured_mesh.hpp"

#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

std::vector<double> linear_field(const polymesh::fea::NodalMesh& mesh,
                                 const Eigen::Vector3d& slope) {
    std::vector<double> s(mesh.nodes.size(), 0.0);
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        s[i] = slope.dot(mesh.nodes[i]);
    }
    return s;
}

/// One Tet4 whose four nodes lie in a plane, so every node's patch (the other
/// three) leaves one direction unsampled. `rotate` tilts that plane off the
/// coordinate axes, which is the harder case for the rank test: axis-aligned,
/// the null direction is a row of the normal matrix that is exactly zero, while
/// rotated it is a cancellation of three nonzero rows and λ_min lands on
/// round-off of either sign (measured -3.1e-17 relative for this patch).
polymesh::fea::NodalMesh flat_tet(bool rotate) {
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {{0.0, 0.0, 0.0}, {1.0, 0.3, 0.0}, {0.2, 1.0, 0.0}, {1.3, 1.1, 0.0}};
    if (rotate) {
        const Eigen::Matrix3d r =
            Eigen::AngleAxisd(0.7391, Eigen::Vector3d{0.31, -0.83, 0.46}.normalized())
                .matrix();
        for (auto& p : mesh.nodes) {
            p = r * p;
        }
    }
    mesh.elements.emplace_back(polymesh::fea::ElementType::kTet4,
                               std::vector<std::uint32_t>{0, 1, 2, 3});
    return mesh;
}

} // namespace

// The one property that makes the recovery trustworthy on screen: where the
// field is linear the reported magnitude is the true |grad|, not an average of
// finite differences that happens to be close. Checked on hexes and on Kuhn
// tets (different patch shapes and valences), and with the mesh moved far from
// the origin — the fit is posed in offsets from the node, so a translation must
// not move a digit. Absolute coordinates are what made the ZZ patch fit rank
// deficient at |x| ~ 500 m (see zz.cpp), and this fit shares the geometry.
TEST_CASE("nodal scalar gradient is exact for a field linear in x", "[gradient]") {
    const Eigen::Vector3d slope{0.3, -0.7, 1.1};
    const double expected = slope.norm();
    REQUIRE(expected > 0.0);

    for (const Eigen::Vector3d& origin :
         {Eigen::Vector3d{0.0, 0.0, 0.0}, Eigen::Vector3d{500.0, -300.0, 120.0}}) {
        for (int kind = 0; kind < 2; ++kind) {
            auto mesh = kind == 0
                            ? polymesh::test_support::box_hex_mesh(3, 2, 4, {1.0, 0.4, 0.7})
                            : polymesh::test_support::box_tet_mesh(3, 2, 4, {1.0, 0.4, 0.7});
            for (auto& p : mesh.nodes) {
                p += origin;
            }
            const auto field = linear_field(mesh, slope);
            std::size_t unresolved = 1;
            const auto grad =
                polymesh::fea::nodal_scalar_gradient_magnitude(mesh, field, &unresolved);
            REQUIRE(grad.size() == mesh.nodes.size());
            // A structured lattice patch spans all three directions at every
            // node, corners included, so nothing may be reported unresolved.
            REQUIRE(unresolved == 0);
            double worst = 0.0;
            for (const double g : grad) {
                worst = std::max(worst, std::abs(g - expected));
            }
            REQUIRE(worst <= 1e-9 * expected);
        }
    }
}

TEST_CASE("nodal scalar gradient of a constant field is exactly zero", "[gradient]") {
    const auto mesh = polymesh::test_support::box_hex_mesh(3, 3, 3, {1.0, 1.0, 1.0});
    const std::vector<double> field(mesh.nodes.size(), 7.5);
    std::size_t unresolved = 1;
    const auto grad = polymesh::fea::nodal_scalar_gradient_magnitude(mesh, field, &unresolved);
    REQUIRE(grad.size() == mesh.nodes.size());
    REQUIRE(unresolved == 0);
    for (const double g : grad) {
        // Exactly, not to a tolerance: every right-hand side entry is a sum of
        // (s_j - s_i) = 0 terms, so the solved gradient is the zero vector.
        REQUIRE(g == 0.0);
    }
}

TEST_CASE("nodal scalar gradient refuses a field of the wrong length", "[gradient]") {
    const auto mesh = polymesh::test_support::box_hex_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    REQUIRE(mesh.nodes.size() > 2);
    std::size_t unresolved = 99;
    const std::vector<double> too_short(mesh.nodes.size() - 1, 1.0);
    REQUIRE(
        polymesh::fea::nodal_scalar_gradient_magnitude(mesh, too_short, &unresolved).empty());
    REQUIRE(unresolved == 0);
    const std::vector<double> too_long(mesh.nodes.size() + 1, 1.0);
    REQUIRE(polymesh::fea::nodal_scalar_gradient_magnitude(mesh, too_long).empty());
    REQUIRE(polymesh::fea::nodal_scalar_gradient_magnitude(mesh, {}).empty());
}

// A coplanar patch has no gradient to report: the in-plane slope is real but
// the out-of-plane component is unsampled, so the 3-vector the caller asked for
// does not exist. It must come back as an explicit 0.0 plus an unresolved
// count, never as the LDLT's arbitrary null-space point (the failure mode that
// produced 1e12 Pa nodal stress in the ZZ recovery) and never as NaN.
TEST_CASE("nodal scalar gradient reports a rank-deficient patch instead of inventing one",
          "[gradient]") {
    for (const bool rotate : {false, true}) {
        const auto mesh = flat_tet(rotate);
        std::vector<double> field(mesh.nodes.size(), 0.0);
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            field[i] = mesh.nodes[i][0] + 2.0 * mesh.nodes[i][1];
        }
        std::size_t unresolved = 0;
        const auto grad =
            polymesh::fea::nodal_scalar_gradient_magnitude(mesh, field, &unresolved);
        REQUIRE(grad.size() == 4);
        REQUIRE(unresolved == 4);
        for (const double g : grad) {
            REQUIRE(std::isfinite(g));
            REQUIRE(g == 0.0);
        }

        // Same call without the out-parameter must not fault and must agree.
        const auto anonymous = polymesh::fea::nodal_scalar_gradient_magnitude(mesh, field);
        REQUIRE(anonymous == grad);
    }
}

// The guard is a rank test, not a "is it flat" test: lift one node out of the
// plane and the same four patches resolve the true gradient exactly. Without
// this the previous case would also pass a function that always returns 0.
TEST_CASE("nodal scalar gradient resolves the same patch once it spans three directions",
          "[gradient]") {
    auto mesh = flat_tet(true);
    mesh.nodes[3] += Eigen::Vector3d{0.11, -0.29, 0.6};
    const Eigen::Vector3d slope{1.0, 2.0, -0.5};
    const auto field = linear_field(mesh, slope);
    std::size_t unresolved = 99;
    const auto grad = polymesh::fea::nodal_scalar_gradient_magnitude(mesh, field, &unresolved);
    REQUIRE(grad.size() == 4);
    REQUIRE(unresolved == 0);
    for (const double g : grad) {
        REQUIRE(std::abs(g - slope.norm()) <= 1e-12 * slope.norm());
    }
}
