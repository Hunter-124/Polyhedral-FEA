// SPDX-License-Identifier: BSD-3-Clause
//
// Curved-geometry mesher scorecard (T0): authentic geometric metrics M1–M6.
//
// History: hex used to outrank graded tet / hybrid zoo on rounded features.
// Root causes fixed 2026-07-10: hybrid v3 emitted non-conforming pyramid
// transitions (cracked meshes, exposed interior faces) → v4 conforming
// polygon-fan closure; graded snap left degenerate sliver caps (min boundary
// aspect ~1e-18) and hole-void jut nodes → S4 cap collapse + S5 void carve +
// second snap round. The test now *enforces* competitiveness: hybrid must
// match/beat hex, graded must stay within a fixed fraction with clean
// residuals. Thresholds from measured product fills at fixed equal h (not
// auto-h). ADR-0015: scores measure lattice+snap fidelity, not CAD Delaunay.

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
                    vol += std::abs(
                        mesh::tet_signed_volume(m.nodes[n[static_cast<std::size_t>(t[0])]],
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
    const std::vector<std::array<std::uint32_t, 4>>* tet_ptr = tets.empty() ? nullptr : &tets;

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
    INFO(
        std::format("{}: score={:.4f} M1max={:.4g} M2max={:.4g} M3={:.4g} M4={:.4g} M5={:.4g} "
                    "M6={:.4g} elems={} nodes={} h={:.4g}",
                    sc.mesher, sc.m.composite_score, sc.m.m1_max, sc.m.m2_max,
                    sc.m.m3_rel_volume_err, sc.m.m4_radial_rel, sc.m.m5_max_azimuth_gap,
                    sc.m.m6_min_boundary_aspect, sc.n_elems, sc.n_nodes, sc.h));
}

// --- Frozen thresholds (recalibrated 2026-07-10 after quality fixes) ---
// Measured composites (equal h, product fills):
//   sphere   h=0.15*ext: hex≈0.849  graded≈0.799  hybrid≈0.896
//   cylinder h=0.12*ext: hex≈0.860  graded≈0.780  hybrid≈0.860
//   hole     h=0.10*ext: hex≈0.568  graded≈0.530  hybrid≈0.577
// Residuals: graded/hybrid M1max ≈ 0 (≤0.06 h worst), min boundary tet
// aspect ≥ ~0.04 (was ~1e-18 degenerate).

constexpr double kHexFloorSphere = 0.70;
constexpr double kHexFloorCylinder = 0.70;
constexpr double kHexFloorHole = 0.40;

// Post-fix pass floors (absolute) — margin ~0.03-0.05 under measured.
constexpr double kGradedFloorSphere = 0.75;
constexpr double kGradedFloorCylinder = 0.74;
constexpr double kGradedFloorHole = 0.48;
constexpr double kHybridFloorSphere = 0.85;
constexpr double kHybridFloorCylinder = 0.82;
constexpr double kHybridFloorHole = 0.50;

// Relative competitiveness: graded (all-tet, pays the M6 tet-aspect term hex
// never does) must stay within 0.88×hex; hybrid must effectively match hex.
constexpr double kGradedKeepFraction = 0.88; // measured ≥0.906×hex
constexpr double kHybridKeepFraction = 0.97; // measured ≥0.9999×hex

// Residual hygiene: boundary nodes must sit on the surface (no juts/cracks).
constexpr double kResidualFrac = 0.08;      // ×h, M1max bound (measured ≤0.053)
constexpr double kMinBoundaryAspect = 0.02; // measured ≥0.044 (was ~1e-18)

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
    // Post-fix: graded competitive (all-tet pays M6; keep-fraction of hex).
    REQUIRE(graded.m.composite_score >= kGradedFloorSphere);
    REQUIRE(graded.m.composite_score >= hex.m.composite_score * kGradedKeepFraction);
    REQUIRE(graded.m.m1_max <= kResidualFrac * h);
    REQUIRE(graded.m.m6_min_boundary_aspect >= kMinBoundaryAspect);
    // Post-fix: hybrid v4 (conforming fan transitions) matches or beats hex.
    REQUIRE(hybrid.m.composite_score >= kHybridFloorSphere);
    REQUIRE(hybrid.m.composite_score >= hex.m.composite_score * kHybridKeepFraction);
    REQUIRE(hybrid.m.m1_max <= kResidualFrac * h);
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
    REQUIRE(graded.m.composite_score >= kGradedFloorCylinder);
    REQUIRE(graded.m.composite_score >= hex.m.composite_score * kGradedKeepFraction);
    REQUIRE(graded.m.m1_max <= kResidualFrac * h);
    REQUIRE(graded.m.m6_min_boundary_aspect >= kMinBoundaryAspect);
    REQUIRE(hybrid.m.composite_score >= kHybridFloorCylinder);
    REQUIRE(hybrid.m.composite_score >= hex.m.composite_score * kHybridKeepFraction);
    REQUIRE(hybrid.m.m1_max <= kResidualFrac * h);
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
    const double half_xy = 0.5 * std::min(model.bbox_max[0] - model.bbox_min[0],
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

    // Post-fix: the historical graded defect (jut nodes ~0.4 h into the hole
    // void, degenerate caps) is gone — node residual is bounded and the
    // composite keeps pace with hex despite the under-resolved hole (R≈2h).
    REQUIRE(graded.m.composite_score >= kGradedFloorHole);
    REQUIRE(graded.m.composite_score >= hex.m.composite_score * kGradedKeepFraction);
    REQUIRE(graded.m.m1_max <= kResidualFrac * h);
    REQUIRE(graded.m.m6_min_boundary_aspect >= kMinBoundaryAspect);
    REQUIRE(hybrid.m.composite_score >= kHybridFloorHole);
    REQUIRE(hybrid.m.composite_score >= hex.m.composite_score * kHybridKeepFraction);
    REQUIRE(hybrid.m.m1_max <= kResidualFrac * h);
}
