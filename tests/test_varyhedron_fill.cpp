// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "mesh/varyhedron_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using polymesh::geom::CadModel;
using polymesh::geom::extract_topology;
using polymesh::geom::occ_enabled;
using polymesh::mesh::varyhedron_fill_surface;
using polymesh::pipeline::Model;
using polymesh::pipeline::VolumeMesher;
using polymesh::pipeline::volume_mesh;

TEST_CASE("varyhedron_fill_surface unit cube with topology") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const CadModel cad = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = extract_topology(cad, 6);
    const auto surface = cad.tessellate();
    auto fill =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), 0.25, 1, {}, 0.0, {},
                                0.0, 15.0, &topo);
    REQUIRE(fill.n_tets > 0);
    REQUIRE(fill.n_edge_seeds > 0);
    REQUIRE_FALSE(fill.mesh.nodes.empty());
}

TEST_CASE("Model::load STEP retains CadModel (V1c)") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/unit_cube.step";
    REQUIRE(std::filesystem::exists(path));
    const Model model = Model::load(path.string());
    REQUIRE(model.cad.has_value());
    REQUIRE_FALSE(model.cad->empty());
    REQUIRE(model.cad->has_brep());
    REQUIRE_FALSE(model.surface.triangles.empty());
    CHECK(model.source_path == path.string());
    // BBox comes from CadModel, not only the tessellation hull.
    CHECK((model.bbox_max - model.bbox_min).norm() > 0.5);
}

TEST_CASE("Model::load STL has no retained CadModel") {
    const std::filesystem::path path = "bench/geometries/public/unit_box.stl";
    if (!std::filesystem::exists(path)) {
        SKIP("unit_box.stl missing");
    }
    const Model model = Model::load(path.string());
    CHECK_FALSE(model.cad.has_value());
    REQUIRE_FALSE(model.surface.triangles.empty());
}

TEST_CASE("volume_mesh varyhedron on plate_hole.step") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/plate_hole.step";
    if (!std::filesystem::exists(path)) {
        SKIP("plate_hole.step missing");
    }
    const Model model = Model::load(path.string());
    REQUIRE(model.cad.has_value());
    REQUIRE_FALSE(model.cad->empty());
    const auto out = volume_mesh(model, 0.025, VolumeMesher::kVaryhedron, 1, true);
    REQUIRE(out.mesh.elements.size() > 10);
    REQUIRE(out.mesher_note.find("varyhedron") != std::string::npos);
    // Retained BRep should drive topology seeds (not surface-only).
    REQUIRE(out.mesher_note.find("geom_source=brep_topology") != std::string::npos);
}
