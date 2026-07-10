// SPDX-License-Identifier: BSD-3-Clause

// Headless test of the GUI scene pipeline: STL import -> CAD-style face
// regions -> draft voxel mesh -> fixture/load mapping -> solve. Keeps the
// interactive path covered by CI without a display.

#include "fea/solve.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

/// Writes a unit-ish box (lx, ly, lz metres) as ASCII STL.
std::filesystem::path write_box_stl(double lx, double ly, double lz) {
    const auto path = std::filesystem::temp_directory_path() / "polymesh_test_box.stl";
    std::ofstream out(path);
    out << "solid box\n";
    const auto facet = [&](std::array<double, 3> n, std::array<std::array<double, 3>, 3> v) {
        out << std::format(" facet normal {} {} {}\n  outer loop\n", n[0], n[1], n[2]);
        for (const auto& p : v) {
            out << std::format("   vertex {} {} {}\n", p[0] * lx, p[1] * ly, p[2] * lz);
        }
        out << "  endloop\n endfacet\n";
    };
    // 12 triangles, outward normals, unit-cube corner coordinates.
    facet({0, 0, -1}, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}}});
    facet({0, 0, -1}, {{{0, 0, 0}, {1, 1, 0}, {1, 0, 0}}});
    facet({0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}}});
    facet({0, 0, 1}, {{{0, 0, 1}, {1, 1, 1}, {0, 1, 1}}});
    facet({0, -1, 0}, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}}});
    facet({0, -1, 0}, {{{0, 0, 0}, {1, 0, 1}, {0, 0, 1}}});
    facet({0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}}});
    facet({0, 1, 0}, {{{0, 1, 0}, {1, 1, 1}, {1, 1, 0}}});
    facet({-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}}});
    facet({-1, 0, 0}, {{{0, 0, 0}, {0, 1, 1}, {0, 1, 0}}});
    facet({1, 0, 0}, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}}});
    facet({1, 0, 0}, {{{1, 0, 0}, {1, 1, 1}, {1, 0, 1}}});
    out << "endsolid box\n";
    return path;
}

} // namespace

TEST_CASE("GUI pipeline: box STL segments into six faces and solves end-to-end") {
    const auto path = write_box_stl(0.1, 0.02, 0.02);
    const auto model = Model::load(path.string());
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
    const auto u = fea::solve_elastostatics(voxel.mesh, aluminum, bc, loads);

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
    const auto path = write_box_stl(1.0, 1.0, 1.0);
    const auto model = Model::load(path.string());
    SimSetup setup;
    setup.mesh_size = 0.25;
    setup.mesher = VolumeMesher::kTetFill;
    SolveJob job;
    job.start_mesh(model, setup);
    // Poll until done (mesh is small).
    VolumeMeshOutput mesh;
    for (int i = 0; i < 200; ++i) {
        if (auto m = job.take_mesh()) {
            mesh = std::move(*m);
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(mesh.mesh.elements.empty());
    REQUIRE_FALSE(mesh.boundary_quads.empty());
    REQUIRE_FALSE(mesh.mesher_note.empty());
    REQUIRE_NOTHROW(mesh.mesh.check_validity());
}

TEST_CASE("solve job fills nodal ZZ eta for error-field display") {
    const auto path = write_box_stl(0.1, 0.02, 0.02);
    const auto model = Model::load(path.string());
    SimSetup setup;
    setup.mesh_size = 0.01;
    setup.mesher = VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;
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
    const auto path = write_box_stl(0.1, 0.02, 0.02);
    const auto model = Model::load(path.string());
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
    const auto path = write_box_stl(0.1, 0.02, 0.02);
    const auto model = Model::load(path.string());
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

TEST_CASE("D5: mesh_size=0 auto h yields finite mesh and note with auto h") {
    const auto path = write_box_stl(1.0, 1.0, 1.0);
    const auto model = Model::load(path.string());

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
    VolumeMeshOutput mesh;
    for (int i = 0; i < 300; ++i) {
        if (auto m = job.take_mesh()) {
            mesh = std::move(*m);
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(mesh.mesh.elements.empty());
    REQUIRE_FALSE(mesh.mesh.nodes.empty());
    REQUIRE_NOTHROW(mesh.mesh.check_validity());
    REQUIRE(mesh.mesher_note.find("auto h=") != std::string::npos);
}
