// SPDX-License-Identifier: BSD-3-Clause
#include "geom/tri_surface.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/tet_fill.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace polymesh::mesh;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    // Outward CCW faces
    s.triangles = {
        {0, 2, 1}, {0, 3, 2}, // z=0 (outward -z: 0,1,2 would be inward; use 0,2,1)
        {4, 5, 6}, {4, 6, 7}, // z=1
        {0, 1, 5}, {0, 5, 4}, // y=0
        {2, 3, 7}, {2, 7, 6}, // y=1
        {0, 4, 7}, {0, 7, 3}, // x=0
        {1, 2, 6}, {1, 6, 5}, // x=1
    };
    // Fix bottom face orientation for outward -z: normal should be (0,0,-1)
    // (1-0)×(2-0) for 0,1,2 = z-positive — so for bottom outward use 0,2,1
    s.triangles[0] = {0, 2, 1};
    s.triangles[1] = {0, 3, 2};
    return s;
}

} // namespace

TEST_CASE("tet_fill unit box produces positive-volume tet4s") {
    const auto surf = unit_box();
    surf.validate();
    const auto fill = tet_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, 0.5);
    REQUIRE_FALSE(fill.tets.empty());
    REQUIRE_FALSE(fill.boundary_quads.empty());
    REQUIRE_NOTHROW(check_tet_fill_geometry(fill));
    // Coarse 2x2x2 grid → up to 8 voxels * 6 tets, some may be outside parity
    REQUIRE(fill.tets.size() >= 6);
}

TEST_CASE("single tet poly mesh passes geometry") {
    PolyMesh m;
    m.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    // Outward faces from tet
    m.faces.push_back(Face{{0, 2, 1}, 0, {}});
    m.faces.push_back(Face{{0, 1, 3}, 0, {}});
    m.faces.push_back(Face{{0, 3, 2}, 0, {}});
    m.faces.push_back(Face{{1, 2, 3}, 0, {}});
    m.cells.push_back(Cell{CellKind::kTet, {0, 1, 2, 3}});
    REQUIRE_NOTHROW(m.check_geometry());
}
