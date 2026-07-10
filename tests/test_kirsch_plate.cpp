// SPDX-License-Identifier: BSD-3-Clause

// Tier-1: plate with circular hole under uniaxial tension vs Kirsch SCF = 3.
// Reference values live in bench/reference/kirsch-plate.json (rule #1).
//
// Model: quarter annular plate, exact Kirsch traction on the outer arc (so the
// continuum solution on this domain is the infinite-plate field), free hole,
// symmetry BCs on the cut planes. Hex20 mesh built in (r, theta, z) parameter
// space then mapped so mid-side nodes lie on the circular arcs.

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

std::array<std::int64_t, 3> param_key(const Eigen::Vector3d& p) {
    return {static_cast<std::int64_t>(std::llround(p[0] * 1e9)),
            static_cast<std::int64_t>(std::llround(p[1] * 1e9)),
            static_cast<std::int64_t>(std::llround(p[2] * 1e9))};
}

/// Kirsch stress tensor in Cartesian components at point q (origin at hole
/// centre). Remote tension sigma along +x. Plane-stress in-plane field;
/// sigma_zz = tau_xz = tau_yz = 0.
Eigen::Matrix3d kirsch_stress(const Eigen::Vector3d& q, double a, double sigma) {
    const double r = std::hypot(q[0], q[1]);
    // Guard the origin; callers should not evaluate inside the hole.
    const double rr = std::max(r, 1e-15);
    const double th = std::atan2(q[1], q[0]);
    const double a2 = a * a;
    const double a4 = a2 * a2;
    const double r2 = rr * rr;
    const double r4 = r2 * r2;
    const double c2 = std::cos(2.0 * th);
    const double s2 = std::sin(2.0 * th);
    const double sig_r = 0.5 * sigma * (1.0 - a2 / r2) +
                         0.5 * sigma * (1.0 - 4.0 * a2 / r2 + 3.0 * a4 / r4) * c2;
    const double sig_t =
        0.5 * sigma * (1.0 + a2 / r2) - 0.5 * sigma * (1.0 + 3.0 * a4 / r4) * c2;
    const double tau_rt = -0.5 * sigma * (1.0 + 2.0 * a2 / r2 - 3.0 * a4 / r4) * s2;
    const double cr = std::cos(th);
    const double sr = std::sin(th);
    // Transform polar stress to Cartesian.
    Eigen::Matrix3d s = Eigen::Matrix3d::Zero();
    s(0, 0) = sig_r * cr * cr + sig_t * sr * sr - 2.0 * tau_rt * cr * sr;
    s(1, 1) = sig_r * sr * sr + sig_t * cr * cr + 2.0 * tau_rt * cr * sr;
    s(0, 1) = s(1, 0) = (sig_r - sig_t) * cr * sr + tau_rt * (cr * cr - sr * sr);
    return s;
}

} // namespace

TEST_CASE("Kirsch plate: hole-edge hoop stress matches SCF = 3") {
    const auto ref = polymesh::bench::load_reference("bench/reference/kirsch-plate.json");
    const double a = ref.values.at("hole_radius_m");
    const double b = ref.values.at("outer_radius_m");
    const double thickness = ref.values.at("thickness_m");
    const double sigma = ref.values.at("remote_tension_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};
    const double scf_exact = ref.values.at("scf");
    const double hoop_exact = ref.values.at("sigma_hoop_max_pa");

    const int nr = 6, nt = 10, nz = 1;
    const double half_pi = std::numbers::pi / 2.0;
    NodalMesh mesh =
        promote_to_quadratic(box_hex_mesh(nr, nt, nz, {b - a, half_pi, thickness}));

    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        lookup[param_key(mesh.nodes[i])] = static_cast<std::uint32_t>(i);
    }
    const double dt = half_pi / nt;
    const double dz = thickness / nz;
    // Outer-arc faces (parameter r = b-a) for the Kirsch traction.
    std::vector<SurfaceFace> outer_faces;
    const auto node_at = [&](double r_param, double t, double z) {
        return lookup.at(param_key({r_param, t, z}));
    };
    const double r_out = b - a;
    for (int j = 0; j < nt; ++j) {
        for (int k = 0; k < nz; ++k) {
            const double t0 = j * dt, t1 = (j + 1) * dt;
            const double z0 = k * dz, z1 = (k + 1) * dz;
            // Outward normal is +r; order face CCW when viewed from outside.
            outer_faces.push_back(
                {FaceType::kQuad8,
                 {node_at(r_out, t0, z0), node_at(r_out, t1, z0), node_at(r_out, t1, z1),
                  node_at(r_out, t0, z1), node_at(r_out, 0.5 * (t0 + t1), z0),
                  node_at(r_out, t1, 0.5 * (z0 + z1)), node_at(r_out, 0.5 * (t0 + t1), z1),
                  node_at(r_out, t0, 0.5 * (z0 + z1))}});
        }
    }

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
        // Symmetry: bottom cut (y = 0, theta = 0) → u_y = 0.
        if (std::abs(q[1]) < 1e-9) {
            bc.dof_values[3 * ni + 1] = 0.0;
        }
        // Symmetry: left cut (x = 0, theta = pi/2) → u_x = 0.
        if (std::abs(q[0]) < 1e-9) {
            bc.dof_values[3 * ni + 0] = 0.0;
        }
        // Plane strain: in-plane Kirsch stresses are the same for the Airy
        // solution under plane stress or plane strain; u_z = 0 is the clean 3D
        // verification setup (matches the Lamé cylinder test pattern).
        bc.dof_values[3 * ni + 2] = 0.0;
    }

    // Materialize s*n into a Vector3d: returning the Eigen product expression
    // through std::function can yield a zero vector (expression-template trap).
    const auto loads = assemble_traction_load(
        mesh, outer_faces, [&](const Eigen::Vector3d& q) -> Eigen::Vector3d {
            const Eigen::Matrix3d s = kirsch_stress(q, a, sigma);
            const double r = std::hypot(q[0], q[1]);
            const Eigen::Vector3d n(q[0] / r, q[1] / r, 0.0);
            return Eigen::Vector3d(s * n);
        });
    REQUIRE(loads.norm() > 0.0);
    const auto u = solve_elastostatics(mesh, material, bc, loads);
    REQUIRE(u.norm() > 0.0);
    const auto stress = recover_nodal_stress(mesh, material, u);

    // Peak hoop stress at the hole equator (theta = pi/2, r = a).
    double best_hoop = -1e300;
    double any_vm = 0.0;
    int checked = 0;
    int hole_nodes = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const double r = std::hypot(q[0], q[1]);
        if (std::abs(r - a) > 1e-9) {
            continue;
        }
        ++hole_nodes;
        const Eigen::Vector3d t_hat(-q[1] / r, q[0] / r, 0.0);
        const auto& sv = stress[i];
        Eigen::Matrix3d sig;
        sig << sv[0], sv[5], sv[4], //
            sv[5], sv[1], sv[3],    //
            sv[4], sv[3], sv[2];
        const double hoop = t_hat.dot(sig * t_hat);
        any_vm = std::max(any_vm, von_mises(sv));
        // Equator neighbourhood: near x = 0 (theta = pi/2).
        if (std::abs(q[0]) > 0.20 * a) {
            continue;
        }
        ++checked;
        best_hoop = std::max(best_hoop, hoop);
    }
    REQUIRE(checked > 0);
    const double scf_err = std::abs(best_hoop - hoop_exact) / hoop_exact;
    INFO("hole nodes " << hole_nodes << ", equator nodes " << checked << ", peak hoop "
                       << best_hoop << " Pa, SCF " << best_hoop / sigma << " (exact "
                       << scf_exact << "), rel err " << scf_err << ", max hole VM " << any_vm
                       << ", |u|=" << u.norm() << ", |f|=" << loads.norm());
    // Quadratic annular mesh with exact far-field traction: SCF within 5%.
    CHECK(scf_err < 0.05);
}
