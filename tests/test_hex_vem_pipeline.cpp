// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>

using namespace polymesh::pipeline;
namespace fea = polymesh::fea;


TEST_CASE("hex-VEM volume mesh solves cantilever-style BCs") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
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
