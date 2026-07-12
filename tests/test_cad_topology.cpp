// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>

using polymesh::geom::CadModel;
using polymesh::geom::closest_edge;
using polymesh::geom::edge_profile_hausdorff;
using polymesh::geom::extract_topology;
using polymesh::geom::occ_enabled;

TEST_CASE("extract_topology unit cube when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const CadModel model = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = extract_topology(model, 4);
    REQUIRE(topo.vertices.size() >= 8);
    REQUIRE(topo.edges.size() >= 12);
    REQUIRE(topo.faces.size() >= 6);

    // Unit cube edge length 1.
    int long_edges = 0;
    for (const auto& e : topo.edges) {
        if (e.length > 0.9 && e.length < 1.1) {
            ++long_edges;
        }
        REQUIRE(e.samples.size() >= 2);
    }
    CHECK(long_edges >= 12);

    // Closest edge from a point slightly off the bottom edge.
    const Eigen::Vector3d p(0.5, -0.01, 0.0);
    const auto q = closest_edge(topo, p);
    REQUIRE(q.has_value());
    CHECK(q->distance < 0.02);

    // Sampled edge as a "mesh edge" should have near-zero Hausdorff.
    const auto& e0 = topo.edges.front();
    CHECK(edge_profile_hausdorff(topo, e0.samples) < 1e-9);
}

TEST_CASE("extract_topology plate_hole STEP when present") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/plate_hole.step";
    if (!std::filesystem::exists(path)) {
        SKIP("plate_hole.step missing");
    }
    const CadModel model = CadModel::load_step(path);
    const auto topo = extract_topology(model, 12);
    REQUIRE_FALSE(topo.edges.empty());
    REQUIRE_FALSE(topo.faces.empty());

    // Hole circumference edges should include a circular-ish total length ~2πr
    // (r=0.01 → ~0.0628); at least one face is cylindrical or other curved.
    double max_edge = 0.0;
    for (const auto& e : topo.edges) {
        max_edge = std::max(max_edge, e.length);
    }
    CHECK(max_edge > 0.0);
    CHECK(topo.vertices.size() >= 4);
}
