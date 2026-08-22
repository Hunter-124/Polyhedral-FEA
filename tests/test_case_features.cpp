// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "geom/step.hpp"

#include <Eigen/Core>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <vector>

namespace {

polymesh::pipeline::Model make_box(double scale) {
    polymesh::pipeline::Model model;
    const double lx = scale;
    const double ly = 2.0 * scale;
    const double lz = 0.5 * scale;
    model.surface.vertices = {
        {0, 0, 0},  {lx, 0, 0},  {lx, ly, 0},  {0, ly, 0},
        {0, 0, lz}, {lx, 0, lz}, {lx, ly, lz}, {0, ly, lz},
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

std::array<double, 35> doubles(const polymesh::pipeline::CaseFeatures& f) {
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
            f.poisson,
            f.geo_n_inner_loops,
            f.geo_hole_spacing_min_rel,
            f.geo_hole_spacing_p10_rel,
            f.geo_feat_pair_dist_min_rel,
            f.geo_feat_pair_dist_p10_rel,
            f.geo_feat_pair_dist_mean_rel,
            f.geo_dihedral_p10,
            f.geo_dihedral_p50,
            f.geo_dihedral_p90,
            f.geo_singular_lambda_min,
            f.load_to_feature_dist_min_rel,
            f.fix_to_feature_dist_min_rel,
            f.case_load_multiaxiality};
}

std::vector<polymesh::pipeline::RefineRegion> fix_regions(double scale) {
    return {{{-0.01 * scale, -0.01 * scale, -0.01 * scale},
             {0.01 * scale, 2.01 * scale, 0.51 * scale},
             0.5}};
}

std::vector<polymesh::pipeline::RefineRegion> load_regions(double scale) {
    return {{{0.99 * scale, -0.01 * scale, -0.01 * scale},
             {1.01 * scale, 2.01 * scale, 0.51 * scale},
             0.25}};
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
    const auto f =
        polymesh::pipeline::extract_case_features(model, invalid, empty, {nan, nan, nan}, nan);

    for (const double value : doubles(f)) {
        REQUIRE(std::isfinite(value));
    }
    REQUIRE(f.n_fix_faces == 0);
    REQUIRE(f.n_load_faces == 0);
}

// The proximity block has to be measured on real CAD: a hand-built triangle box
// carries no BRep, so it can only ever report the sentinels. This fixture is a
// 120x60x10 mm plate with two 8 mm-radius through holes whose axes are exactly
// 40 mm apart, which makes the expected spacing a consequence of the fixture's
// construction rather than a recorded number.
TEST_CASE("two-hole plate reports both bores and their spacing", "[features]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("built without OpenCASCADE");
    }
    const std::filesystem::path step = "tests/fixtures/parts/two_hole_plate.step";
    REQUIRE(std::filesystem::exists(step));
    const auto model = polymesh::pipeline::Model::load(step.string());
    const auto f = polymesh::pipeline::extract_case_features(model, {}, {}, {1, 0, 0}, 0.3);

    REQUIRE(f.geo_available);
    REQUIRE(f.geo_n_inner_loops == 2.0);

    const double diag = (model.bbox_max - model.bbox_min).norm();
    REQUIRE(diag > 0.0);
    // Axis-to-axis distance of the two drills, normalized like every other
    // length: the two hole centres were placed 40 mm apart.
    REQUIRE_THAT(f.geo_hole_spacing_min_rel, Catch::Matchers::WithinRel(0.040 / diag, 1e-9));
    REQUIRE_THAT(f.geo_hole_spacing_p10_rel,
                 Catch::Matchers::WithinRel(f.geo_hole_spacing_min_rel, 1e-12));

    // Salient features exist (the two hole walls and the two short end faces),
    // so the pair distances are measurements rather than the 1.0 sentinel, and
    // no distance can exceed the diagonal it is divided by.
    REQUIRE(f.geo_feat_pair_dist_min_rel > 0.0);
    REQUIRE(f.geo_feat_pair_dist_min_rel < 1.0);
    REQUIRE(f.geo_feat_pair_dist_min_rel <= f.geo_feat_pair_dist_p10_rel);
    REQUIRE(f.geo_feat_pair_dist_p10_rel <= f.geo_feat_pair_dist_mean_rel);
    REQUIRE(f.geo_feat_pair_dist_mean_rel < 1.0);

    // Every crease of a drilled plate is a convex 90 degrees, so the dihedral
    // distribution collapses onto pi/2 and nothing is singular.
    REQUIRE_THAT(f.geo_dihedral_p50, Catch::Matchers::WithinAbs(0.5 * std::numbers::pi, 1e-9));
    REQUIRE(f.geo_dihedral_p10 <= f.geo_dihedral_p50);
    REQUIRE(f.geo_dihedral_p50 <= f.geo_dihedral_p90);
    REQUIRE(f.geo_singular_lambda_min == 1.0);

    // No boundary-condition regions were given, so the case columns stay at
    // their documented sentinels instead of collapsing to zero distance.
    REQUIRE(f.load_to_feature_dist_min_rel == 1.0);
    REQUIRE(f.fix_to_feature_dist_min_rel == 1.0);
    REQUIRE(f.case_load_multiaxiality == 0.0);
}

// Two load patches pulling along different axes: the summed load direction
// cannot express that, which is why the per-region traction vectors are a
// separate argument. Orthogonal tractions must read pi/2, and the same two
// patches pulling along one axis must read 0.
TEST_CASE("load multiaxiality measures the angle between the two largest loads",
          "[features]") {
    const auto model = make_box(1.0);
    const std::vector<polymesh::pipeline::RefineRegion> loads{
        {{-0.01, -0.01, -0.01}, {0.01, 2.01, 0.51}, 0.25},
        {{0.99, -0.01, -0.01}, {1.01, 2.01, 0.51}, 0.25},
    };
    const std::vector<Eigen::Vector3d> crossed{{1.0e6, 0.0, 0.0}, {0.0, 0.0, 1.0e6}};
    const std::vector<Eigen::Vector3d> aligned{{1.0e6, 0.0, 0.0}, {-2.0e6, 0.0, 0.0}};

    const auto multi =
        polymesh::pipeline::extract_case_features(model, {}, loads, {1, 0, 1}, 0.3, crossed);
    REQUIRE_THAT(multi.case_load_multiaxiality,
                 Catch::Matchers::WithinAbs(0.5 * std::numbers::pi, 1e-12));

    // Opposed but collinear tractions share one axis: uniaxial, not biaxial.
    const auto uniaxial =
        polymesh::pipeline::extract_case_features(model, {}, loads, {-1, 0, 0}, 0.3, aligned);
    REQUIRE_THAT(uniaxial.case_load_multiaxiality, Catch::Matchers::WithinAbs(0.0, 1e-12));

    // Without the per-region vectors there is nothing to measure, and the
    // documented single-axis sentinel is what comes back.
    const auto summed =
        polymesh::pipeline::extract_case_features(model, {}, loads, {1, 0, 1}, 0.3);
    REQUIRE(summed.case_load_multiaxiality == 0.0);
}
