// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/error.hpp"
#include "adapt/loop.hpp"
#include "geom/features.hpp"
#include "mesh/hybrid_fill.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("unit cube has sharp edges at 90 degree faces") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    s.validate();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    REQUIRE(edges.size() >= 12); // cube has 12 edges
}

TEST_CASE("dorfler marks largest contributors") {
    const std::vector<double> eta{1.0, 0.1, 0.1, 0.1};
    const auto m = polymesh::adapt::dorfler_mark(eta, 0.5);
    REQUIRE_FALSE(m.empty());
    REQUIRE(m.front() == 0);
}

TEST_CASE("suggest_refine returns seeds at marked centroids") {
    const std::vector<Eigen::Vector3d> cents{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const std::vector<double> eta{1.0, 0.01, 0.01, 0.01};
    const auto sug = polymesh::adapt::suggest_refine(cents, eta, 0.2, 0.5, 0.7, 0.05);
    REQUIRE(sug.n_marked >= 1);
    REQUIRE_FALSE(sug.refine_seeds.empty());
    REQUIRE(sug.refine_seeds.front().isApprox(cents.front()));
    REQUIRE(sug.h_next < 0.2);
    REQUIRE(sug.seed_band > 0.0);
}

TEST_CASE("error seeds force additional graded fine blocks") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    // Interior seed + large band forces core refinement beyond surface skin alone.
    const std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.5}};
    auto skin = polymesh::mesh::graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1);
    auto seeded = polymesh::mesh::graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, {},
                                                          0.0, seeds, 1.0);
    REQUIRE(seeded.n_seed_cells > 0);
    REQUIRE(seeded.n_fine_cells >= skin.n_fine_cells);
    REQUIRE(seeded.mesh.tets.size() >= skin.mesh.tets.size());
}
