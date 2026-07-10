// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/sizing_field.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace {

/// Axis-aligned box [0,lx]×[0,ly]×[0,lz] as 12 triangles (outward CCW).
polymesh::geom::TriSurface make_box(double lx, double ly, double lz) {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0},  {lx, 0, 0},  {lx, ly, 0},  {0, ly, 0},
                  {0, 0, lz}, {lx, 0, lz}, {lx, ly, lz}, {0, ly, lz}};
    // Winding matches existing unit-cube tests (outward for standard tet fill).
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    s.validate();
    return s;
}

/// Unit sphere via recursive octahedron subdivision (normalized vertices).
polymesh::geom::TriSurface make_sphere(int subdivisions) {
    // Regular octahedron.
    std::vector<Eigen::Vector3d> verts = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    std::vector<std::array<std::uint32_t, 3>> faces = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4},
                                                       {3, 0, 4}, {0, 5, 2}, {2, 5, 1},
                                                       {1, 5, 3}, {3, 5, 0}};

    auto mid_key = [](std::uint32_t a, std::uint32_t b) {
        return a < b ? std::pair{a, b} : std::pair{b, a};
    };

    for (int s = 0; s < subdivisions; ++s) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoint;
        std::vector<std::array<std::uint32_t, 3>> next;
        next.reserve(faces.size() * 4);
        auto get_mid = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = mid_key(a, b);
            if (auto it = midpoint.find(key); it != midpoint.end()) {
                return it->second;
            }
            Eigen::Vector3d m = 0.5 * (verts[a] + verts[b]);
            m.normalize();
            const auto id = static_cast<std::uint32_t>(verts.size());
            verts.push_back(m);
            midpoint.emplace(key, id);
            return id;
        };
        for (const auto& f : faces) {
            const auto a = f[0], b = f[1], c = f[2];
            const auto ab = get_mid(a, b);
            const auto bc = get_mid(b, c);
            const auto ca = get_mid(c, a);
            next.push_back({a, ab, ca});
            next.push_back({b, bc, ab});
            next.push_back({c, ca, bc});
            next.push_back({ab, bc, ca});
        }
        faces = std::move(next);
    }

    polymesh::geom::TriSurface s;
    s.vertices = std::move(verts);
    s.triangles = std::move(faces);
    s.validate();
    return s;
}

/// Flat unit square in z=0 (open surface) — zero mean curvature interior.
polymesh::geom::TriSurface make_flat_square() {
    polymesh::geom::TriSurface s;
    // 3×3 grid for interior vertices with flat 1-rings.
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            s.vertices.push_back(
                {0.5 * static_cast<double>(i), 0.5 * static_cast<double>(j), 0.0});
        }
    }
    auto vid = [](int i, int j) { return static_cast<std::uint32_t>(j * 3 + i); };
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const auto v00 = vid(i, j);
            const auto v10 = vid(i + 1, j);
            const auto v01 = vid(i, j + 1);
            const auto v11 = vid(i + 1, j + 1);
            s.triangles.push_back({v00, v10, v11});
            s.triangles.push_back({v00, v11, v01});
        }
    }
    s.validate();
    return s;
}

} // namespace

TEST_CASE("thin plate thickness smaller than bulk cube") {
    const auto thin = make_box(1.0, 1.0, 0.1);
    const auto bulk = make_box(1.0, 1.0, 1.0);

    const auto t_thin = polymesh::geom::estimate_local_thickness(thin);
    const auto t_bulk = polymesh::geom::estimate_local_thickness(bulk);

    // Face-center vertices on top/bottom: thin z=0.1 plate → thickness ~0.1;
    // cube → thickness ~1.0.
    REQUIRE(t_thin.thickness.size() == 8);
    REQUIRE(t_bulk.thickness.size() == 8);

    double min_thin = 1e300;
    double min_bulk = 1e300;
    for (double t : t_thin.thickness) {
        if (polymesh::geom::has_finite_thickness(t)) {
            min_thin = std::min(min_thin, t);
        }
    }
    for (double t : t_bulk.thickness) {
        if (polymesh::geom::has_finite_thickness(t)) {
            min_bulk = std::min(min_bulk, t);
        }
    }
    REQUIRE(min_thin < 1e299);
    REQUIRE(min_bulk < 1e299);
    CHECK(min_thin < min_bulk);
    CHECK(min_thin == Catch::Approx(0.1).margin(0.05));
    CHECK(min_bulk == Catch::Approx(1.0).margin(0.25));
}

TEST_CASE("geometry sizing: mid-thin plate smaller h than bulk cube") {
    const auto thin = make_box(1.0, 1.0, 0.1);
    const auto bulk = make_box(1.0, 1.0, 1.0);
    const std::vector<polymesh::geom::SharpEdge> no_edges;

    // Disable sharp-edge influence; curvature on boxes is crease-dominated at
    // corners — empty edges isolates thickness grading.
    auto field_thin =
        polymesh::adapt::make_geometry_sizing(0.01, 0.5, 1.0, thin, no_edges, 0.25, 0.4);
    auto field_bulk =
        polymesh::adapt::make_geometry_sizing(0.01, 0.5, 1.0, bulk, no_edges, 0.25, 0.4);

    // Query near face centers (not corners) so nearest vertex is a face corner
    // still carrying opposite-face thickness.
    const Eigen::Vector3d mid_thin{0.5, 0.5, 0.05};
    const Eigen::Vector3d mid_bulk{0.5, 0.5, 0.5};
    // Also sample just outside a large face so nearest verts are face verts.
    const Eigen::Vector3d near_thin_face{0.5, 0.5, 0.0};
    const Eigen::Vector3d near_bulk_face{0.5, 0.5, 0.0};

    const double h_thin = field_thin->size_at(near_thin_face);
    const double h_bulk = field_bulk->size_at(near_bulk_face);
    CHECK(h_thin < h_bulk);
    CHECK(h_thin >= 0.01 - 1e-12);
    CHECK(h_bulk <= 0.5 + 1e-12);

    // Interior mid of thin plate also tighter than cube centre bulk.
    const double h_mid_thin = field_thin->size_at(mid_thin);
    const double h_mid_bulk = field_bulk->size_at(mid_bulk);
    CHECK(h_mid_thin < h_mid_bulk);
}

TEST_CASE("sphere curvature higher than flat square") {
    const auto sphere = make_sphere(2); // 128 tris, unit radius
    const auto flat = make_flat_square();

    const auto k_sph = polymesh::geom::estimate_vertex_curvature(sphere);
    const auto k_flat = polymesh::geom::estimate_vertex_curvature(flat);

    double max_sph = 0.0;
    double max_flat = 0.0;
    for (double k : k_sph.kappa) {
        max_sph = std::max(max_sph, k);
    }
    for (double k : k_flat.kappa) {
        max_flat = std::max(max_flat, k);
    }
    // Unit sphere: |H| = 1. Discrete proxy should be O(1), clearly > flat.
    CHECK(max_sph > 0.2);
    CHECK(max_sph > 5.0 * max_flat + 1e-9);
}

TEST_CASE("geometry sizing: high curvature sphere smaller h than flat") {
    const auto sphere = make_sphere(2);
    const auto flat = make_flat_square();
    const std::vector<polymesh::geom::SharpEdge> no_edges;

    auto field_sph =
        polymesh::adapt::make_geometry_sizing(0.02, 0.5, 2.0, sphere, no_edges, 0.25, 0.5);
    auto field_flat =
        polymesh::adapt::make_geometry_sizing(0.02, 0.5, 2.0, flat, no_edges, 0.25, 0.5);

    // On sphere surface (vertex near +z pole).
    const Eigen::Vector3d on_sphere{0.0, 0.0, 1.0};
    // On flat interior.
    const Eigen::Vector3d on_flat{0.5, 0.5, 0.0};

    const double h_sph = field_sph->size_at(on_sphere);
    const double h_flat = field_flat->size_at(on_flat);
    CHECK(h_sph < h_flat);
    CHECK(h_sph < 0.5);
    // Flat has ~zero kappa and no thickness → should stay near h_max.
    CHECK(h_flat == Catch::Approx(0.5).margin(0.05));
}

TEST_CASE("make_geometry_sizing also tightens at sharp cube edges") {
    const auto cube = make_box(1.0, 1.0, 1.0);
    const auto edges = polymesh::geom::detect_sharp_edges(cube, 30.0);
    REQUIRE_FALSE(edges.empty());
    auto field = polymesh::adapt::make_geometry_sizing(0.02, 0.2, 0.5, cube, edges);
    const double h_near = field->size_at({0.0, 0.0, 0.0});
    const double h_far = field->size_at({0.5, 0.5, 0.5});
    CHECK(h_near < h_far);
}
