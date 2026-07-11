// SPDX-License-Identifier: BSD-3-Clause
// Gate for DAG node fe-vem-assembly (ADR-0019 §1):
// Mixed FE tet/hex + VEM PolyCells assemble into ONE global K; constant-strain
// (linear displacement) patch test is exact across the FE/VEM interface.

#include "fea/assembly.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "geom/features.hpp"
#include "mesh/mixed_fill.hpp"
#include "pipeline/scene.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace polymesh::fea;
using namespace polymesh::test_support;
namespace mesh = polymesh::mesh;
namespace pipeline = polymesh::pipeline;

namespace {
const Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

/// Constant-strain patch: fix all boundary nodes to u = G·x; interior free.
double constant_strain_max_error(const NodalMesh& mesh, const Eigen::Matrix3d& g) {
    Eigen::Vector3d lo = mesh.nodes.front(), hi = mesh.nodes.front();
    for (const auto& p : mesh.nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& p = mesh.nodes[i];
        const bool boundary =
            (p - lo).cwiseAbs().minCoeff() < 1e-9 || (hi - p).cwiseAbs().minCoeff() < 1e-9;
        if (boundary) {
            bc.fix_node(static_cast<std::uint32_t>(i), g * p);
        }
    }
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = solve_elastostatics(mesh, kSteel, bc, loads);
    double max_err = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d ue = g * mesh.nodes[i];
        max_err =
            std::max(max_err, (u.segment<3>(3 * static_cast<Eigen::Index>(i)) - ue).norm());
    }
    return max_err;
}

Eigen::Matrix3d sample_strain_gradient() {
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;
    return g;
}

polymesh::geom::TriSurface unit_box_surface() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    s.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    return s;
}
} // namespace

TEST_CASE("FE/VEM interface patch test: checkerboard hex FE + hex-as-poly VEM") {
    // 2×2×2 unit-cube hex mesh; convert every other cell to kPolyVem so every
    // FE/VEM interface is an interior face. Linear u = G·x must be exact.
    auto mesh = box_hex_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    std::size_t n_fe = 0, n_vem = 0;
    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        if ((e % 2) == 0) {
            auto poly = hex8_as_poly(mesh.elements[e]);
            mesh.elements[e].type = ElementType::kPolyVem;
            mesh.elements[e].faces = std::move(poly.faces);
            ++n_vem;
        } else {
            ++n_fe;
        }
    }
    REQUIRE(n_fe > 0);
    REQUIRE(n_vem > 0);
    REQUIRE_NOTHROW(mesh.check_validity());

    // Unified assembly must produce a single SPD-ish global K of size 3N.
    const auto K = assemble_stiffness(mesh, kSteel);
    REQUIRE(K.rows() == 3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    REQUIRE(K.cols() == K.rows());
    REQUIRE(K.nonZeros() > 0);

    const double max_err = constant_strain_max_error(mesh, sample_strain_gradient());
    INFO("checkerboard FE+VEM max |u−G·x| = " << max_err);
    REQUIRE(max_err < 1e-9);
}

TEST_CASE("FE/VEM interface patch test: tet FE + hex VEM sharing a mid-plane") {
    // 2×1×1 hex grid: left cell Kuhn-split to tet4 FE, right cell as VEM poly.
    // Shared face at x=0.5 carries identical vertex DOFs → H¹ conformity.
    auto hex = box_hex_mesh(2, 1, 1, {1.0, 1.0, 1.0});
    REQUIRE(hex.elements.size() == 2);
    NodalMesh mesh;
    mesh.nodes = hex.nodes;
    constexpr std::array<std::array<int, 4>, 6> kTets{
        {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}}};
    const auto& left = hex.elements[0].nodes;
    for (const auto& t : kTets) {
        mesh.elements.push_back(NodalElement{
            ElementType::kTet4,
            {left[static_cast<std::size_t>(t[0])], left[static_cast<std::size_t>(t[1])],
             left[static_cast<std::size_t>(t[2])], left[static_cast<std::size_t>(t[3])]}});
    }
    auto poly = hex8_as_poly(hex.elements[1]);
    mesh.elements.emplace_back(ElementType::kPolyVem, poly.nodes, poly.faces);
    REQUIRE_NOTHROW(mesh.check_validity());

    const double max_err = constant_strain_max_error(mesh, sample_strain_gradient());
    INFO("tet FE + hex VEM max |u−G·x| = " << max_err);
    REQUIRE(max_err < 1e-9);
}

TEST_CASE("hybrid native-poly fill emits unsplit VEM transitions (no fan apex)") {
    const auto s = unit_box_surface();
    // Seed ball sized to leave a transition ring (too small stamps nothing on this
    // lattice; too large floods every cell to fine).
    const std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.5}};
    auto fill = mesh::mixed_fill_surface(s, {-0.05, -0.05, -0.05}, {1.05, 1.05, 1.05}, 0.2,
                                         /*skin=*/1, {}, 0.0, seeds, 0.4,
                                         /*snap=*/false, /*turn=*/0.0,
                                         /*native_poly=*/true);
    REQUIRE(fill.native_poly_transitions);
    REQUIRE(fill.n_fine_cells > 0);
    REQUIRE(fill.n_transition_cells > 0);
    REQUIRE(fill.n_poly == fill.n_transition_cells);
    REQUIRE(fill.n_tet == 0); // no fan-split tets
    // Bulk / fine remain hex FE candidates.
    REQUIRE(fill.n_hex > 0);

    NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& cell : fill.cells) {
        if (cell.kind == mesh::MixedCellKind::kPolyVem) {
            mesh.elements.emplace_back(ElementType::kPolyVem, cell.poly_nodes, cell.poly_faces);
        } else if (cell.kind == mesh::MixedCellKind::kHex8) {
            mesh.elements.push_back(NodalElement{
                ElementType::kHex8,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4],
                 cell.nodes[5], cell.nodes[6], cell.nodes[7]}});
        } else {
            FAIL("native poly path should not emit pyramid/tet fans");
        }
    }
    REQUIRE_NOTHROW(mesh.check_validity());

    // Mixed FE hex + VEM poly assemble and pass the constant-strain gate.
    const double max_err = constant_strain_max_error(mesh, sample_strain_gradient());
    INFO("native-poly hybrid max |u−G·x| = " << max_err);
    REQUIRE(max_err < 1e-9);
}

TEST_CASE("pipeline hybrid-VEM volume mesh is mixed hex FE + poly VEM") {
    pipeline::Model m;
    m.surface = unit_box_surface();
    m.bbox_min = {-0.05, -0.05, -0.05};
    m.bbox_max = {1.05, 1.05, 1.05};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    // Feature refine on the unit box has no sharp edges; use a seed via
    // volume_mesh refine_seeds to force transitions.
    const std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.5}};
    auto vol = pipeline::volume_mesh(m, 0.2, pipeline::VolumeMesher::kHybridVem, 1,
                                     /*feature_refine=*/false, seeds, 0.4);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid-VEM") != std::string::npos);

    std::size_t n_hex = 0, n_vem = 0, n_other = 0;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == ElementType::kHex8) {
            ++n_hex;
        } else if (el.type == ElementType::kPolyVem) {
            ++n_vem;
        } else {
            ++n_other;
        }
    }
    REQUIRE(n_hex > 0);
    REQUIRE(n_vem > 0);
    REQUIRE(n_other == 0);

    const double max_err = constant_strain_max_error(vol.mesh, sample_strain_gradient());
    INFO("pipeline hybrid-VEM max |u−G·x| = " << max_err);
    REQUIRE(max_err < 1e-9);
}

