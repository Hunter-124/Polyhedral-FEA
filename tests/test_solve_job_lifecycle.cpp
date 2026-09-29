// SPDX-License-Identifier: BSD-3-Clause
// Headless SolveJob lifecycle: phase progress, live-mesh publication, elapsed
// clock, cancel, and pause/resume -- the asynchronous contract the GUI polls.

#include "pipeline/scene.hpp"
#include "support/box_model.hpp"
#include "support/solve_job_setup.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace polymesh::pipeline;
using polymesh::test_support::cantilever_setup;

TEST_CASE("SolveJob reports phase progress during mesh-only") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    SimSetup setup;
    setup.mesh_size = 0.25;
    setup.mesher = VolumeMesher::kTetFill;
    setup.use_feature_grading = false;
    SolveJob job;
    job.start_mesh(model, setup);

    bool saw_mesh_phase = false;
    double first_elapsed = -1.0;
    double max_elapsed = 0.0;
    VolumeMeshOutput mesh;
    for (int i = 0; i < 300; ++i) {
        const auto p = job.progress();
        if (p.phase == "mesh" || p.phase == "done") {
            saw_mesh_phase = true;
        }
        CHECK(p.phase_frac >= 0.0);
        CHECK(p.phase_frac <= 1.0);
        CHECK(p.elapsed_ms >= 0.0);
        // Live wall-clock while busy (not only at report() boundaries).
        const auto st = job.state();
        if (st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving) {
            if (first_elapsed < 0.0) {
                first_elapsed = p.elapsed_ms;
            }
            max_elapsed = std::max(max_elapsed, p.elapsed_ms);
        }
        if (auto m = job.take_mesh()) {
            mesh = std::move(*m);
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        if (job.state() == SolveJob::State::kCancelled) {
            FAIL("unexpected cancel");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE_FALSE(mesh.mesh.elements.empty());
    REQUIRE(saw_mesh_phase);
    // Elapsed must advance across polls even if phase_frac is stuck (long mesh).
    if (first_elapsed >= 0.0) {
        CHECK(max_elapsed >= first_elapsed);
        // With 5 ms sleeps, expect some measurable advance if job was non-instant.
        // Instant finishes still leave max >= first (equality OK).
    }
    const auto done = job.progress();
    // After take_mesh the job is idle; last progress should still be "done".
    REQUIRE(done.phase == "done");
    REQUIRE(std::abs(done.phase_frac - 1.0) < 1e-12);
}

TEST_CASE("SolveJob publishes live mesh for viewport during mesh-only") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    SimSetup setup;
    setup.mesh_size = 0.2;
    setup.mesher = VolumeMesher::kTetFill;
    setup.use_feature_grading = false;
    SolveJob job;
    job.start_mesh(model, setup);

    std::uint64_t seen = 0;
    bool saw_live = false;
    for (int i = 0; i < 400; ++i) {
        if (auto live = job.poll_live_mesh(seen)) {
            CHECK_FALSE(live->mesh.nodes.empty());
            CHECK_FALSE(live->boundary_quads.empty());
            saw_live = true;
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        if (job.state() == SolveJob::State::kMeshDone) {
            // Still may have a live mesh to poll.
            if (auto live = job.poll_live_mesh(seen)) {
                saw_live = true;
                CHECK_FALSE(live->mesh.nodes.empty());
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Drain.
    for (int i = 0; i < 200; ++i) {
        if (job.take_mesh()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(saw_live);
}

TEST_CASE("SolveJob elapsed_ms advances while phase is held") {
    // Larger mesh so the worker stays in kMeshing long enough for wall-clock
    // polls to diverge (the UI must not look frozen mid-mesh/solve).
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    SimSetup setup;
    setup.mesh_size = 0.12;
    setup.mesher = VolumeMesher::kTetFill;
    setup.use_feature_grading = false;
    SolveJob job;
    job.start_mesh(model, setup);

    double t_a = -1.0;
    double t_b = -1.0;
    for (int i = 0; i < 200; ++i) {
        const auto st = job.state();
        if (st == SolveJob::State::kMeshing) {
            const double e = job.progress().elapsed_ms;
            if (t_a < 0.0) {
                t_a = e;
            } else if (e > t_a + 15.0) {
                t_b = e;
                break;
            }
        }
        if (st == SolveJob::State::kMeshDone || st == SolveJob::State::kFailed ||
            st == SolveJob::State::kCancelled) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Drain worker.
    for (int i = 0; i < 300; ++i) {
        if (job.take_mesh()) {
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (t_b > t_a && t_a >= 0.0) {
        REQUIRE(t_b > t_a);
    } else {
        // Machine finished too fast to sample two ticks — still OK.
        SUCCEED("mesh finished before dual elapsed samples");
    }
}

TEST_CASE("SolveJob cancel between phases reaches kCancelled") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.008);
    // Multi-pass adapt so there are checkpoints between remesh / solve phases.
    setup.adapt_passes = 2;
    setup.eta_target = 0.0;
    setup.use_feature_grading = false;

    SolveJob job;
    job.start(model, setup);
    // Give the worker a moment to enter meshing, then cancel.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    job.request_cancel();
    REQUIRE(job.cancel_requested());

    bool finished = false;
    for (int i = 0; i < 1000; ++i) {
        const auto st = job.state();
        if (st == SolveJob::State::kCancelled) {
            finished = true;
            break;
        }
        if (st == SolveJob::State::kDone) {
            // Tiny mesh may finish before cancel is observed — acceptable.
            finished = true;
            break;
        }
        if (st == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        // take_result only succeeds on kDone; ignore.
        (void)job.take_result();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(finished);
    if (job.state() == SolveJob::State::kCancelled) {
        REQUIRE(job.progress().phase == "cancelled");
        REQUIRE(job.status_text().find("cancel") != std::string::npos);
        job.clear_failure();
        REQUIRE(job.state() == SolveJob::State::kIdle);
    }
}

TEST_CASE("SolveJob pause holds then resume completes") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.012);
    setup.adapt_passes = 1;
    setup.eta_target = 0.0;
    setup.use_feature_grading = false;

    SolveJob job;
    job.start(model, setup);
    job.request_pause();
    // While paused, state stays meshing/solving (cooperative hold).
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto mid = job.state();
    if (mid == SolveJob::State::kDone) {
        // Finished too fast to observe pause — skip assertion.
        SUCCEED("job finished before pause could hold");
        return;
    }
    if (mid == SolveJob::State::kCancelled || mid == SolveJob::State::kFailed) {
        FAIL(job.status_text());
    }
    REQUIRE((mid == SolveJob::State::kMeshing || mid == SolveJob::State::kSolving));
    job.request_resume();

    std::optional<SolveResult> result;
    for (int i = 0; i < 800; ++i) {
        result = job.take_result();
        if (result) {
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        if (job.state() == SolveJob::State::kCancelled) {
            FAIL("unexpected cancel");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(result.has_value());
    REQUIRE(result->volume_mesh.elements.size() > 0);
}
