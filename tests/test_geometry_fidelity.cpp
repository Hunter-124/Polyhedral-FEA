// SPDX-License-Identifier: BSD-3-Clause
// Curved-CAD import fidelity + LEB robustness (the pipe-part regressions).
//
// 1. A cylinder wall must tessellate with sub-percent chord deviation from the
//    true radius — imported pipes should not show coarse facets.
// 2. Longest-edge bisection on a large-coordinate curved mesh must not abort on
//    a degenerate child ("local_refine_tets: non-positive child volume"); the
//    sliver region is skipped and the mesh stays valid.

#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/local_refine.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <vector>

namespace {
constexpr double kPipeR = 30.0; // matches tests/fixtures/parts/pipe.step
constexpr double kPipeH = 400.0;
constexpr char kPipe[] = "tests/fixtures/parts/pipe.step";
} // namespace

TEST_CASE("curved CAD import: cylinder wall tessellates with sub-percent deviation") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPipe)) {
        SKIP("pipe.step missing");
    }
    const auto cad = polymesh::geom::CadModel::load_step(kPipe);
    const auto surf = cad.tessellate();

    // Lateral-wall facets: normal ~perpendicular to the z axis. Their centroids
    // sit on the tessellation chord; distance to axis vs R = the sag we resolve.
    double max_dev = 0.0;
    std::size_t wall_facets = 0;
    for (const auto& t : surf.triangles) {
        const Eigen::Vector3d a = surf.vertices[t[0]];
        const Eigen::Vector3d b = surf.vertices[t[1]];
        const Eigen::Vector3d c = surf.vertices[t[2]];
        const Eigen::Vector3d n = Eigen::Vector3d(b - a).cross(Eigen::Vector3d(c - a));
        if (n.norm() < 1e-12) {
            continue;
        }
        const Eigen::Vector3d nn = n.normalized();
        if (std::abs(nn.z()) > 0.5) {
            continue; // end cap, not the curved wall
        }
        const Eigen::Vector3d ctr = (a + b + c) / 3.0;
        const double r = std::hypot(ctr.x(), ctr.y());
        max_dev = std::max(max_dev, std::abs(r - kPipeR));
        ++wall_facets;
    }
    // ~2π/0.2rad ≈ 31 facets around → many wall facets, chord sag < 1% R.
    REQUIRE(wall_facets >= 40);
    CHECK(max_dev / kPipeR < 0.01);
}

TEST_CASE("LEB on a large-coordinate curved mesh does not abort on slivers") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPipe)) {
        SKIP("pipe.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kPipe);
    const double h = 0.18 * kPipeH; // coarse; LEB then refines locally
    const auto vol = polymesh::pipeline::volume_mesh(model, h,
                                                     polymesh::pipeline::VolumeMesher::kGradedTet);
    REQUIRE_FALSE(vol.mesh.elements.empty());

    std::vector<std::array<std::uint32_t, 4>> tets;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == polymesh::fea::ElementType::kTet4 && el.nodes.size() == 4) {
            tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
        }
    }
    REQUIRE_FALSE(tets.empty());

    // Mark every tet and cascade a few LEB waves against the CAD surface —
    // exactly the pipeline adapt path that used to throw on a degenerate child.
    auto nodes = vol.mesh.nodes;
    for (int wave = 0; wave < 3; ++wave) {
        std::vector<std::size_t> marks(tets.size());
        std::iota(marks.begin(), marks.end(), 0);
        polymesh::mesh::LocalRefineStats st;
        polymesh::mesh::TetFillOutput refined;
        REQUIRE_NOTHROW(refined = polymesh::mesh::local_refine_tets(nodes, tets, marks, &st,
                                                                    &model.surface));
        // Every kept tet is strictly positive (validated inside); mesh grew.
        REQUIRE(refined.tets.size() >= tets.size());
        nodes = std::move(refined.nodes);
        tets = std::move(refined.tets);
    }
}
