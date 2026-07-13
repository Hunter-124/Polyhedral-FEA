// SPDX-License-Identifier: BSD-3-Clause
// G2: Lloyd CVT + 1/h³ density (same size field contract as N_pred).

#include "mesh/cvt_lloyd.hpp"
#include "mesh/geogram_clip.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using polymesh::mesh::ClipBox;
using polymesh::mesh::CvtLloydParams;
using polymesh::mesh::CvtSite;
using polymesh::mesh::density_from_size;
using polymesh::mesh::geogram_available;
using polymesh::mesh::lloyd_cvt;
using polymesh::mesh::restricted_voronoi_cell;
using polymesh::mesh::restricted_voronoi_centroid;
using polymesh::mesh::seed_lattice_sites;

TEST_CASE("density_from_size is 1/h^3", "[cvt][g2]") {
    REQUIRE(density_from_size(2.0) == Approx(0.125));
    REQUIRE(density_from_size(1.0) == Approx(1.0));
}

TEST_CASE("two-site restricted Voronoi bisects the unit cube", "[cvt][g2]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    const Eigen::Vector3d a(0.25, 0.5, 0.5);
    const Eigen::Vector3d b(0.75, 0.5, 0.5);
    std::vector<Eigen::Vector3d> others_a = {b};
    std::vector<Eigen::Vector3d> others_b = {a};

    const auto ca = restricted_voronoi_cell(box, a, others_a);
    const auto cb = restricted_voronoi_cell(box, b, others_b);
    REQUIRE(ca.has_value());
    REQUIRE(cb.has_value());
    REQUIRE_FALSE(ca->empty);
    REQUIRE_FALSE(cb->empty);
    REQUIRE(ca->volume == Approx(0.5).margin(1e-9));
    REQUIRE(cb->volume == Approx(0.5).margin(1e-9));
    REQUIRE(ca->volume + cb->volume == Approx(1.0).margin(1e-9));
}

TEST_CASE("uniform Lloyd on lattice stays near lattice (low energy move)",
          "[cvt][g2]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    auto sites = seed_lattice_sites(box, 3);  // 27 sites
    CvtLloydParams p;
    p.max_iters = 20;
    p.move_tol_rel = 1e-5;
    // Uniform density (no size_at) — geometric CVT; cubic lattice is near-opt.
    const auto result = lloyd_cvt(box, sites, p);
    REQUIRE(result.stats.geogram_ok);
    REQUIRE(result.stats.n_free == 27);
    REQUIRE(result.stats.n_iters >= 1);
    // Lattice of 3³ is already a good CVT seed — residual motion small.
    REQUIRE(result.stats.max_move < 0.15);
}

TEST_CASE("fixed sites do not move under Lloyd", "[cvt][g2]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    std::vector<CvtSite> sites;
    CvtSite fixed;
    fixed.pos = Eigen::Vector3d(0.1, 0.1, 0.1);
    fixed.fixed = true;
    sites.push_back(fixed);
    for (int i = 0; i < 4; ++i) {
        CvtSite s;
        s.pos = Eigen::Vector3d(0.3 + 0.1 * i, 0.5, 0.5);
        s.fixed = false;
        sites.push_back(s);
    }

    CvtLloydParams p;
    p.max_iters = 10;
    const auto result = lloyd_cvt(box, sites, p);
    REQUIRE(result.stats.n_fixed == 1);
    REQUIRE(result.positions[0].isApprox(fixed.pos, 1e-15));
}

TEST_CASE("graded 1/h^3 density pulls free sites toward fine region",
          "[cvt][g2]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    // h small near x=0 → ρ large → sites should concentrate toward x=0.
    ClipBox box;
    box.min = Eigen::Vector3d(0, 0, 0);
    box.max = Eigen::Vector3d(1, 1, 1);

    std::vector<CvtSite> sites = seed_lattice_sites(box, 2);  // 8 free sites
    CvtLloydParams p;
    p.max_iters = 40;
    p.move_tol_rel = 1e-4;
    p.size_at = [](const Eigen::Vector3d& x) {
        // h from 0.05 at x=0 to 0.25 at x=1 — same functional form one would
        // feed N_pred / GeometrySizing (here a 1D ramp for a clear bias).
        return 0.05 + 0.20 * x.x();
    };

    const auto result = lloyd_cvt(box, sites, p);
    REQUIRE(result.stats.geogram_ok);
    REQUIRE(result.stats.n_iters >= 1);

    double mean_x0 = 0.0;
    double mean_x1 = 0.0;
    for (const auto& s : sites) {
        mean_x0 += s.pos.x();
    }
    for (const auto& pnt : result.positions) {
        mean_x1 += pnt.x();
    }
    mean_x0 /= static_cast<double>(sites.size());
    mean_x1 /= static_cast<double>(result.positions.size());

    // Weighted CVT should shift the mean site x toward the fine (small-h) side.
    REQUIRE(mean_x1 < mean_x0 - 0.02);
}

TEST_CASE("restricted_voronoi_centroid matches barycenter when h constant",
          "[cvt][g2]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    const Eigen::Vector3d site(0.3, 0.4, 0.5);
    std::vector<Eigen::Vector3d> others = {
        Eigen::Vector3d(0.8, 0.5, 0.5),
        Eigen::Vector3d(0.2, 0.9, 0.2),
    };
    const auto cell = restricted_voronoi_cell(box, site, others);
    REQUIRE(cell.has_value());
    auto size = [](const Eigen::Vector3d&) { return 0.1; };
    const auto c = restricted_voronoi_centroid(box, site, others, size, 1e-12);
    REQUIRE(c.has_value());
    REQUIRE(c->x() == Approx(cell->barycenter.x()).margin(1e-9));
    REQUIRE(c->y() == Approx(cell->barycenter.y()).margin(1e-9));
    REQUIRE(c->z() == Approx(cell->barycenter.z()).margin(1e-9));
}
