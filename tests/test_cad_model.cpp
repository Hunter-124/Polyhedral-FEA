// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/step.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>

using polymesh::geom::CadModel;
using polymesh::geom::GeomError;
using polymesh::geom::load_cad;
using polymesh::geom::load_step;
using polymesh::geom::occ_enabled;

TEST_CASE("CadModel empty default") {
    CadModel m;
    CHECK(m.empty());
    CHECK_FALSE(m.has_brep());
    CHECK(m.shape_handle() == nullptr);
}

TEST_CASE("CadModel::load_step when OCC disabled throws") {
    if (occ_enabled()) {
        SKIP("OpenCASCADE enabled; stub path not exercised");
    }
    REQUIRE_THROWS_MATCHES(CadModel::load_step("/definitely/missing/polymesh_no_such.step"),
                           GeomError,
                           Catch::Matchers::MessageMatches(
                               Catch::Matchers::ContainsSubstring("OpenCASCADE not enabled")));
}

TEST_CASE("CadModel unit cube retains BRep and tessellates when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const std::filesystem::path path = "tests/fixtures/unit_cube.step";
    REQUIRE(std::filesystem::exists(path));

    const CadModel model = CadModel::load_step(path);
    REQUIRE_FALSE(model.empty());
    REQUIRE(model.has_brep());
    REQUIRE(model.shape_handle() != nullptr);
    CHECK(model.bbox_diagonal() > 0.5); // unit cube diagonal ~√3

    const auto surface = model.tessellate();
    REQUIRE_FALSE(surface.triangles.empty());
    REQUIRE_FALSE(surface.vertices.empty());
    REQUIRE_NOTHROW(surface.validate());
    CHECK(surface.triangles.size() >= 12);

    // Historical load_step stays consistent with CadModel tessellation path.
    const auto legacy = load_step(path);
    CHECK(legacy.triangles.size() == surface.triangles.size());
}

TEST_CASE("load_cad dispatches by extension when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const auto model = load_cad("tests/fixtures/unit_cube.step");
    REQUIRE_FALSE(model.empty());
    REQUIRE_THROWS_AS(load_cad("tests/fixtures/parts/plate_hole.stl"), GeomError);
}
