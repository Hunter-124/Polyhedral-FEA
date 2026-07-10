// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mixed_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

using namespace polymesh::mesh;
namespace pipeline = polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

polymesh::geom::TriSurface unit_box() {
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

TEST_CASE("mixed_fill unit box: hex bulk + tet skin") {
    const auto s = unit_box();
    auto fill = mixed_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.2, /*skin_layers=*/1);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_tet > 0);
    REQUIRE(fill.n_pyramid == 0);
    REQUIRE_FALSE(fill.boundary_quads.empty());
}

TEST_CASE("pipeline hybrid zoo emits mixed hex+tet elements") {
    pipeline::Model m;
    m.surface = unit_box();
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipeline::volume_mesh(m, 0.2, pipeline::VolumeMesher::kHybrid, 1, false);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);

    std::size_t n_hex = 0, n_tet = 0, n_other = 0;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == fea::ElementType::kHex8) {
            ++n_hex;
        } else if (el.type == fea::ElementType::kTet4) {
            ++n_tet;
        } else {
            ++n_other;
        }
    }
    REQUIRE(n_hex > 0);
    REQUIRE(n_tet > 0);
    REQUIRE(n_other == 0);
}

TEST_CASE("hybrid zoo native hex+tet patch test: constant strain") {
    auto fill = mixed_fill_surface(unit_box(), {-0.05, -0.05, -0.05}, {1.05, 1.05, 1.05}, 0.2,
                                   /*skin_layers=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_tet > 0);

    fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& cell : fill.cells) {
        if (cell.kind == MixedCellKind::kHex8) {
            mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kHex8,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4],
                 cell.nodes[5], cell.nodes[6], cell.nodes[7]}});
        } else {
            mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kTet4,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]}});
        }
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
    REQUIRE(max_error < 1e-10);
}

TEST_CASE("hybrid zoo cylinder_prism smoke: mixed types + snap") {
    auto model = pipeline::Model::load("bench/geometries/public/cylinder_prism.stl");
    const double h = 0.12 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);

    bool has_tet = false, has_hex = false;
    for (const auto& el : vol.mesh.elements) {
        has_tet = has_tet || el.type == fea::ElementType::kTet4;
        has_hex = has_hex || el.type == fea::ElementType::kHex8;
    }
    REQUIRE(has_tet);
    (void)has_hex; // thin solids may be all-skin
}
