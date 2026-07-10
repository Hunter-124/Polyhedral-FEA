// SPDX-License-Identifier: BSD-3-Clause

// First Tier-1 physics smoke test: gravity-loaded cantilever vs Timoshenko
// beam theory. Uses a consistent body-force load (no traction machinery
// needed yet). The full Tier-1 harness with energy norms and convergence
// orders lands with the MMS tooling.

#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace polymesh::fea;
using namespace polymesh::test_support;

TEST_CASE("gravity-loaded cantilever matches Timoshenko beam theory") {
    // Beam along x: L = 1 m, square cross-section b = h = 0.1 m.
    const double length = 1.0;
    const double width = 0.1;
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const double bz = -1e6; // body force density, N/m^3

    NodalMesh mesh = promote_to_quadratic(box_hex_mesh(16, 2, 2, {length, width, width}));
    mesh.check_validity();

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] < 1e-12) {
            bc.fix_node(static_cast<std::uint32_t>(i));
        }
    }

    const auto loads = assemble_body_load(
        mesh, [&](const Eigen::Vector3d&) { return Eigen::Vector3d(0.0, 0.0, bz); });
    const auto u = solve_elastostatics(mesh, steel, bc, loads);

    // Average tip deflection over the end-face nodes.
    double tip = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] > length - 1e-12) {
            tip += u[3 * static_cast<Eigen::Index>(i) + 2];
            ++count;
        }
    }
    REQUIRE(count > 0);
    tip /= count;

    // Timoshenko cantilever under uniform load q [N/m]:
    // delta = q L^4 / (8 E I) + q L^2 / (2 kappa G A),
    // kappa = 10(1+nu)/(12+11nu) for a rectangular section.
    const double area = width * width;
    const double inertia = width * width * width * width / 12.0;
    const double q = bz * area;
    const double shear_mod = steel.mu();
    const double kappa =
        10.0 * (1.0 + steel.poissons_ratio) / (12.0 + 11.0 * steel.poissons_ratio);
    const double delta =
        q * length * length * length * length / (8.0 * steel.youngs_modulus * inertia) +
        q * length * length / (2.0 * kappa * shear_mod * area);

    INFO("fem tip " << tip << " m, beam theory " << delta << " m");
    CHECK(std::abs(tip - delta) / std::abs(delta) < 0.03);
}
