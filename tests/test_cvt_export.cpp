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
