// SPDX-License-Identifier: AGPL-3.0-or-later

// Headless test of the GUI scene pipeline: STL import -> CAD-style face
// regions -> draft voxel mesh -> fixture/load mapping -> solve. Keeps the
// interactive path covered by CI without a display.

#include "fea/solve.hpp"
#include "scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>

using namespace polymesh::gui;
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
