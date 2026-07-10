// SPDX-License-Identifier: BSD-3-Clause
#include "geom/step.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>

using polymesh::geom::GeomError;
using polymesh::geom::load_step;
using polymesh::geom::occ_enabled;

TEST_CASE("occ_enabled matches compile-time flag") {
#ifdef POLYMESH_WITH_OCC
    CHECK(occ_enabled());
#else
    CHECK_FALSE(occ_enabled());
#endif
}

TEST_CASE("load_step when OCC disabled throws OpenCASCADE not enabled") {
    if (occ_enabled()) {
        SKIP("OpenCASCADE enabled; stub path not exercised");
    }
    REQUIRE_THROWS_MATCHES(load_step("/definitely/missing/polymesh_no_such_file.step"),
                           GeomError,
                           Catch::Matchers::MessageMatches(
                               Catch::Matchers::ContainsSubstring("OpenCASCADE not enabled")));
}

TEST_CASE("load_step missing file throws GeomError when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    REQUIRE_THROWS_AS(load_step("/definitely/missing/polymesh_no_such_file.step"), GeomError);
}

TEST_CASE("load_step unit cube fixture when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    // Relative to repo root (catch_discover_tests WORKING_DIRECTORY).
    const std::filesystem::path path = "tests/fixtures/unit_cube.step";
    REQUIRE(std::filesystem::exists(path));
    const auto surface = load_step(path);
    REQUIRE_FALSE(surface.triangles.empty());
    REQUIRE_FALSE(surface.vertices.empty());
    REQUIRE_NOTHROW(surface.validate());
    // Unit cube: 6 faces, at least 2 triangles each.
    CHECK(surface.triangles.size() >= 12);
    CHECK(surface.vertices.size() >= 8);
}
