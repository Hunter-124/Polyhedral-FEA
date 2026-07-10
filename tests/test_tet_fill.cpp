// SPDX-License-Identifier: BSD-3-Clause
#include "geom/tri_surface.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace polymesh::mesh;
namespace pipeline = polymesh::pipeline;

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

TEST_CASE("tet_fill snap path: all tet volumes positive (B3 Jacobian safety)") {
    const auto surf = unit_box();
    surf.validate();
    // Explicit snap_boundary=true; coarser and finer h both must stay valid.
    for (const double h : {0.5, 0.25}) {
        const auto fill = tet_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, h, true);
        REQUIRE_FALSE(fill.tets.empty());
        REQUIRE_NOTHROW(check_tet_fill_geometry(fill, 0.0));
        for (const auto& n : fill.tets) {
            const double v = tet_signed_volume(fill.nodes[n[0]], fill.nodes[n[1]],
                                               fill.nodes[n[2]], fill.nodes[n[3]]);
            REQUIRE(v > 0.0);
        }
    }
}

TEST_CASE("L-domain public STL grid-fills with validity only (ADR-0015)") {
    // Documents expected product-mesh behaviour on the L fixture: load + mesh
    // + structural validity. Does NOT claim analytical L-domain energy accuracy
    // (GATE 1 Tier-1 uses structured imported meshes — ADR-0009).
    auto model = pipeline::Model::load("bench/geometries/public/l_domain.stl");
    REQUIRE(model.surface.triangles.size() >= 4);
    auto vol = pipeline::volume_mesh(model, 0.25, pipeline::VolumeMesher::kTetFill);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("not Delaunay") != std::string::npos);
    for (const auto& el : vol.mesh.elements) {
        REQUIRE(el.type == polymesh::fea::ElementType::kTet4);
        REQUIRE(el.nodes.size() == 4);
        const auto& a = vol.mesh.nodes[el.nodes[0]];
        const auto& b = vol.mesh.nodes[el.nodes[1]];
        const auto& c = vol.mesh.nodes[el.nodes[2]];
        const auto& d = vol.mesh.nodes[el.nodes[3]];
        const double v = tet_signed_volume(a, b, c, d);
        REQUIRE(v > 0.0);
        REQUIRE(std::isfinite(v));
    }
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
