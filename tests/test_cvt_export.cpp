// SPDX-License-Identifier: BSD-3-Clause
// G4: clipped Voronoi → PolyMesh polyhedra export.

#include "mesh/cvt_export.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/geogram_clip.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using polymesh::mesh::CellKind;
using polymesh::mesh::ClipBox;
using polymesh::mesh::DomainClipParams;
using polymesh::mesh::export_clipped_voronoi;
using polymesh::mesh::geogram_available;
using polymesh::mesh::seed_lattice_sites;

TEST_CASE("single site unit cube exports one hex-like poly cell", "[cvt][g4]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    std::vector<Eigen::Vector3d> sites = {Eigen::Vector3d(0.5, 0.5, 0.5)};
    const auto exp = export_clipped_voronoi(box, sites);
    REQUIRE(exp.stats.geogram_ok);
    REQUIRE(exp.stats.n_cells == 1);
    REQUIRE(exp.stats.n_empty_cells == 0);
    REQUIRE(exp.mesh.cells.size() == 1);
    REQUIRE(exp.mesh.cells[0].kind == CellKind::kPolyhedron);
    REQUIRE(exp.mesh.cells[0].faces.size() >= 6);  // box has 6 faces
    REQUIRE(exp.stats.sum_cell_volume == Approx(1.0).margin(1e-9));
    REQUIRE_NOTHROW(exp.mesh.check_validity());
}

TEST_CASE("two sites export two cells with one interior face", "[cvt][g4]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    std::vector<Eigen::Vector3d> sites = {Eigen::Vector3d(0.25, 0.5, 0.5),
                                          Eigen::Vector3d(0.75, 0.5, 0.5)};
    const auto exp = export_clipped_voronoi(box, sites);
    REQUIRE(exp.stats.n_cells == 2);
    REQUIRE(exp.stats.sum_cell_volume == Approx(1.0).margin(1e-8));
    REQUIRE(exp.stats.n_interior_faces >= 1);
    REQUIRE_NOTHROW(exp.mesh.check_validity());

    // Both cells reference the shared interior face.
    std::size_t shared = 0;
    for (const auto& f : exp.mesh.faces) {
        if (f.neighbour) {
            ++shared;
        }
    }
    REQUIRE(shared >= 1);
}

TEST_CASE("surface domain clip shrinks cell volume below AABB", "[cvt][g4][m5]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    // Unit cube AABB; solid is inset box [0.1,0.9]^3.
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);

    polymesh::geom::TriSurface solid;
    const double lo = 0.1, hi = 0.9;
    solid.vertices = {
        {lo, lo, lo}, {hi, lo, lo}, {hi, hi, lo}, {lo, hi, lo},
        {lo, lo, hi}, {hi, lo, hi}, {hi, hi, hi}, {lo, hi, hi},
    };
    solid.triangles = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
        {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
    };

    std::vector<Eigen::Vector3d> sites = {Eigen::Vector3d(0.5, 0.5, 0.5)};
    const auto aabb_only = export_clipped_voronoi(box, sites);
    REQUIRE(aabb_only.stats.n_cells == 1);
    REQUIRE(aabb_only.stats.sum_cell_volume == Approx(1.0).margin(1e-6));

    DomainClipParams dclip;
    dclip.surface = &solid;
    dclip.clip_radius = 2.0;  // whole domain
    const auto clipped = export_clipped_voronoi(box, sites, dclip);
    REQUIRE(clipped.stats.domain_clip_used);
    REQUIRE(clipped.stats.n_cells == 1);
    REQUIRE(clipped.stats.n_domain_plane_clips > 0);
    // Solid volume = 0.8^3 = 0.512
    REQUIRE(clipped.stats.sum_cell_volume == Approx(0.512).margin(1e-3));
    REQUIRE(clipped.stats.sum_cell_volume < aabb_only.stats.sum_cell_volume * 0.9);
}

TEST_CASE("lattice CVT sites export covering poly mesh", "[cvt][g4]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    auto sites = seed_lattice_sites(box, 2);  // 8 sites
    const auto exp = export_clipped_voronoi(box, sites);
    REQUIRE(exp.stats.n_cells == 8);
    REQUIRE(exp.stats.sum_cell_volume == Approx(1.0).margin(1e-6));
    REQUIRE(exp.mesh.cells.size() == 8);
    for (const auto& c : exp.mesh.cells) {
        REQUIRE(c.kind == CellKind::kPolyhedron);
        REQUIRE(c.faces.size() >= 4);
    }
    REQUIRE_NOTHROW(exp.mesh.check_validity());
}
