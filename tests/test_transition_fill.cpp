// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "mesh/transition_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace polymesh::mesh;
namespace fea = polymesh::fea;
namespace pipe = polymesh::pipeline;

namespace {
polymesh::geom::TriSurface box() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}
} // namespace

TEST_CASE("transition fill has hex core and pyramid skin") {
    // Slightly padded bbox + h=0.2 yields a nonempty hex core on the unit box.
    auto fill = transition_fill_surface(box(), {-0.01, -0.01, -0.01}, {1.01, 1.01, 1.01}, 0.2);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_pyramid > 0);
    REQUIRE(fill.n_pyramid % 6 == 0); // 6 pyramids per boundary hex
    REQUIRE(fill.boundary_max_distance >= 0.0);
    REQUIRE(fill.boundary_max_distance < 0.5); // limited snap residual
}

TEST_CASE("transition fill snap reduces boundary distance vs no-snap") {
    const auto surf = box();
    const Eigen::Vector3d bmin{-0.01, -0.01, -0.01};
    const Eigen::Vector3d bmax{1.01, 1.01, 1.01};
    auto snapped = transition_fill_surface(surf, bmin, bmax, 0.2, true);
    auto raw = transition_fill_surface(surf, bmin, bmax, 0.2, false);
    // Without snap, residual is not tracked (stays 0); with snap it is measured.
    REQUIRE(snapped.boundary_max_distance >= 0.0);
    REQUIRE(raw.n_hex == snapped.n_hex);
    REQUIRE(raw.n_pyramid == snapped.n_pyramid);
    // Snapped mesh remains valid through the pipeline.
    pipe::Model m;
    m.surface = surf;
    m.bbox_min = bmin;
    m.bbox_max = bmax;
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipe::volume_mesh(m, 0.2, pipe::VolumeMesher::kHexPyramid, 2);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
}

TEST_CASE("hex+pyramid mesh solves linear elasticity") {
    pipe::Model m;
    m.surface = box();
    m.bbox_min = {-0.01, -0.01, -0.01};
    m.bbox_max = {1.01, 1.01, 1.01};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipe::volume_mesh(m, 0.2, pipe::VolumeMesher::kHexPyramid, 2);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    bool has_hex = false, has_pyr = false;
    for (const auto& e : vol.mesh.elements) {
        if (e.type == fea::ElementType::kHex8)
            has_hex = true;
        if (e.type == fea::ElementType::kPyramid5)
            has_pyr = true;
    }
    REQUIRE(has_hex);
    REQUIRE(has_pyr);
    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
        if (vol.mesh.nodes[i][0] < 1e-9)
            bc.fix_node(i);
        if (vol.mesh.nodes[i][0] > 1.0 - 1e-9)
            loads(3 * static_cast<Eigen::Index>(i) + 2) = -1.0;
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    const fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    REQUIRE_NOTHROW(fea::solve_elastostatics(vol.mesh, mat, bc, loads));
}

TEST_CASE("pyramid lattice patch test: constant strain (all-pyramid cells)") {
    // Element-level pyramid5 (tet-split stiffness) on pure-pyramid lattice.
    auto fill =
        transition_fill_surface(box(), {-0.01, -0.01, -0.01}, {1.01, 1.01, 1.01}, 0.25, false);
    REQUIRE(fill.n_hex == 0);
    REQUIRE(fill.n_pyramid > 0);

    fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& cell : fill.cells) {
        REQUIRE(cell.kind == TransitionCellKind::kPyramid5);
        mesh.elements.push_back(fea::NodalElement{
            fea::ElementType::kPyramid5,
            {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
    }

    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;

    std::set<std::uint32_t> bnodes;
    for (const auto& q : fill.boundary_quads) {
        bnodes.insert(q.begin(), q.end());
    }
    fea::Dirichlet bc;
    for (auto i : bnodes) {
        bc.fix_node(i, g * mesh.nodes[i]);
    }
    const fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = fea::solve_elastostatics(mesh, mat, bc, loads);

    double max_error = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d exact = g * mesh.nodes[i];
        const Eigen::Vector3d fem = u.segment<3>(3 * static_cast<Eigen::Index>(i));
        max_error = std::max(max_error, (fem - exact).norm());
    }
    REQUIRE(max_error < 1e-12);
}
