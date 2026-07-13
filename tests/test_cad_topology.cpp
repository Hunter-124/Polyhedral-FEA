// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>

using polymesh::geom::CadEdgeFeature;
using polymesh::geom::CadModel;
using polymesh::geom::chordal_edge_metrics;
using polymesh::geom::closest_edge;
using polymesh::geom::count_edge_features;
using polymesh::geom::edge_profile_hausdorff;
using polymesh::geom::edge_profile_hausdorff_filtered;
using polymesh::geom::extract_topology;
using polymesh::geom::occ_enabled;

TEST_CASE("extract_topology unit cube when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const CadModel model = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = extract_topology(model, 4);
    REQUIRE(topo.vertices.size() >= 8);
    REQUIRE(topo.edges.size() >= 12);
    REQUIRE(topo.faces.size() >= 6);

    // Unit cube edge length 1.
    int long_edges = 0;
    for (const auto& e : topo.edges) {
        if (e.length > 0.9 && e.length < 1.1) {
            ++long_edges;
        }
        REQUIRE(e.samples.size() >= 2);
    }
    CHECK(long_edges >= 12);

    // Closest edge from a point slightly off the bottom edge.
    const Eigen::Vector3d p(0.5, -0.01, 0.0);
    const auto q = closest_edge(topo, p);
    REQUIRE(q.has_value());
    CHECK(q->distance < 0.02);

    // Sampled edge as a "mesh edge" should have near-zero Hausdorff.
    const auto& e0 = topo.edges.front();
    CHECK(edge_profile_hausdorff(topo, e0.samples) < 1e-9);

    // Cube: all 12 edges are 90° creases → sharp (no seams on a box).
    const auto counts = count_edge_features(topo);
    CHECK(counts.n_sharp >= 12);
    CHECK(counts.n_seam == 0);
    for (const auto& e : topo.edges) {
        if (e.length > 0.9 && e.length < 1.1) {
            CHECK(e.feature == CadEdgeFeature::kSharp);
            // Interior dihedral ~π/2; allow loose tol for normal sampling.
            if (e.dihedral_rad > 0.0) {
                CHECK(e.dihedral_rad > 0.5);
                CHECK(e.dihedral_rad < 2.0);
            }
        }
    }
}

TEST_CASE("extract_topology plate_hole STEP when present") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/plate_hole.step";
    if (!std::filesystem::exists(path)) {
        SKIP("plate_hole.step missing");
    }
    const CadModel model = CadModel::load_step(path);
    const auto topo = extract_topology(model, 12);
    REQUIRE_FALSE(topo.edges.empty());
    REQUIRE_FALSE(topo.faces.empty());

    // Hole circumference edges should include a circular-ish total length ~2πr
    // (r=0.01 → ~0.0628); at least one face is cylindrical or other curved.
    double max_edge = 0.0;
    for (const auto& e : topo.edges) {
        max_edge = std::max(max_edge, e.length);
    }
    CHECK(max_edge > 0.0);
    CHECK(topo.vertices.size() >= 4);

    // Hole rim: cylinder ∩ plane → sharp closed circles (r≈0.01 → L≈2πr).
    int sharp_hole_rims = 0;
    for (const auto& e : topo.edges) {
        if (e.feature != CadEdgeFeature::kSharp || e.samples.size() < 4) {
            continue;
        }
        // Closed circular: endpoints coincide; use chord mid-sample sagitta.
        const Eigen::Vector3d& a = e.samples.front();
        const Eigen::Vector3d& b = e.samples.back();
        const bool closed = (b - a).norm() < 1e-9 * std::max(1.0, e.length);
        double sagitta = 0.0;
        if (closed) {
            const Eigen::Vector3d& p0 = e.samples.front();
            const Eigen::Vector3d& p1 = e.samples[e.samples.size() / 4];
            const Eigen::Vector3d& p2 = e.samples[e.samples.size() / 2];
            const Eigen::Vector3d chord = p2 - p0;
            const double c2 = chord.squaredNorm();
            if (c2 > 1e-18) {
                const double t = std::clamp((p1 - p0).dot(chord) / c2, 0.0, 1.0);
                sagitta = (p1 - (p0 + t * chord)).norm();
            }
        } else {
            const Eigen::Vector3d mid = e.samples[e.samples.size() / 2];
            const Eigen::Vector3d ab = b - a;
            const double ab2 = ab.squaredNorm();
            if (ab2 > 1e-18) {
                const double t = std::clamp((mid - a).dot(ab) / ab2, 0.0, 1.0);
                sagitta = (mid - (a + t * ab)).norm();
            }
        }
        // Hole rims: L ~ 0.06 and positive sagitta (radius ~0.01).
        if (e.length > 0.04 && e.length < 0.1 && sagitta > 1e-3) {
            ++sharp_hole_rims;
        }
    }
    CHECK(sharp_hole_rims >= 1);
}

TEST_CASE("extract_topology cylinder classifies seams/smooth and sharp rims") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const CadModel model = CadModel::load_step(path);
    const auto topo = extract_topology(model, 16);
    REQUIRE_FALSE(topo.edges.empty());

    const auto counts = count_edge_features(topo);
    // End-cap rims (plane ∩ cylinder) are hard features.
    CHECK(counts.n_sharp >= 2);
    // Parameterization seam and/or G1 wall splits on the cylindrical wall.
    CHECK(counts.n_seam + counts.n_smooth >= 1);

    // Circular end-cap rims are closed sharp curves (plane ∩ cylinder).
    int sharp_circles = 0;
    for (const auto& e : topo.edges) {
        if (e.feature != CadEdgeFeature::kSharp || e.samples.size() < 4) {
            continue;
        }
        const Eigen::Vector3d& p0 = e.samples.front();
        const Eigen::Vector3d& p1 = e.samples[e.samples.size() / 4];
        const Eigen::Vector3d& p2 = e.samples[e.samples.size() / 2];
        // Closed circle: endpoints coincide; sagitta of quarter-arc vs diameter.
        const bool closed =
            (e.samples.back() - p0).norm() < 1e-9 * std::max(1.0, e.length);
        if (!closed) {
            continue;
        }
        const Eigen::Vector3d chord = p2 - p0;
        const double c2 = chord.squaredNorm();
        if (c2 < 1e-18) {
            continue;
        }
        const double t = std::clamp((p1 - p0).dot(chord) / c2, 0.0, 1.0);
        const double sagitta = (p1 - (p0 + t * chord)).norm();
        if (sagitta > 1e-3 * std::max(1.0, e.length)) {
            ++sharp_circles;
        }
    }
    CHECK(sharp_circles >= 1);
}

TEST_CASE("chordal_edge_metrics near-zero on exact CAD samples") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const CadModel model = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = extract_topology(model, 8);
    REQUIRE_FALSE(topo.edges.empty());

    // Polyline exactly on a sharp CAD edge → residual and chordal ≈ 0.
    const auto& e = topo.edges.front();
    REQUIRE(e.feature == CadEdgeFeature::kSharp);
    const auto m = chordal_edge_metrics(topo, e.samples, /*h=*/0.1, true);
    CHECK(m.hausdorff < 1e-9);
    CHECK(m.max_chordal < 1e-9);
    CHECK(m.n_segments == static_cast<int>(e.samples.size()) - 1);
    // Straight cube edges → κ≈0 → efficiency left at 0.
    CHECK(m.max_efficiency == 0.0);

    // Filtered Hausdorff on the same polyline.
    CHECK(edge_profile_hausdorff_filtered(topo, e.samples, true) < 1e-9);

    // Off-edge polyline should report positive residual.
    std::vector<Eigen::Vector3d> offset = e.samples;
    for (auto& p : offset) {
        p.y() += 0.05;
    }
    const auto m_off = chordal_edge_metrics(topo, offset, 0.1, true);
    CHECK(m_off.hausdorff > 0.04);
    CHECK(m_off.max_chordal > 0.04);
}

TEST_CASE("chordal_edge_metrics_segments e≈1 on synthetic circle chord") {
    // Manual CadTopology (no OCC): unit circle in xy, R=1, κ=1.
    // A chord of length ℓ subtending angle θ has mid-chord sagitta
    //   d = R (1 − cos(θ/2)) ≈ ℓ²/(8R) for small θ,
    // so e = d / (ℓ²κ/8) ≈ 1.
    using polymesh::geom::CadEdge;
    using polymesh::geom::CadTopology;
    using polymesh::geom::MeshEdgeSegment;
    using polymesh::geom::chordal_edge_metrics_segments;

    constexpr double kR = 1.0;
    constexpr double kKappa = 1.0 / kR;
    constexpr double kTheta = 30.0 * 3.14159265358979323846 / 180.0; // 30°
    constexpr int kNSamples = 65;

    CadTopology topo;
    CadEdge ce;
    ce.id = 0;
    ce.feature = CadEdgeFeature::kSharp;
    ce.samples.reserve(static_cast<std::size_t>(kNSamples));
    ce.kappa_samples.reserve(static_cast<std::size_t>(kNSamples));
    for (int i = 0; i < kNSamples; ++i) {
        const double a = 2.0 * 3.14159265358979323846 * static_cast<double>(i) /
                         static_cast<double>(kNSamples - 1);
        // Open chain covering full circle (last ≈ first).
        ce.samples.emplace_back(kR * std::cos(a), kR * std::sin(a), 0.0);
        ce.kappa_samples.push_back(kKappa);
    }
    ce.length = 0.0;
    for (std::size_t k = 1; k < ce.samples.size(); ++k) {
        ce.length += (ce.samples[k] - ce.samples[k - 1]).norm();
    }
    topo.edges.push_back(std::move(ce));

    // Chord from −θ/2 to +θ/2 about +x (mid at (1,0) direction).
    const double half = 0.5 * kTheta;
    const Eigen::Vector3d a(kR * std::cos(-half), kR * std::sin(-half), 0.0);
    const Eigen::Vector3d b(kR * std::cos(half), kR * std::sin(half), 0.0);
    const double ell = (b - a).norm();
    REQUIRE(ell > 1e-9);

    // Exact circular sagitta and small-angle theory.
    const Eigen::Vector3d mid = 0.5 * (a + b);
    const double d_exact = kR - mid.norm(); // mid is on the x-axis inside the circle
    const double theory = (ell * ell * kKappa) / 8.0;
    REQUIRE(theory > 1e-12);
    // Sanity: for 30° the approximation is within a few percent.
    CHECK(d_exact / theory > 0.9);
    CHECK(d_exact / theory < 1.15);

    const auto m = chordal_edge_metrics_segments(topo, {MeshEdgeSegment{a, b}}, true);
    CHECK(m.n_segments == 1);
    // Residual is vs discrete CAD samples, so allow small sample error vs exact circle.
    CHECK(m.max_chordal == Catch::Approx(d_exact).margin(1e-3));
    CHECK(m.hausdorff == Catch::Approx(d_exact).margin(1e-3));
    // Efficiency e = d/(ℓ²κ/8) should be O(1) ≈ 1 (mission: within 10–15%).
    REQUIRE(m.max_efficiency > 0.0);
    CHECK(m.max_efficiency == Catch::Approx(1.0).margin(0.15));
    CHECK(m.max_efficiency > 0.85);
    CHECK(m.max_efficiency < 1.20);
    // Reported residual should track the circular sagitta order of magnitude.
    CHECK(m.max_chordal / theory == Catch::Approx(1.0).margin(0.15));
}

TEST_CASE("extract_topology fills kappa_samples on curved edges when OCC") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const CadModel model = CadModel::load_step(path);
    const auto topo = extract_topology(model, 16);
    // At least one sharp closed rim should carry positive κ ≈ 1/R.
    int curved_with_kappa = 0;
    for (const auto& e : topo.edges) {
        if (e.feature != CadEdgeFeature::kSharp || e.samples.size() < 4) {
            continue;
        }
        if (e.kappa_samples.size() != e.samples.size()) {
            continue;
        }
        double kmax = 0.0;
        for (double k : e.kappa_samples) {
            kmax = std::max(kmax, k);
        }
        if (kmax > 1e-6) {
            ++curved_with_kappa;
        }
    }
    CHECK(curved_with_kappa >= 1);
}
