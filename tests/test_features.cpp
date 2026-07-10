// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/error.hpp"
#include "geom/features.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("unit cube has sharp edges at 90 degree faces") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    s.validate();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    REQUIRE(edges.size() >= 12); // cube has 12 edges
}

TEST_CASE("dorfler marks largest contributors") {
    const std::vector<double> eta{1.0, 0.1, 0.1, 0.1};
    const auto m = polymesh::adapt::dorfler_mark(eta, 0.5);
    REQUIRE_FALSE(m.empty());
    REQUIRE(m.front() == 0);
}
