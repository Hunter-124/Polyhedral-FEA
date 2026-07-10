// SPDX-License-Identifier: BSD-3-Clause

// E4: Product-mesh Tier-1-style smoke — volume_mesh (our tet fill) from simple
// constructed geometry, then a coarse elastostatic solve that finishes with
// finite stress / non-zero displacement. Not a tight analytical gate (grid
// fill is stair-cased); Lamé product path is documented smoke-only until B1.

#include "fea/solve.hpp"
#include "fea/stress.hpp"
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

std::filesystem::path write_box_stl(double lx, double ly, double lz, const char* name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path);
    out << "solid box\n";
    const auto facet = [&](std::array<double, 3> n, std::array<std::array<double, 3>, 3> v) {
        out << std::format(" facet normal {} {} {}\n  outer loop\n", n[0], n[1], n[2]);
        for (const auto& p : v) {
            out << std::format("   vertex {} {} {}\n", p[0] * lx, p[1] * ly, p[2] * lz);
        }
        out << "  endloop\n endfacet\n";
    };
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

/// Identify face regions on a box by mean triangle x-coordinate.
void box_end_regions(const Model& model, double lx, int& fixed, int& loaded) {
    fixed = -1;
    loaded = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        double x = 0.0;
        for (auto v : model.surface.triangles[t]) {
            x += model.surface.vertices[v][0];
        }
        if (x < 1e-12) {
            fixed = model.triangle_region[t];
        }
        if (x > 3.0 * lx - 1e-9) {
            loaded = model.triangle_region[t];
        }
    }
}

} // namespace

TEST_CASE("E4 product mesh: unit box cantilever, max |u| > 0, finite stress") {
    // Coarse tet product mesh on a unit box; fix x=0, load +Fx on x=1.
    // Smoke: solve succeeds, tip moves, von Mises finite and positive.
    const auto path = write_box_stl(1.0, 1.0, 1.0, "polymesh_e4_unit_box.stl");
    const auto model = Model::load(path.string());
    REQUIRE(model.region_count == 6);

    int fixed = -1, loaded = -1;
    box_end_regions(model, 1.0, fixed, loaded);
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);
    REQUIRE(fixed != loaded);

    const double h = 0.35; // coarse — CI-friendly
    auto vol = volume_mesh(model, h, VolumeMesher::kTetFill);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesh.elements.size() >= 4);
    REQUIRE(vol.mesh.nodes.size() >= 8);

    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    std::vector<std::uint32_t> load_nodes;
    for (const auto& [node, region] : vol.boundary_node_region) {
        if (region == fixed) {
            bc.fix_node(node);
        } else if (region == loaded) {
            load_nodes.push_back(node);
        }
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    REQUIRE_FALSE(load_nodes.empty());
    const double total_fx = 1.0e3; // N
    for (const auto node : load_nodes) {
        loads[3 * static_cast<Eigen::Index>(node) + 0] =
            total_fx / static_cast<double>(load_nodes.size());
    }

    const fea::Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto u = fea::solve_elastostatics(vol.mesh, steel, bc, loads);
    REQUIRE(u.size() == 3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));

    double max_u = 0.0;
    for (Eigen::Index i = 0; i < u.size() / 3; ++i) {
        max_u = std::max(max_u, u.segment<3>(3 * i).norm());
    }
    REQUIRE(std::isfinite(max_u));
    REQUIRE(max_u > 0.0);

    const auto stress = fea::recover_nodal_stress(vol.mesh, steel, u);
    double max_vm = 0.0;
    for (const auto& s : stress) {
        const double vm = fea::von_mises(s);
        REQUIRE(std::isfinite(vm));
        max_vm = std::max(max_vm, vm);
    }
    REQUIRE(max_vm > 0.0);
    REQUIRE(std::isfinite(max_vm));
}

TEST_CASE("E4 product mesh: slender box SolveJob finishes with finite stress") {
    // Pipeline SolveJob path (same product mesher) — cantilever-like, coarse.
    const double lx = 0.1, ly = 0.02, lz = 0.02;
    const auto path = write_box_stl(lx, ly, lz, "polymesh_e4_cantilever.stl");
    const auto model = Model::load(path.string());

    int fixed = -1, loaded = -1;
    box_end_regions(model, lx, fixed, loaded);
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);

    SimSetup setup;
    setup.mesh_size = 0.012;
    setup.mesher = VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;
    setup.use_feature_grading = false;
    setup.fixtures.insert(fixed);
    setup.loads[loaded].force = {0.0, 0.0, -50.0};

    SolveJob job;
    job.start(model, setup);
    std::optional<SolveResult> result;
    for (int i = 0; i < 600; ++i) {
        result = job.take_result();
        if (result) {
            break;
        }
        if (job.state() == SolveJob::State::kFailed) {
            FAIL(job.status_text());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    REQUIRE(result.has_value());
    REQUIRE(result->max_displacement > 0.0);
    REQUIRE(std::isfinite(result->max_displacement));
    REQUIRE(result->max_von_mises > 0.0);
    REQUIRE(std::isfinite(result->max_von_mises));
    REQUIRE(result->volume_mesh.elements.size() >= 4);
}

TEST_CASE("E4 product mesh: public cylinder_prism smoke (mesh+solve, not Lamé tol)") {
    // Product tet mesh on published fixture. Stair-cased grid fill cannot meet
    // Lamé analytical tolerance — assert solve + finite stress only.
    const std::filesystem::path geom = "bench/geometries/public/cylinder_prism.stl";
    if (!std::filesystem::exists(geom)) {
        SKIP("cylinder_prism.stl not found (run from repo root)");
    }
    const auto model = Model::load(geom.string());
    REQUIRE(model.surface.triangles.size() >= 8);

    // Coarse h from bbox so runtime stays small.
    const auto resolved = resolve_mesh_size(model, 0.0);
    // Override to a coarser size for CI: at least h_auto, prefer larger.
    const double h = std::max(resolved.h, (model.bbox_max - model.bbox_min).maxCoeff() / 8.0);
    auto vol = volume_mesh(model, h, VolumeMesher::kTetFill);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE_FALSE(vol.mesh.elements.empty());

    // Fixture min-x face nodes, +Fy on max-x (same as CLI solve convention).
    const double xmin = model.bbox_min[0];
    const double xmax = model.bbox_max[0];
    const double tol = 0.51 * h;
    fea::Dirichlet bc;
    std::vector<std::uint32_t> load_nodes;
    for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
        const double x = vol.mesh.nodes[i][0];
        if (x <= xmin + tol) {
            bc.fix_node(i);
        }
        if (x >= xmax - tol) {
            load_nodes.push_back(i);
        }
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    REQUIRE_FALSE(load_nodes.empty());

    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    const Eigen::Vector3d f(0.0, 500.0 / static_cast<double>(load_nodes.size()), 0.0);
    for (auto n : load_nodes) {
        loads.segment<3>(3 * static_cast<Eigen::Index>(n)) += f;
    }

    const fea::Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto u = fea::solve_elastostatics(vol.mesh, steel, bc, loads);
    double max_u = 0.0;
    for (Eigen::Index i = 0; i < u.size() / 3; ++i) {
        max_u = std::max(max_u, u.segment<3>(3 * i).norm());
    }
    REQUIRE(std::isfinite(max_u));
    REQUIRE(max_u > 0.0);

    const auto stress = fea::recover_nodal_stress(vol.mesh, steel, u);
    double max_vm = 0.0;
    for (const auto& s : stress) {
        const double vm = fea::von_mises(s);
        REQUIRE(std::isfinite(vm));
        max_vm = std::max(max_vm, vm);
    }
    REQUIRE(max_vm > 0.0);
}
