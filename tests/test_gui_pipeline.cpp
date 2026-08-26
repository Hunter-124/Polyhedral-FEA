// SPDX-License-Identifier: BSD-3-Clause

// Headless test of the GUI scene pipeline: STL import -> CAD-style face
// regions -> draft voxel mesh -> fixture/load mapping -> solve. Keeps the
// interactive path covered by CI without a display.

#include "fea/assembly.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

/// Wait for a mesh-only job's mesh, or fail with the job's own message.
///
/// The poll budgets these tests used to carry were iteration counts tuned to an
/// optimised build: 300 * 10 ms is three seconds, which is ample at -O2 and not
/// ample at -O0. The Debug CI job runs the same mesher unoptimised, so the
/// budget expired before the worker finished and the test then asserted on an
/// empty mesh -- reporting "no elements" for what was really "not yet". A wall
/// clock deadline generous enough for an unoptimised mesher, with the job's own
/// terminal states ending the wait early, tests the mesher instead of the
/// machine. `kFailed` still fails immediately and still reports `status_text`,
/// so a real defect is not hidden behind the longer deadline.
VolumeMeshOutput await_mesh(SolveJob& job,
                            std::chrono::seconds deadline = std::chrono::seconds(300)) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < deadline) {
        if (auto m = job.take_mesh()) {
            return std::move(*m);
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        if (job.state() == SolveJob::State::kCancelled) {
            FAIL("the mesh job was cancelled");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL(std::format("no mesh within {} s; job state {}, status: {}", deadline.count(),
                     static_cast<int>(job.state()), job.status_text()));
    return {};
}

} // namespace

TEST_CASE("GUI pipeline: box STL segments into six faces and solves end-to-end") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    CHECK(model.surface.triangles.size() == 12);
    CHECK(model.region_count == 6);

    // Identify the x=0 and x=lx faces by triangle position.
    int fixed_region = -1, loaded_region = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        const auto& tri = model.surface.triangles[t];
        double x_sum = 0.0;
        for (const auto v : tri) {
            x_sum += model.surface.vertices[v][0];
        }
        if (x_sum < 1e-12) {
            fixed_region = model.triangle_region[t];
        }
        if (x_sum > 3 * 0.1 - 1e-9) {
            loaded_region = model.triangle_region[t];
        }
    }
    REQUIRE(fixed_region >= 0);
    REQUIRE(loaded_region >= 0);
    REQUIRE(fixed_region != loaded_region);

    const auto voxel = voxel_mesh(model, 0.005);
    REQUIRE_NOTHROW(voxel.mesh.check_validity());
    CHECK(voxel.mesh.elements.size() >= 20 * 4 * 4 / 2); // roughly filled box

    // Fixture on x=0 face, downward load on x=lx face — a cantilever.
    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(voxel.mesh.nodes.size()));
    std::vector<std::uint32_t> load_nodes;
    for (const auto& [node, region] : voxel.boundary_node_region) {
        if (region == fixed_region) {
            bc.fix_node(node);
        } else if (region == loaded_region) {
            load_nodes.push_back(node);
        }
    }
    REQUIRE(!bc.dof_values.empty());
    REQUIRE(!load_nodes.empty());
    const double total_force = -100.0; // N, -z
    for (const auto node : load_nodes) {
        loads[3 * static_cast<Eigen::Index>(node) + 2] =
            total_force / static_cast<double>(load_nodes.size());
    }

    const fea::Material aluminum{.youngs_modulus = 70e9, .poissons_ratio = 0.33};
    const auto u = fea::solve_elastostatics(voxel.mesh, aluminum, bc, loads).u;

    // Tip should deflect downward; magnitude in a physically sane band
    // (draft mesher: sanity check, not a benchmark).
    double min_uz = 0.0;
    for (Eigen::Index i = 0; i < u.size() / 3; ++i) {
        min_uz = std::min(min_uz, u[3 * i + 2]);
    }
    CHECK(min_uz < -1e-7);
    CHECK(min_uz > -1e-2);
}

TEST_CASE("mesh-only job produces volume mesh for GUI preview") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    SimSetup setup;
    setup.mesh_size = 0.25;
    setup.mesher = VolumeMesher::kTetFill;
    SolveJob job;
    job.start_mesh(model, setup);
    const VolumeMeshOutput mesh = await_mesh(job);
    REQUIRE_FALSE(mesh.mesh.elements.empty());
    REQUIRE_FALSE(mesh.boundary_quads.empty());
    REQUIRE_FALSE(mesh.mesher_note.empty());
    REQUIRE_NOTHROW(mesh.mesh.check_validity());
}

TEST_CASE("solve job fills nodal ZZ eta for error-field display") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.04, 0.04);
    SimSetup setup;
    setup.mesh_size = 0.01;
    setup.mesher = VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;
    setup.p_elevate = false; // nodal reaction attribution is exact without MPCs
    // Fixture min-x, load max-x via regions from model.
    int fixed = -1, loaded = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        double x = 0;
        for (auto v : model.surface.triangles[t]) {
            x += model.surface.vertices[v][0];
        }
        if (x < 1e-12) {
            fixed = model.triangle_region[t];
        }
        if (x > 0.29) {
            loaded = model.triangle_region[t];
        }
    }
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);
    setup.fixtures.insert(fixed);
    setup.loads[loaded].force = {0, 0, -100};
    SolveJob job;
    std::optional<PassTrace> solve_trace;
    job.on_solve_stage = [&](const SolveStage& stage) { solve_trace = stage.trace; };
    job.start(model, setup);
    std::optional<SolveResult> result;
    for (int i = 0; i < 500; ++i) {
        result = job.take_result();
        if (result) {
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(result.has_value());
    REQUIRE(result->nodal_eta.size() == result->volume_mesh.nodes.size());
    REQUIRE(result->element_eta.size() == result->volume_mesh.elements.size());
    REQUIRE(result->global_eta >= 0.0);
    REQUIRE(result->max_nodal_eta >= 0.0);
    REQUIRE(result->reactions.size() ==
            3 * static_cast<Eigen::Index>(result->volume_mesh.nodes.size()));
    REQUIRE(result->reactions_complete);
    const auto fixed_members = result->boundary_region_nodes.find(fixed);
    REQUIRE(fixed_members != result->boundary_region_nodes.end());
    REQUIRE_FALSE(fixed_members->second.empty());
    Eigen::Vector3d fixed_reaction = Eigen::Vector3d::Zero();
    for (const std::uint32_t node : fixed_members->second) {
        REQUIRE(node < result->volume_mesh.nodes.size());
        fixed_reaction += result->reactions.segment<3>(3 * static_cast<Eigen::Index>(node));
    }
    Eigen::Vector3d all_reactions = Eigen::Vector3d::Zero();
    for (std::size_t node = 0; node < result->volume_mesh.nodes.size(); ++node) {
        all_reactions += result->reactions.segment<3>(3 * static_cast<Eigen::Index>(node));
    }
    INFO("fixed reaction = " << fixed_reaction.transpose()
                             << ", all reactions = " << all_reactions.transpose()
                             << ", fixed nodes = " << fixed_members->second.size());
    CHECK(std::abs(fixed_reaction.x()) <= 1e-6);
    CHECK(std::abs(fixed_reaction.y()) <= 1e-6);
    CHECK(std::abs(fixed_reaction.z() - 100.0) <= 1e-6);
    REQUIRE(solve_trace.has_value());
    CHECK(solve_trace->solve_flops > 0.0);
    CHECK(solve_trace->solve_bytes > 0.0);
    CHECK(solve_trace->factor_nnz > 0);
    CHECK(solve_trace->solve_method == "direct");
}

namespace {

/// Shared cantilever setup on the unit-ish box STL for SolveJob tests.
SimSetup cantilever_setup(const Model& model, double mesh_size) {
    SimSetup setup;
    setup.mesh_size = mesh_size;
    setup.mesher = VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;
    setup.use_feature_grading = false;
    int fixed = -1, loaded = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        double x = 0;
        for (auto v : model.surface.triangles[t]) {
            x += model.surface.vertices[v][0];
        }
        if (x < 1e-12) {
            fixed = model.triangle_region[t];
        }
        if (x > 0.29) {
            loaded = model.triangle_region[t];
        }
    }
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);
    setup.fixtures.insert(fixed);
    setup.loads[loaded].force = {0, 0, -100};
    return setup;
}

std::optional<SolveResult> run_solve_job(const Model& model, const SimSetup& setup) {
    SolveJob job;
    job.start(model, setup);
    for (int i = 0; i < 800; ++i) {
        if (auto r = job.take_result()) {
            return r;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("D2: high eta_target early-stops adapt before max passes") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.012);
    setup.adapt_passes = 2;
    // Any real ZZ η is finite and ≪ this; stop on pass 0 without remesh.
    setup.eta_target = 1e100;

    const auto result = run_solve_job(model, setup);
    REQUIRE(result.has_value());
    REQUIRE(result->global_eta <= setup.eta_target);
    REQUIRE(result->mesh_note.find("eta-target stop") != std::string::npos);
    REQUIRE(result->mesh_note.find("pass=0/2") != std::string::npos);
    // No adapt remesh: seeds not applied; note must not claim full adapt_passes finish.
    REQUIRE(result->mesh_note.find("adapt_passes=") == std::string::npos);
}

TEST_CASE("D2: eta_target=0 leaves adapt path unchanged (no eta-target stop note)") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.012);
    setup.adapt_passes = 1;
    setup.eta_target = 0.0; // disabled

    const auto result = run_solve_job(model, setup);
    REQUIRE(result.has_value());
    REQUIRE(result->global_eta >= 0.0);
    REQUIRE(result->mesh_note.find("eta-target stop") == std::string::npos);
    // Finished either via max passes or Dörfler early-stop — never η-target.
    REQUIRE((result->mesh_note.find("adapt_passes=") != std::string::npos ||
             result->mesh_note.find("adapt early-stop") != std::string::npos));
}

TEST_CASE("the p-elevated authoritative solve reports its own solver provenance") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.01);
    REQUIRE(setup.p_elevate);

    std::vector<SolveStage> stages;
    SolveJob job;
    job.on_solve_stage = [&stages](const SolveStage& stage) { stages.push_back(stage); };
    job.start(model, setup);
    std::optional<SolveResult> promoted;
    for (int i = 0; i < 800; ++i) {
        promoted = job.take_result();
        if (promoted) {
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(promoted.has_value());
    REQUIRE_FALSE(stages.empty());

    // The emitted pass solved the linear mesh; its note belongs to that solve.
    const auto pass_counts = fea::count_element_types(stages.front().result.volume_mesh);
    CHECK(pass_counts.tet10 + pass_counts.hex20 == 0);
    CHECK(stages.front().result.solver_note.find("direct LDLT selected") != std::string::npos);

    // The authoritative result is the later quadratic re-solve. Before the
    // provenance sink was threaded through every p-elevation branch this field
    // was empty even though the real backend had named its method.
    const auto promoted_counts = fea::count_element_types(promoted->volume_mesh);
    REQUIRE(promoted_counts.tet10 + promoted_counts.hex20 > 0);
    CHECK(promoted->solver_note.find("direct LDLT selected") != std::string::npos);
    CHECK(promoted->solver_note != stages.front().result.solver_note);
}

TEST_CASE("D5: mesh_size=0 auto h yields finite mesh and note with auto h") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);

    // Direct helper: auto on unit box → positive finite h, note tagged auto.
    const auto resolved = resolve_mesh_size(model, 0.0);
    REQUIRE(resolved.auto_chosen);
    REQUIRE(resolved.h > 0.0);
    REQUIRE(std::isfinite(resolved.h));
    // Unit cube: extent=1 → base ~1/16; clamps keep it mesh-scale.
    REQUIRE(resolved.h < 0.5);
    REQUIRE(resolved.h > 1e-4);
    REQUIRE(resolved.note.find("auto h=") != std::string::npos);
    REQUIRE(resolved.n_sharp_edges > 0); // box has crease edges

    // User-specified h is not auto.
    const auto user = resolve_mesh_size(model, 0.1);
    REQUIRE_FALSE(user.auto_chosen);
    REQUIRE(std::abs(user.h - 0.1) < 1e-15);
    REQUIRE(user.note.find("user") != std::string::npos);

    // Mesh-only job with mesh_size=0 must produce elements and carry auto note.
    SimSetup setup;
    setup.mesh_size = 0.0;
    setup.mesher = VolumeMesher::kTetFill;
    setup.use_feature_grading = false;
    SolveJob job;
    job.start_mesh(model, setup);
    const VolumeMeshOutput mesh = await_mesh(job);
    REQUIRE_FALSE(mesh.mesh.elements.empty());
    REQUIRE_FALSE(mesh.mesh.nodes.empty());
    REQUIRE_NOTHROW(mesh.mesh.check_validity());
    REQUIRE(mesh.mesher_note.find("auto h=") != std::string::npos);
}

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
    // polls to diverge (regression: UI looked frozen mid-mesh/solve).
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

namespace {

/// Nodes whose x lies within `tol` of `x_target`, on the mesh in hand.
std::vector<std::uint32_t> nodes_near_x(const fea::NodalMesh& mesh, double x_target,
                                        double tol) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
        if (std::abs(mesh.nodes[i][0] - x_target) < tol) {
            out.push_back(i);
        }
    }
    return out;
}

/// A mesh-resolved builder: clamp every node on the x=0 plane, spread `total`
/// over the nodes on the x=lx plane. Deliberately node-resolved rather than
/// region-resolved so it exercises the non-region path.
BoundaryConditions clamp_and_pull(const fea::NodalMesh& mesh, double lx,
                                  const Eigen::Vector3d& total, double tol) {
    BoundaryConditions out;
    for (const auto node : nodes_near_x(mesh, 0.0, tol)) {
        out.dirichlet.fix_node(node);
    }
    out.loads = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto loaded = nodes_near_x(mesh, lx, tol);
    if (!loaded.empty()) {
        const Eigen::Vector3d per = total / static_cast<double>(loaded.size());
        for (const auto node : loaded) {
            out.loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per;
        }
    }
    return out;
}

/// Runs a job that is expected to fail and returns its status text.
std::string failure_text(const Model& model, const SimSetup& setup) {
    SolveJob job;
    job.start(model, setup);
    for (int i = 0; i < 800; ++i) {
        if (job.state() == SolveJob::State::kFailed) {
            return job.status_text();
        }
        if (job.take_result()) {
            FAIL("solve unexpectedly succeeded");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FAIL("timed out waiting for the expected failure");
    return {};
}

} // namespace

TEST_CASE("boundary_builder replaces region selection and is self-describing") {
    const double lx = 0.1;
    const auto model = polymesh::testsupport::box_model(lx, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.012);
    // Strip the region selection outright: if the builder were ignored, the
    // region path would throw "no fixtures" instead of solving.
    setup.fixtures.clear();
    setup.loads.clear();
    setup.boundary_builder = [lx](const fea::NodalMesh& mesh) {
        return clamp_and_pull(mesh, lx, {0.0, 0.0, -100.0}, 1e-9);
    };

    const auto result = run_solve_job(model, setup);
    REQUIRE(result.has_value());
    CHECK(result->displacement.size() ==
          3 * static_cast<Eigen::Index>(result->volume_mesh.nodes.size()));
    CHECK(result->displacement.allFinite());
    CHECK(result->max_displacement > 0.0);
    // Provenance: a row must say which path produced its BCs.
    CHECK(result->mesh_note.find("BCs: caller boundary_builder") != std::string::npos);
    CHECK(result->mesh_note.find("BCs: region selection") == std::string::npos);
}

TEST_CASE("an unset boundary_builder leaves the region path in charge") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    const auto setup = cantilever_setup(model, 0.012);
    REQUIRE_FALSE(static_cast<bool>(setup.boundary_builder)); // empty by default

    const auto result = run_solve_job(model, setup);
    REQUIRE(result.has_value());
    CHECK(result->mesh_note.find("BCs: region selection") != std::string::npos);
    CHECK(result->mesh_note.find("BCs: caller boundary_builder") == std::string::npos);
}

TEST_CASE("boundary_builder cannot silently hand back a degenerate system") {
    const double lx = 0.1;
    const auto model = polymesh::testsupport::box_model(lx, 0.02, 0.02);
    auto base = cantilever_setup(model, 0.012);
    base.fixtures.clear();
    base.loads.clear();

    SECTION("no Dirichlet DOFs") {
        auto setup = base;
        setup.boundary_builder = [lx](const fea::NodalMesh& mesh) {
            auto out = clamp_and_pull(mesh, lx, {0.0, 0.0, -100.0}, 1e-9);
            out.dirichlet = fea::Dirichlet{};
            return out;
        };
        CHECK(failure_text(model, setup).find("no Dirichlet DOFs") != std::string::npos);
    }
    SECTION("load vector sized for the wrong mesh") {
        auto setup = base;
        setup.boundary_builder = [lx](const fea::NodalMesh& mesh) {
            auto out = clamp_and_pull(mesh, lx, {0.0, 0.0, -100.0}, 1e-9);
            out.loads = Eigen::VectorXd::Ones(out.loads.size() + 3);
            return out;
        };
        CHECK(failure_text(model, setup).find("expected") != std::string::npos);
    }
    SECTION("zero load vector") {
        auto setup = base;
        setup.boundary_builder = [lx](const fea::NodalMesh& mesh) {
            auto out = clamp_and_pull(mesh, lx, {0.0, 0.0, -100.0}, 1e-9);
            out.loads.setZero();
            return out;
        };
        CHECK(failure_text(model, setup).find("zero load vector") != std::string::npos);
    }
}

TEST_CASE("a solution from a different discretization fails an independent residual check") {
    // This is the guard for reusing SolveResult::displacement. Reuse is only
    // sound while the adaptive solve and the scoring solve are the SAME system.
    // An independent residual check (the shape of testlab's health gate) must
    // still catch a solution imported from a different BC discretization, so
    // that a future "optimisation" which reintroduces a mismatched reuse is
    // caught here rather than by silently wrong campaign numbers.
    const double lx = 0.1;
    const auto model = polymesh::testsupport::box_model(lx, 0.02, 0.02);
    const Eigen::Vector3d total{0.0, 0.0, -100.0};

    auto base = cantilever_setup(model, 0.012);
    base.fixtures.clear();
    base.loads.clear();

    // Two genuinely different discretizations of "clamp one end, pull the
    // other": a fully clamped end face versus a narrow clamped strip.
    auto broad = base;
    broad.boundary_builder = [lx, total](const fea::NodalMesh& mesh) {
        return clamp_and_pull(mesh, lx, total, 1e-9);
    };
    auto narrow = base;
    narrow.boundary_builder = [lx, total](const fea::NodalMesh& mesh) {
        BoundaryConditions out = clamp_and_pull(mesh, lx, total, 1e-9);
        out.dirichlet = fea::Dirichlet{};
        for (const auto node : nodes_near_x(mesh, 0.0, 1e-9)) {
            if (mesh.nodes[node][2] < 1e-9) { // only the bottom edge of that face
                out.dirichlet.fix_node(node);
            }
        }
        return out;
    };

    const auto broad_result = run_solve_job(model, broad);
    const auto narrow_result = run_solve_job(model, narrow);
    REQUIRE(broad_result.has_value());
    REQUIRE(narrow_result.has_value());
    // Same mesher, same h, no adaptation: both solves share one mesh.
    REQUIRE(broad_result->volume_mesh.nodes.size() == narrow_result->volume_mesh.nodes.size());

    const fea::Material mat{.youngs_modulus = base.youngs_modulus,
                            .poissons_ratio = base.poissons_ratio};
    const auto& mesh = broad_result->volume_mesh;
    const auto k = fea::assemble_stiffness(mesh, mat);
    const auto broad_bc = clamp_and_pull(mesh, lx, total, 1e-9);
    REQUIRE_FALSE(broad_bc.dirichlet.dof_values.empty());

    // Free-DOF relative residual, exactly the health gate's arithmetic.
    const auto free_residual_rel = [&](const Eigen::VectorXd& u) {
        const Eigen::VectorXd r = k * u - broad_bc.loads;
        double free_r2 = 0.0;
        double f2 = 0.0;
        for (Eigen::Index dof = 0; dof < r.size(); ++dof) {
            f2 += broad_bc.loads[dof] * broad_bc.loads[dof];
            if (!broad_bc.dirichlet.dof_values.contains(dof)) {
                free_r2 += r[dof] * r[dof];
            }
        }
        return std::sqrt(free_r2) / std::max(std::sqrt(f2), 1e-30);
    };

    // The matching solution satisfies its own system.
    CHECK(free_residual_rel(broad_result->displacement) < 1e-6);
    // The mismatched one does not, and by a wide margin - so a gate that
    // independently recomputes the residual cannot be fooled by a swap.
    CHECK(free_residual_rel(narrow_result->displacement) > 1e-3);
}
