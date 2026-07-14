// SPDX-License-Identifier: BSD-3-Clause
#include "geom/tri_surface.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"
#include "geom/stl.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/public/l_domain.stl"));
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

TEST_CASE("tet_fill unit box has no diagonal voids (ray parity)") {
    // Regression: shared face-diagonal edges used to double-count crossings so
    // cells with cx≈cy were marked outside — visible tunnels through the cube.
    const auto surf = unit_box();
    for (const double h : {0.5, 0.25, 0.1, 0.3}) {
        const auto fill = tet_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, h, true);
        REQUIRE_NOTHROW(check_tet_fill_geometry(fill));
        // Full AABB coverage: mesh nodes must reach all six faces.
        Eigen::Vector3d mn = fill.nodes.front();
        Eigen::Vector3d mx = fill.nodes.front();
        for (const auto& p : fill.nodes) {
            mn = mn.cwiseMin(p);
            mx = mx.cwiseMax(p);
        }
        REQUIRE(mn[0] == Catch::Approx(0.0).margin(1e-12));
        REQUIRE(mn[1] == Catch::Approx(0.0).margin(1e-12));
        REQUIRE(mn[2] == Catch::Approx(0.0).margin(1e-12));
        REQUIRE(mx[0] == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(mx[1] == Catch::Approx(1.0).margin(1e-12));
        REQUIRE(mx[2] == Catch::Approx(1.0).margin(1e-12));
        // Volume of Kuhn-split voxels ≈ box volume (all cells inside).
        double vol = 0.0;
        for (const auto& n : fill.tets) {
            vol += tet_signed_volume(fill.nodes[n[0]], fill.nodes[n[1]], fill.nodes[n[2]],
                                     fill.nodes[n[3]]);
        }
        REQUIRE(vol == Catch::Approx(1.0).margin(1e-9));
        // 6 tets per interior voxel; unit box must fill every voxel in the lattice.
        REQUIRE(fill.tets.size() % 6 == 0);
        const auto n_vox = fill.tets.size() / 6;
        const int nx = std::max(1, static_cast<int>(std::ceil(1.0 / h - 1e-14)));
        REQUIRE(n_vox == static_cast<std::size_t>(nx) * nx * nx);
    }
}

TEST_CASE("tet_fill public unit_box.stl solid (no interior gaps)") {
    auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    for (const double h : {0.25, 0.1, 0.3}) {
        auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kTetFill);
        REQUIRE_FALSE(vol.mesh.elements.empty());
        REQUIRE_NOTHROW(vol.mesh.check_validity());
        Eigen::Vector3d mn = vol.mesh.nodes.front();
        Eigen::Vector3d mx = vol.mesh.nodes.front();
        for (const auto& p : vol.mesh.nodes) {
            mn = mn.cwiseMin(p);
            mx = mx.cwiseMax(p);
        }
        REQUIRE((mn - model.bbox_min).norm() < 1e-9);
        REQUIRE((mx - model.bbox_max).norm() < 1e-9);
    }
}

TEST_CASE("tet_fill edge-case STLs: exact volume on AABB solids") {
    // thin plate, slender beam, offset box — no interior voids; volume = AABB vol
    // when the solid is a filled rectangular brick.
    struct Case {
        const char* path;
        double volume;
    };
    const Case cases[] = {
        {"bench/geometries/edge/unit_box.stl", 1.0},
        {"bench/geometries/edge/offset_box.stl", 1.0},
        {"bench/geometries/edge/thin_plate.stl", 0.1},    // 2×1×0.05
        {"bench/geometries/edge/slender_beam.stl", 0.16}, // 4×0.2×0.2
    };
    for (const auto& c : cases) {
        auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl(c.path));
        auto vol = pipeline::volume_mesh(model, 0.1, pipeline::VolumeMesher::kTetFill);
        REQUIRE_NOTHROW(vol.mesh.check_validity());
        double mesh_vol = 0.0;
        for (const auto& el : vol.mesh.elements) {
            REQUIRE(el.type == polymesh::fea::ElementType::kTet4);
            mesh_vol +=
                tet_signed_volume(vol.mesh.nodes[el.nodes[0]], vol.mesh.nodes[el.nodes[1]],
                                  vol.mesh.nodes[el.nodes[2]], vol.mesh.nodes[el.nodes[3]]);
        }
        REQUIRE(mesh_vol == Catch::Approx(c.volume).margin(1e-9 * std::max(1.0, c.volume)));
        Eigen::Vector3d mn = vol.mesh.nodes.front();
        Eigen::Vector3d mx = vol.mesh.nodes.front();
        for (const auto& p : vol.mesh.nodes) {
            mn = mn.cwiseMin(p);
            mx = mx.cwiseMax(p);
        }
        REQUIRE((mn - model.bbox_min).cwiseAbs().maxCoeff() < 1e-9);
        REQUIRE((mx - model.bbox_max).cwiseAbs().maxCoeff() < 1e-9);
    }
}

TEST_CASE("tet_fill sphere edge STL is solid (no crash, positive volume)") {
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/edge/sphere.stl"));
    auto vol = pipeline::volume_mesh(model, 0.1, pipeline::VolumeMesher::kTetFill);
    REQUIRE(vol.mesh.elements.size() > 100);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    double mesh_vol = 0.0;
    for (const auto& el : vol.mesh.elements) {
        mesh_vol +=
            tet_signed_volume(vol.mesh.nodes[el.nodes[0]], vol.mesh.nodes[el.nodes[1]],
                              vol.mesh.nodes[el.nodes[2]], vol.mesh.nodes[el.nodes[3]]);
    }
    // Staircased sphere ≈ 0.47 vs analytical 4/3 π r³ ≈ 0.524 at h=0.1 — band only.
    REQUIRE(mesh_vol > 0.35);
    REQUIRE(mesh_vol < 0.55);
}
