// SPDX-License-Identifier: BSD-3-Clause

// Tier-1: spherical cavity under remote uniaxial tension vs Goodier SCF.
// Reference values live in bench/reference/goodier-cavity.json (rule #1).
//
// Model: one octant of a thick spherical shell (cavity radius a, outer radius
// b = outer_radius_m). Symmetry BCs on the coordinate planes; remote uniaxial
// strain corresponding to σ_zz = T is imposed as Dirichlet on the outer sphere
// (Saint-Venant stand-in for the infinite body). Structured hex20 mesh in
// (r, theta, phi) with logarithmic radial spacing (nodes clustered at the
// cavity) and theta starting above zero so Jacobians stay positive at the pole.
//
// Finite-domain + nodal-averaged stress recovery under-predicts the continuum
// SCF slightly; the acceptance bar is documented in ADR-0009 and tightens once
// exact Goodier-field BCs and ZZ recovery land.

#include "bench/reference_case.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

double solve_scf(int nr, int nt, int np, const polymesh::bench::ReferenceCase& ref) {
    const double a = ref.values.at("cavity_radius_m");
    const double b = ref.values.at("outer_radius_m");
    const double T = ref.values.at("remote_tension_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};

    const double theta0 = 0.15;
    const double half_pi = std::numbers::pi / 2.0;
    // Uniform parameter box, then log-map radius.
    NodalMesh mesh =
        promote_to_quadratic(box_hex_mesh(nr, nt, np, {1.0, half_pi - theta0, half_pi}));

    for (auto& node : mesh.nodes) {
        const double xi = node[0]; // 0 → cavity, 1 → outer
        const double theta = theta0 + node[1];
        const double phi = node[2];
        const double r = a * std::pow(b / a, xi); // log spacing
        node = {r * std::sin(theta) * std::cos(phi), r * std::sin(theta) * std::sin(phi),
                r * std::cos(theta)};
    }
    mesh.check_validity();

    const double exx = -material.poissons_ratio * T / material.youngs_modulus;
    const double ezz = T / material.youngs_modulus;

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const auto ni = static_cast<Eigen::Index>(i);
        const double r = q.norm();
        if (std::abs(q[0]) < 1e-9) {
            bc.dof_values[3 * ni + 0] = 0.0;
        }
        if (std::abs(q[1]) < 1e-9) {
            bc.dof_values[3 * ni + 1] = 0.0;
        }
        if (std::abs(q[2]) < 1e-9) {
            bc.dof_values[3 * ni + 2] = 0.0;
        }
        if (std::abs(r - b) < 1e-8 * b) {
            bc.dof_values[3 * ni + 0] = exx * q[0];
            bc.dof_values[3 * ni + 1] = exx * q[1];
            bc.dof_values[3 * ni + 2] = ezz * q[2];
        }
    }

    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = solve_elastostatics(mesh, material, bc, loads);
    const auto stress = recover_nodal_stress(mesh, material, u);

    double peak_hoop = -1e300;
    int checked = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const double r = q.norm();
        if (std::abs(r - a) > 1e-8 * a) {
            continue;
        }
        if (std::abs(q[2]) > 0.25 * a) {
            continue;
        }
        ++checked;
        const double theta = std::acos(std::clamp(q[2] / r, -1.0, 1.0));
        const double phi = std::atan2(q[1], q[0]);
        const Eigen::Vector3d e_theta(std::cos(theta) * std::cos(phi),
                                      std::cos(theta) * std::sin(phi), -std::sin(theta));
        // Also sample e_phi (azimuthal) — for Goodier the meridional hoop e_θ is
        // the classic peak, but take the larger of the two tangential normals.
        const Eigen::Vector3d e_phi(-std::sin(phi), std::cos(phi), 0.0);
        const auto& sv = stress[i];
        Eigen::Matrix3d sig;
        sig << sv[0], sv[5], sv[4], //
            sv[5], sv[1], sv[3],    //
            sv[4], sv[3], sv[2];
        const double s_th = e_theta.dot(sig * e_theta);
        const double s_ph = e_phi.dot(sig * e_phi);
        peak_hoop = std::max(peak_hoop, std::max(s_th, s_ph));
    }
    REQUIRE(checked > 0);
    return peak_hoop / T;
}

} // namespace

TEST_CASE("Goodier cavity: equatorial hoop stress approaches analytical SCF") {
    const auto ref = polymesh::bench::load_reference("bench/reference/goodier-cavity.json");
    const double scf_exact = ref.values.at("scf");

    // Single well-resolved shell (log-radial hex20, b/a = 15). Angular
    // refinement alone can move the nodal-averaged peak either way by a
    // percent, so we do not assert monotonicity — only accuracy vs Goodier.
    const double scf = solve_scf(8, 8, 8, ref);
    const double scf_err = std::abs(scf - scf_exact) / scf_exact;

    INFO("SCF " << scf << " (exact " << scf_exact << "), rel err " << scf_err);
    CHECK(scf > 1.5); // concentration present
    CHECK(scf_err < 0.12);
}
