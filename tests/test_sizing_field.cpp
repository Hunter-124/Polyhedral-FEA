// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/sizing_field.hpp"
#include "geom/features.hpp"
#include "geom/tri_surface.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("uniform sizing is constant") {
    const polymesh::adapt::UniformSizing f(0.05);
    CHECK(f.size_at({0.0, 0.0, 0.0}) == 0.05);
    CHECK(f.size_at({1e3, -2e3, 5.5}) == 0.05);
}

TEST_CASE("feature sizing blends from h_min to h_max") {
    polymesh::adapt::FeatureSizing f(0.01, 0.10, 1.0, [](const Eigen::Vector3d& p) {
        return p[0]; // distance = x
    });
    CHECK(f.size_at({0.0, 0, 0}) == Catch::Approx(0.01));
    CHECK(f.size_at({0.5, 0, 0}) == Catch::Approx(0.055));
    CHECK(f.size_at({1.0, 0, 0}) == Catch::Approx(0.10));
    CHECK(f.size_at({5.0, 0, 0}) == Catch::Approx(0.10));
}

TEST_CASE("make_feature_sizing uses sharp edges on unit cube") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    REQUIRE_FALSE(edges.empty());
    auto field = polymesh::adapt::make_feature_sizing(0.02, 0.2, 0.5, s, edges);
    // Near a cube edge/corner → closer to h_min
    const double h_near = field->size_at({0.0, 0.0, 0.0});
    const double h_far = field->size_at({0.5, 0.5, 0.5});
    CHECK(h_near < h_far);
    CHECK(h_near >= 0.02 - 1e-12);
    CHECK(h_far <= 0.2 + 1e-12);
}
