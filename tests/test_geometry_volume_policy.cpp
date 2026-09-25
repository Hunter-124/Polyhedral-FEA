// SPDX-License-Identifier: BSD-3-Clause
// Geometry-completeness guard and the solved-geometry volume policy: exact
// CAD volume against the delivered mesh, including quadratic boundary mids,
// and the degraded/egregious bands around kGeometryVolumeHardLimit.

#include "fea/nodal_mesh.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <string>

namespace {

constexpr char kUnitBox[] = "bench/geometries/public/unit_box.step";

} // namespace

TEST_CASE("geometry completeness guard rejects a synthetic aliased solid",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto cad = polymesh::geom::CadModel::load_step(kUnitBox);
    const auto inspection = polymesh::geom::inspect_brep(cad);
    REQUIRE(inspection.available);
    REQUIRE(inspection.volume > 0.0);

    const auto exact = polymesh::mesh::evaluate_geometry_completeness(cad, inspection.volume);
    REQUIRE(exact.available);
    CHECK(exact.complete);
    CHECK(exact.relative_volume_error == Catch::Approx(0.0));

    const double aliased_volume =
        inspection.volume *
        (1.0 + 2.0 * polymesh::mesh::kGeometryCompletenessRelVolumeTolerance);
    const auto aliased = polymesh::mesh::evaluate_geometry_completeness(cad, aliased_volume);
    REQUIRE(aliased.available);
    CHECK_FALSE(aliased.complete);
    CHECK(aliased.relative_volume_error > aliased.relative_volume_tolerance);
}

TEST_CASE("solved geometry volume integrates quadratic boundary mids",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kUnitBox);
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.5, 0.0, 0.0},
        {0.5, 0.5, 0.0}, {0.0, 0.5, 0.0}, {0.0, 0.0, 0.5}, {0.5, 0.0, 0.5}, {0.0, 0.5, 0.5},
    };
    mesh.elements.push_back(
        {polymesh::fea::ElementType::kTet10, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}});

    const auto straight = polymesh::pipeline::measure_geometry_volume(model, mesh);
    REQUIRE(straight.available);
    CHECK(straight.mesh_volume == Catch::Approx(1.0 / 6.0).epsilon(1e-12));

    // Corner-only surface volume is unchanged, but the actual Tet10 geometry
    // contracts when the three mids of the face opposite node 0 move inward.
    mesh.nodes[5].z() = 0.1;
    mesh.nodes[8].y() = 0.1;
    mesh.nodes[9].x() = 0.1;
    const auto curved = polymesh::pipeline::measure_geometry_volume(model, mesh);
    REQUIRE(curved.available);
    CHECK(std::abs(curved.mesh_volume - straight.mesh_volume) > 1e-4);
}

TEST_CASE("geometry volume policy retains degraded meshes and rejects egregious loss",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kUnitBox);
    REQUIRE(model.cad);
    const double cad_volume = polymesh::geom::inspect_brep(*model.cad).volume;
    REQUIRE(cad_volume > 0.0);
    const auto output_with_volume = [&](double fraction) {
        polymesh::pipeline::VolumeMeshOutput output;
        output.mesh.nodes = {{0.0, 0.0, 0.0},
                             {1.0, 0.0, 0.0},
                             {0.0, 1.0, 0.0},
                             {0.0, 0.0, 6.0 * fraction * cad_volume}};
        output.mesh.elements.push_back({polymesh::fea::ElementType::kTet4, {0, 1, 2, 3}});
        return output;
    };

    auto degraded = output_with_volume(0.95);
    CHECK_NOTHROW(polymesh::pipeline::update_solved_geometry_volume(model, degraded));
    CHECK(degraded.solved_geometry_volume.relative_error == Catch::Approx(0.05));
    CHECK(degraded.mesher_note.find("band=degraded") != std::string::npos);

    auto egregious = output_with_volume(0.80);
    CHECK_THROWS_AS(polymesh::pipeline::update_solved_geometry_volume(model, egregious),
                    polymesh::pipeline::GeometryVolumeLimitError);
}
