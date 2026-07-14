// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/graded_sizing.hpp"
#include "geom/tri_surface.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <vector>

using polymesh::adapt::GradedSizing;
using polymesh::adapt::SizeSource;
using Eigen::Vector3d;

namespace {
polymesh::geom::TriSurface unit_cube() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}
} // namespace

TEST_CASE("empty source set is uniform h_max") {
    const GradedSizing f({}, 0.01, 0.10, 1.0);
    CHECK(f.size_at({0, 0, 0}) == 0.10);
    CHECK(f.size_at({5, -3, 2}) == 0.10);
}

TEST_CASE("single source grades linearly then clamps to h_max") {
    // h(x) = clamp(h0 + beta*|x|, h_min, h_max), h0=0.02, beta=0.5, h_max=0.10.
    const GradedSizing f({SizeSource{{0, 0, 0}, 0.02}}, 0.02, 0.10, 0.5);
    CHECK(f.size_at({0, 0, 0}) == Catch::Approx(0.02));
    CHECK(f.size_at({0.10, 0, 0}) == Catch::Approx(0.07)); // 0.02 + 0.5*0.10
    CHECK(f.size_at({1.0, 0, 0}) == Catch::Approx(0.10));  // clamped
}

TEST_CASE("source target below h_min is clamped up") {
    const GradedSizing f({SizeSource{{0, 0, 0}, 0.001}}, 0.02, 0.10, 1.0);
    CHECK(f.size_at({0, 0, 0}) == Catch::Approx(0.02)); // 0.001 clamped to h_min
}

TEST_CASE("field is the lower envelope of its sources (min-plus)") {
    // Two sources at x=0 and x=1, both h=0.02, beta=1.0, h_max large enough not
    // to clamp: at x=0.5 both give 0.02 + 0.5 = 0.52.
    const GradedSizing f(
        {SizeSource{{0, 0, 0}, 0.02}, SizeSource{{1, 0, 0}, 0.02}}, 0.02, 2.0, 1.0);
    CHECK(f.size_at({0, 0, 0}) == Catch::Approx(0.02));
    CHECK(f.size_at({1, 0, 0}) == Catch::Approx(0.02));
    CHECK(f.size_at({0.5, 0, 0}) == Catch::Approx(0.52));
    // Beyond both sources the nearer one wins (envelope, not sum).
    CHECK(f.size_at({2, 0, 0}) == Catch::Approx(0.02 + 1.0)); // from source at x=1
}

TEST_CASE("field is Lipschitz with constant beta") {
    const double beta = 0.8;
    const GradedSizing f(
        {SizeSource{{0, 0, 0}, 0.01}, SizeSource{{2, 1, 0}, 0.03}}, 0.01, 100.0, beta);
    const std::array<Vector3d, 5> pts = {Vector3d{0, 0, 0}, Vector3d{0.3, 0.2, 0.1},
                                         Vector3d{1, 0.5, 0}, Vector3d{1.7, 0.9, 0.4},
                                         Vector3d{2, 1, 0}};
    for (const auto& a : pts) {
        for (const auto& b : pts) {
            const double lhs = std::abs(f.size_at(a) - f.size_at(b));
            const double rhs = beta * (a - b).norm();
            CHECK(lhs <= rhs + 1e-9);
        }
    }
}

TEST_CASE("point_size_sources tags every point with the given h") {
    std::vector<Vector3d> pts = {{0, 0, 0}, {1, 1, 1}};
    const auto src = polymesh::adapt::point_size_sources(pts, 0.05);
    REQUIRE(src.size() == 2);
    CHECK(src[0].h == 0.05);
    CHECK(src[1].x == Vector3d{1, 1, 1});
}

TEST_CASE("seed_plan keeps only sources finer than h_coarse") {
    std::vector<SizeSource> src = {
        SizeSource{{0, 0, 0}, 0.02}, // fine  → seed
        SizeSource{{1, 0, 0}, 0.20}, // coarse → dropped (== h_coarse)
        SizeSource{{2, 0, 0}, 0.05}, // fine  → seed
    };
    const auto plan = polymesh::adapt::seed_plan(src, /*h_coarse=*/0.20, /*band_frac=*/2.0);
    REQUIRE(plan.refine_seeds.size() == 2);
    CHECK(plan.h_fine == Catch::Approx(0.02));
    CHECK(plan.seed_band == Catch::Approx(0.40)); // 2.0 * 0.20
}

TEST_CASE("seed_plan with no fine sources yields no seeds and zero band") {
    std::vector<SizeSource> src = {SizeSource{{0, 0, 0}, 0.30}};
    const auto plan = polymesh::adapt::seed_plan(src, 0.20, 2.0);
    CHECK(plan.refine_seeds.empty());
    CHECK(plan.seed_band == 0.0);
}

TEST_CASE("geometry_size_sources returns in-range sources on a cube") {
    const auto s = unit_cube();
    const double h_min = 0.02;
    const double h_max = 0.20;
    const auto src = polymesh::adapt::geometry_size_sources(s, h_min, h_max);
    // Every emitted source must be a finer request pinned to a surface vertex.
    for (const auto& g : src) {
        CHECK(g.h >= h_min);
        CHECK(g.h <= h_max);
    }
    // Feed them through the field: any query stays within bounds and is finite.
    const GradedSizing f(src, h_min, h_max, 1.0);
    const double q = f.size_at({0.5, 0.5, 0.5});
    CHECK(q >= h_min);
    CHECK(q <= h_max);
}
