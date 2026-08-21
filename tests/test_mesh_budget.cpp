// SPDX-License-Identifier: BSD-3-Clause

#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace polymesh::pipeline;

namespace {

SimSetup cantilever_setup(const Model& model, double h) {
    SimSetup setup;
    setup.mesh_size = h;
    setup.mesher = VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;

    int fixed = -1;
    int loaded = -1;
    const double xmax = model.bbox_max.x();
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        double x = 0.0;
        for (const auto v : model.surface.triangles[t]) {
            x += model.surface.vertices[v].x();
        }
        if (x < 1e-12) {
            fixed = model.triangle_region[t];
        }
        if (x > 3.0 * xmax - 1e-9) {
            loaded = model.triangle_region[t];
        }
    }
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);
    setup.fixtures.insert(fixed);
    setup.loads[loaded].force = Eigen::Vector3d(0.0, 0.0, -100.0);
    return setup;
}

SolveResult wait_for_result(SolveJob& job) {
    for (int i = 0; i < 1500; ++i) {
        if (auto result = job.take_result()) {
            return std::move(*result);
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("timed out waiting for SolveJob");
    throw std::runtime_error("timed out waiting for SolveJob");
}

} // namespace

TEST_CASE("mesh budget: auto h is clamped and reports the element ceiling") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const auto resolved = resolve_mesh_size(model, 0.0, 30.0, 5000, 1000000);

    REQUIRE(resolved.auto_chosen);
    REQUIRE(resolved.ceiling_clamped);
    REQUIRE(resolved.predicted_elements <= 5000.0);
    CHECK(resolved.note.find("auto h clamped from") != std::string::npos);
    CHECK(resolved.note.find("element ceiling 5000") != std::string::npos);
}

TEST_CASE("mesh budget: explicit tiny ceiling refuses before fill with prediction") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    try {
        (void)volume_mesh(model, 0.05, VolumeMesher::kHybrid, 2, false, {}, 0.0, 0.0, 5000,
                          1000000);
        FAIL("expected explicit mesh ceiling refusal");
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        CHECK(message.find("mesh element ceiling 5000 exceeded") != std::string::npos);
        CHECK(message.find("predicted 48000 elements") != std::string::npos);
    }
}

TEST_CASE("mesh budget: generous explicit ceiling preserves exact mesh") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const auto baseline = volume_mesh(model, 0.2, VolumeMesher::kHybrid, 2, false);
    const auto guarded = volume_mesh(model, 0.2, VolumeMesher::kHybrid, 2, false, {}, 0.0, 0.0,
                                     1000000, 3000000);

    REQUIRE(guarded.mesh.elements.size() == baseline.mesh.elements.size());
    REQUIRE(guarded.mesh.nodes.size() == baseline.mesh.nodes.size());
    REQUIRE(guarded.boundary_quads == baseline.boundary_quads);
    for (std::size_t i = 0; i < guarded.mesh.elements.size(); ++i) {
        REQUIRE(guarded.mesh.elements[i].type == baseline.mesh.elements[i].type);
        REQUIRE(guarded.mesh.elements[i].nodes == baseline.mesh.elements[i].nodes);
        REQUIRE(guarded.mesh.elements[i].faces == baseline.mesh.elements[i].faces);
    }
    for (std::size_t i = 0; i < guarded.mesh.nodes.size(); ++i) {
        REQUIRE((guarded.mesh.nodes[i].array() == baseline.mesh.nodes[i].array()).all());
    }
}

TEST_CASE("mesh budget: adapt stops before the predicted next-pass growth") {
    const auto model = polymesh::testsupport::box_model(0.1, 0.02, 0.02);
    auto setup = cantilever_setup(model, 0.01);
    setup.mesher = VolumeMesher::kHybrid;
    setup.adapt_passes = 3;
    setup.max_elems = 10000;
    setup.max_dof = 1600;

    SolveJob job;
    job.start(model, setup);
    const auto result = wait_for_result(job);
    CHECK(result.mesh_note.find("adapt growth cap stop: next pass predicted") !=
          std::string::npos);
    CHECK(result.mesh_note.find("DOF ceiling 1600") != std::string::npos);
}

TEST_CASE("mesh budget: cooperative hybrid fill cancellation returns promptly") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    SimSetup setup;
    setup.mesh_size = 0.01;
    setup.mesher = VolumeMesher::kHybrid;
    setup.max_elems = 10000000;
    setup.max_dof = 30000000;

    SolveJob job;
    job.start_mesh(model, setup);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto requested = std::chrono::steady_clock::now();
    job.request_cancel();
    while (job.state() == SolveJob::State::kMeshing) {
        REQUIRE(std::chrono::steady_clock::now() - requested < std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto latency = std::chrono::steady_clock::now() - requested;
    CHECK(latency < std::chrono::seconds(1));
    CHECK(job.state() == SolveJob::State::kCancelled);
}
