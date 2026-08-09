// SPDX-License-Identifier: BSD-3-Clause
// G4: clipped Voronoi → PolyMesh polyhedra export.

#include "mesh/cvt_export.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/geogram_clip.hpp"

#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(_OPENMP)
#include <omp.h>
#endif

#include <array>
#include <cmath>

using Catch::Approx;
using polymesh::mesh::CellKind;
using polymesh::mesh::ClipBox;
using polymesh::mesh::DomainClipParams;
using polymesh::mesh::DomainTet;
using polymesh::mesh::export_clipped_voronoi;
using polymesh::mesh::export_rvd_tet_clipped;
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
    REQUIRE(exp.mesh.cells[0].faces.size() >= 6); // box has 6 faces
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
    dclip.clip_radius = 2.0; // whole domain
    const auto clipped = export_clipped_voronoi(box, sites, dclip);
    REQUIRE(clipped.stats.domain_clip_used);
    REQUIRE(clipped.stats.n_cells == 1);
    REQUIRE(clipped.stats.n_domain_plane_clips > 0);
    // Solid volume = 0.8^3 = 0.512
    REQUIRE(clipped.stats.sum_cell_volume == Approx(0.512).margin(1e-3));
    REQUIRE(clipped.stats.sum_cell_volume < aabb_only.stats.sum_cell_volume * 0.9);
}

TEST_CASE("rvd tet clip keeps volume inside a cube of tets", "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    // Unit cube as 5–6 tets is tedious; one tet + site inside should produce
    // a non-empty piece with volume < tet volume.
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);
    DomainTet tet;
    tet.v0 = {0, 0, 0};
    tet.v1 = {1, 0, 0};
    tet.v2 = {0, 1, 0};
    tet.v3 = {0, 0, 1};
    tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
    const Eigen::Vector3d e1 = tet.v1 - tet.v0;
    const Eigen::Vector3d e2 = tet.v2 - tet.v0;
    const Eigen::Vector3d e3 = tet.v3 - tet.v0;
    const double tet_vol = std::abs(e1.dot(e2.cross(e3))) / 6.0;

    std::vector<Eigen::Vector3d> sites = {tet.centroid};
    const auto exp =
        export_rvd_tet_clipped(box, sites, std::span<const DomainTet>(&tet, 1), 2.0);
    REQUIRE(exp.stats.domain_clip_used);
    REQUIRE(exp.stats.n_cells >= 1);
    REQUIRE(exp.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(exp.stats.sum_cell_volume > 0.0);
    REQUIRE(exp.stats.sum_cell_volume <= tet_vol * 1.01 + 1e-9);
    REQUIRE_NOTHROW(exp.mesh.check_validity());
}

TEST_CASE("rvd tet clip removes scaffold interfaces from the free boundary",
          "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);

    const std::array<Eigen::Vector3d, 8> vertices{{
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        {0, 0, 1},
        {1, 0, 1},
        {1, 1, 1},
        {0, 1, 1},
    }};
    constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
        {0, 1, 2, 6},
        {0, 2, 3, 6},
        {0, 3, 7, 6},
        {0, 7, 4, 6},
        {0, 4, 5, 6},
        {0, 5, 1, 6},
    }};
    std::vector<DomainTet> tets;
    tets.reserve(kCubeTets.size());
    for (const auto& ids : kCubeTets) {
        DomainTet tet;
        tet.v0 = vertices[static_cast<std::size_t>(ids[0])];
        tet.v1 = vertices[static_cast<std::size_t>(ids[1])];
        tet.v2 = vertices[static_cast<std::size_t>(ids[2])];
        tet.v3 = vertices[static_cast<std::size_t>(ids[3])];
        tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
        tets.push_back(tet);
    }

    const std::vector<Eigen::Vector3d> sites = {
        {0.25, 0.5, 0.5},
        {0.75, 0.5, 0.5},
    };
    const auto exp = export_rvd_tet_clipped(box, sites, tets, 2.0);
    REQUIRE(exp.stats.n_unpaired_bisector_faces == 0);
    REQUIRE(exp.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(exp.stats.n_invalid_face_claims == 0);
    REQUIRE(exp.stats.n_coalesced_faces > 0);
    REQUIRE(exp.stats.n_coalesced_face_fragments > 0);
    REQUIRE(exp.stats.n_cells == 2);
    REQUIRE_NOTHROW(exp.mesh.check_validity());

    const auto repeated = export_rvd_tet_clipped(box, sites, tets, 2.0);
    REQUIRE(repeated.stats.n_unpaired_bisector_faces == 0);
    REQUIRE(repeated.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(repeated.stats.n_invalid_face_claims == 0);
    CHECK(repeated.stats.n_coalesced_faces == exp.stats.n_coalesced_faces);
    CHECK(repeated.stats.n_coalesced_face_fragments == exp.stats.n_coalesced_face_fragments);
    REQUIRE(repeated.mesh.vertices.size() == exp.mesh.vertices.size());
    REQUIRE(repeated.mesh.faces.size() == exp.mesh.faces.size());
    REQUIRE(repeated.mesh.cells.size() == exp.mesh.cells.size());
    for (std::size_t i = 0; i < exp.mesh.vertices.size(); ++i) {
        CHECK((repeated.mesh.vertices[i] - exp.mesh.vertices[i]).squaredNorm() == 0.0);
    }
    for (std::size_t i = 0; i < exp.mesh.faces.size(); ++i) {
        CHECK(repeated.mesh.faces[i].vertices == exp.mesh.faces[i].vertices);
        CHECK(repeated.mesh.faces[i].owner == exp.mesh.faces[i].owner);
        CHECK(repeated.mesh.faces[i].neighbour == exp.mesh.faces[i].neighbour);
    }
    for (std::size_t i = 0; i < exp.mesh.cells.size(); ++i) {
        CHECK(repeated.mesh.cells[i].faces == exp.mesh.cells[i].faces);
    }
    REQUIRE_NOTHROW(exp.mesh.check_geometry());

    // The six Kuhn tets are only an integration scaffold. Every free face of
    // the merged two-cell RVD must lie on one of the six unit-cube planes.
    for (const auto& face : exp.mesh.faces) {
        if (face.neighbour || face.vertices.empty()) {
            continue;
        }
        bool on_domain_boundary = false;
        for (int axis = 0; axis < 3; ++axis) {
            for (double side : {0.0, 1.0}) {
                bool on_plane = true;
                for (const auto vertex : face.vertices) {
                    REQUIRE(vertex < exp.mesh.vertices.size());
                    on_plane =
                        on_plane && std::abs(exp.mesh.vertices[vertex][axis] - side) < 1e-10;
                }
                on_domain_boundary = on_domain_boundary || on_plane;
            }
        }
        CHECK(on_domain_boundary);
    }
}

TEST_CASE("rvd weld topology is invariant under large rigid translation", "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    const auto export_cube = [](const Eigen::Vector3d& translation) {
        ClipBox box;
        box.min = translation;
        box.max = translation + Eigen::Vector3d::Ones();
        const std::array<Eigen::Vector3d, 8> vertices{{
            translation + Eigen::Vector3d(0, 0, 0),
            translation + Eigen::Vector3d(1, 0, 0),
            translation + Eigen::Vector3d(1, 1, 0),
            translation + Eigen::Vector3d(0, 1, 0),
            translation + Eigen::Vector3d(0, 0, 1),
            translation + Eigen::Vector3d(1, 0, 1),
            translation + Eigen::Vector3d(1, 1, 1),
            translation + Eigen::Vector3d(0, 1, 1),
        }};
        constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
            {0, 1, 2, 6},
            {0, 2, 3, 6},
            {0, 3, 7, 6},
            {0, 7, 4, 6},
            {0, 4, 5, 6},
            {0, 5, 1, 6},
        }};
        std::vector<DomainTet> tets;
        for (const auto& ids : kCubeTets) {
            DomainTet tet;
            tet.v0 = vertices[static_cast<std::size_t>(ids[0])];
            tet.v1 = vertices[static_cast<std::size_t>(ids[1])];
            tet.v2 = vertices[static_cast<std::size_t>(ids[2])];
            tet.v3 = vertices[static_cast<std::size_t>(ids[3])];
            tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
            tets.push_back(tet);
        }
        const std::array<Eigen::Vector3d, 2> sites{
            translation + Eigen::Vector3d(0.25, 0.5, 0.5),
            translation + Eigen::Vector3d(0.75, 0.5, 0.5),
        };
        return export_rvd_tet_clipped(box, sites, tets, 2.0);
    };

    const Eigen::Vector3d translation(17179869184.0, -34359738368.0, 68719476736.0);
    const auto base = export_cube(Eigen::Vector3d::Zero());
    const auto moved = export_cube(translation);
    REQUIRE(base.stats.n_invalid_face_claims == 0);
    REQUIRE(moved.stats.n_invalid_face_claims == 0);
    REQUIRE(base.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(moved.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(base.stats.n_unpaired_bisector_faces == 0);
    REQUIRE(moved.stats.n_unpaired_bisector_faces == 0);
    REQUIRE(moved.mesh.vertices.size() == base.mesh.vertices.size());
    REQUIRE(moved.mesh.faces.size() == base.mesh.faces.size());
    REQUIRE(moved.mesh.cells.size() == base.mesh.cells.size());
    for (std::size_t i = 0; i < base.mesh.vertices.size(); ++i) {
        CHECK(((moved.mesh.vertices[i] - translation) - base.mesh.vertices[i]).norm() < 1e-4);
    }
    for (std::size_t i = 0; i < base.mesh.faces.size(); ++i) {
        CHECK(moved.mesh.faces[i].vertices == base.mesh.faces[i].vertices);
        CHECK(moved.mesh.faces[i].owner == base.mesh.faces[i].owner);
        CHECK(moved.mesh.faces[i].neighbour == base.mesh.faces[i].neighbour);
    }
    for (std::size_t i = 0; i < base.mesh.cells.size(); ++i) {
        CHECK(moved.mesh.cells[i].faces == base.mesh.cells[i].faces);
    }
}

TEST_CASE("rvd exact faces reject overlapping third claims", "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);
    DomainTet tet;
    tet.v0 = {0, 0, 0};
    tet.v1 = {1, 0, 0};
    tet.v2 = {0, 1, 0};
    tet.v3 = {0, 0, 1};
    tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
    const std::array<DomainTet, 4> overlapping{tet, tet, tet, tet};
    const std::array<Eigen::Vector3d, 1> sites{tet.centroid};
    const auto exp = export_rvd_tet_clipped(box, sites, overlapping, 2.0);
    CHECK(exp.stats.n_invalid_face_claims > 0);
    CHECK(exp.stats.n_unpaired_scaffold_faces > 0);
}

TEST_CASE("rvd rejects an unmatched internal scaffold claim", "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);

    DomainTet valid;
    valid.v0 = {0, 0, 0};
    valid.v1 = {1, 0, 0};
    valid.v2 = {0, 1, 0};
    valid.v3 = {0, 0, 1};
    valid.centroid = 0.25 * (valid.v0 + valid.v1 + valid.v2 + valid.v3);

    // This degenerate neighbour contributes the exact shared scaffold face to
    // multiplicity, but cannot produce the opposite clipped-cell claim.
    DomainTet degenerate;
    degenerate.v0 = valid.v1;
    degenerate.v1 = valid.v2;
    degenerate.v2 = valid.v3;
    degenerate.v3 = 0.5 * (valid.v2 + valid.v3);
    degenerate.centroid =
        0.25 * (degenerate.v0 + degenerate.v1 + degenerate.v2 + degenerate.v3);

    const std::array<DomainTet, 2> tets{valid, degenerate};
    const std::array<Eigen::Vector3d, 1> sites{valid.centroid};
    const auto exp = export_rvd_tet_clipped(box, sites, tets, 2.0);
    REQUIRE(exp.stats.n_unpaired_scaffold_faces > 0);

    // The unmatched x+y+z=1 cut is removed, never reclassified as domain skin.
    for (const auto& face : exp.mesh.faces) {
        if (face.neighbour) {
            continue;
        }
        bool on_internal_cut = true;
        for (const auto vertex : face.vertices) {
            REQUIRE(vertex < exp.mesh.vertices.size());
            on_internal_cut =
                on_internal_cut && std::abs(exp.mesh.vertices[vertex].sum() - 1.0) < 1e-10;
        }
        CHECK_FALSE(on_internal_cut);
    }
}

TEST_CASE("rvd splits disconnected regions of one site into separate cells",
          "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);
    DomainTet lower;
    lower.v0 = {0.0, 0.0, 0.0};
    lower.v1 = {0.4, 0.0, 0.0};
    lower.v2 = {0.0, 0.4, 0.0};
    lower.v3 = {0.0, 0.0, 0.4};
    lower.centroid = 0.25 * (lower.v0 + lower.v1 + lower.v2 + lower.v3);
    DomainTet upper;
    upper.v0 = {0.6, 0.6, 0.6};
    upper.v1 = {1.0, 0.6, 0.6};
    upper.v2 = {0.6, 1.0, 0.6};
    upper.v3 = {0.6, 0.6, 1.0};
    upper.centroid = 0.25 * (upper.v0 + upper.v1 + upper.v2 + upper.v3);
    const std::array<DomainTet, 2> tets{lower, upper};
    const std::array<Eigen::Vector3d, 1> sites{Eigen::Vector3d(0.5, 0.5, 0.5)};
    const auto exp = export_rvd_tet_clipped(box, sites, tets, 2.0);
    REQUIRE(exp.stats.n_cells == 2);
    REQUIRE(exp.stats.n_split_site_components == 1);
    REQUIRE(exp.stats.n_unpaired_scaffold_faces == 0);
    REQUIRE(exp.stats.n_invalid_face_claims == 0);
    REQUIRE_NOTHROW(exp.mesh.check_geometry());
}

TEST_CASE("rvd export is identical across OpenMP thread counts", "[cvt][m5][rvd]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
#if !defined(_OPENMP)
    SKIP("OpenMP is unavailable");
#else
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);
    const std::array<Eigen::Vector3d, 8> vertices{{
        {0, 0, 0},
        {1, 0, 0},
        {1, 1, 0},
        {0, 1, 0},
        {0, 0, 1},
        {1, 0, 1},
        {1, 1, 1},
        {0, 1, 1},
    }};
    constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
        {0, 1, 2, 6},
        {0, 2, 3, 6},
        {0, 3, 7, 6},
        {0, 7, 4, 6},
        {0, 4, 5, 6},
        {0, 5, 1, 6},
    }};
    std::vector<DomainTet> tets;
    for (const auto& ids : kCubeTets) {
        DomainTet tet;
        tet.v0 = vertices[static_cast<std::size_t>(ids[0])];
        tet.v1 = vertices[static_cast<std::size_t>(ids[1])];
        tet.v2 = vertices[static_cast<std::size_t>(ids[2])];
        tet.v3 = vertices[static_cast<std::size_t>(ids[3])];
        tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
        tets.push_back(tet);
    }
    const auto sites = seed_lattice_sites(box, 4);

    struct OmpSettingsGuard {
        int threads = 1;
        int dynamic = 0;
        ~OmpSettingsGuard() {
            omp_set_dynamic(0);
            omp_set_num_threads(threads);
            omp_set_dynamic(dynamic);
        }
    };
    [[maybe_unused]] const OmpSettingsGuard omp_settings{omp_get_max_threads(),
                                                         omp_get_dynamic()};
    omp_set_dynamic(0);
    omp_set_num_threads(1);
    REQUIRE(omp_get_max_threads() == 1);
    const auto serial = export_rvd_tet_clipped(box, sites, tets, 2.0);

    omp_set_num_threads(4);
    REQUIRE(omp_get_max_threads() == 4);
    int parallel_team_size = 0;
#pragma omp parallel
    {
#pragma omp single
        parallel_team_size = omp_get_num_threads();
    }
    if (parallel_team_size < 2) {
        SKIP("OpenMP runtime cannot form a parallel team");
    }
    const auto parallel = export_rvd_tet_clipped(box, sites, tets, 2.0);

    REQUIRE(serial.stats.n_sites == parallel.stats.n_sites);
    REQUIRE(serial.stats.n_cells == parallel.stats.n_cells);
    REQUIRE(serial.stats.n_empty_cells == parallel.stats.n_empty_cells);
    REQUIRE(serial.stats.n_split_site_components == parallel.stats.n_split_site_components);
    REQUIRE(serial.stats.n_faces == parallel.stats.n_faces);
    REQUIRE(serial.stats.n_interior_faces == parallel.stats.n_interior_faces);
    REQUIRE(serial.stats.n_boundary_faces == parallel.stats.n_boundary_faces);
    REQUIRE(serial.stats.n_unpaired_bisector_faces ==
            parallel.stats.n_unpaired_bisector_faces);
    REQUIRE(serial.stats.n_unpaired_scaffold_faces ==
            parallel.stats.n_unpaired_scaffold_faces);
    REQUIRE(serial.stats.n_invalid_face_claims == parallel.stats.n_invalid_face_claims);
    REQUIRE(serial.stats.n_coalesced_face_fragments ==
            parallel.stats.n_coalesced_face_fragments);
    REQUIRE(serial.stats.n_coalesced_faces == parallel.stats.n_coalesced_faces);
    REQUIRE(serial.stats.n_vertices == parallel.stats.n_vertices);
    REQUIRE(serial.stats.n_domain_plane_clips == parallel.stats.n_domain_plane_clips);
    REQUIRE(serial.stats.sum_cell_volume == parallel.stats.sum_cell_volume);
    REQUIRE(serial.stats.geogram_ok == parallel.stats.geogram_ok);
    REQUIRE(serial.stats.domain_clip_used == parallel.stats.domain_clip_used);
    REQUIRE(serial.site_to_cell == parallel.site_to_cell);
    REQUIRE(serial.mesh.vertices.size() == parallel.mesh.vertices.size());
    REQUIRE(serial.mesh.faces.size() == parallel.mesh.faces.size());
    REQUIRE(serial.mesh.cells.size() == parallel.mesh.cells.size());
    for (std::size_t i = 0; i < serial.mesh.vertices.size(); ++i) {
        CHECK((serial.mesh.vertices[i] - parallel.mesh.vertices[i]).squaredNorm() == 0.0);
    }
    for (std::size_t i = 0; i < serial.mesh.faces.size(); ++i) {
        CHECK(serial.mesh.faces[i].vertices == parallel.mesh.faces[i].vertices);
        CHECK(serial.mesh.faces[i].owner == parallel.mesh.faces[i].owner);
        CHECK(serial.mesh.faces[i].neighbour == parallel.mesh.faces[i].neighbour);
    }
    for (std::size_t i = 0; i < serial.mesh.cells.size(); ++i) {
        CHECK(serial.mesh.cells[i].kind == parallel.mesh.cells[i].kind);
        CHECK(serial.mesh.cells[i].faces == parallel.mesh.cells[i].faces);
    }
#endif
}

TEST_CASE("lattice CVT sites export covering poly mesh", "[cvt][g4]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    auto sites = seed_lattice_sites(box, 2); // 8 sites
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
