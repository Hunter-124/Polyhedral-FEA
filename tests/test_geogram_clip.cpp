// SPDX-License-Identifier: BSD-3-Clause
// G1 smoke: vendored Geogram ConvexCell on the unit cube (ADR-0025).

#include "mesh/geogram_clip.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using polymesh::mesh::ClipBox;
using polymesh::mesh::ClipPlane;
using polymesh::mesh::ClippedCell;
using polymesh::mesh::clip_convex_cell;
using polymesh::mesh::geogram_available;
using polymesh::mesh::unit_cube_cell;

TEST_CASE("geogram_available matches POLYMESH_WITH_GEOGRAM", "[geogram][g1]") {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    REQUIRE(geogram_available());
#else
    REQUIRE_FALSE(geogram_available());
#endif
}

TEST_CASE("unit cube ConvexCell volume and barycenter", "[geogram][g1]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    const auto cell = unit_cube_cell();
    REQUIRE(cell.has_value());
    REQUIRE_FALSE(cell->empty);
    REQUIRE(cell->volume == Approx(1.0).margin(1e-12));
    REQUIRE(cell->barycenter.x() == Approx(0.5).margin(1e-12));
    REQUIRE(cell->barycenter.y() == Approx(0.5).margin(1e-12));
    REQUIRE(cell->barycenter.z() == Approx(0.5).margin(1e-12));
    REQUIRE(cell->n_triangles > 0);
    REQUIRE(cell->n_planes > 0);
}

TEST_CASE("clip unit cube by midplane x=0.5 keeps half-volume", "[geogram][g1]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    // Plane x - 0.5 = 0 written as x + 0y + 0z - 0.5 = 0.
    // ConvexCell keeps a·x + d ≥ 0 ⇒ x ≥ 0.5 (volume 0.5, bary x = 0.75).
    std::vector<ClipPlane> planes = {ClipPlane{1.0, 0.0, 0.0, -0.5}};
    const auto cell = clip_convex_cell(ClipBox{}, planes);
    REQUIRE(cell.has_value());
    REQUIRE_FALSE(cell->empty);
    REQUIRE(cell->volume == Approx(0.5).margin(1e-12));
    REQUIRE(cell->barycenter.x() == Approx(0.75).margin(1e-12));
    REQUIRE(cell->barycenter.y() == Approx(0.5).margin(1e-12));
    REQUIRE(cell->barycenter.z() == Approx(0.5).margin(1e-12));
}

TEST_CASE("clip oversized box down to unit cube via six planes", "[geogram][g1]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    box.min = Eigen::Vector3d(-0.1, -0.1, -0.1);
    box.max = Eigen::Vector3d(1.1, 1.1, 1.1);

    // Keep halfspaces that define [0,1]³ (same convention as midplane test).
    const std::vector<ClipPlane> planes = {
        ClipPlane{1.0, 0.0, 0.0, 0.0},   // x ≥ 0
        ClipPlane{-1.0, 0.0, 0.0, 1.0},  // x ≤ 1
        ClipPlane{0.0, 1.0, 0.0, 0.0},   // y ≥ 0
        ClipPlane{0.0, -1.0, 0.0, 1.0},  // y ≤ 1
        ClipPlane{0.0, 0.0, 1.0, 0.0},   // z ≥ 0
        ClipPlane{0.0, 0.0, -1.0, 1.0},  // z ≤ 1
    };

    const auto cell = clip_convex_cell(box, planes);
    REQUIRE(cell.has_value());
    REQUIRE_FALSE(cell->empty);
    REQUIRE(cell->volume == Approx(1.0).margin(1e-9));
    REQUIRE(cell->barycenter.x() == Approx(0.5).margin(1e-9));
    REQUIRE(cell->barycenter.y() == Approx(0.5).margin(1e-9));
    REQUIRE(cell->barycenter.z() == Approx(0.5).margin(1e-9));
}

TEST_CASE("ClipPlane::from_point_normal matches explicit coefficients",
          "[geogram][g1]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    const Eigen::Vector3d p(0.5, 0.0, 0.0);
    const Eigen::Vector3d n(1.0, 0.0, 0.0);
    const ClipPlane pl = ClipPlane::from_point_normal(p, n);
    REQUIRE(pl.a == Approx(1.0));
    REQUIRE(pl.d == Approx(-0.5));

    const auto cell = clip_convex_cell(ClipBox{}, std::span{&pl, 1});
    REQUIRE(cell.has_value());
    REQUIRE(cell->volume == Approx(0.5).margin(1e-12));
}
