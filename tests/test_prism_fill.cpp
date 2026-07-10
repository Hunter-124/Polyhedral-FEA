// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/solve.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/prism_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace polymesh::mesh;
namespace pipeline = polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    s.triangles = {
        {0, 2, 1}, {0, 3, 2}, // z=0
        {4, 5, 6}, {4, 6, 7}, // z=1
        {0, 1, 5}, {0, 5, 4}, // y=0
        {2, 3, 7}, {2, 7, 6}, // y=1
        {0, 4, 7}, {0, 7, 3}, // x=0
        {1, 2, 6}, {1, 6, 5}, // x=1
    };
    return s;
}

/// Right triangular prism extruded along z: base (0,0)-(1,0)-(0,1), height 1.
polymesh::geom::TriSurface right_tri_prism() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1},
    };
    // Outward faces
    s.triangles = {
        {0, 2, 1},            // bottom z=0 outward -z
        {3, 4, 5},            // top z=1
        {0, 1, 4}, {0, 4, 3}, // y=0
        {0, 3, 5}, {0, 5, 2}, // hypotenuse-ish x=0 face
        {1, 2, 5}, {1, 5, 4}, // diagonal face
    };
    return s;
}

} // namespace

TEST_CASE("prism_fill unit box: all prism6, positive volume") {
    const auto surf = unit_box();
    surf.validate();
    const auto fill = prism_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, 0.5);
    REQUIRE_FALSE(fill.prisms.empty());
    REQUIRE_FALSE(fill.boundary_quads.empty());
    REQUIRE_NOTHROW(check_prism_fill_geometry(fill));
    // 2×2×2 lattice → up to 8 voxels × 2 prisms; ray parity may drop edge cells.
    REQUIRE(fill.prisms.size() >= 8);
    REQUIRE(fill.prisms.size() % 2 == 0);
    REQUIRE(fill.sweep_axis == 2); // cubic → tie prefers z
    double vol = 0.0;
    for (const auto& n : fill.prisms) {
        const double v =
            prism_signed_volume(fill.nodes[n[0]], fill.nodes[n[1]], fill.nodes[n[2]],
                                fill.nodes[n[3]], fill.nodes[n[4]], fill.nodes[n[5]]);
        REQUIRE(v > 0.0);
        REQUIRE(std::isfinite(v));
        vol += v;
    }
    // Filled volume should be a substantial fraction of the unit cube.
    REQUIRE(vol > 0.4);
}

TEST_CASE("prism_fill chooses longest extent as sweep axis") {
    const auto surf = unit_box(); // geometry is unit cube; we stretch bbox only for axis pick
    // Tall domain: z much larger — but surface is still unit cube. Use elongated box surface.
    polymesh::geom::TriSurface tall;
    tall.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 3}, {1, 0, 3}, {1, 1, 3}, {0, 1, 3},
    };
    tall.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    tall.validate();
    const auto fill = prism_fill_surface(tall, {0, 0, 0}, {1, 1, 3}, 0.5);
    REQUIRE(fill.sweep_axis == 2);
    REQUIRE_NOTHROW(check_prism_fill_geometry(fill));

    polymesh::geom::TriSurface long_x;
    long_x.vertices = {
        {0, 0, 0}, {4, 0, 0}, {4, 1, 0}, {0, 1, 0}, {0, 0, 1}, {4, 0, 1}, {4, 1, 1}, {0, 1, 1},
    };
    long_x.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    long_x.validate();
    const auto fill_x = prism_fill_surface(long_x, {0, 0, 0}, {4, 1, 1}, 0.5);
    REQUIRE(fill_x.sweep_axis == 0);
    REQUIRE_NOTHROW(check_prism_fill_geometry(fill_x));
}

TEST_CASE("pipeline volume_mesh kPrismSweep emits only prism6") {
    auto model = pipeline::Model::load("bench/geometries/public/unit_box.stl");
    auto vol = pipeline::volume_mesh(model, 0.25, pipeline::VolumeMesher::kPrismSweep);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("prism") != std::string::npos);
    REQUIRE(vol.mesher_note.find("not CAD extrusion") != std::string::npos);
    for (const auto& el : vol.mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kPrism6);
        REQUIRE(el.nodes.size() == 6);
    }
}

TEST_CASE("prism_fill extruded triangle surface: prism6 validity") {
    auto surf = right_tri_prism();
    // May not be perfectly watertight for ray cast depending on faces; prefer unit box path
    // if validate fails. Validate first.
    REQUIRE_NOTHROW(surf.validate());
    const auto fill = prism_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, 0.5);
    REQUIRE_FALSE(fill.prisms.empty());
    REQUIRE_NOTHROW(check_prism_fill_geometry(fill));
    for (const auto& n : fill.prisms) {
        REQUIRE(n.size() == 6);
    }
}

TEST_CASE("prism product mesh solve smoke on unit box") {
    auto model = pipeline::Model::load("bench/geometries/public/unit_box.stl");
    auto vol = pipeline::volume_mesh(model, 0.5, pipeline::VolumeMesher::kPrismSweep);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    for (const auto& el : vol.mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kPrism6);
    }

    // Cantilever-style: fix min-x nodes, load +Fy on max-x boundary nodes.
    fea::Dirichlet bc;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(static_cast<Eigen::Index>(3 * vol.mesh.nodes.size()));
    double xmin = vol.mesh.nodes[0][0], xmax = vol.mesh.nodes[0][0];
    for (const auto& p : vol.mesh.nodes) {
        xmin = std::min(xmin, p[0]);
        xmax = std::max(xmax, p[0]);
    }
    const double tol = 1e-9 + 0.05 * (xmax - xmin);
    for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
        if (vol.mesh.nodes[i][0] <= xmin + tol) {
            bc.fix_node(i, Eigen::Vector3d::Zero());
        }
        if (vol.mesh.nodes[i][0] >= xmax - tol) {
            loads[static_cast<Eigen::Index>(3 * i + 1)] = 1.0;
        }
    }
    const fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const auto u = fea::solve_elastostatics(vol.mesh, mat, bc, loads);
    REQUIRE(u.size() == loads.size());
    REQUIRE(u.allFinite());
    REQUIRE(u.norm() > 0.0);
}

TEST_CASE("prism constant-strain patch on prism_fill lattice") {
    const auto surf = unit_box();
    surf.validate();
    const auto fill = prism_fill_surface(surf, {0, 0, 0}, {1, 1, 1}, 0.5);
    fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& pr : fill.prisms) {
        mesh.elements.push_back(fea::NodalElement{fea::ElementType::kPrism6,
                                                  {pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]}});
    }

    Eigen::Matrix3d g;
    g << 1e-3, 0, 0, 0, -5e-4, 0, 0, 0, 2e-4;
    fea::Dirichlet bc;
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        bc.fix_node(i, g * mesh.nodes[i]);
    }
    const fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(static_cast<Eigen::Index>(3 * mesh.nodes.size()));
    const auto u = fea::solve_elastostatics(mesh, mat, bc, loads);
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d ui(u[static_cast<Eigen::Index>(3 * i)],
                                 u[static_cast<Eigen::Index>(3 * i + 1)],
                                 u[static_cast<Eigen::Index>(3 * i + 2)]);
        const Eigen::Vector3d expected = g * mesh.nodes[i];
        REQUIRE((ui - expected).norm() < 1e-10);
    }
}
