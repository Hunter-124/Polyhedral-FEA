// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

polymesh::pipeline::Model make_box(double scale) {
    polymesh::pipeline::Model model;
    const double lx = scale;
    const double ly = 2.0 * scale;
    const double lz = 0.5 * scale;
    model.surface.vertices = {
        {0, 0, 0},   {lx, 0, 0},   {lx, ly, 0},   {0, ly, 0},
        {0, 0, lz},  {lx, 0, lz},  {lx, ly, lz},  {0, ly, lz},
    };
    model.surface.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    model.surface.validate();
    model.bbox_min = Eigen::Vector3d::Zero();
    model.bbox_max = {lx, ly, lz};
    return model;
}

std::array<double, 22> doubles(const polymesh::pipeline::CaseFeatures& f) {
    return {f.bbox_dx,
            f.bbox_dy,
            f.bbox_dz,
            f.diag,
            f.volume,
            f.surface_area,
            f.sa_over_v23,
            f.sharp_edge_len_total,
            f.curved_frac,
            f.kappa_max_h,
            f.kappa_mean_h,
            f.thin_min_over_diag,
            f.thin_p10_over_diag,
            f.min_feature_h,
            f.fix_area_frac,
            f.load_area_frac,
            f.load_dir_x,
            f.load_dir_y,
            f.load_dir_z,
            f.fix_load_dist_over_diag,
            f.load_axis_alignment,
            f.poisson};
}

std::vector<polymesh::pipeline::RefineRegion> fix_regions(double scale) {
    return {{{-0.01 * scale, -0.01 * scale, -0.01 * scale},
             {0.01 * scale, 2.01 * scale, 0.51 * scale}, 0.5}};
}

std::vector<polymesh::pipeline::RefineRegion> load_regions(double scale) {
    return {{{0.99 * scale, -0.01 * scale, -0.01 * scale},
             {1.01 * scale, 2.01 * scale, 0.51 * scale}, 0.25}};
}

} // namespace

TEST_CASE("case features are deterministic", "[features]") {
    const auto model = make_box(1.0);
    const auto fix = fix_regions(1.0);
    const auto load = load_regions(1.0);
    const auto a = polymesh::pipeline::extract_case_features(model, fix, load, {4, 0, 0}, 0.3);
    const auto b = polymesh::pipeline::extract_case_features(model, fix, load, {4, 0, 0}, 0.3);

    REQUIRE(doubles(a) == doubles(b));
    REQUIRE(a.n_faces == b.n_faces);
    REQUIRE(a.n_sharp_edges == b.n_sharp_edges);
    REQUIRE(a.n_fix_faces == b.n_fix_faces);
    REQUIRE(a.n_load_faces == b.n_load_faces);
}

TEST_CASE("case feature geometry is invariant under uniform scaling", "[features]") {
    const auto unit = polymesh::pipeline::extract_case_features(
        make_box(1.0), fix_regions(1.0), load_regions(1.0), {1, 0, 0}, 0.3);
    const auto scaled = polymesh::pipeline::extract_case_features(
        make_box(1000.0), fix_regions(1000.0), load_regions(1000.0), {1, 0, 0}, 0.3);

    const auto a = doubles(unit);
    const auto b = doubles(scaled);
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE_THAT(b[i], Catch::Matchers::WithinRel(a[i], 1e-10));
    }
    REQUIRE(unit.n_faces == scaled.n_faces);
    REQUIRE(unit.n_sharp_edges == scaled.n_sharp_edges);
    REQUIRE(unit.n_fix_faces == scaled.n_fix_faces);
    REQUIRE(unit.n_load_faces == scaled.n_load_faces);
}

TEST_CASE("case features keep invalid and empty regions finite", "[features]") {
    const auto model = make_box(1.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<polymesh::pipeline::RefineRegion> invalid{
        {{1, 1, 1}, {-1, -1, -1}, 0.5},
        {{nan, 0, 0}, {nan, 1, 1}, 0.5},
    };
    const std::vector<polymesh::pipeline::RefineRegion> empty;
    const auto f = polymesh::pipeline::extract_case_features(
        model, invalid, empty, {nan, nan, nan}, nan);

    for (const double value : doubles(f)) {
        REQUIRE(std::isfinite(value));
    }
    REQUIRE(f.n_fix_faces == 0);
    REQUIRE(f.n_load_faces == 0);
}
