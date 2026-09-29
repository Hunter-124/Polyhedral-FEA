// SPDX-License-Identifier: BSD-3-Clause
// Authoritative curved geometry through the pipeline: quadratic boundary mids
// projected onto the exact BRep, and the SolveJob solve / mesh-only / auto-h
// paths that must all ship the same curved (Tet10) geometry.

#include "fea/nodal_mesh.hpp"
#include "fea/p_elevate.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/surface_project.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr char kPlateHole[] = "tests/fixtures/parts/plate_hole.step";
constexpr char kLBracket[] = "bench/geometries/corpus/primitives/l_bracket_s0.step";

std::vector<std::uint32_t> boundary_quadratic_mids(const polymesh::fea::NodalMesh& mesh) {
    std::set<std::uint32_t> mids;
    for (const auto& face : polymesh::fea::boundary_surface_faces(mesh)) {
        const std::size_t n_corners = (face.type == polymesh::fea::FaceType::kTri6) ? 3
                                      : (face.type == polymesh::fea::FaceType::kQuad8)
                                          ? 4
                                          : face.nodes.size();
        mids.insert(face.nodes.begin() + static_cast<std::ptrdiff_t>(n_corners),
                    face.nodes.end());
    }
    return {mids.begin(), mids.end()};
}

} // namespace

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
        const auto vol = polymesh::pipeline::volume_mesh(
            model, h, polymesh::pipeline::VolumeMesher::kHybrid,
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
        // NOT `pre_outliers > 0` per resolution: the exterior conformity gate
        // (ADR-0035) lands the linear boundary on the exact BRep, so a chord
        // midpoint on a planar or ruled patch is already exact. The guard that
        // the projection pass is exercised is taken over the whole sweep, below.
        total_pre_outliers += pre_outliers;

        std::vector<polymesh::mesh::BoundarySupport> provenance;
        polymesh::mesh::BoundaryProjectionContext projection;
        REQUIRE(polymesh::pipeline::make_boundary_projection(*model.cad, h, &projection,
                                                             &provenance));
        std::vector<std::uint32_t> reverted;
        std::vector<std::uint32_t> partial;
        const std::size_t projected = polymesh::pipeline::project_quadratic_boundary_mids(
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
        const std::size_t limited_limit = std::max<std::size_t>(8, (mids.size() + 99) / 100);
        CAPTURE(h_rel, mids.size(), pre_outliers, post_outliers.size(), projected,
                partial.size(), reverted.size(), limited_limit, post_max, full_max,
                partial_max, reverted_max);
        CHECK(limited_count <= limited_limit);
        CHECK(std::includes(limited_set.begin(), limited_set.end(), post_outliers.begin(),
                            post_outliers.end()));
    }
    // Somewhere in the sweep the chord midpoints must actually miss the BRep,
    // otherwise this test would pass without the projection pass doing anything
    // (outliers occur at h_rel = 0.10).
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
    // Wait on the job's own state, not on a wall-clock poll budget: this solve
    // runs for minutes under parallel ctest load. `kSolving`/`kMeshing` are the
    // only non-terminal states; a hung worker is caught by ctest's per-test timeout.
    while (job.state() == polymesh::pipeline::SolveJob::State::kSolving ||
           job.state() == polymesh::pipeline::SolveJob::State::kMeshing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (job.state() == polymesh::pipeline::SolveJob::State::kFailed) {
        FAIL(job.status_text());
    }
    if (job.state() == polymesh::pipeline::SolveJob::State::kCancelled) {
        FAIL("graded authoritative curvature was cancelled");
    }
    result = job.take_result();
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
// geometry the solve would use, never a linear preview beside a curved solve.
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
    // State-driven wait, for the same reason as the solve above.
    while (job.state() == polymesh::pipeline::SolveJob::State::kMeshing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (job.state() == polymesh::pipeline::SolveJob::State::kFailed) {
        FAIL(job.status_text());
    }
    const auto preview = job.take_mesh();
    REQUIRE(preview);
    const auto counts = polymesh::fea::count_element_types(preview->mesh);
    CHECK(counts.tet4 == 0);
    CHECK(counts.tet10 > 0);
    CHECK(preview->mesher_note.find("curved_volume promoted=") != std::string::npos);
}

// Auto sizing has to price curved geometry in, or "mesh only" on a rounded part
// silently ships ~8× the cells and ~12× the DOF the interactive ceiling allows.
TEST_CASE("brep_fidelity: auto h keeps curved geometry inside the DOF ceiling",
          "[cad][fidelity][regression]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    const std::filesystem::path sphere = "tests/fixtures/parts/sphere.step";
    if (!std::filesystem::exists(sphere)) {
        SKIP("sphere.step missing");
    }

    const auto model = polymesh::pipeline::Model::load(sphere.string());
    REQUIRE(model.cad);
    polymesh::pipeline::SimSetup setup;
    setup.mesh_size = 0.0; // auto
    setup.mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    setup.use_feature_grading = true;
    setup.p_elevate = true;
    setup.max_elems = 120'000;
    setup.max_dof = 300'000;

    polymesh::pipeline::SolveJob job;
    job.start_mesh(model, setup);
    while (job.state() == polymesh::pipeline::SolveJob::State::kMeshing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (job.state() == polymesh::pipeline::SolveJob::State::kFailed) {
        FAIL(job.status_text());
    }
    const auto preview = job.take_mesh();
    REQUIRE(preview);
    const auto counts = polymesh::fea::count_element_types(preview->mesh);
    CHECK(counts.tet10 > 0);
    CHECK(3 * preview->mesh.nodes.size() <= setup.max_dof);
    CHECK(preview->mesh.elements.size() <= setup.max_elems);
}
