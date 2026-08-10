// SPDX-License-Identifier: BSD-3-Clause
// Advisor geometric-fidelity metric: the scale-free mesh-vs-BRep summary that
// campaign rows carry as `geo_fidelity` (ADR-0027 learned mesh advisor).
//
// The evaluator itself is covered by test_geometry_fidelity.cpp. What is
// regression-tested here is the advisor contract on top of it:
//   1. the ordering invariant chamfer_mean <= dist_p95 <= dist_max,
//   2. an actual accuracy claim on a flat-faced part (a box mesh interpolates
//      planes exactly, so the normalized deviation must be tiny),
//   3. monotone improvement under refinement on a curved part, which is the
//      whole reason the advisor gets a geometry target at all,
//   4. the bounded sample budget that makes the metric affordable per run.

#include "fea/boundary_faces.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

namespace {

constexpr char kUnitBox[] = "bench/geometries/public/unit_box.step";
constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";

polymesh::mesh::BrepFidelitySummary
summary_for(const char* path, double h_rel,
            std::size_t max_samples = polymesh::mesh::kCampaignFidelitySamples) {
    const auto model = polymesh::pipeline::Model::load(path);
    REQUIRE(model.cad);
    const double diag = (model.bbox_max - model.bbox_min).norm();
    const double h = h_rel * diag;
    const auto vol = polymesh::pipeline::volume_mesh(
        model, h, polymesh::pipeline::VolumeMesher::kGradedTet);
    REQUIRE_FALSE(vol.mesh.nodes.empty());
    const auto quads = polymesh::fea::extract_boundary_faces(vol.mesh);
    const std::vector<polymesh::mesh::FreeFace> faces(quads.begin(), quads.end());
    return polymesh::mesh::brep_fidelity_summary(*model.cad, vol.mesh.nodes, faces, h,
                                                 max_samples);
}

} // namespace

TEST_CASE("advisor fidelity summary is unavailable without a BRep", "[cad][fidelity]") {
    const polymesh::geom::CadModel empty;
    const auto summary = polymesh::mesh::brep_fidelity_summary(empty, {}, {}, 1.0);
    CHECK_FALSE(summary.available);
    CHECK(summary.n_samples == 0);
    CHECK(summary.chamfer_mean == 0.0);

    // The two ways to reach "no report" are distinct and both must return an
    // unavailable summary rather than a fabricated zero-distance one: no
    // boundary faces (short-circuits before the evaluator), and a live call
    // into an empty BRep (reaches inspect_brep's unavailable branch).
    if (polymesh::geom::occ_enabled() && std::filesystem::exists(kUnitBox)) {
        const auto cad = polymesh::geom::CadModel::load_step(kUnitBox);
        const auto no_faces = polymesh::mesh::brep_fidelity_summary(cad, {}, {}, 0.1);
        CHECK_FALSE(no_faces.available);
        CHECK(no_faces.n_samples == 0);
    }

    // A non-empty face list against an empty model: only this reaches
    // `!inspect_brep(...).available` inside evaluate_brep_geometry_fidelity.
    const std::vector<Eigen::Vector3d> unit_tri_nodes{
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    const std::vector<polymesh::mesh::FreeFace> one_face{{0, 1, 2, 2}};
    const auto no_brep =
        polymesh::mesh::brep_fidelity_summary(empty, unit_tri_nodes, one_face, 0.1);
    CHECK_FALSE(no_brep.available);
    CHECK(no_brep.n_samples == 0);
    CHECK(no_brep.dist_max == 0.0);
}

TEST_CASE("planar part meshed at h_rel=0.1 sits on its BRep", "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto summary = summary_for(kUnitBox, 0.1);
    REQUIRE(summary.available);
    REQUIRE(summary.n_samples > 0);

    // Every face of a box is planar, so a conforming tet mesh reproduces the
    // surface up to sampling: the worst normalized deviation stays well under
    // 5% of the bbox diagonal.
    CHECK(summary.dist_max < 0.05);
    // Ordering that actually holds. Note chamfer_mean is NOT bounded by
    // dist_p95: boundary nodes are projected onto the BRep, so most samples are
    // exactly zero and p95 collapses while the mean is carried by the tail.
    CHECK(summary.dist_p95 <= summary.dist_p99);
    CHECK(summary.dist_p99 <= summary.dist_max);
    CHECK(summary.chamfer_mean <= summary.dist_max);
    CHECK(summary.chamfer_mean > 0.0);
    // A box's boundary facets are coplanar with the planar B-rep faces they sit
    // on, so the unoriented normal deviation must be small. `>= 0` would be an
    // identity (acos of a clamped |dot| is always in [0, pi/2]) and could not
    // catch a wrong normal, a flipped winding, or a mis-projected centroid.
    CHECK(summary.normal_angle_p95_rad < 0.2);
}

TEST_CASE("curved part fidelity improves as h halves", "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kSphere)) {
        SKIP("sphere.step missing");
    }
    const auto coarse = summary_for(kSphere, 0.20);
    const auto fine = summary_for(kSphere, 0.10);
    REQUIRE(coarse.available);
    REQUIRE(fine.available);

    // A faceted sphere always deviates from the exact surface, and halving h
    // must reduce that deviation — this is the signal the advisor's geometry
    // heads have to learn. p99, not p95: p95 is ~0 on any conforming mesh.
    CHECK(coarse.dist_p99 > 0.0);
    CHECK(fine.dist_p99 > 0.0);
    CHECK(fine.chamfer_mean < coarse.chamfer_mean);
    CHECK(fine.dist_p99 < coarse.dist_p99);
}

TEST_CASE("fidelity sample budget is honoured and still tracks the metric",
          "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kSphere)) {
        SKIP("sphere.step missing");
    }
    constexpr std::size_t kTightBudget = 64;
    const auto budgeted = summary_for(kSphere, 0.10, kTightBudget);
    REQUIRE(budgeted.available);

    // One shared stride over three mesh-to-BRep sources bounds that direction
    // by 3*(cap + 1); the BRep-to-mesh direction adds at most `cap` more.
    CHECK(budgeted.n_samples <= 3 * (kTightBudget + 1) + kTightBudget);
    CHECK(budgeted.n_samples > 0);
    CHECK(budgeted.chamfer_mean > 0.0);
    CHECK(budgeted.dist_p95 <= budgeted.dist_p99);
    CHECK(budgeted.dist_p99 <= budgeted.dist_max);

    // The whole point of one shared stride is that the cap changes the sample
    // density, not the sample MIX, so the estimate must not move much when the
    // budget changes by an order of magnitude. A per-source cap failed this:
    // the three sources saturate at different mesh sizes and shift the mean.
    const auto generous = summary_for(kSphere, 0.10, 16 * kTightBudget);
    REQUIRE(generous.available);
    CHECK(generous.n_samples > budgeted.n_samples);
    CHECK(budgeted.chamfer_mean ==
          Catch::Approx(generous.chamfer_mean).epsilon(0.35));
}
