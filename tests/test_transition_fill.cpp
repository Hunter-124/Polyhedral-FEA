// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "mesh/transition_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace polymesh::mesh;
namespace fea = polymesh::fea;
namespace pipe = polymesh::pipeline;

namespace {
polymesh::geom::TriSurface box() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}
} // namespace

TEST_CASE("transition fill has hex core and pyramid skin") {
    // Slightly padded bbox + h=0.2 yields a nonempty hex core on the unit box.
    auto fill = transition_fill_surface(box(), {-0.01, -0.01, -0.01}, {1.01, 1.01, 1.01}, 0.2);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_pyramid > 0);
    REQUIRE(fill.n_pyramid % 6 == 0); // 6 pyramids per boundary hex
}

TEST_CASE("hex+pyramid mesh solves linear elasticity") {
    pipe::Model m;
    m.surface = box();
    m.bbox_min = {-0.01, -0.01, -0.01};
    m.bbox_max = {1.01, 1.01, 1.01};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipe::volume_mesh(m, 0.2, pipe::VolumeMesher::kHexPyramid, 2);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    bool has_hex = false, has_pyr = false;
    for (const auto& e : vol.mesh.elements) {
        if (e.type == fea::ElementType::kHex8)
            has_hex = true;
        if (e.type == fea::ElementType::kPyramid5)
            has_pyr = true;
    }
    REQUIRE(has_hex);
    REQUIRE(has_pyr);
    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
        if (vol.mesh.nodes[i][0] < 1e-9)
            bc.fix_node(i);
        if (vol.mesh.nodes[i][0] > 1.0 - 1e-9)
            loads(3 * static_cast<Eigen::Index>(i) + 2) = -1.0;
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    const fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    REQUIRE_NOTHROW(fea::solve_elastostatics(vol.mesh, mat, bc, loads));
}
