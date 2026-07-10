// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>

using namespace polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

std::filesystem::path write_box() {
    const auto path = std::filesystem::temp_directory_path() / "pm_hexvem_box.stl";
    std::ofstream out(path);
    out << "solid box\n";
    auto facet = [&](std::array<double, 3> n, std::array<std::array<double, 3>, 3> v) {
        out << std::format(" facet normal {} {} {}\n  outer loop\n", n[0], n[1], n[2]);
        for (auto& p : v) {
            out << std::format("   vertex {} {} {}\n", p[0], p[1], p[2]);
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

} // namespace

TEST_CASE("hex-VEM volume mesh solves cantilever-style BCs") {
    const auto model = Model::load(write_box().string());
    auto vol = volume_mesh(model, 0.25, VolumeMesher::kHexVem);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE(vol.mesh.elements.front().type == fea::ElementType::kPolyVem);
    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
        if (vol.mesh.nodes[i][0] < 1e-9) {
            bc.fix_node(i);
        }
        if (vol.mesh.nodes[i][0] > 1.0 - 1e-9) {
            loads(3 * static_cast<Eigen::Index>(i) + 2) = -1.0;
        }
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    const fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    REQUIRE_NOTHROW(fea::solve_elastostatics(vol.mesh, mat, bc, loads));
}
