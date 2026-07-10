// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/quality.hpp"
#include "mesh/tet_fill.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace polymesh::mesh;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}

} // namespace

TEST_CASE("uniform tet fill is face-conforming (no hanging faces)") {
    const auto s = unit_box();
    auto fill = tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, false);
    REQUIRE_FALSE(fill.tets.empty());
    // Margin: free-surface faces may sit ~0.5 cell inside stair-case; use ~0.2 h.
    const auto conf =
        tet4_face_conformity(fill.nodes, fill.tets, {0, 0, 0}, {1, 1, 1}, 0.2 * fill.h);
    REQUIRE(conf.n_nonconforming == 0);
    REQUIRE(conf.n_hanging_faces == 0);
    REQUIRE(conf.is_conforming);
}

TEST_CASE("graded tet fill is face-conforming after LEB (ADR-0018)") {
    const auto s = unit_box();
    // Skin layers force fine marking near free surface → used to hang at 2:1.
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, /*skin=*/2);
    REQUIRE_FALSE(graded.mesh.tets.empty());
    REQUIRE(graded.n_fine_cells > 0);
    check_tet_fill_geometry(graded.mesh);

    const double margin = 0.2 * graded.h_coarse;
    const auto conf = tet4_face_conformity(graded.mesh.nodes, graded.mesh.tets, {0, 0, 0},
                                           {1, 1, 1}, margin);
    REQUIRE(conf.n_nonconforming == 0);
    REQUIRE(conf.n_hanging_faces == 0);
    REQUIRE(conf.is_conforming);

    // Volume of unit box should be recovered (geometry tiles the solid).
    double vol = 0.0;
    for (const auto& t : graded.mesh.tets) {
        vol += std::abs(tet_signed_volume(graded.mesh.nodes[t[0]], graded.mesh.nodes[t[1]],
                                          graded.mesh.nodes[t[2]], graded.mesh.nodes[t[3]]));
    }
    REQUIRE(std::abs(vol - 1.0) < 1e-9);
}

TEST_CASE("graded with features remains conforming") {
    const auto s = unit_box();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.3, 1, edges, 0.5);
    REQUIRE(graded.n_feature_cells > 0);
    const auto conf = tet4_face_conformity(graded.mesh.nodes, graded.mesh.tets, {0, 0, 0},
                                           {1, 1, 1}, 0.25 * graded.h_coarse);
    REQUIRE(conf.is_conforming);
}
