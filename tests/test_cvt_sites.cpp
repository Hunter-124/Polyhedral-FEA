// SPDX-License-Identifier: BSD-3-Clause
// G3: constrained CVT sites + OCC wall project bridge.

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "mesh/cvt_sites.hpp"
#include "mesh/geogram_clip.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

using Catch::Approx;
using polymesh::mesh::ClipBox;
using polymesh::mesh::ConstrainedLloydParams;
using polymesh::mesh::ConstrainedSiteSeedParams;
using polymesh::mesh::constrained_lloyd_cvt;
using polymesh::mesh::geogram_available;
using polymesh::mesh::seed_constrained_cvt_sites;

namespace {

polymesh::geom::CadTopology make_synthetic_topo() {
    polymesh::geom::CadTopology topo;
    // Sharp edge along x-axis bottom of unit cube.
    polymesh::geom::CadEdge sharp;
    sharp.id = 0;
    sharp.feature = polymesh::geom::CadEdgeFeature::kSharp;
    sharp.length = 1.0;
    sharp.samples = {Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0.5, 0, 0),
                     Eigen::Vector3d(1, 0, 0)};
    topo.edges.push_back(sharp);

    // Seam — must never become a fixed protector.
    polymesh::geom::CadEdge seam;
    seam.id = 1;
    seam.feature = polymesh::geom::CadEdgeFeature::kSeam;
    seam.length = 1.0;
    seam.samples = {Eigen::Vector3d(0, 1, 0), Eigen::Vector3d(1, 1, 0)};
    topo.edges.push_back(seam);

    // Smooth — free wall only, not fixed.
    polymesh::geom::CadEdge smooth;
    smooth.id = 2;
    smooth.feature = polymesh::geom::CadEdgeFeature::kSmooth;
    smooth.length = 1.0;
    smooth.samples = {Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(1, 0, 1)};
    topo.edges.push_back(smooth);
    return topo;
}

}  // namespace

TEST_CASE("seed_constrained_cvt_sites fixes sharp only", "[cvt][g3]") {
    ClipBox box;
    const auto topo = make_synthetic_topo();
    ConstrainedSiteSeedParams p;
    p.interior_n_side = 2;
    p.sharp_min_sep_frac = 1e-4;

    const auto seeded = seed_constrained_cvt_sites(box, &topo, p);
    REQUIRE(seeded.n_sharp_fixed == 3);
    REQUIRE(seeded.n_seams_skipped == 1);
    REQUIRE(seeded.n_smooth_skipped == 1);
    REQUIRE(seeded.n_sharp_edges_used == 1);
    REQUIRE(seeded.n_interior_free > 0);

    std::size_t n_fixed = 0;
    for (const auto& s : seeded.sites) {
        if (s.fixed) {
            ++n_fixed;
            // All fixed samples are on the sharp edge z=y=0.
            REQUIRE(std::abs(s.pos.y()) < 1e-15);
            REQUIRE(std::abs(s.pos.z()) < 1e-15);
        }
    }
    REQUIRE(n_fixed == 3);
}

TEST_CASE("constrained_lloyd keeps sharp sites fixed", "[cvt][g3]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }

    ClipBox box;
    const auto topo = make_synthetic_topo();
    ConstrainedLloydParams p;
    p.seed.interior_n_side = 2;
    p.lloyd.max_iters = 8;
    p.project_final = false;  // no CAD

    const auto result = constrained_lloyd_cvt(box, nullptr, &topo, p);
    REQUIRE(result.lloyd_stats.geogram_ok);
    REQUIRE(result.seed_stats.n_sharp_fixed == 3);

    // Recompute expected fixed positions from seed.
    const auto seeded = seed_constrained_cvt_sites(box, &topo, p.seed);
    std::size_t checked = 0;
    for (std::size_t i = 0; i < seeded.sites.size(); ++i) {
        if (!seeded.sites[i].fixed) {
            continue;
        }
        REQUIRE(result.sites[i].fixed);
        REQUIRE(result.sites[i].pos.isApprox(seeded.sites[i].pos, 1e-14));
        ++checked;
    }
    REQUIRE(checked == 3);
}

TEST_CASE("OCC project free wall sites on cube STEP when available",
          "[cvt][g3][occ]") {
    if (!geogram_available()) {
        SKIP("POLYMESH_WITH_GEOGRAM is OFF");
    }
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OCC not enabled");
    }

    // Prefer a committed product STEP if present.
    namespace fs = std::filesystem;
    const std::vector<fs::path> candidates = {
        "tests/fixtures/parts/plate_hole.step",
        "tests/fixtures/unit_cube.step",
        "tests/fixtures/parts/cylinder.step",
    };
    fs::path step;
    for (const auto& c : candidates) {
        if (fs::exists(c)) {
            step = c;
            break;
        }
    }
    if (step.empty()) {
        SKIP("No STEP fixture found for OCC G3 smoke");
    }

    auto cad = polymesh::geom::CadModel::load_step(step);
    REQUIRE_FALSE(cad.empty());
    auto topo = polymesh::geom::extract_topology(cad, 4);

    ClipBox box;
    box.min = cad.bbox_min();
    box.max = cad.bbox_max();

    ConstrainedLloydParams p;
    p.seed.interior_n_side = 2;
    p.lloyd.max_iters = 4;
    p.project_final = true;
    p.wall_band_frac = 0.5;  // generous so some free sites hit the wall band

    const auto result = constrained_lloyd_cvt(box, &cad, &topo, p);
    REQUIRE(result.lloyd_stats.geogram_ok);
    REQUIRE(result.seed_stats.n_sharp_fixed > 0);
    // With OCC + wall band, at least attempt projections (may be zero if all
    // free sites stay deep interior on a thin part — still exercise path).
    REQUIRE(result.project_stats.n_failed == 0);
}
