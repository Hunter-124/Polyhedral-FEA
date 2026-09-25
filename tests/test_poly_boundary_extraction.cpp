// SPDX-License-Identifier: BSD-3-Clause
// Polyhedral boundary extraction: shared-face cancellation across differing
// face partitions (n-gon vs fan, split edges/triangles), non-manifold groups,
// degenerate-loop sanitizing, and triangulation of collinear corners.

#include "fea/boundary_faces.hpp"
#include "fea/nodal_mesh.hpp"
#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using namespace polymesh::fea;

TEST_CASE("poly boundary extraction cancels shared faces before triangulation") {
    NodalMesh mesh;
    constexpr double kSqrt3Over2 = 0.86602540378443864676;
    const std::array<Eigen::Vector2d, 6> ring{{
        {1.0, 0.0},
        {0.5, kSqrt3Over2},
        {-0.5, kSqrt3Over2},
        {-1.0, 0.0},
        {-0.5, -kSqrt3Over2},
        {0.5, -kSqrt3Over2},
    }};
    for (double z : {0.0, 1.0, 2.0}) {
        for (const auto& xy : ring) {
            mesh.nodes.emplace_back(xy.x(), xy.y(), z);
        }
    }

    auto prism = [](std::uint32_t lower) {
        std::vector<std::uint32_t> nodes;
        nodes.reserve(12);
        for (std::uint32_t i = 0; i < 12; ++i) {
            nodes.push_back(lower + i);
        }
        std::vector<std::vector<std::uint32_t>> faces;
        faces.push_back({5, 4, 3, 2, 1, 0});
        faces.push_back({6, 7, 8, 9, 10, 11});
        for (std::uint32_t i = 0; i < 6; ++i) {
            const std::uint32_t next = (i + 1) % 6;
            faces.push_back({i, next, next + 6, i + 6});
        }
        return NodalElement{ElementType::kPolyVem, std::move(nodes), std::move(faces)};
    };
    mesh.elements.push_back(prism(0));
    mesh.elements.push_back(prism(6));
    REQUIRE_NOTHROW(mesh.check_validity());

    const auto boundary = extract_boundary_faces(mesh);
    REQUIRE(boundary.size() == 20);
    for (const auto& face : boundary) {
        bool lies_on_shared_plane = true;
        for (const std::uint32_t node : face) {
            REQUIRE(node < mesh.nodes.size());
            lies_on_shared_plane =
                lies_on_shared_plane && std::abs(mesh.nodes[node].z() - 1.0) < 1e-12;
        }
        CHECK_FALSE(lies_on_shared_plane);
    }
}

TEST_CASE("poly boundary extraction cross-cancels an n-gon and its triangle fan") {
    NodalMesh mesh;
    constexpr double kSqrt3Over2 = 0.86602540378443864676;
    mesh.nodes = {
        {1.0, 0.0, 0.0},  {0.5, kSqrt3Over2, 0.0},   {-0.5, kSqrt3Over2, 0.0},
        {-1.0, 0.0, 0.0}, {-0.5, -kSqrt3Over2, 0.0}, {0.5, -kSqrt3Over2, 0.0},
    };
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3, 4, 5};
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 3, 4, 5}});
    mesh.elements.emplace_back(
        ElementType::kPolyVem, nodes,
        std::vector<std::vector<std::uint32_t>>{{2, 1, 0}, {2, 0, 5}, {2, 5, 4}, {2, 4, 3}});
    CHECK(extract_boundary_faces(mesh).empty());
    CHECK(extract_boundary_polys(mesh).empty());
}

TEST_CASE("poly boundary extraction cross-cancels a quad and two triangles") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {1.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},
    };
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3};
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 3}});
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 2, 1}, {0, 3, 2}});

    CHECK(extract_boundary_faces(mesh).empty());
    CHECK(extract_boundary_polys(mesh).empty());
}

TEST_CASE("poly boundary extraction suppresses non-manifold exact owner groups") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
    };
    for (int owner = 0; owner < 3; ++owner) {
        mesh.elements.emplace_back(ElementType::kPolyVem, std::vector<std::uint32_t>{0, 1, 2},
                                   std::vector<std::vector<std::uint32_t>>{{0, 1, 2}});
    }

    CHECK(extract_boundary_faces(mesh).empty());
    CHECK(extract_boundary_polys(mesh).empty());
}

TEST_CASE("poly boundary extraction atomizes collinear split boundary edges") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {2.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
    };
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3, 4};
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 3, 4}});
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 4, 3, 2}});

    CHECK(extract_boundary_faces(mesh).empty());
    CHECK(extract_boundary_polys(mesh).empty());
}

TEST_CASE("poly boundary extraction cross-cancels a split coarse triangle") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
    };
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3};
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 2, 3}});
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 3, 1}, {1, 3, 2}});

    CHECK(extract_boundary_faces(mesh).empty());
    CHECK(extract_boundary_polys(mesh).empty());
}

TEST_CASE("poly boundary extraction rejects a folded square partition") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, {0.5, 0.5, 0.0},
    };
    const std::vector<std::uint32_t> nodes{0, 1, 2, 3, 4};
    mesh.elements.emplace_back(ElementType::kPolyVem, nodes,
                               std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 3}});
    mesh.elements.emplace_back(
        ElementType::kPolyVem, nodes,
        std::vector<std::vector<std::uint32_t>>{{0, 1, 4}, {1, 4, 2}, {2, 4, 3}, {3, 4, 0}});

    CHECK(extract_boundary_faces(mesh).size() == 5);
    CHECK(extract_boundary_polys(mesh).size() == 5);
}

TEST_CASE(
    "poly boundary extraction sanitizes adjacent duplicates and rejects repeated nodes") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.5, 0.5, 0.0}, {0.5, 1.0, 0.0}, {-0.5, 0.5, 0.0},
    };
    mesh.elements.emplace_back(
        ElementType::kPolyVem, std::vector<std::uint32_t>{0, 1, 2, 3, 4},
        std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 2, 3, 4}, {0, 1, 2, 1, 3, 4}});
    const auto boundary = extract_boundary_faces(mesh);
    REQUIRE(boundary.size() == 3);
    for (const auto& tri : boundary) {
        CHECK(tri[0] != tri[1]);
        CHECK(tri[1] != tri[2]);
        CHECK(tri[0] != tri[2]);
    }
}

TEST_CASE("poly boundary triangulation skips a distinct collinear mid-edge corner") {
    NodalMesh mesh;
    mesh.nodes = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {2.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
    };
    mesh.elements.emplace_back(ElementType::kPolyVem,
                               std::vector<std::uint32_t>{0, 1, 2, 3, 4},
                               std::vector<std::vector<std::uint32_t>>{{0, 1, 2, 3, 4}});

    const auto boundary = extract_boundary_faces(mesh);
    REQUIRE(boundary.size() == 3);
    double area_sum = 0.0;
    for (const auto& tri : boundary) {
        REQUIRE(tri[0] != tri[1]);
        REQUIRE(tri[1] != tri[2]);
        REQUIRE(tri[0] != tri[2]);
        const Eigen::Vector3d ab = mesh.nodes[tri[1]] - mesh.nodes[tri[0]];
        const Eigen::Vector3d ac = mesh.nodes[tri[2]] - mesh.nodes[tri[0]];
        const double area = 0.5 * ab.cross(ac).norm();
        CHECK(area > 0.0);
        area_sum += area;
    }
    CHECK(area_sum == Catch::Approx(2.0).margin(1e-12));
}
