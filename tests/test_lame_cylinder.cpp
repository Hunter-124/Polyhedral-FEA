// SPDX-License-Identifier: BSD-3-Clause

// Tier-1: thick-walled cylinder under internal pressure vs the Lamé closed
// form (plane strain). Reference values live in bench/reference/ and are
// loaded through the bench harness — never hardcoded here (rule #1).
//
// Model: 90-degree sector, symmetry BCs on the flat faces, u_z = 0 on both
// z-faces (plane strain), consistent pressure traction on the inner surface.
// The mesh is built as a structured grid in (r, theta, z) parameter space,
// promoted to quadratic there, then mapped — so hex20 mid-side nodes land
// exactly on the curved surfaces (true curved isoparametric elements).

#include "bench/reference_case.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <numbers>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

/// Key for exact node lookup in parameter space (lattice arithmetic is exact).
std::array<std::int64_t, 3> param_key(const Eigen::Vector3d& p) {
    // Parameter coords are multiples of 1/2^k of the lattice spacing; scale
    // to a fine integer grid to avoid float-compare fragility.
    return {static_cast<std::int64_t>(std::llround(p[0] * 1e9)),
            static_cast<std::int64_t>(std::llround(p[1] * 1e9)),
            static_cast<std::int64_t>(std::llround(p[2] * 1e9))};
}

} // namespace

TEST_CASE("Lamé thick-walled cylinder: displacement and hoop stress vs closed form") {
    const auto ref = polymesh::bench::load_reference("bench/reference/lame-cylinder.json");
    const double a = ref.values.at("inner_radius_m");
    const double b = ref.values.at("outer_radius_m");
    const double p = ref.values.at("pressure_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};

    // Parameter-space structured grid: (radial, hoop, axial).
    const int nr = 4, nt = 12, nz = 1;
    const double half_pi = std::numbers::pi / 2.0;
    const double thickness = 0.02;
    NodalMesh mesh =
        promote_to_quadratic(box_hex_mesh(nr, nt, nz, {b - a, half_pi, thickness}));

    // Collect inner-surface quad8 faces (parameter r = 0 plane) BEFORE
    // mapping, via exact parameter-space lookups.
    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        lookup[param_key(mesh.nodes[i])] = static_cast<std::uint32_t>(i);
    }
    const double dt = half_pi / nt;
    const double dz = thickness / nz;
    std::vector<SurfaceFace> inner_faces;
    const auto node_at = [&](double t, double z) { return lookup.at(param_key({0.0, t, z})); };
    for (int j = 0; j < nt; ++j) {
        for (int k = 0; k < nz; ++k) {
            const double t0 = j * dt, t1 = (j + 1) * dt;
            const double z0 = k * dz, z1 = (k + 1) * dz;
            SurfaceFace face{FaceType::kQuad8,
                             {node_at(t0, z0), node_at(t1, z0), node_at(t1, z1),
                              node_at(t0, z1), node_at(0.5 * (t0 + t1), z0),
                              node_at(t1, 0.5 * (z0 + z1)), node_at(0.5 * (t0 + t1), z1),
                              node_at(t0, 0.5 * (z0 + z1))}};
            inner_faces.push_back(std::move(face));
        }
    }

    // Map parameter space -> physical cylinder sector.
    for (auto& node : mesh.nodes) {
        const double r = a + node[0];
        const double theta = node[1];
        node = {r * std::cos(theta), r * std::sin(theta), node[2]};
    }
    mesh.check_validity();

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const auto ni = static_cast<Eigen::Index>(i);
        if (std::abs(q[1]) < 1e-9) {
            bc.dof_values[3 * ni + 1] = 0.0; // theta = 0 plane: u_y = 0
        }
        if (std::abs(q[0]) < 1e-9) {
            bc.dof_values[3 * ni + 0] = 0.0; // theta = 90 plane: u_x = 0
        }
        bc.dof_values[3 * ni + 2] = 0.0; // plane strain: u_z = 0 everywhere
    }

    // Internal pressure: traction p * r_hat on the inner surface.
    const auto loads =
        assemble_traction_load(mesh, inner_faces, [&](const Eigen::Vector3d& q) {
            const Eigen::Vector3d r_hat(q[0], q[1], 0.0);
            return Eigen::Vector3d(p * r_hat.normalized());
        });
    const auto u = solve_elastostatics(mesh, material, bc, loads);
    const auto stress = recover_nodal_stress(mesh, material, u);

    // Compare u_r and sigma_theta at inner-surface nodes against Lamé.
    const double u_r_exact = ref.values.at("u_r_inner_m");
    const double hoop_exact = ref.values.at("sigma_hoop_inner_pa");
    double worst_u = 0.0, worst_hoop = 0.0;
    int checked = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const double r = std::hypot(q[0], q[1]);
        if (std::abs(r - a) > 1e-9) {
            continue;
        }
        ++checked;
        const Eigen::Vector3d r_hat(q[0] / r, q[1] / r, 0.0);
        const Eigen::Vector3d t_hat(-q[1] / r, q[0] / r, 0.0);
        const Eigen::Vector3d ui = u.segment<3>(3 * static_cast<Eigen::Index>(i));
        worst_u = std::max(worst_u, std::abs(ui.dot(r_hat) - u_r_exact) / u_r_exact);

        const auto& s = stress[i];
        Eigen::Matrix3d sig;
        sig << s[0], s[5], s[4], //
            s[5], s[1], s[3],    //
            s[4], s[3], s[2];
        const double hoop = t_hat.dot(sig * t_hat);
        worst_hoop = std::max(worst_hoop, std::abs(hoop - hoop_exact) / hoop_exact);
    }
    REQUIRE(checked > 0);
    INFO("inner-surface nodes " << checked << ", worst u_r rel err " << worst_u
                                << ", worst hoop rel err " << worst_hoop);
    CHECK(worst_u < 0.01);
    CHECK(worst_hoop < 0.04);
}
