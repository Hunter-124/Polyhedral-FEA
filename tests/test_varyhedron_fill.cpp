// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "mesh/varyhedron_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>

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
    const double h = 0.25;
    auto fill =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0, {}, 0.0,
                                15.0, &topo);
    REQUIRE(fill.n_tets > 0);
    REQUIRE(fill.n_edge_seeds > 0);
    // CDS protect radii: r ≤ α h (α=0.45), plus corner shrink can go lower.
    REQUIRE(fill.max_protect_radius > 0.0);
    REQUIRE(fill.min_protect_radius > 0.0);
    REQUIRE(fill.min_protect_radius <= fill.max_protect_radius);
    CHECK(fill.max_protect_radius <= 0.45 * h + 1e-9);
    // V6c packing seed engine: interior bubbles + relax stats.
    REQUIRE(fill.n_volume_seeds > 0);
    REQUIRE(fill.n_pack_relax_iters > 0);
    CHECK(fill.pack_fill_frac >= 0.0);
    CHECK(fill.pack_fill_frac <= 1.0);
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

TEST_CASE("Model::load rejects STL inputs (CAD-only)") {
    REQUIRE_THROWS_AS(Model::load("bench/geometries/public/unit_box.stl"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(Model::load("/tmp/nope.stl"), std::runtime_error);
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
    // Packing stats appear in the note (sharp-only protect; dual deferred).
    REQUIRE(out.mesher_note.find("vol_seeds=") != std::string::npos);
    REQUIRE(out.mesher_note.find("sharp-only") != std::string::npos);
    REQUIRE(out.mesher_note.find("dual deferred") != std::string::npos);
}

TEST_CASE("varyhedron protects only sharp edges on cylinder (M3)") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const CadModel cad = CadModel::load_step(path);
    const auto topo = extract_topology(cad, 8);
    const auto counts = polymesh::geom::count_edge_features(topo);
    // Cylinder: end rims sharp; at least one seam on the lateral wall.
    REQUIRE(counts.n_sharp >= 2);
    REQUIRE(counts.n_seam >= 1);

    const auto surface = cad.tessellate();
    const double h = 0.04;
    auto fill = varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0,
                                        {}, 0.0, 15.0, &topo);
    REQUIRE(fill.n_tets > 0);
    REQUIRE(fill.n_sharp_edges == static_cast<std::size_t>(counts.n_sharp));
    REQUIRE(fill.n_seam_edges == static_cast<std::size_t>(counts.n_seam));
    // Protecting balls only on sharp features — seams never get seeds.
    REQUIRE(fill.n_edge_seeds > 0);
    // With only ~2 sharp circles, seed count should stay modest vs protecting all edges.
    CHECK(fill.n_edge_seeds < 400);
    // CDS: max protect radius never exceeds α h (α=0.45).
    REQUIRE(fill.max_protect_radius > 0.0);
    CHECK(fill.max_protect_radius <= 0.45 * h + 1e-9);
    CHECK(fill.min_protect_radius <= fill.max_protect_radius);
}
