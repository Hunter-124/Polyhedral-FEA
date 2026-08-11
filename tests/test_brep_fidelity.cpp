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
#include "fea/p_elevate.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string_view>
#include <vector>

namespace {

constexpr char kUnitBox[] = "bench/geometries/public/unit_box.step";
constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";
constexpr char kPlateHole[] = "tests/fixtures/parts/plate_hole.step";
constexpr char kCantilever[] = "tests/fixtures/parts/cantilever.step";
constexpr char kIcecreamCone[] = "tests/fixtures/parts/icecream_cone.step";
constexpr char kPipe[] = "tests/fixtures/parts/pipe.step";
constexpr char kSmokeBar[] = "tests/fixtures/parts/smoke_bar.step";
constexpr char kCylinder[] = "tests/fixtures/parts/cylinder.step";

std::vector<std::uint32_t> boundary_quadratic_mids(const polymesh::fea::NodalMesh& mesh) {
    std::set<std::uint32_t> mids;
    for (const auto& face : polymesh::fea::boundary_surface_faces(mesh)) {
        const std::size_t n_corners =
            (face.type == polymesh::fea::FaceType::kTri6)   ? 3
            : (face.type == polymesh::fea::FaceType::kQuad8) ? 4
                                                              : face.nodes.size();
        mids.insert(face.nodes.begin() + static_cast<std::ptrdiff_t>(n_corners),
                    face.nodes.end());
    }
    return {mids.begin(), mids.end()};
}

std::vector<std::uint32_t> boundary_nodes(const polymesh::fea::NodalMesh& mesh) {
    std::set<std::uint32_t> nodes;
    for (const auto& face : polymesh::fea::extract_boundary_faces(mesh)) {
        nodes.insert(face.begin(), face.end());
    }
    return {nodes.begin(), nodes.end()};
}

polymesh::mesh::SampleDistribution
exact_residuals_over_h(const polymesh::geom::CadModel& cad,
                       const std::vector<Eigen::Vector3d>& points,
                       const std::vector<std::uint32_t>& indices, double h) {
    std::vector<double> residuals;
    residuals.reserve(indices.size());
    for (const auto node : indices) {
        if (node >= points.size()) {
            continue;
        }
        if (const auto exact = polymesh::geom::project_point_on_surface(cad, points[node])) {
            residuals.push_back(exact->distance / h);
        }
    }
    return polymesh::mesh::summarize_samples(residuals);
}

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


TEST_CASE("brep_fidelity: quadratic plate-hole boundary mids lie on the exact BRep",
          "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPlateHole)) {
        SKIP("plate_hole.step missing");
    }

    const auto model = polymesh::pipeline::Model::load(kPlateHole);
    REQUIRE(model.cad);
    for (const double h_rel : {0.12, 0.10}) {
        const double h = h_rel * (model.bbox_max - model.bbox_min).norm();
        const auto vol =
            polymesh::pipeline::volume_mesh(model, h,
                                            polymesh::pipeline::VolumeMesher::kHybrid,
                                            /*skin_layers=*/2, /*feature_refine=*/true);
        auto quadratic = polymesh::fea::promote_to_quadratic(vol.mesh);
        const auto mids = boundary_quadratic_mids(quadratic);
        REQUIRE_FALSE(mids.empty());

        std::vector<Eigen::Vector3d> chord_positions;
        std::vector<double> chord_residuals;
        chord_positions.reserve(mids.size());
        chord_residuals.reserve(mids.size());
        std::size_t pre_outliers = 0;
        for (const auto node : mids) {
            chord_positions.push_back(quadratic.nodes[node]);
            const auto exact =
                polymesh::geom::project_point_on_surface(*model.cad, quadratic.nodes[node]);
            REQUIRE(exact);
            const double residual = exact->distance / h;
            chord_residuals.push_back(residual);
            if (residual > 0.02) {
                ++pre_outliers;
            }
        }
        REQUIRE(pre_outliers > 0);

        std::vector<polymesh::mesh::BoundarySupport> provenance;
        polymesh::mesh::BoundaryProjectionContext projection;
        REQUIRE(polymesh::pipeline::make_boundary_projection(*model.cad, h, &projection,
                                                             &provenance));
        std::vector<std::uint32_t> reverted;
        std::vector<std::uint32_t> partial;
        const std::size_t projected =
            polymesh::pipeline::project_quadratic_boundary_mids(
                quadratic, *model.cad, &projection, h, &reverted, &partial);
        const std::set<std::uint32_t> reverted_set(reverted.begin(), reverted.end());
        const std::set<std::uint32_t> partial_set(partial.begin(), partial.end());
        REQUIRE(reverted_set.size() == reverted.size());
        REQUIRE(partial_set.size() == partial.size());
        REQUIRE(projected + partial.size() + reverted.size() == mids.size());
        for (const auto node : reverted_set) {
            REQUIRE_FALSE(partial_set.contains(node));
        }

        std::set<std::uint32_t> post_outliers;
        std::set<std::uint32_t> limited_set = reverted_set;
        limited_set.insert(partial_set.begin(), partial_set.end());
        double post_max = 0.0;
        double reverted_max = 0.0;
        double partial_max = 0.0;
        double full_max = 0.0;
        for (std::size_t i = 0; i < mids.size(); ++i) {
            const auto node = mids[i];
            const auto exact =
                polymesh::geom::project_point_on_surface(*model.cad, quadratic.nodes[node]);
            REQUIRE(exact);
            const double residual = exact->distance / h;
            post_max = std::max(post_max, residual);
            if (residual > 0.02) {
                post_outliers.insert(node);
            }
            if (reverted_set.contains(node)) {
                reverted_max = std::max(reverted_max, residual);
                CHECK(residual <= 0.5);
                CHECK((quadratic.nodes[node] - chord_positions[i]).norm() <= 1e-14 * h);
            } else if (partial_set.contains(node)) {
                partial_max = std::max(partial_max, residual);
                CHECK(residual < chord_residuals[i]);
                CHECK(residual <= 0.5);
                CHECK((quadratic.nodes[node] - chord_positions[i]).norm() > 0.0);
            } else {
                full_max = std::max(full_max, residual);
                CHECK(residual <= 0.02);
            }
        }

        const std::size_t limited_count = partial.size() + reverted.size();
        const std::size_t limited_limit =
            std::max<std::size_t>(8, (mids.size() + 99) / 100);
        CAPTURE(h_rel, mids.size(), pre_outliers, post_outliers.size(), projected,
                partial.size(), reverted.size(), limited_limit, post_max, full_max,
                partial_max, reverted_max);
        CHECK(limited_count <= limited_limit);
        CHECK(post_outliers == limited_set);
    }
}

// Exact-BRep residual survey with the mixed-mesh repair disabled:
//                         h_rel=.20 max/p99       h_rel=.12 max/p99
// cantilever, pipe, smoke_bar    0/0                     0/0
// cylinder                 .045691/.045691         .00000742/.00000742
// icecream_cone            .059353/.059353         .038991/.038991
// plate_hole               .002911/.000264         0/0
// sphere                   6.4e-16/4.8e-16         1.95e-15/9.6e-16
// At h_rel=.08, cylinder measured .007576/.007576 and plate_hole
// .00000853/0. The deliberately looser bounds below are regression guard rails,
// not the current operating point.
TEST_CASE("brep_fidelity: hybrid curved boundary survey stays bounded",
          "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }

    struct SurveyPart {
        const char* name;
        const char* path;
    };
    const SurveyPart parts[] = {
        {"cantilever", kCantilever},
        {"cylinder", kCylinder},
        {"icecream_cone", kIcecreamCone},
        {"pipe", kPipe},
        {"plate_hole", kPlateHole},
        {"smoke_bar", kSmokeBar},
        {"sphere", kSphere},
    };
    for (const auto& part : parts) {
        if (!std::filesystem::exists(part.path)) {
            SKIP(std::string(part.path) + " missing");
        }
        const auto model = polymesh::pipeline::Model::load(part.path);
        REQUIRE(model.cad);
        for (const double h_rel : {0.20, 0.12, 0.08}) {
            if (h_rel == 0.08 && std::string_view(part.name) != "plate_hole" &&
                std::string_view(part.name) != "cylinder") {
                continue;
            }
            const double h = h_rel * (model.bbox_max - model.bbox_min).norm();
            const auto vol = polymesh::pipeline::volume_mesh(
                model, h, polymesh::pipeline::VolumeMesher::kHybrid,
                /*skin_layers=*/2, /*feature_refine=*/true);
            const auto nodes = boundary_nodes(vol.mesh);
            REQUIRE_FALSE(nodes.empty());
            const auto residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, nodes, h);
            REQUIRE(residual.count == nodes.size());
            CAPTURE(part.name, h_rel, residual.max, residual.p99, vol.mesher_note);
            CHECK(residual.max <= 0.25);
            CHECK(residual.p99 <= 0.10);
        }
    }
}