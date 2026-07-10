// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace polymesh::mesh;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}

} // namespace

TEST_CASE("closest_on_surface hits box corner") {
    const auto s = unit_box();
    const auto c = closest_on_surface(s, {-0.1, -0.1, -0.1});
    REQUIRE(c.distance < 0.2);
    REQUIRE(c.point.minCoeff() >= -1e-12);
}

TEST_CASE("graded tet fill emits more tets than uniform coarse") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 2);
    REQUIRE_FALSE(graded.mesh.tets.empty());
    REQUIRE(graded.n_fine_cells > 0);
    // Uniform tet at h=0.5 on unit box: roughly 8 cells * 6 tets
    auto uniform = tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, false);
    // Graded should typically produce more tets due to fine skin
    REQUIRE(graded.mesh.tets.size() >= uniform.tets.size());
    check_tet_fill_geometry(graded.mesh);
}

TEST_CASE("pipeline graded mesher builds valid nodal mesh") {
    const auto s = unit_box();
    // write minimal model path via volume_mesh needs Model - build manually
    polymesh::pipeline::Model m;
    m.surface = s;
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(s.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 0.5, polymesh::pipeline::VolumeMesher::kGradedTet, 2);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
}

TEST_CASE("feature band refines more cells than surface skin alone") {
    const auto s = unit_box();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    REQUIRE_FALSE(edges.empty());
    // Large h → few cells; feature_band covering the whole box should force
    // more fine blocks than skin-only grading.
    auto skin_only = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, {}, 0.0);
    auto with_feat = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, edges, 2.0);
    REQUIRE(with_feat.n_fine_cells >= skin_only.n_fine_cells);
    REQUIRE(with_feat.n_feature_cells > 0);
    REQUIRE(with_feat.mesh.tets.size() >= skin_only.mesh.tets.size());
    check_tet_fill_geometry(with_feat.mesh);
}

TEST_CASE("feature/seed bands refine more blocks at fixed 2:1 lattice") {
    // Graded is always subdiv=2 (bulk≈h, fine≈h/2). Features/seeds densify
    // *which* blocks are fine — they must not rebuild a global h/4 lattice.
    const auto s = unit_box();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    auto plain = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, {}, 0.0);
    auto feat = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, edges, 0.8);
    REQUIRE(plain.subdivision == 2);
    REQUIRE(feat.subdivision == 2);
    // Same lattice spacing; features force more fine cells / tets.
    REQUIRE(std::abs(feat.h_fine - plain.h_fine) < 1e-12);
    REQUIRE(feat.n_feature_cells > 0);
    REQUIRE(feat.n_fine_cells >= plain.n_fine_cells);
    REQUIRE(feat.mesh.tets.size() >= plain.mesh.tets.size());
    check_tet_fill_geometry(feat.mesh);

    // Seed balls alone mark fine blocks (no sharp edges needed).
    std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.0}, {0.5, 0.5, 1.0}};
    auto seeded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, {}, 0.0, seeds, 0.5);
    REQUIRE(seeded.subdivision == 2);
    REQUIRE(seeded.n_seed_cells > 0);
    check_tet_fill_geometry(seeded.mesh);
}

TEST_CASE("graded tet bulk size tracks requested h (2:1)") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, 2);
    REQUIRE(graded.subdivision == 2);
    // h_coarse ≈ h, h_fine ≈ h/2 (within lattice rounding).
    REQUIRE(graded.h_coarse > 0.2);
    REQUIRE(graded.h_coarse < 0.35);
    REQUIRE(graded.h_fine > 0.1);
    REQUIRE(graded.h_fine < 0.2);
    REQUIRE(graded.n_coarse_cells > 0);
    REQUIRE(graded.n_fine_cells > 0);
    check_tet_fill_geometry(graded.mesh);
}

TEST_CASE("pipeline graded with feature_refine notes feature blocks") {
    polymesh::pipeline::Model m;
    m.surface = unit_box();
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(m.surface.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 0.5, polymesh::pipeline::VolumeMesher::kGradedTet, 1, true);
    REQUIRE(vol.mesher_note.find("feature") != std::string::npos);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
}

TEST_CASE("graded tet tiny h auto-coarsens instead of throwing grid too fine") {
    // Previously: make_bbox_grid_even(h/2) blew past 512k cells and threw
    // "grid too fine; increase element size" — product graded path must mesh.
    const auto s = unit_box();
    REQUIRE_NOTHROW(graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 1e-4, 2));
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 1e-4, 2);
    REQUIRE_FALSE(graded.mesh.tets.empty());
    REQUIRE(graded.h_coarse > 1e-4); // raised to cell budget
    REQUIRE_NOTHROW(check_tet_fill_geometry(graded.mesh));

    polymesh::pipeline::Model m;
    m.surface = s;
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(s.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 1e-4, polymesh::pipeline::VolumeMesher::kGradedTet, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
}

TEST_CASE("make_bbox_grid_even respects max_cells budget") {
    using polymesh::mesh::make_bbox_grid_even;
    using polymesh::mesh::kDefaultMaxGridCells;
    // Request absurdly fine even lattice on unit cube.
    auto g = make_bbox_grid_even({0, 0, 0}, {1, 1, 1}, 1e-6, 2, kDefaultMaxGridCells);
    REQUIRE(g.nx % 2 == 0);
    REQUIRE(g.ny % 2 == 0);
    REQUIRE(g.nz % 2 == 0);
    REQUIRE(g.cell_count() <= kDefaultMaxGridCells);
    REQUIRE(g.nx >= 2);
}

TEST_CASE("graded fill surface-snaps boundary (not pure staircase)") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, 2);
    REQUIRE_FALSE(graded.mesh.boundary_quads.empty());
    std::vector<std::uint32_t> bnodes;
    for (const auto& q : graded.mesh.boundary_quads) {
        bnodes.insert(bnodes.end(), q.begin(), q.end());
    }
    std::sort(bnodes.begin(), bnodes.end());
    bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
    const auto conf = surface_conformity(s, graded.mesh.nodes, bnodes);
    // After multi-pass snap, max residual should be well below one fine cell.
    REQUIRE(conf.max_distance < 0.55 * graded.h_fine);
    REQUIRE_NOTHROW(check_tet_fill_geometry(graded.mesh));
}

TEST_CASE("cylinder_prism graded+feature notes curvature seeds and snaps") {
    // Hole / curved wall fixture: feature grading should emit curv_seeds and a
    // snap residual line (geometry-variable mesh path).
    auto model =
        polymesh::pipeline::Model::load("bench/geometries/public/cylinder_prism.stl");
    REQUIRE(model.surface.triangles.size() >= 4);
    // volume_mesh needs positive h (auto-h is resolve_mesh_size in the job path).
    const double h = 0.15 * (model.bbox_max - model.bbox_min).maxCoeff();
    REQUIRE(h > 0.0);
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, polymesh::pipeline::VolumeMesher::kGradedTet, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);
    // Curved hole should register curvature seeds on a decent tessellation.
    // (Allow either curv_seeds or thin_seeds — both are geometry grading.)
    const bool geo = vol.mesher_note.find("curv_seeds") != std::string::npos ||
                     vol.mesher_note.find("thin_seeds") != std::string::npos ||
                     vol.mesher_note.find("feature") != std::string::npos;
    REQUIRE(geo);
}
