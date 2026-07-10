// SPDX-License-Identifier: BSD-3-Clause

// C5 / GATE 3 exit criterion (partial): at equal free DOFs, a priori radial
// grading toward the hole beats a uniform tet mesh on Kirsch peak hoop / SCF
// relative error vs analytical SCF = 3.
//
// Product Cartesian fill is stair-cased on a plate-with-hole (ADR-0015), so
// both legs use the same structured annular tet10 topology as the GATE 1
// Kirsch setup (ADR-0009), solved with the product elastostatic path. Uniform
// maps r linearly; graded uses logarithmic r (nodes clustered at the hole).
// Equal topology ⇒ exact equal free DOFs. Mesh note: test-support structured
// generator, not tet_fill_surface.

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

/// Kirsch stress tensor (Cartesian) at q; remote tension sigma along +x.
Eigen::Matrix3d kirsch_stress(const Eigen::Vector3d& q, double a, double sigma) {
    const double r = std::hypot(q[0], q[1]);
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
    Eigen::Matrix3d s = Eigen::Matrix3d::Zero();
    s(0, 0) = sig_r * cr * cr + sig_t * sr * sr - 2.0 * tau_rt * cr * sr;
    s(1, 1) = sig_r * sr * sr + sig_t * cr * cr + 2.0 * tau_rt * cr * sr;
    s(0, 1) = s(1, 0) = (sig_r - sig_t) * cr * sr + tau_rt * (cr * cr - sr * sr);
    return s;
}

enum class RadialMap { kUniform, kLogGraded };

struct KirschRun {
    double scf = 0.0;
    double scf_rel_err = 0.0;
    std::size_t free_dofs = 0;
    std::size_t n_nodes = 0;
    std::size_t n_elements = 0;
};

/// Quarter annular plate: structured tet4 → tet10, polar map, exact Kirsch
/// traction on outer arc, symmetry + plane-strain BCs (same as GATE 1 Kirsch).
KirschRun run_kirsch_annulus(int nr, int nt, int nz, RadialMap map,
                             const polymesh::bench::ReferenceCase& ref) {
    const double a = ref.values.at("hole_radius_m");
    const double b = ref.values.at("outer_radius_m");
    const double thickness = ref.values.at("thickness_m");
    const double sigma = ref.values.at("remote_tension_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};
    const double hoop_exact = ref.values.at("sigma_hoop_max_pa");

    const double half_pi = std::numbers::pi / 2.0;
    // Parameter box: xi ∈ [0,1] (cavity→outer), theta, z. Outer faces at xi=1.
    NodalMesh mesh = promote_to_quadratic(box_tet_mesh(nr, nt, nz, {1.0, half_pi, thickness}));

    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        lookup[param_key(mesh.nodes[i])] = static_cast<std::uint32_t>(i);
    }
    const double dt = half_pi / nt;
    const double dz = thickness / nz;
    const auto node_at = [&](double xi, double t, double z) {
        return lookup.at(param_key({xi, t, z}));
    };

    // Outer-arc faces (xi = 1). Kuhn outer quad (1,2,6,5) → triangles 1-2-6
    // and 1-6-5, CCW from +r. tri6 mids: edges (0,1),(1,2),(0,2).
    std::vector<SurfaceFace> outer_faces;
    const double xi_out = 1.0;
    for (int j = 0; j < nt; ++j) {
        for (int k = 0; k < nz; ++k) {
            const double t0 = j * dt, t1 = (j + 1) * dt;
            const double z0 = k * dz, z1 = (k + 1) * dz;
            const double tm = 0.5 * (t0 + t1);
            const double zm = 0.5 * (z0 + z1);
            // Tri 1-2-6: (t0,z0)-(t1,z0)-(t1,z1)
            outer_faces.push_back(
                {FaceType::kTri6,
                 {node_at(xi_out, t0, z0), node_at(xi_out, t1, z0), node_at(xi_out, t1, z1),
                  node_at(xi_out, tm, z0), node_at(xi_out, t1, zm), node_at(xi_out, tm, zm)}});
            // Tri 1-6-5: (t0,z0)-(t1,z1)-(t0,z1)
            outer_faces.push_back(
                {FaceType::kTri6,
                 {node_at(xi_out, t0, z0), node_at(xi_out, t1, z1), node_at(xi_out, t0, z1),
                  node_at(xi_out, tm, zm), node_at(xi_out, tm, z1), node_at(xi_out, t0, zm)}});
        }
    }

    for (auto& node : mesh.nodes) {
        const double xi = node[0];
        const double theta = node[1];
        const double z = node[2];
        double r = 0.0;
        if (map == RadialMap::kUniform) {
            r = a + (b - a) * xi;
        } else {
            // Log spacing: denser near hole (Goodier-style a priori grading).
            r = a * std::pow(b / a, xi);
        }
        node = {r * std::cos(theta), r * std::sin(theta), z};
    }
    mesh.check_validity();

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const auto ni = static_cast<Eigen::Index>(i);
        if (std::abs(q[1]) < 1e-9) {
            bc.dof_values[3 * ni + 1] = 0.0;
        }
        if (std::abs(q[0]) < 1e-9) {
            bc.dof_values[3 * ni + 0] = 0.0;
        }
        bc.dof_values[3 * ni + 2] = 0.0;
    }

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

    double best_hoop = -1e300;
    int checked = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const double r = std::hypot(q[0], q[1]);
        if (std::abs(r - a) > 1e-8 * a) {
            continue;
        }
        if (std::abs(q[0]) > 0.20 * a) {
            continue;
        }
        const Eigen::Vector3d t_hat(-q[1] / r, q[0] / r, 0.0);
        const auto& sv = stress[i];
        Eigen::Matrix3d sig;
        sig << sv[0], sv[5], sv[4], //
            sv[5], sv[1], sv[3],    //
            sv[4], sv[3], sv[2];
        const double hoop = t_hat.dot(sig * t_hat);
        ++checked;
        best_hoop = std::max(best_hoop, hoop);
    }
    REQUIRE(checked > 0);

    KirschRun out;
    out.scf = best_hoop / sigma;
    out.scf_rel_err = std::abs(best_hoop - hoop_exact) / hoop_exact;
    out.free_dofs = 3 * mesh.nodes.size() - bc.dof_values.size();
    out.n_nodes = mesh.nodes.size();
    out.n_elements = mesh.elements.size();
    return out;
}

} // namespace

TEST_CASE("C5 Kirsch: graded radial tet beats uniform tet at equal free DOF") {
    const auto ref = polymesh::bench::load_reference("bench/reference/kirsch-plate.json");
    const double scf_exact = ref.values.at("scf");

    // Coarse tet10 annulus — CI budget: two solves well under 30s.
    // Same topology for both maps ⇒ free DOFs identical. At this density
    // uniform radial spacing under-resolves the hole-edge gradient; log
    // grading concentrates DOFs at r=a and cuts SCF relative error.
    const int nr = 4, nt = 6, nz = 1;

    const KirschRun uniform = run_kirsch_annulus(nr, nt, nz, RadialMap::kUniform, ref);
    const KirschRun graded = run_kirsch_annulus(nr, nt, nz, RadialMap::kLogGraded, ref);

    REQUIRE(uniform.free_dofs == graded.free_dofs);
    REQUIRE(uniform.n_elements == graded.n_elements);
    REQUIRE(uniform.free_dofs > 0);
    REQUIRE(std::isfinite(uniform.scf));
    REQUIRE(std::isfinite(graded.scf));
    // Plausible SCF band (coarse tet can undershoot or mild overshoot).
    REQUIRE(uniform.scf > 1.0);
    REQUIRE(uniform.scf < 6.0);
    REQUIRE(graded.scf > 1.0);
    REQUIRE(graded.scf < 6.0);

    INFO("uniform: free_dofs=" << uniform.free_dofs << " nodes=" << uniform.n_nodes
                               << " tets=" << uniform.n_elements << " SCF=" << uniform.scf
                               << " |err|=" << uniform.scf_rel_err << " (exact SCF "
                               << scf_exact << ")");
    INFO("graded:  free_dofs=" << graded.free_dofs << " nodes=" << graded.n_nodes
                               << " tets=" << graded.n_elements << " SCF=" << graded.scf
                               << " |err|=" << graded.scf_rel_err);

    // GATE 3 / C5: feature-aware (a priori hole grading) lower relative error.
    REQUIRE(graded.scf_rel_err < uniform.scf_rel_err);
}
