// SPDX-License-Identifier: BSD-3-Clause
// BRep geometric-fidelity evaluator: sample summaries, exact trimmed-surface
// sampling, and the scale-free mesh-vs-BRep summary that campaign rows carry
// as `geo_fidelity` (ADR-0027 learned mesh advisor). Pinned contracts:
//   1. the ordering invariant chamfer_mean <= dist_p95 <= dist_max,
//   2. an actual accuracy claim on a flat-faced part (a box mesh interpolates
//      planes exactly, so the normalized deviation must be tiny),
//   3. monotone improvement under refinement on a curved part, which is the
//      whole reason the advisor gets a geometry target at all,
//   4. the bounded sample budget that makes the metric affordable per run.

#include "fea/boundary_faces.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
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
    const auto coarse = summary_for(kSphere, 0.08);
    const auto fine = summary_for(kSphere, 0.06);
    REQUIRE(coarse.available);
    REQUIRE(fine.available);

    // The distance tail is not strictly monotone under refinement because LEB
    // changes which child-face centroids are sampled, so use the aggregate
    // Chamfer signal that the advisor actually consumes.
    CHECK(coarse.dist_p99 > 0.0);
    CHECK(fine.dist_p99 > 0.0);
    CHECK(fine.chamfer_mean < coarse.chamfer_mean);
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
    const auto budgeted = summary_for(kSphere, 0.08, kTightBudget);
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
    // budget changes by an order of magnitude (a per-source cap would not hold:
    // the three sources saturate at different mesh sizes and shift the mean).
    const auto generous = summary_for(kSphere, 0.08, 16 * kTightBudget);
    REQUIRE(generous.available);
    CHECK(generous.n_samples > budgeted.n_samples);
    CHECK(budgeted.chamfer_mean == Catch::Approx(generous.chamfer_mean).epsilon(0.35));
}

TEST_CASE("BRep fidelity sample summaries filter and normalize deterministically") {
    const std::vector<double> samples{
        0.0,
        1.0,
        2.0,
        3.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };
    const auto summary = polymesh::mesh::summarize_samples(samples);
    REQUIRE(summary.count == 4);
    CHECK(summary.rms == Catch::Approx(std::sqrt(3.5)));
    CHECK(summary.p95 == Catch::Approx(2.85));
    CHECK(summary.p99 == Catch::Approx(2.97));
    CHECK(summary.max == Catch::Approx(3.0));

    const std::vector<double> distances{
        0.0,
        1.0,
        2.0,
        3.0,
        -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };
    const auto normalized = polymesh::mesh::summarize_distances(distances, 2.0, 4.0);
    REQUIRE(normalized.metres.count == 4);
    CHECK(normalized.metres.p95 == Catch::Approx(2.85));
    REQUIRE(normalized.over_h.count == 4);
    CHECK(normalized.over_h.rms == Catch::Approx(std::sqrt(3.5) / 2.0));
    CHECK(normalized.over_h.p95 == Catch::Approx(1.425));
    CHECK(normalized.over_h.p99 == Catch::Approx(1.485));
    CHECK(normalized.over_h.max == Catch::Approx(1.5));
    REQUIRE(normalized.over_bbox_diagonal.count == 4);
    CHECK(normalized.over_bbox_diagonal.p95 == Catch::Approx(0.7125));
    CHECK(normalized.over_bbox_diagonal.p99 == Catch::Approx(0.7425));
    CHECK(normalized.over_bbox_diagonal.max == Catch::Approx(0.75));

    const auto invalid_scales = polymesh::mesh::summarize_distances(
        distances, 0.0, std::numeric_limits<double>::infinity());
    CHECK(invalid_scales.metres.count == 4);
    CHECK(invalid_scales.over_h.count == 0);
    CHECK(invalid_scales.over_bbox_diagonal.count == 0);

    const polymesh::geom::CadModel empty;
    CHECK_FALSE(polymesh::geom::inspect_brep(empty).available);
    const auto unavailable =
        polymesh::mesh::evaluate_brep_geometry_fidelity(empty, {}, {}, {}, 1.0, 0.0);
    CHECK_FALSE(unavailable.available);

    CHECK(unavailable.mesh_boundary_samples_to_brep_surface.metres.count == 0);
}

TEST_CASE("exact trimmed BRep surface sampling obeys a hard budget") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    const auto cad = polymesh::geom::CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto inspection = polymesh::geom::inspect_brep(cad);
    REQUIRE(inspection.face_count > 1);
    CHECK_THROWS_AS(polymesh::geom::sample_brep_surface(cad, inspection.face_count - 1),
                    polymesh::geom::GeomError);

    const auto coverage = polymesh::geom::sample_brep_surface(cad, inspection.face_count);
    REQUIRE(coverage.face_count == inspection.face_count);
    REQUIRE(coverage.points.size() == inspection.face_count);
    REQUIRE(coverage.uv_attempt_count <= 9 * inspection.face_count);
    REQUIRE(coverage.fallback_vertex_count <= inspection.face_count);
    std::set<std::uint32_t> owning_faces;
    for (const Eigen::Vector3d& point : coverage.points) {
        const auto projected = polymesh::geom::project_point_on_surface(cad, point);
        REQUIRE(projected);
        CHECK(projected->distance < 1e-10);
        owning_faces.insert(projected->face_id);
    }
    CHECK(owning_faces.size() == inspection.face_count);

    constexpr std::size_t kBudget = 128;
    const auto first = polymesh::geom::sample_brep_surface(cad, kBudget);
    const auto second = polymesh::geom::sample_brep_surface(cad, kBudget);
    REQUIRE(first.points.size() >= inspection.face_count);
    REQUIRE(first.points.size() <= kBudget);
    REQUIRE(first.uv_attempt_count <= 9 * kBudget);
    REQUIRE(first.fallback_vertex_count <= inspection.face_count);
    REQUIRE(second.points.size() == first.points.size());
    bool deterministic = first.uv_attempt_count == second.uv_attempt_count &&
                         first.fallback_vertex_count == second.fallback_vertex_count;
    bool all_on_exact_brep = true;
    for (std::size_t i = 0; i < first.points.size(); ++i) {
        deterministic = deterministic && first.points[i].isApprox(second.points[i], 0.0);
        const auto projected = polymesh::geom::project_point_on_surface(cad, first.points[i]);
        all_on_exact_brep =
            all_on_exact_brep && projected.has_value() && projected->distance < 1e-10;
    }
    CHECK(deterministic);
    CHECK(all_on_exact_brep);
}
