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
#include "fea/element_validity.hpp"
#include "fea/p_elevate.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr char kUnitBox[] = "bench/geometries/public/unit_box.step";
constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";
constexpr char kPlateHole[] = "tests/fixtures/parts/plate_hole.step";
constexpr char kLBracket[] = "bench/geometries/corpus/primitives/l_bracket_s0.step";
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
using BoundaryQuad = std::array<std::uint32_t, 4>;

std::array<std::uint32_t, 4> face_key(BoundaryQuad face) {
    std::sort(face.begin(), face.end());
    return face;
}

std::vector<std::uint32_t>
nodes_on_faces(std::span<const BoundaryQuad> faces) {
    std::set<std::uint32_t> nodes;
    for (const auto& face : faces) {
        nodes.insert(face.begin(), face.end());
    }
    return {nodes.begin(), nodes.end()};
}

std::vector<std::uint32_t>
nodes_on_original_boundary(const polymesh::pipeline::VolumeMeshOutput& volume) {
    std::set<std::array<std::uint32_t, 4>> local_faces;
    std::set<std::uint32_t> local_nodes;
    for (const auto& face : volume.local_child_boundary_quads) {
        local_faces.insert(face_key(face));
        local_nodes.insert(face.begin(), face.end());
    }
    std::set<std::uint32_t> nodes;
    for (const auto& face : volume.boundary_quads) {
        if (local_faces.contains(face_key(face))) {
            continue;
        }
        for (const auto node : face) {
            if (!local_nodes.contains(node)) {
                nodes.insert(node);
            }
        }
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

TEST_CASE("geometry completeness guard rejects a synthetic aliased solid",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto cad = polymesh::geom::CadModel::load_step(kUnitBox);
    const auto inspection = polymesh::geom::inspect_brep(cad);
    REQUIRE(inspection.available);
    REQUIRE(inspection.volume > 0.0);

    const auto exact =
        polymesh::mesh::evaluate_geometry_completeness(cad, inspection.volume);
    REQUIRE(exact.available);
    CHECK(exact.complete);
    CHECK(exact.relative_volume_error == Catch::Approx(0.0));

    const double aliased_volume =
        inspection.volume *
        (1.0 + 2.0 * polymesh::mesh::kGeometryCompletenessRelVolumeTolerance);
    const auto aliased =
        polymesh::mesh::evaluate_geometry_completeness(cad, aliased_volume);
    REQUIRE(aliased.available);
    CHECK_FALSE(aliased.complete);
    CHECK(aliased.relative_volume_error >
          aliased.relative_volume_tolerance);
}

TEST_CASE("solved geometry volume integrates quadratic boundary mids",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kUnitBox);
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0},
        {0.5, 0.0, 0.0}, {0.5, 0.5, 0.0}, {0.0, 0.5, 0.0}, {0.0, 0.0, 0.5},
        {0.5, 0.0, 0.5}, {0.0, 0.5, 0.5},
    };
    mesh.elements.push_back({polymesh::fea::ElementType::kTet10,
                             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}});

    const auto straight = polymesh::pipeline::measure_geometry_volume(model, mesh);
    REQUIRE(straight.available);
    CHECK(straight.mesh_volume == Catch::Approx(1.0 / 6.0).epsilon(1e-12));

    // Corner-only surface volume is unchanged, but the actual Tet10 geometry
    // contracts when the three mids of the face opposite node 0 move inward.
    mesh.nodes[5].z() = 0.1;
    mesh.nodes[8].y() = 0.1;
    mesh.nodes[9].x() = 0.1;
    const auto curved = polymesh::pipeline::measure_geometry_volume(model, mesh);
    REQUIRE(curved.available);
    CHECK(std::abs(curved.mesh_volume - straight.mesh_volume) > 1e-4);
}

TEST_CASE("geometry volume policy retains degraded meshes and rejects egregious loss",
          "[cad][geometry-completeness]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kUnitBox);
    REQUIRE(model.cad);
    const double cad_volume = polymesh::geom::inspect_brep(*model.cad).volume;
    REQUIRE(cad_volume > 0.0);
    const auto output_with_volume = [&](double fraction) {
        polymesh::pipeline::VolumeMeshOutput output;
        output.mesh.nodes = {{0.0, 0.0, 0.0},
                             {1.0, 0.0, 0.0},
                             {0.0, 1.0, 0.0},
                             {0.0, 0.0, 6.0 * fraction * cad_volume}};
        output.mesh.elements.push_back(
            {polymesh::fea::ElementType::kTet4, {0, 1, 2, 3}});
        return output;
    };

    auto degraded = output_with_volume(0.95);
    CHECK_NOTHROW(polymesh::pipeline::update_solved_geometry_volume(model, degraded));
    CHECK(degraded.solved_geometry_volume.relative_error == Catch::Approx(0.05));
    CHECK(degraded.mesher_note.find("band=degraded") != std::string::npos);

    auto egregious = output_with_volume(0.80);
    CHECK_THROWS_AS(polymesh::pipeline::update_solved_geometry_volume(model, egregious),
                    polymesh::pipeline::GeometryVolumeLimitError);
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

    // The delivered volume error falls from 4.06% to 3.08% over this valid
    // refinement pair. The distance tail is not strictly monotone because LEB
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
    // budget changes by an order of magnitude. A per-source cap failed this:
    // the three sources saturate at different mesh sizes and shift the mean.
    const auto generous = summary_for(kSphere, 0.08, 16 * kTightBudget);
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
    std::size_t total_pre_outliers = 0;
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
        // NOT `pre_outliers > 0` per resolution. Since the exterior conformity
        // gate (ADR-0035) lands the linear boundary on the exact BRep, a chord
        // midpoint between two exact nodes on a planar or ruled patch is exact
        // too: plate_hole at h_rel = 0.12 measures a chord max of 5.2e-16·h,
        // with zero outliers, and demanding a defective fixture there would be
        // demanding that the mesher be worse. The guard that this test still
        // exercises the projection pass is therefore taken over the whole
        // resolution sweep, below.
        total_pre_outliers += pre_outliers;

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
        CHECK(std::includes(limited_set.begin(), limited_set.end(), post_outliers.begin(),
                            post_outliers.end()));
    }
    // Somewhere in the sweep the chord midpoints must actually miss the BRep,
    // otherwise this test would pass without the projection pass doing anything
    // (measured: 20 outliers of 960 mids at h_rel = 0.10).
    CHECK(total_pre_outliers > 0);
}

TEST_CASE("brep_fidelity: graded authoritative curvature stays stiffness-valid",
          "[cad][fidelity][regression]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kLBracket)) {
        SKIP("l_bracket_s0.step missing");
    }

    const auto model = polymesh::pipeline::Model::load(kLBracket);
    REQUIRE(model.cad);
    polymesh::pipeline::SimSetup setup;
    setup.mesh_size = 0.002802519264087257;
    setup.mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    setup.skin_layers = 2;
    setup.use_feature_grading = true;
    setup.bc_grading = true;
    setup.p_elevate = true;
    setup.max_elems = 60'000;
    setup.max_dof = 200'000;

    const double diag = (model.bbox_max - model.bbox_min).norm();
    for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
        const auto& tri = model.surface.triangles[ti];
        const Eigen::Vector3d centroid =
            (model.surface.vertices[tri[0]] + model.surface.vertices[tri[1]] +
             model.surface.vertices[tri[2]]) /
            3.0;
        const int region = model.triangle_region[ti];
        if (centroid.z() >= model.bbox_max.z() - 0.01 * diag) {
            setup.fixtures.insert(region);
        }
        if (centroid.x() >= model.bbox_max.x() - 0.01 * diag) {
            setup.loads[region].force = Eigen::Vector3d{1'000.0, 0.0, 0.0};
        }
    }
    REQUIRE_FALSE(setup.fixtures.empty());
    REQUIRE_FALSE(setup.loads.empty());

    polymesh::pipeline::SolveJob job;
    job.start(model, setup);
    std::optional<polymesh::pipeline::SolveResult> result;
    for (int poll = 0; poll < 12'000; ++poll) {
        result = job.take_result();
        if (result) {
            break;
        }
        if (job.state() == polymesh::pipeline::SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        if (job.state() == polymesh::pipeline::SolveJob::State::kCancelled) {
            FAIL("graded authoritative curvature was cancelled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(result);
    const auto counts = polymesh::fea::count_element_types(result->volume_mesh);
    CHECK(counts.tet10 > 0);
    CHECK(counts.tet4 == 0);
    CHECK(result->displacement.size() ==
          3 * static_cast<Eigen::Index>(result->volume_mesh.nodes.size()));
    CHECK(result->mesh_note.find("curved-volume=") != std::string::npos);
}

// Studio's "mesh only" button calls SolveJob::start_mesh, which is a different
// entry point from start(). It must hand back the same authoritative curved
// geometry the solve would use — a linear preview beside a curved solve is what
// made the old display-only pass so misleading.
TEST_CASE("brep_fidelity: mesh-only preview is authoritative curved geometry",
          "[cad][fidelity][regression]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kLBracket)) {
        SKIP("l_bracket_s0.step missing");
    }

    const auto model = polymesh::pipeline::Model::load(kLBracket);
    REQUIRE(model.cad);
    polymesh::pipeline::SimSetup setup;
    setup.mesh_size = 0.006;
    setup.mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    setup.use_feature_grading = true;
    setup.p_elevate = true;
    setup.max_elems = 400'000;
    setup.max_dof = 1'500'000;

    polymesh::pipeline::SolveJob job;
    job.start_mesh(model, setup);
    std::optional<polymesh::pipeline::VolumeMeshOutput> preview;
    for (int poll = 0; poll < 12'000; ++poll) {
        preview = job.take_mesh();
        if (preview) {
            break;
        }
        if (job.state() == polymesh::pipeline::SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(preview);
    const auto counts = polymesh::fea::count_element_types(preview->mesh);
    CHECK(counts.tet4 == 0);
    CHECK(counts.tet10 > 0);
    CHECK(preview->mesher_note.find("curved_volume promoted=") != std::string::npos);
}

// The local h/2 classifier creates live/void child faces that the old
// parent-face-only boundary list did not send through snapping. Provenance made
// the pre-fix split explicit (worst sampled valid h, p99/max, normalized by h):
//   cantilever       original 0/0              local none
//   cylinder .12     original 6e-17/1.5e-15    local .1032/.1032
//   icecream .12     original 1.9e-15/2.0e-15  local .3503/.3551
//   pipe .20         original 0/0              local none
//   plate_hole .12   original 0/0              local .1862/.1862
//   smoke_bar        original 0/0              local none
//   sphere .12       original 9.0e-16/1.2e-15  local .3160/.3160
// Sphere and icecream have no bore: their "local" faces are new outer curved
// live/void interfaces inside mixed parents. Once every such face enters the
// same snap/projection path, the worst local p99/max is .0273/.0273 (icecream)
// and the worst original p99/max remains 1.6e-15/2.0e-15. Keep those surfaces
// on their old 0.10/0.25 rails and independently cap the harder local subset.
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
            std::optional<polymesh::pipeline::VolumeMeshOutput> maybe_vol;
            try {
                maybe_vol = polymesh::pipeline::volume_mesh(
                    model, h, polymesh::pipeline::VolumeMesher::kHybrid,
                    /*skin_layers=*/2, /*feature_refine=*/true);
            } catch (const polymesh::pipeline::GeometryVolumeLimitError& e) {
                CAPTURE(part.name, h_rel, e.what());
                CHECK_FALSE(e.solved_stage);
                CHECK(e.assessment.available);
                const bool expected_guard =
                    std::string(e.what()).find("feature unresolved") !=
                        std::string::npos ||
                    e.assessment.relative_error >
                        polymesh::pipeline::kGeometryVolumeHardLimit;
                CHECK(expected_guard);
                continue;
            }
            const auto& vol = *maybe_vol;
            const auto nodes = boundary_nodes(vol.mesh);
            const auto original_nodes = nodes_on_original_boundary(vol);
            const auto local_nodes = nodes_on_faces(vol.local_child_boundary_quads);
            REQUIRE_FALSE(nodes.empty());
            REQUIRE_FALSE(original_nodes.empty());
            const auto residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, nodes, h);
            const auto original_residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, original_nodes, h);
            const auto local_residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, local_nodes, h);
            REQUIRE(residual.count == nodes.size());
            REQUIRE(original_residual.count == original_nodes.size());
            REQUIRE(local_residual.count == local_nodes.size());
            CHECK(local_residual.max <= 0.06);
            CHECK(local_residual.p99 <= 0.06);
            CAPTURE(part.name, h_rel, residual.max, residual.p99,
                    original_residual.max, original_residual.p99,
                    local_residual.max, local_residual.p99, vol.mesher_note);
            CHECK(original_residual.max <= 0.25);
            CHECK(original_residual.p99 <= 0.10);
            CHECK(residual.max <= 0.25);
            CHECK(residual.p99 <= 0.10);
        }
    }
}
// --- ADR-0035: boundary nodes sit ON the exact BRep ------------------------
//
// The mesher used to project boundary nodes to the nearest point of the
// *tessellated* surface and, at a sharp edge, to the nearest point of a FACE
// rather than the edge curve. Both are structural: no amount of h removes
// them. Measured before the fix at h = 8 mm, worst boundary node over h:
// sphere graded 0.059, cylinder graded 0.180, icecream_cone graded 0.196,
// plate_hole tet 0.650 — and the varyhedron "edge attraction" moved a crease
// node only 35 % of the way onto its curve, which is a chamfer by
// construction.
//
// This test measures the one thing a mesher fully controls: where it puts a
// node. Facet centroids and edge midpoints are deliberately excluded — a
// straight facet spanning a curve always carries the chord sag h²κ/8, which is
// a discretisation property, not a placement error.
TEST_CASE("boundary nodes land on the exact BRep for every CAD mesher",
          "[cad][fidelity][feature_pin]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* name;
        const char* path;
        polymesh::pipeline::VolumeMesher mesher;
        double node_p99_over_h; // ceiling on the 99th percentile
        double node_max_over_h; // ceiling on the worst single node
    };
    // Ceilings are the measured post-fix values with headroom. The graded and
    // varyhedron paths place every node on the BRep to machine precision, so
    // their ceilings are 1e-12 — a real regression cannot hide under that.
    const Case cases[] = {
        {"sphere/graded", kSphere, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12, 1e-12},
        {"sphere/varyhedron", kSphere, polymesh::pipeline::VolumeMesher::kVaryhedron, 1e-12,
         1e-12},
        {"cylinder/graded", kCylinder, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12,
         0.02},
        {"cylinder/varyhedron", kCylinder, polymesh::pipeline::VolumeMesher::kVaryhedron,
         1e-12, 1e-12},
        {"plate_hole/graded", kPlateHole, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12,
         0.002},
        {"plate_hole/varyhedron", kPlateHole, polymesh::pipeline::VolumeMesher::kVaryhedron,
         1e-12, 0.002},
        {"icecream_cone/graded", kIcecreamCone, polymesh::pipeline::VolumeMesher::kGradedTet,
         1e-12, 0.25},
        {"icecream_cone/varyhedron", kIcecreamCone,
         polymesh::pipeline::VolumeMesher::kVaryhedron, 1e-12, 0.25},
    };
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        const auto vol = polymesh::pipeline::volume_mesh(model, kH, c.mesher);
        REQUIRE_FALSE(vol.mesh.nodes.empty());
        const auto nodes = boundary_nodes(vol.mesh);
        REQUIRE_FALSE(nodes.empty());
        const auto residual = exact_residuals_over_h(*model.cad, vol.mesh.nodes, nodes, kH);
        CAPTURE(c.name, residual.p99, residual.max, vol.mesher_note);
        CHECK(residual.p99 <= c.node_p99_over_h);
        CHECK(residual.max <= c.node_max_over_h);
    }
}

// The pinning pass is what reproduces a sharp CAD edge. Its direction of
// interest is CAD -> mesh: for every sampled point of a sharp BRep edge, how
// far is the nearest mesh feature segment? A mesh that chamfers a 90 deg edge
// fails here even when every node is on some face of the solid.
TEST_CASE("sharp BRep edges are reproduced by mesh feature segments",
          "[cad][fidelity][feature_pin]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* name;
        const char* path;
        double edge_p99_over_h;
    };
    const Case cases[] = {
        {"plate_hole", kPlateHole, 0.09},
        {"cylinder", kCylinder, 0.20},
        {"icecream_cone", kIcecreamCone, 0.10},
    };
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        // feature_refine=true is what the product CLI runs; without the
        // feature band the rim is meshed at bulk h and the reproduction of a
        // small circular edge is bounded by the lattice, not by the pin.
        const auto vol = polymesh::pipeline::volume_mesh(
            model, kH, polymesh::pipeline::VolumeMesher::kGradedTet, /*skin_layers=*/2,
            /*feature_refine=*/true);
        const auto quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        const std::vector<polymesh::mesh::FreeFace> faces(quads.begin(), quads.end());
        const auto segments = polymesh::mesh::mesh_dihedral_feature_segments(vol.mesh.nodes, faces);
        const auto fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
            *model.cad, vol.mesh.nodes, faces, segments, kH, 0.0);
        REQUIRE(fidelity.available);
        const auto& reverse = fidelity.sharp_brep_edge_samples_to_mesh_feature_segments;
        CAPTURE(c.name, reverse.over_h.p99, reverse.over_h.max, vol.mesher_note);
        if (reverse.metres.count == 0) {
            continue; // no sharp edges on this part
        }
        CHECK(reverse.over_h.p99 <= c.edge_p99_over_h);
    }
}

// The exterior conformity gate (ADR-0035). Two claims, both about the mesh that
// actually ships rather than about any mesher's own intermediate boundary set:
// every emitted element is integrable by the assembly's own rule, and the true
// element exterior — not the lattice skin the snap ran on — is on the BRep.
TEST_CASE("brep_fidelity: the shipped exterior conforms and every cell is integrable",
          "[cad][fidelity][exterior_gate]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* path;
        polymesh::pipeline::VolumeMesher mesher;
        const char* name;
        double node_p99_over_h; // ceiling on the shipped boundary NODES
    };
    // Ceilings are the measured numbers plus headroom, per part×mesher, because
    // what the lattice can reach differs: the conforming meshers land on the
    // BRep to machine precision, while the uniform Cartesian tet fill is bounded
    // by its own cell-shape floor (ADR-0035 §5) and only improves.
    const std::array<Case, 6> cases{{
        {kSphere, polymesh::pipeline::VolumeMesher::kGradedTet, "sphere/graded", 1e-12},
        {kSphere, polymesh::pipeline::VolumeMesher::kVaryhedron, "sphere/varyhedron", 1e-12},
        {kSphere, polymesh::pipeline::VolumeMesher::kHybrid, "sphere/hybrid", 0.01},
        {kIcecreamCone, polymesh::pipeline::VolumeMesher::kHybrid, "cone/hybrid", 1e-12},
        {kIcecreamCone, polymesh::pipeline::VolumeMesher::kGradedTet, "cone/graded", 1e-12},
        {kPlateHole, polymesh::pipeline::VolumeMesher::kVaryhedron, "plate_hole/varyhedron",
         1e-12},
    }};
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        const auto vol = polymesh::pipeline::volume_mesh(model, kH, c.mesher,
                                                         /*skin_layers=*/2,
                                                         /*feature_refine=*/true);
        // Integrability of what ships. This is the claim `fea::cell_quality`
        // cannot make: a cell can clear the shape floor and still have a
        // non-positive Jacobian at a quadrature point, which is exactly how an
        // earlier version of the gate shipped det J = -6.085e-09.
        std::size_t nonintegrable = 0;
        for (const auto& element : vol.mesh.elements) {
            if (!polymesh::fea::element_jacobians_positive(vol.mesh, element)) {
                ++nonintegrable;
            }
        }
        CAPTURE(c.name, vol.mesher_note);
        CHECK(nonintegrable == 0);

        const auto quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        REQUIRE_FALSE(quads.empty());
        std::set<std::uint32_t> exterior;
        for (const auto& quad : quads) {
            exterior.insert(quad.begin(), quad.end());
        }
        std::vector<double> residuals;
        residuals.reserve(exterior.size());
        for (const auto node : exterior) {
            const auto exact =
                polymesh::geom::project_point_on_surface(*model.cad, vol.mesh.nodes[node]);
            if (exact) {
                residuals.push_back(exact->distance / kH);
            }
        }
        REQUIRE_FALSE(residuals.empty());
        std::sort(residuals.begin(), residuals.end());
        const double p99 = residuals[static_cast<std::size_t>(
            0.99 * static_cast<double>(residuals.size() - 1))];
        CAPTURE(p99, residuals.back(), residuals.size());
        CHECK(p99 <= c.node_p99_over_h);
    }
}
