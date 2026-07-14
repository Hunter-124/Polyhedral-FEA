// SPDX-License-Identifier: BSD-3-Clause
// Geometry + boundary-condition refinement plan (ADR-0021): the mesh must grade
// toward the simulation setup (BC/load selection boxes) fused with geometry
// features, not just uniformly fill the bbox.

#include "geom/step.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Eigen/Core>

#include <filesystem>
#include <vector>

using polymesh::pipeline::build_refinement_plan;
using polymesh::pipeline::RefineRegion;

TEST_CASE("refinement plan: no regions, no geometry → uniform (empty plan)") {
    const auto m = polymesh::testsupport::box_model(1, 1, 1);
    const auto plan = build_refinement_plan(m, 0.25, {}, /*use_geometry=*/false);
    CHECK(plan.refine_seeds.empty());
    CHECK(plan.n_bc_seeds == 0);
    CHECK(plan.n_geometry_seeds == 0);
    CHECK(plan.seed_band == 0.0);
}

TEST_CASE("refinement plan: a BC/load box seeds the selected face") {
    const auto m = polymesh::testsupport::box_model(1, 1, 1);
    const double h = 0.25;
    std::vector<RefineRegion> regions{
        {Eigen::Vector3d(0.9, -1, -1), Eigen::Vector3d(2, 2, 2), 0.25}};
    const auto plan = build_refinement_plan(m, h, regions, /*use_geometry=*/false);

    CHECK(plan.n_bc_seeds > 0);
    REQUIRE_FALSE(plan.refine_seeds.empty());
    CHECK(plan.seed_band > 0.0);
    // Seeds lie on the +x face inside the selection box.
    for (const auto& s : plan.refine_seeds) {
        CHECK(s.x() >= 0.9 - 1e-9);
    }
    // Finest requested target ≈ target_fraction * h_coarse.
    CHECK(plan.h_fine == Catch::Approx(0.25 * h).margin(1e-9));
}

TEST_CASE("refinement plan: loads refine finer than fixtures") {
    const auto m = polymesh::testsupport::box_model(1, 1, 1);
    const double h = 0.25;
    const Eigen::Vector3d lo(0.9, -1, -1);
    const Eigen::Vector3d hi(2, 2, 2);
    const std::vector<RefineRegion> load{{lo, hi, 0.25}}; // finest
    const std::vector<RefineRegion> fix{{lo, hi, 0.5}};   // moderate
    const auto pl = build_refinement_plan(m, h, load, /*use_geometry=*/false);
    const auto pf = build_refinement_plan(m, h, fix, /*use_geometry=*/false);
    CHECK(pl.h_fine < pf.h_fine);
}

TEST_CASE("refinement plan: geometry sources appear on a curved CAD part") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const auto m = polymesh::pipeline::Model::load(path.string());
    // Use a COARSE bulk h so the cylinder-wall curvature target (finer than h)
    // emits geometry sources. (Auto h already resolves features globally, which
    // legitimately leaves no extra seeds — seeds add *local* refinement.)
    const double extent = (m.bbox_max - m.bbox_min).maxCoeff();
    const double h_coarse = 0.5 * extent;
    const auto plan = build_refinement_plan(m, h_coarse, {}, /*use_geometry=*/true);
    CHECK(plan.n_geometry_seeds > 0);
}
