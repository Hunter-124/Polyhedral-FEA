// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"
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
    auto with_feat =
        graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, edges, 2.0);
    REQUIRE(with_feat.n_fine_cells >= skin_only.n_fine_cells);
    REQUIRE(with_feat.n_feature_cells > 0);
    REQUIRE(with_feat.mesh.tets.size() >= skin_only.mesh.tets.size());
    check_tet_fill_geometry(with_feat.mesh);
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
