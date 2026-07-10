// SPDX-License-Identifier: AGPL-3.0-or-later
#include "geom/tri_surface.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using polymesh::geom::GeomError;
using polymesh::geom::TriSurface;

TEST_CASE("validate rejects out-of-range index") {
    const TriSurface s{
        .vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
        .triangles = {{0, 1, 2}},
    };
    REQUIRE_THROWS_MATCHES(s.validate(), GeomError,
                           Catch::Matchers::MessageMatches(
                               Catch::Matchers::ContainsSubstring("out-of-range vertex 2")));
}

TEST_CASE("validate rejects degenerate triangle") {
    const TriSurface s{
        .vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}},
        .triangles = {{0, 1, 2}},
    };
    REQUIRE_THROWS_MATCHES(
        s.validate(), GeomError,
        Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring("degenerate")));
}

TEST_CASE("validate accepts unit triangle") {
    const TriSurface s{
        .vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
        .triangles = {{0, 1, 2}},
    };
    REQUIRE_NOTHROW(s.validate());
}
