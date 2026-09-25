// SPDX-License-Identifier: BSD-3-Clause

// Per-run geometry / mesh-quality scorecard fields and pre-mesh sizing
// diagnostics. Serialization only; the metrics live in geom / mesh.

#include "fea/boundary_faces.hpp"
#include "geom/cad_topology.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "mesh/surface_metrics.hpp"
#include "testlab_internal.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace polymesh::testlab::detail {

/// Optional geometry scorecard fields (null when CAD/surface unavailable).
json compute_scorecard_geom(const pipeline::Model& model, const fea::NodalMesh& mesh,
                            double h) {
    json edge_hd = nullptr;
    json chord_eff = nullptr;
    json normal_dev = nullptr;

    if (model.cad && !model.cad->empty() && h > 0.0) {
        try {
            const geom::CadTopology topo = geom::extract_topology(*model.cad, 8);
            if (!topo.empty()) {
                // Free-boundary undirected edges near sharp CAD features.
                // (Do not treat unordered node lists as a polyline — that
                // invents nonsense segments and inflates chordal efficiency.)
                const auto faces = free_faces_as_surface(mesh);
                std::set<std::pair<std::uint32_t, std::uint32_t>> bedges;
                std::set<std::uint32_t> bnodes;
                auto add_edge = [&](std::uint32_t i, std::uint32_t j) {
                    if (i > j) {
                        std::swap(i, j);
                    }
                    bedges.insert({i, j});
                };
                for (const auto& face : faces) {
                    for (const auto ni : face.nodes) {
                        bnodes.insert(ni);
                    }
                    const auto& n = face.nodes;
                    if (n.size() == 3) {
                        add_edge(n[0], n[1]);
                        add_edge(n[1], n[2]);
                        add_edge(n[2], n[0]);
                    } else if (n.size() >= 4) {
                        add_edge(n[0], n[1]);
                        add_edge(n[1], n[2]);
                        add_edge(n[2], n[3]);
                        add_edge(n[3], n[0]);
                    }
                }
                std::vector<Eigen::Vector3d> samples;
                samples.reserve(bnodes.size());
                for (const auto ni : bnodes) {
                    if (ni < mesh.nodes.size()) {
                        samples.push_back(mesh.nodes[ni]);
                    }
                }
                const double near_band = 0.75 * h;
                std::vector<geom::MeshEdgeSegment> segs;
                segs.reserve(bedges.size());
                for (const auto& [ia, ib] : bedges) {
                    if (ia >= mesh.nodes.size() || ib >= mesh.nodes.size()) {
                        continue;
                    }
                    const Eigen::Vector3d& pa = mesh.nodes[ia];
                    const Eigen::Vector3d& pb = mesh.nodes[ib];
                    const auto qa = geom::closest_edge(topo, pa, /*sharp_only=*/true);
                    const auto qb = geom::closest_edge(topo, pb, /*sharp_only=*/true);
                    if (!qa || !qb || qa->distance > near_band || qb->distance > near_band) {
                        continue;
                    }
                    segs.push_back(geom::MeshEdgeSegment{pa, pb});
                }
                if (!samples.empty()) {
                    const double hd = geom::edge_profile_hausdorff_filtered(
                        topo, samples, /*sharp_only=*/true);
                    edge_hd = hd / std::max(h, 1e-15);
                }
                if (!segs.empty()) {
                    const auto chord = geom::chordal_edge_metrics_segments(
                        topo, segs, /*sharp_edges_only=*/true);
                    chord_eff = chord.max_efficiency;
                    if (edge_hd.is_null() && chord.hausdorff > 0.0) {
                        edge_hd = chord.hausdorff / std::max(h, 1e-15);
                    }
                }
            }
        } catch (...) {
            edge_hd = nullptr;
            chord_eff = nullptr;
        }
    }

    // Normal deviation: max angle (deg) between boundary face normal and a
    // nearby, orientation-compatible model.surface triangle. Skip faces with
    // no good match; leave null if nothing reliable is found.
    if (!model.surface.triangles.empty() && !model.surface.vertices.empty() && h > 0.0) {
        try {
            const double max_match_d2 = (2.0 * h) * (2.0 * h);
            // Require at least ~cos(30°) alignment to count as same-face match.
            constexpr double kMinAbsDot = 0.866; // cos 30°
            double max_deg = 0.0;
            bool any = false;
            for (const auto& face : free_faces_as_surface(mesh)) {
                if (face.nodes.size() < 3) {
                    continue;
                }
                const Eigen::Vector3d& p0 = mesh.nodes[face.nodes[0]];
                const Eigen::Vector3d& p1 = mesh.nodes[face.nodes[1]];
                const Eigen::Vector3d& p2 = mesh.nodes[face.nodes[2]];
                const Eigen::Vector3d e01 = p1 - p0;
                const Eigen::Vector3d e02 = p2 - p0;
                Eigen::Vector3d fn = e01.cross(e02);
                const double fn_n = fn.norm();
                if (!(fn_n > 1e-30)) {
                    continue;
                }
                fn /= fn_n;
                const Eigen::Vector3d c = face_centroid(mesh, face);
                double best_abs_dot = -1.0;
                for (const auto& tri : model.surface.triangles) {
                    const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
                    const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
                    const Eigen::Vector3d& c3 = model.surface.vertices[tri[2]];
                    const Eigen::Vector3d tc = (a + b + c3) / 3.0;
                    if ((tc - c).squaredNorm() > max_match_d2) {
                        continue;
                    }
                    const Eigen::Vector3d ab = b - a;
                    const Eigen::Vector3d ac = c3 - a;
                    const Eigen::Vector3d sn = ab.cross(ac);
                    const double snn = sn.norm();
                    if (!(snn > 1e-30)) {
                        continue;
                    }
                    best_abs_dot = std::max(best_abs_dot, std::abs(fn.dot(sn / snn)));
                }
                // No nearby co-oriented surface triangle → skip (do not report 90°).
                if (best_abs_dot < kMinAbsDot) {
                    continue;
                }
                const double cosang = std::clamp(best_abs_dot, 0.0, 1.0);
                const double deg = std::acos(cosang) * (180.0 / 3.14159265358979323846);
                max_deg = std::max(max_deg, deg);
                any = true;
            }
            if (any) {
                normal_dev = max_deg;
            }
        } catch (...) {
            normal_dev = nullptr;
        }
    }

    return {{"edge_hausdorff_over_h", edge_hd},
            {"chordal_efficiency_max", chord_eff},
            {"normal_dev_deg_max", normal_dev}};
}

/// Crude N_pred ≈ C · V / h³ before meshing (ADR-0023 M4 diagnosis).
/// If N_pred already busts the tier, blame sizing not the mesher.
double predict_elem_count(const pipeline::Model& model, double h) {
    if (!(h > 0.0)) {
        return 0.0;
    }
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseMax(0.0);
    const double vol = std::max(ext[0] * ext[1] * ext[2], 0.0);
    // C~6 for tet packing density in a bounding box (order-of-magnitude only).
    constexpr double kC = 6.0;
    return kC * vol / (h * h * h);
}

json geom_class_of(const pipeline::Model& model, double h_ref) {
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseAbs();
    const double min_ext = ext.minCoeff();
    const double max_ext = ext.maxCoeff();
    // Curved-vertex fraction, the SAME definition as pipeline::CaseFeatures
    // (src/pipeline/src/scene.cpp): share of vertices with kappa * bbox_diag > 1e-8
    // from geom::estimate_vertex_curvature; malformed/empty surfaces yield 0.
    double curved_frac = 0.0;
    try {
        const auto curvature = geom::estimate_vertex_curvature(model.surface);
        const double bbox_diag = ext.norm();
        std::size_t n = 0;
        std::size_t curved = 0;
        for (const double kappa : curvature.kappa) {
            if (!(kappa >= 0.0) || !std::isfinite(kappa)) {
                continue;
            }
            ++n;
            if (kappa * bbox_diag > 1e-8) {
                ++curved;
            }
        }
        if (n > 0) {
            curved_frac = static_cast<double>(curved) / static_cast<double>(n);
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero default.
    }
    const bool thin = (h_ref > 0.0) && (min_ext < 2.5 * h_ref);
    const double min_feature_h = (h_ref > 0.0) ? (min_ext / h_ref) : 0.0;
    (void)max_ext;
    return {{"curved_frac", curved_frac}, {"thin", thin}, {"min_feature_h", min_feature_h}};
}

HminFeatureReport detect_hmin_features(const pipeline::Model& model, double h) {
    HminFeatureReport rep;
    if (!(h > 0.0) || !geom::occ_enabled() || !model.cad || model.cad->empty()) {
        return rep;
    }
    // Tier-implied floor: ~1/4 of campaign h (resolved.h * h_scale).
    const double h_min = h * 0.25;
    try {
        const geom::CadTopology topo = geom::extract_topology(*model.cad, 4);
        for (const auto& e : topo.edges) {
            if (e.feature != geom::CadEdgeFeature::kSharp) {
                continue;
            }
            if (!(e.length > 0.0) || !std::isfinite(e.length)) {
                continue;
            }
            // Sharp edges shorter than 2·h → count as below h_min class.
            if (e.length < 2.0 * h) {
                ++rep.n_features_below_h_min;
            }
            // Would-want h_edge = L/3 under the floor → explicit feature_flags entry.
            const double h_edge = e.length / 3.0;
            if (h_edge < h_min) {
                rep.feature_flags.push_back({{"edge_id", e.id},
                                             {"reason", "below_h_min"},
                                             {"length", e.length},
                                             {"h_edge", h_edge},
                                             {"h_min", h_min}});
            }
        }
    } catch (...) {
        // Surface-only / extract failure: leave flags empty.
    }
    return rep;
}

json quality_of(const pipeline::Model& model, const fea::NodalMesh& mesh, double h) {
    const auto quads = fea::extract_boundary_faces(mesh);
    std::vector<mesh::FreeFace> faces(quads.begin(), quads.end());
    std::vector<std::array<std::uint32_t, 4>> tets;
    for (const auto& el : mesh.elements) {
        if (el.type == fea::ElementType::kTet4 && el.nodes.size() >= 4) {
            tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
        }
    }
    const auto* tet_ptr = tets.empty() ? nullptr : &tets;
    const auto m = mesh::evaluate_curved_mesh_quality(model.surface, mesh.nodes, faces, h,
                                                      -1.0, -1.0, nullptr, tet_ptr);
    return {{"M1max", m.m1_max},
            {"M2max", m.m2_max},
            {"M6", m.m6_min_boundary_aspect},
            {"score", m.composite_score}};
}

/// Per-run mesh-vs-BRep fidelity row fields. The metric itself lives in
/// mesh::brep_fidelity_summary; this only serializes it, so the campaign
/// columns and the diagnostic sweep can never drift apart.
json geo_fidelity_of(const pipeline::Model& model, const fea::NodalMesh& nodal, double h) {
    mesh::BrepFidelitySummary summary;
    if (geom::occ_enabled() && model.cad && !model.cad->empty()) {
        const auto quads = fea::extract_boundary_faces(nodal);
        const std::vector<mesh::FreeFace> faces(quads.begin(), quads.end());
        summary = mesh::brep_fidelity_summary(*model.cad, nodal.nodes, faces, h);
    }
    return {{"available", summary.available},
            {"chamfer_mean", summary.chamfer_mean},
            {"dist_p95", summary.dist_p95},
            {"dist_p99", summary.dist_p99},
            {"dist_max", summary.dist_max},
            {"normal_angle_p95_rad", summary.normal_angle_p95_rad},
            {"rel_volume_err", summary.rel_volume_err},
            {"n_samples", summary.n_samples}};
}

} // namespace polymesh::testlab::detail
