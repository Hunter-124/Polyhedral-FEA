// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/quality.hpp"
#include "mesh/tet_fill.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_CASE("regular-ish tet quality positive") {
    std::vector<Eigen::Vector3d> nodes{{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
    for (auto& p : nodes) {
        p *= 0.5;
    }
    std::vector<std::array<std::uint32_t, 4>> tets{{{0, 1, 2, 3}}};
    const auto q = polymesh::mesh::summarize_tet4_quality(nodes, tets);
    REQUIRE(q.min_aspect > 0.5);
    REQUIRE(q.n_sliver == 0);
}

TEST_CASE("tet fill box reports positive quality") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    auto fill = polymesh::mesh::tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, true);
    const auto q = polymesh::mesh::summarize_tet4_quality(fill.nodes, fill.tets);
    REQUIRE(q.min_volume > 0.0);
    REQUIRE(q.mean_aspect > 0.0);
}
