// SPDX-License-Identifier: BSD-3-Clause

#include "fea/cell_quality.hpp"
#include "mesh/hybrid_fill.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

using namespace polymesh::pipeline;

TEST_CASE("mesh budget: auto h respects the predicted element ceiling") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const auto resolved = resolve_mesh_size(model, 0.0, 30.0, 5000, 1000000);

    REQUIRE(resolved.auto_chosen);
    REQUIRE(resolved.ceiling_clamped);
    REQUIRE(resolved.predicted_elements <= 5000.0);
}

TEST_CASE("mesh budget: explicit tiny ceiling refuses before fill") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    REQUIRE_THROWS_AS(
        volume_mesh(model, 0.05, VolumeMesher::kHybrid, 2, false, {}, 0.0, 0.0, 5000, 1000000),
        std::runtime_error);
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

TEST_CASE("mesh budget: early graded refinement retries only when allowed") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const std::array<Eigen::Vector3d, 1> seeds{Eigen::Vector3d{0.5, 0.5, 0.5}};
    constexpr std::size_t ceiling = 2000;
    const auto fill = [&](int retries) {
        return volume_mesh(model, 0.25, VolumeMesher::kGradedTet, 2, false, seeds, 1.0, 0.0,
                           ceiling, 0, retries);
    };
    REQUIRE_THROWS_AS(fill(0), polymesh::mesh::RefinementLimitError);
    const auto recovered = fill(3);
    CHECK(recovered.mesh.elements.size() <= ceiling);
    CHECK(polymesh::fea::mesh_volume(recovered.mesh) == Catch::Approx(1.0).margin(1e-10));
}
