// SPDX-License-Identifier: AGPL-3.0-or-later

// Tier-2 MMS convergence verification (BENCHMARKS.md): an element claiming
// order p must demonstrate O(h^p) energy-norm convergence under uniform
// refinement, within ±0.2 of theory (engineering rule #3). The manufactured
// field's coefficients come from a seeded generator, so nothing about the
// exact solution can be hardcoded.

#include "fea/solve.hpp"
#include "support/mms.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

NodalMesh make_mesh(ElementType type, int n) {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    NodalMesh mesh = (type == ElementType::kTet4 || type == ElementType::kTet10)
                         ? box_tet_mesh(n, n, n, size)
                         : box_hex_mesh(n, n, n, size);
    if (type == ElementType::kTet10 || type == ElementType::kHex20) {
        mesh = promote_to_quadratic(mesh);
    }
    return mesh;
}

double solve_mms_error(ElementType type, int n, const ManufacturedSolution& mms) {
    const auto mesh = make_mesh(type, n);

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
    const auto u = solve_elastostatics(mesh, mms.material, bc, loads);
    return energy_norm_error(mesh, mms.material, u, mms);
}

struct Expectation {
    ElementType type;
    int order;
    const char* name;
};

} // namespace

TEST_CASE("MMS energy-norm convergence matches theoretical order") {
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    // Cubic manufactured field: outside every FE space in the P1 zoo, so the
    // error is nonzero and the asymptotic rate is observable.
    const auto mms = ManufacturedSolution::random(/*degree=*/3, /*seed=*/2026, steel);

    const std::vector<Expectation> cases{
        {ElementType::kTet4, 1, "tet4"},
        {ElementType::kHex8, 1, "hex8"},
        {ElementType::kTet10, 2, "tet10"},
        {ElementType::kHex20, 2, "hex20"},
    };
    for (const auto& c : cases) {
        const double e_coarse = solve_mms_error(c.type, 4, mms);
        const double e_fine = solve_mms_error(c.type, 8, mms);
        const double observed = std::log(e_coarse / e_fine) / std::log(2.0);
        INFO(c.name << ": errors " << e_coarse << " -> " << e_fine << ", observed order "
                    << observed << ", expected " << c.order);
        CHECK(std::abs(observed - c.order) < 0.2);
    }
}

TEST_CASE("MMS exact-representation sanity: quadratic field solved exactly by p=2") {
    // A degree-2 manufactured field lies inside the tet10/hex20 FE spaces, so
    // the energy-norm error must vanish to solver precision — this guards the
    // whole MMS pipeline (loads, BCs, error integration) against silent bias.
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto mms = ManufacturedSolution::random(/*degree=*/2, /*seed=*/7, steel);
    for (const auto type : {ElementType::kTet10, ElementType::kHex20}) {
        const double error = solve_mms_error(type, 2, mms);
        // Energy-norm scale of the field itself, for a relative comparison.
        const auto zero = Eigen::VectorXd::Zero(
            3 * static_cast<Eigen::Index>(make_mesh(type, 2).nodes.size()));
        const double scale = energy_norm_error(make_mesh(type, 2), steel, zero, mms);
        INFO("type " << static_cast<int>(type) << " error " << error << " scale " << scale);
        CHECK(error < 1e-9 * scale);
    }
}
