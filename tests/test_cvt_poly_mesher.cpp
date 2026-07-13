// SPDX-License-Identifier: BSD-3-Clause
// M5 substrate: PolyMesh→VEM convert + cvt_poly volume_mesh smoke.

#include "fea/poly_to_vem.hpp"
#include "mesh/cvt_export.hpp"
#include "mesh/geogram_clip.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;
using polymesh::fea::ElementType;
using polymesh::fea::poly_mesh_to_vem;
using polymesh::mesh::ClipBox;
using polymesh::mesh::export_clipped_voronoi;
using polymesh::mesh::geogram_available;

TEST_CASE("poly_mesh_to_vem maps unit-cube Voronoi cell", "[cvt][m5]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    ClipBox box;
    std::vector<Eigen::Vector3d> sites = {Eigen::Vector3d(0.5, 0.5, 0.5)};
    const auto exp = export_clipped_voronoi(box, sites);
    REQUIRE(exp.stats.n_cells == 1);
    const auto nodal = poly_mesh_to_vem(exp.mesh);
    REQUIRE(nodal.elements.size() == 1);
    REQUIRE(nodal.elements[0].type == ElementType::kPolyVem);
    REQUIRE(nodal.elements[0].nodes.size() >= 8);  // cube corners
    REQUIRE(nodal.elements[0].faces.size() >= 6);
    REQUIRE_NOTHROW(nodal.check_validity());
}

TEST_CASE("volume_mesh cvt_poly on unit surface produces poly VEM", "[cvt][m5]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    // Minimal closed box surface (unit cube tessellation).
    polymesh::pipeline::Model model;
    model.bbox_min = Eigen::Vector3d(0, 0, 0);
    model.bbox_max = Eigen::Vector3d(1, 1, 1);
    model.name = "unit_box";
    // 12 triangles for a unit cube (two per face).
    model.surface.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    model.surface.triangles = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
        {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
    };
    model.region_count = 1;
    model.triangle_region.assign(model.surface.triangles.size(), 0);

    const double h = 0.5;  // → n_side ~ 2
    auto out = polymesh::pipeline::volume_mesh(
        model, h, polymesh::pipeline::VolumeMesher::kCvtPoly);
    REQUIRE(out.mesh.elements.size() >= 4);
    std::size_t n_poly = 0;
    for (const auto& el : out.mesh.elements) {
        if (el.type == ElementType::kPolyVem) {
            ++n_poly;
        }
    }
    REQUIRE(n_poly == out.mesh.elements.size());
    REQUIRE(out.mesher_note.find("cvt_poly") != std::string::npos);
    REQUIRE_NOTHROW(out.mesh.check_validity());
}
