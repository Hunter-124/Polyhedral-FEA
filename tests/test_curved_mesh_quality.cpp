// SPDX-License-Identifier: BSD-3-Clause
//
// Curved-geometry mesher scorecard (T0): authentic geometric metrics M1–M6 so
// hex can pass (or score far higher) while graded tet / hybrid zoo fail on
// rounded/circular features until quality fixes land.
//
// See plan: curved residual / LEB mid-chords / hole silhouette.
// Thresholds below are frozen from measured product fills at fixed equal h
// (not auto-h). ADR-0015: scores measure lattice+snap fidelity, not CAD Delaunay.

#include "fea/boundary_faces.hpp"
#include "fea/nodal_mesh.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/surface_metrics.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

using namespace polymesh;
namespace pipeline = polymesh::pipeline;
namespace fea = polymesh::fea;
namespace mesh = polymesh::mesh;

namespace {

// Kuhn 6-tet split (same as product fills) for hex solid volume.
constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
    {{0, 1, 2, 6}},
    {{0, 2, 3, 6}},
    {{0, 1, 5, 6}},
    {{0, 3, 7, 6}},
    {{0, 4, 5, 6}},
    {{0, 4, 7, 6}},
}};

double nodal_mesh_volume(const fea::NodalMesh& m) {
    double vol = 0.0;
    for (const auto& el : m.elements) {
        const auto& n = el.nodes;
        switch (el.type) {
        case fea::ElementType::kTet4:
        case fea::ElementType::kTet10:
            if (n.size() >= 4) {
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[0]], m.nodes[n[1]],
                                                        m.nodes[n[2]], m.nodes[n[3]]));
            }
            break;
        case fea::ElementType::kHex8:
        case fea::ElementType::kHex20:
            if (n.size() >= 8) {
                for (const auto& t : kCubeTets) {
                    vol += std::abs(mesh::tet_signed_volume(
                        m.nodes[n[static_cast<std::size_t>(t[0])]],
                        m.nodes[n[static_cast<std::size_t>(t[1])]],
                        m.nodes[n[static_cast<std::size_t>(t[2])]],
                        m.nodes[n[static_cast<std::size_t>(t[3])]]));
                }
            }
            break;
        case fea::ElementType::kPyramid5:
            if (n.size() >= 5) {
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[0]], m.nodes[n[1]],
                                                        m.nodes[n[2]], m.nodes[n[4]]));
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[0]], m.nodes[n[2]],
                                                        m.nodes[n[3]], m.nodes[n[4]]));
            }
            break;
        case fea::ElementType::kPrism6:
            if (n.size() >= 6) {
                // Two tets for wedge: (0,1,2,4)+(0,2,3,4) is wrong; use standard
                // (0,1,2,3)+(1,2,4,3)+(2,4,5,3) style base+extrusion approx:
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[0]], m.nodes[n[1]],
                                                        m.nodes[n[2]], m.nodes[n[3]]));
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[1]], m.nodes[n[2]],
                                                        m.nodes[n[4]], m.nodes[n[3]]));
                vol += std::abs(mesh::tet_signed_volume(m.nodes[n[2]], m.nodes[n[4]],
                                                        m.nodes[n[5]], m.nodes[n[3]]));
            }
            break;
        default:
            break;
        }
    }
    return vol;
}

std::vector<std::array<std::uint32_t, 4>> tet_connectivity(const fea::NodalMesh& m) {
    std::vector<std::array<std::uint32_t, 4>> tets;
    for (const auto& el : m.elements) {
        if ((el.type == fea::ElementType::kTet4 || el.type == fea::ElementType::kTet10) &&
            el.nodes.size() >= 4) {
            tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
        }
    }
    return tets;
}

struct Scorecard {
    std::string mesher;
    mesh::CurvedMeshMetrics m;
    std::size_t n_elems = 0;
    std::size_t n_nodes = 0;
    double h = 0.0;
};

Scorecard score_volume(const pipeline::Model& model, double h, pipeline::VolumeMesher mesher,
                       const char* name, double ref_volume,
                       const mesh::CircularFeature* circ) {
    Scorecard sc;
    sc.mesher = name;
    sc.h = h;
    const bool feature = (mesher == pipeline::VolumeMesher::kGradedTet ||
                          mesher == pipeline::VolumeMesher::kHybrid);
    auto vol = pipeline::volume_mesh(model, h, mesher, /*skin_layers=*/2, feature);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    sc.n_elems = vol.mesh.elements.size();
    sc.n_nodes = vol.mesh.nodes.size();

    auto faces = fea::extract_boundary_faces(vol.mesh);
    if (faces.empty() && !vol.boundary_quads.empty()) {
        faces = vol.boundary_quads;
    }
    REQUIRE_FALSE(faces.empty());

    const double mesh_vol = nodal_mesh_volume(vol.mesh);
    auto tets = tet_connectivity(vol.mesh);
    const std::vector<std::array<std::uint32_t, 4>>* tet_ptr =
        tets.empty() ? nullptr : &tets;

    sc.m = mesh::evaluate_curved_mesh_quality(model.surface, vol.mesh.nodes, faces, h,
                                              mesh_vol, ref_volume, circ, tet_ptr);
    return sc;
}

void dump_score(const Scorecard& sc) {
    CAPTURE(sc.mesher);
    CAPTURE(sc.m.composite_score);
    CAPTURE(sc.m.m1_max);
    CAPTURE(sc.m.m2_max);
    CAPTURE(sc.m.m3_rel_volume_err);
    CAPTURE(sc.m.m4_radial_rel);
    CAPTURE(sc.m.m5_max_azimuth_gap);
    CAPTURE(sc.m.m6_min_boundary_aspect);
    CAPTURE(sc.n_elems);
    CAPTURE(sc.n_nodes);
    CAPTURE(sc.h);
    INFO(std::format(
        "{}: score={:.4f} M1max={:.4g} M2max={:.4g} M3={:.4g} M4={:.4g} M5={:.4g} "
        "M6={:.4g} elems={} nodes={} h={:.4g}",
        sc.mesher, sc.m.composite_score, sc.m.m1_max, sc.m.m2_max, sc.m.m3_rel_volume_err,
        sc.m.m4_radial_rel, sc.m.m5_max_azimuth_gap, sc.m.m6_min_boundary_aspect, sc.n_elems,
        sc.n_nodes, sc.h));
}

// --- Frozen thresholds (calibrated 2026-07-10, product fills, equal h) ---
// Measured composites (approx):
//   sphere  h=0.15*ext: hex≈0.849  graded≈0.804
//   cylinder h=0.12*ext: hex≈0.860  graded≈0.784
//   hole plate h=0.10*ext: hex≈0.568  graded≈0.462  (strong residual signal)
//
// DOCUMENT_BUG pattern: while quality bugs remain, graded/hybrid must sit *below*
// these bars (test stays green documenting the defect). After quality fix, invert
// to REQUIRE(score >= kPassFloor*) and raise bars to match hex competitiveness.

constexpr double kHexFloorSphere = 0.70;
constexpr double kHexFloorCylinder = 0.70;
constexpr double kHexFloorHole = 0.40;

// "Still buggy" ceilings — measured graded sits under these; raise after fix.
constexpr double kBugCeilSphere = 0.83;    // graded≈0.804
constexpr double kBugCeilCylinder = 0.81; // graded≈0.784
constexpr double kBugCeilHole = 0.55;     // graded≈0.462

// Hex must outrank graded (relative). Fraction of hex score graded must stay under.
constexpr double kGradedLagFraction = 0.98; // 0.804 < 0.849*0.98
constexpr double kHybridLagFraction = 0.99;

} // namespace

TEST_CASE("curved scorecard: sphere hex passes, graded/hybrid lag or fail bar",
          "[curved][mesher]") {
    const std::filesystem::path geom = "bench/geometries/edge/sphere.stl";
    if (!std::filesystem::exists(geom)) {
        SKIP("sphere.stl missing");
    }
    const auto model = pipeline::Model::load(geom.string());
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    const double h = 0.15 * extent; // coarse CI-friendly; equal for all meshers
    // Low-res unit sphere inscribed in [0,1]^3, R≈0.5 at centre 0.5^3.
    constexpr double kPi = 3.14159265358979323846;
    const double R = 0.5;
    const double Vref = (4.0 / 3.0) * kPi * R * R * R;
    mesh::CircularFeature circ;
    circ.axis_point = {0.5, 0.5, 0.5};
    circ.axis_dir = {0.0, 0.0, 1.0};
    circ.radius = R;
    circ.select_band = 0.85 * h;

    const auto hex =
        score_volume(model, h, pipeline::VolumeMesher::kHexFill, "hex", Vref, &circ);
    const auto graded =
        score_volume(model, h, pipeline::VolumeMesher::kGradedTet, "graded", Vref, &circ);
    const auto hybrid =
        score_volume(model, h, pipeline::VolumeMesher::kHybrid, "hybrid", Vref, &circ);

    dump_score(hex);
    dump_score(graded);
    dump_score(hybrid);

    REQUIRE(hex.m.composite_score >= kHexFloorSphere);
    // DOCUMENT_BUG: graded LEB mid-chords / curved residual lag hex on pure curves.
    // After quality fix: REQUIRE(graded.m.composite_score >= hex.m.composite_score * 0.95);
    REQUIRE(graded.m.composite_score < kBugCeilSphere);
    REQUIRE(graded.m.composite_score < hex.m.composite_score * kGradedLagFraction);
    // DOCUMENT_BUG: hybrid free-surface stair + size jumps / bloat on pure curves.
    REQUIRE(hybrid.m.composite_score < kBugCeilSphere);
    const bool hybrid_lags =
        hybrid.m.composite_score < hex.m.composite_score * kHybridLagFraction;
    const bool hybrid_bloated =
        hybrid.n_elems > hex.n_elems * 4 &&
        hybrid.m.m2_max + 1e-12 >= hex.m.m2_max * 0.85;
    REQUIRE((hybrid_lags || hybrid_bloated));
}

TEST_CASE("curved scorecard: cylinder_prism hex ranks above graded/hybrid",
          "[curved][mesher]") {
    const std::filesystem::path geom = "bench/geometries/public/cylinder_prism.stl";
    if (!std::filesystem::exists(geom)) {
        SKIP("cylinder_prism.stl missing");
    }
    const auto model = pipeline::Model::load(geom.string());
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    const double h = 0.12 * extent;
    // Regular octagonal prism ≈ cylinder R=0.5, H=1 about z; solid volume of
    // regular octagon * H. Regular octagon area = 2(1+√2)s² with R=0.5 →
    // apothem/vertex: use π R² H as ref (fixture is cylinder-ish; relative OK).
    constexpr double kPi = 3.14159265358979323846;
    const double R = 0.5;
    const double H = 1.0;
    const double Vref = kPi * R * R * H;
    mesh::CircularFeature circ;
    circ.axis_point = {0.0, 0.0, 0.5};
    circ.axis_dir = {0.0, 0.0, 1.0};
    circ.radius = R;
    circ.select_band = 0.9 * h;

    const auto hex =
        score_volume(model, h, pipeline::VolumeMesher::kHexFill, "hex", Vref, &circ);
    const auto graded =
        score_volume(model, h, pipeline::VolumeMesher::kGradedTet, "graded", Vref, &circ);
    const auto hybrid =
        score_volume(model, h, pipeline::VolumeMesher::kHybrid, "hybrid", Vref, &circ);

    dump_score(hex);
    dump_score(graded);
    dump_score(hybrid);

    REQUIRE(hex.m.composite_score >= kHexFloorCylinder);
    REQUIRE(graded.m.composite_score < kBugCeilCylinder);
    REQUIRE(graded.m.composite_score < hex.m.composite_score * kGradedLagFraction);
    // Prefer radial error discrimination when circular feature selects nodes.
    if (hex.m.n_circular_nodes >= 4 && graded.m.n_circular_nodes >= 4) {
        // Graded should not dominate hex on M4 until fixed; allow equality.
        REQUIRE(graded.m.m4_radial_rel + 1e-9 >= hex.m.m4_radial_rel * 0.85);
    }
    REQUIRE(hybrid.m.composite_score < kBugCeilCylinder);
    const bool hybrid_lags =
        hybrid.m.composite_score < hex.m.composite_score * kHybridLagFraction;
    const bool hybrid_bloated =
        hybrid.n_elems > hex.n_elems * 4 &&
        hybrid.m.m2_max + 1e-12 >= hex.m.m2_max * 0.85;
    REQUIRE((hybrid_lags || hybrid_bloated));
}

TEST_CASE("curved scorecard: hole plate test.stl graded residual / ranking",
          "[curved][mesher]") {
    const std::filesystem::path geom = "tests/fixtures/test.stl";
    if (!std::filesystem::exists(geom)) {
        SKIP("test.stl missing");
    }
    const auto model = pipeline::Model::load(geom.string());
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    // Slightly coarser than GUI auto for CI; equal h for all three.
    const double h = 0.10 * extent;

    // Hole geometry: derive centre/radius from bbox mid + curvature scale is hard
    // without CAD; use circular feature only if we can estimate R from surface.
    // Fallback: residual + face-sample only (no M4/M5) — still discriminates.
    const Eigen::Vector3d c = 0.5 * (model.bbox_min + model.bbox_max);
    // Heuristic hole radius from progress notes Rκ≈9.68 on auto-h path for this
    // fixture; use mid-plate feature band from half min-xy extent * 0.15.
    const double half_xy =
        0.5 * std::min(model.bbox_max[0] - model.bbox_min[0],
                       model.bbox_max[1] - model.bbox_min[1]);
    mesh::CircularFeature circ;
    circ.axis_point = c;
    circ.axis_dir = {0.0, 0.0, 1.0};
    circ.radius = 0.25 * half_xy; // order-of-magnitude; select_band wide
    circ.select_band = 1.25 * h;

    // Unknown solid volume — disable M3.
    const double Vref = -1.0;

    const auto hex =
        score_volume(model, h, pipeline::VolumeMesher::kHexFill, "hex", Vref, &circ);
    const auto graded =
        score_volume(model, h, pipeline::VolumeMesher::kGradedTet, "graded", Vref, &circ);
    const auto hybrid =
        score_volume(model, h, pipeline::VolumeMesher::kHybrid, "hybrid", Vref, &circ);

    dump_score(hex);
    dump_score(graded);
    dump_score(hybrid);

    REQUIRE(hex.m.composite_score >= kHexFloorHole);

    // Primary documented graded failure: large free-surface residual vs h.
    // Historical: max|d| ~0.4 h on this part after LEB unsnap (M1 or M2).
    const bool graded_residual_bad =
        (graded.m.m1_max > 0.20 * h) || (graded.m.m2_max > 0.30 * h);
    const bool graded_score_lag =
        graded.m.composite_score < hex.m.composite_score * kGradedLagFraction;
    REQUIRE((graded_residual_bad || graded_score_lag));
    REQUIRE(graded.m.composite_score < kBugCeilHole);

    // Hybrid: may have near-zero node residual but lag composite / efficiency.
    const bool hybrid_score_lag =
        hybrid.m.composite_score < hex.m.composite_score * kHybridLagFraction;
    const bool hybrid_m2_worse = hybrid.m.m2_max > hex.m.m2_max * 1.15 + 1e-12;
    const bool hybrid_bloated =
        hybrid.n_elems > hex.n_elems * 8 && hybrid.m.m2_max >= hex.m.m2_max * 0.5;
    REQUIRE((hybrid_score_lag || hybrid_m2_worse || hybrid_bloated));
}
