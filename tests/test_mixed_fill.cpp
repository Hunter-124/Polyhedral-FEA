// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "geom/stl.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mixed_fill.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

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

TEST_CASE("mixed_fill unit box: hex bulk + pyramid skin") {
    const auto s = unit_box();
    auto fill = mixed_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.2, /*skin_layers=*/1);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_pyramid > 0);
    REQUIRE(fill.n_tet == 0);
    REQUIRE_FALSE(fill.boundary_quads.empty());
}

TEST_CASE("pipeline hybrid zoo emits all-pyramid product FE") {
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

    std::size_t n_pyr = 0, n_other = 0;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == fea::ElementType::kPyramid5) {
            ++n_pyr;
        } else {
            ++n_other;
        }
    }
    REQUIRE(n_pyr > 0);
    REQUIRE(n_other == 0);
}

TEST_CASE("hybrid zoo expanded product path patch test: constant strain") {
    auto raw = mixed_fill_surface(unit_box(), {-0.05, -0.05, -0.05}, {1.05, 1.05, 1.05}, 0.2,
                                  /*skin_layers=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    REQUIRE(raw.n_hex > 0);
    REQUIRE(raw.n_pyramid > 0);
    auto fill = expand_mixed_hex_to_pyramids(raw);
    REQUIRE(fill.n_hex == 0);
    REQUIRE(fill.n_pyramid > 0);

    fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& cell : fill.cells) {
        REQUIRE(cell.kind == MixedCellKind::kPyramid5);
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
    REQUIRE(max_error < 1e-10);
}

TEST_CASE("hybrid zoo cylinder_prism smoke: pyramid FE + snap") {
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.12 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);

    bool has_pyr = false;
    for (const auto& el : vol.mesh.elements) {
        has_pyr = has_pyr || el.type == fea::ElementType::kPyramid5;
    }
    REQUIRE(has_pyr);
}

TEST_CASE("hybrid zoo 2:1 size adaptivity on feature band") {
    // Open box with a sharp crease band: feature grading must refine to h/2.
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.15 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto plain = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                    /*skin=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    auto edges = polymesh::geom::detect_sharp_edges(model.surface, 30.0);
    REQUIRE_FALSE(edges.empty());
    auto graded = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                     /*skin=*/1, edges, 2.0 * h, {}, 0.0,
                                     /*snap=*/false);
    REQUIRE(graded.n_fine_cells > 0);
    REQUIRE(graded.n_transition_cells > 0);
    REQUIRE(graded.h_fine == Catch::Approx(0.5 * graded.h).margin(1e-9));
    // Fine path should produce more cells than plain (2×2×2 in feature bands).
    REQUIRE(graded.cells.size() > plain.cells.size());
}
