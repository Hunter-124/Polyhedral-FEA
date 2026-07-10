// SPDX-License-Identifier: BSD-3-Clause

// Tier-1: L-shaped domain with a re-entrant corner — singularity-limited
// convergence under uniform refinement (Williams 1952). Reference values in
// bench/reference/l-domain.json (rule #1).
//
// Geometry (xy, extruded in z):
//   solid = [0, w]×[0, L] ∪ [w, L]×[w, L], re-entrant corner at (w, w).
// Fully fixed on x = 0; uniform +x traction on x = L (the end of the upper
// arm). Plane-strain restraint u_z = 0. Energy of the discrete solution
// approaches the continuum energy from below; successive energy gaps under
// uniform h-halving converge at rate 2*lambda ≈ 1.09.

#include "bench/reference_case.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <vector>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

/// Build a linear hex mesh of the L by gluing two structured blocks with
/// shared nodes on the interface x = w, y ∈ [w, L].
NodalMesh make_l_hex_mesh(int n, double L, double w, double thickness) {
    // Block A: [0, w] × [0, L] × [0, t] — tall left column.
    // Block B: [w, L] × [w, L] × [0, t] — upper-right arm (no overlap volume).
    const int nx_a = n;
    const int ny_a = 2 * n;
    const int nx_b = n;
    const int ny_b = n;
    const int nz = std::max(1, n / 2);

    NodalMesh mesh;
    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    const auto key_of = [](double x, double y, double z) -> std::array<std::int64_t, 3> {
        return {static_cast<std::int64_t>(std::llround(x * 1e9)),
                static_cast<std::int64_t>(std::llround(y * 1e9)),
                static_cast<std::int64_t>(std::llround(z * 1e9))};
    };
    const auto add_node = [&](double x, double y, double z) {
        const auto key = key_of(x, y, z);
        const auto [it, inserted] =
            lookup.try_emplace(key, static_cast<std::uint32_t>(mesh.nodes.size()));
        if (inserted) {
            mesh.nodes.emplace_back(x, y, z);
        }
        return it->second;
    };

    const auto fill_block = [&](double x0, double x1, double y0, double y1, int nx, int ny) {
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const double xa = x0 + (x1 - x0) * i / nx;
                    const double xb = x0 + (x1 - x0) * (i + 1) / nx;
                    const double ya = y0 + (y1 - y0) * j / ny;
                    const double yb = y0 + (y1 - y0) * (j + 1) / ny;
                    const double za = thickness * k / nz;
                    const double zb = thickness * (k + 1) / nz;
                    const std::array<std::uint32_t, 8> c = {
                        add_node(xa, ya, za), add_node(xb, ya, za), add_node(xb, yb, za),
                        add_node(xa, yb, za), add_node(xa, ya, zb), add_node(xb, ya, zb),
                        add_node(xb, yb, zb), add_node(xa, yb, zb)};
                    mesh.elements.push_back(
                        NodalElement{ElementType::kHex8, {c.begin(), c.end()}});
                }
            }
        }
    };

    fill_block(0.0, w, 0.0, L, nx_a, ny_a);
    fill_block(w, L, w, L, nx_b, ny_b);
    return mesh;
}

struct LSolve {
    double energy = 0.0;
    double peak_vm_at_corner = 0.0;
    Eigen::VectorXd u;
    NodalMesh mesh;
};

LSolve solve_l(int n, const polymesh::bench::ReferenceCase& ref) {
    const double L = ref.values.at("arm_length_m");
    const double w = ref.values.at("arm_width_m");
    const double thickness = ref.values.at("thickness_m");
    const double traction = ref.values.at("traction_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};

    NodalMesh mesh = promote_to_quadratic(make_l_hex_mesh(n, L, w, thickness));
    mesh.check_validity();

    // Collect traction faces on x = L (outer end of the upper arm).
    // For hex20, face nodes on a constant-x plane: find elements with a face
    // whose four corners lie on x = L.
    std::vector<SurfaceFace> load_faces;
    for (const auto& el : mesh.elements) {
        // Hex20 face on +x: corners 1,2,6,5 (canonical) + mids 9,18,13,17.
        // Check whether those corners sit on x = L.
        const auto& nodes = el.nodes;
        const auto on_end = [&](std::uint32_t id) {
            return std::abs(mesh.nodes[id][0] - L) < 1e-9;
        };
        // Try the +x face (corners 1,2,6,5).
        if (on_end(nodes[1]) && on_end(nodes[2]) && on_end(nodes[6]) && on_end(nodes[5])) {
            load_faces.push_back({FaceType::kQuad8,
                                  {nodes[1], nodes[2], nodes[6], nodes[5], nodes[9], nodes[18],
                                   nodes[13], nodes[17]}});
        }
    }
    REQUIRE_FALSE(load_faces.empty());

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const auto ni = static_cast<Eigen::Index>(i);
        if (std::abs(q[0]) < 1e-9) {
            bc.fix_node(static_cast<std::uint32_t>(i)); // fixed wall
        }
        bc.dof_values[3 * ni + 2] = 0.0; // plane strain
    }

    const auto loads = assemble_traction_load(mesh, load_faces, [&](const Eigen::Vector3d&) {
        return Eigen::Vector3d(traction, 0.0, 0.0);
    });
    const auto u = solve_elastostatics(mesh, material, bc, loads);
    const double energy = strain_energy(mesh, material, u);
    const auto stress = recover_nodal_stress(mesh, material, u);

    // Peak von Mises near the re-entrant corner (w, w, *).
    double peak = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const double d = std::hypot(q[0] - w, q[1] - w);
        if (d < 0.08 * L) {
            peak = std::max(peak, von_mises(stress[i]));
        }
    }

    return {energy, peak, u, std::move(mesh)};
}

} // namespace

TEST_CASE("L-domain: singularity-limited energy convergence under uniform refinement") {
    const auto ref = polymesh::bench::load_reference("bench/reference/l-domain.json");
    const double lambda = ref.values.at("singularity_lambda");
    const double theory_energy_gap_order = 2.0 * lambda; // ||u-u_h||_E^2 ~ h^{2λ}

    const auto coarse = solve_l(2, ref);
    const auto mid = solve_l(4, ref);
    const auto fine = solve_l(8, ref);

    INFO("energies E2=" << coarse.energy << " E4=" << mid.energy << " E8=" << fine.energy);
    // Conforming Galerkin: strain energy increases monotonically toward the exact value.
    CHECK(mid.energy > coarse.energy);
    CHECK(fine.energy > mid.energy);

    const double gap_cm = mid.energy - coarse.energy;
    const double gap_mf = fine.energy - mid.energy;
    REQUIRE(gap_cm > 0.0);
    REQUIRE(gap_mf > 0.0);
    const double observed = std::log(gap_cm / gap_mf) / std::log(2.0);
    INFO("energy-gap order " << observed << " (theory 2*lambda = " << theory_energy_gap_order
                             << ")");
    // Coarse meshes on a strong singularity: accept ±0.35 around 2λ ≈ 1.09.
    CHECK(std::abs(observed - theory_energy_gap_order) < 0.35);

    // Peak stress at the re-entrant corner grows under refinement (unbounded
    // continuum singularity) — a qualitative sanity check that we are sampling
    // the singular region, not a smooth patch.
    INFO("corner von Mises " << coarse.peak_vm_at_corner << " -> " << mid.peak_vm_at_corner
                             << " -> " << fine.peak_vm_at_corner);
    CHECK(fine.peak_vm_at_corner > mid.peak_vm_at_corner);
    CHECK(mid.peak_vm_at_corner > coarse.peak_vm_at_corner);
}
