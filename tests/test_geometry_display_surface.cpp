// SPDX-License-Identifier: BSD-3-Clause
// Display surface vs physics surface on quadratic and linear meshes:
// extract_boundary_polys facets through projected mid-edge nodes, while
// extract_boundary_faces stays on the corner topology.

#include "fea/boundary_faces.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace {

/// One hex20 sector of a hollow cylinder: outer face on radius `R`, inner on
/// `r`, spanning `sweep` radians. Mid-edge nodes on the outer wall are snapped
/// onto the true cylinder, exactly as `project_quadratic_boundary_mids` does on
/// a real part; every other mid-edge node is the straight midpoint.
polymesh::fea::NodalMesh curved_hex20_sector(double r, double R, double sweep, double height) {
    const auto at = [](double radius, double angle, double z) {
        return Eigen::Vector3d{radius * std::cos(angle), radius * std::sin(angle), z};
    };
    const double a0 = -0.5 * sweep, a1 = 0.5 * sweep;
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {at(r, a0, 0.0),    at(r, a1, 0.0),    at(R, a1, 0.0),    at(R, a0, 0.0),
                  at(r, a0, height), at(r, a1, height), at(R, a1, height), at(R, a0, height)};
    static constexpr std::array<std::array<std::size_t, 2>, 12> kEdges{{{0, 1},
                                                                        {1, 2},
                                                                        {2, 3},
                                                                        {3, 0},
                                                                        {4, 5},
                                                                        {5, 6},
                                                                        {6, 7},
                                                                        {7, 4},
                                                                        {0, 4},
                                                                        {1, 5},
                                                                        {2, 6},
                                                                        {3, 7}}};
    for (const auto& e : kEdges) {
        const Eigen::Vector3d chord = 0.5 * (mesh.nodes[e[0]] + mesh.nodes[e[1]]);
        const bool on_outer_wall = std::abs(mesh.nodes[e[0]].head<2>().norm() - R) < 1e-12 &&
                                   std::abs(mesh.nodes[e[1]].head<2>().norm() - R) < 1e-12;
        if (on_outer_wall && chord.head<2>().norm() > 1e-12) {
            Eigen::Vector3d snapped = chord;
            snapped.head<2>() *= R / chord.head<2>().norm();
            mesh.nodes.push_back(snapped);
        } else {
            mesh.nodes.push_back(chord);
        }
    }
    polymesh::fea::NodalElement el;
    el.type = polymesh::fea::ElementType::kHex20;
    el.nodes.resize(20);
    for (std::uint32_t i = 0; i < 20; ++i) {
        el.nodes[i] = i;
    }
    mesh.elements.push_back(std::move(el));
    return mesh;
}

/// Worst gap between a rendered facet and the true cylinder: the largest
/// shortfall in radius at the midpoint of any drawn edge whose two endpoints
/// both sit on the outer wall. This is precisely the faceting a viewer sees.
double outer_wall_sag(const polymesh::fea::NodalMesh& mesh,
                      const std::vector<std::vector<std::uint32_t>>& facets, double R) {
    double worst = 0.0;
    for (const auto& facet : facets) {
        for (std::size_t i = 0; i < facet.size(); ++i) {
            const Eigen::Vector3d& p = mesh.nodes[facet[i]];
            const Eigen::Vector3d& q = mesh.nodes[facet[(i + 1) % facet.size()]];
            if (std::abs(p.head<2>().norm() - R) > 1e-9 ||
                std::abs(q.head<2>().norm() - R) > 1e-9) {
                continue;
            }
            worst = std::max(worst, R - (0.5 * (p + q)).head<2>().norm());
        }
    }
    return worst;
}

} // namespace

// The mesher projects quadratic mid-edge nodes onto the exact B-rep (ADR-0028);
// the display surface must facet tet10/hex20 through those mids, not draw the
// straight chord between corner nodes.
TEST_CASE("the display surface follows quadratic curvature, the physics surface does not") {
    constexpr double r = 20.0, R = 30.0, sweep = 1.0471975511965976 /* 60 deg */,
                     height = 40.0;
    const auto mesh = curved_hex20_sector(r, R, sweep, height);

    // Physics topology is corner-only by design: BC/load selection and traction
    // area are defined on it, and this test pins that it did NOT change.
    const auto physics = polymesh::fea::extract_boundary_faces(mesh);
    CHECK(physics.size() == 6);
    std::vector<std::vector<std::uint32_t>> physics_loops;
    for (const auto& face : physics) {
        physics_loops.push_back({face[0], face[1], face[2], face[3]});
        CHECK(*std::max_element(face.begin(), face.end()) < 8U);
    }

    const auto display = polymesh::fea::extract_boundary_polys(mesh);
    // Each of the 6 faces splits into 4 corner triangles plus a central quad.
    CHECK(display.size() == 30);

    // Every projected mid-edge node on the outer wall is actually drawn.
    std::set<std::uint32_t> drawn;
    for (const auto& facet : display) {
        drawn.insert(facet.begin(), facet.end());
    }
    for (std::uint32_t n = 8; n < 20; ++n) {
        INFO("mid-edge node " << n);
        CHECK(drawn.count(n) == 1);
    }

    const double corner_sag = outer_wall_sag(mesh, physics_loops, R);
    const double curved_sag = outer_wall_sag(mesh, display, R);
    INFO("corner-only sag " << corner_sag << " mm, curved sag " << curved_sag << " mm");
    // Chord over the full 60 deg sweep vs over each 30 deg half: R(1-cos30) =
    // 4.019 mm against R(1-cos15) = 1.022 mm, a 3.93x improvement.
    CHECK(corner_sag == Catch::Approx(R * (1.0 - std::cos(0.5 * sweep))).margin(1e-9));
    CHECK(curved_sag == Catch::Approx(R * (1.0 - std::cos(0.25 * sweep))).margin(1e-9));
    CHECK(curved_sag < corner_sag / 3.0);
}

// A linear mesh must be untouched: no mid-edge nodes exist, so the display path
// has to hand back exactly the loops it always did, or every hex/pyramid mesh
// silently changes shape.
TEST_CASE("the display surface is unchanged on linear meshes") {
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    polymesh::fea::NodalElement el;
    el.type = polymesh::fea::ElementType::kHex8;
    el.nodes = {0, 1, 2, 3, 4, 5, 6, 7};
    mesh.elements.push_back(std::move(el));

    const auto display = polymesh::fea::extract_boundary_polys(mesh);
    CHECK(display.size() == 6);
    for (const auto& facet : display) {
        CHECK(facet.size() == 4);
    }
}
