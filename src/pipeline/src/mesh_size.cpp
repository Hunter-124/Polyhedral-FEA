// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>

namespace polymesh::pipeline {

double predict_mesh_elements(const Model& model, double h) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        return 0.0;
    }
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseMax(0.0);
    const double bbox_volume = std::max(ext[0] * ext[1] * ext[2], 0.0);
    // ADR-0023 M4: a conservative tet-equivalent packing estimate.
    constexpr double kElementsPerCell = 6.0;
    return kElementsPerCell * bbox_volume / (h * h * h);
}

namespace {

// Curved-geometry cost policy for the auto-h budget (ADR-0035): a
// curvature-dominated BRep is meshed on a half-size lattice (8x the cells) and
// promoted to tet10/hex20, so it carries well over 3 DOF per element.
// kCurvedDofPerElement must stay above the worst measured 3*nodes/elements at
// auto h (~4.5): the resolver gets one shot before the fill runs.
constexpr double kCurvedAreaLatticeFraction = 0.25;
constexpr double kCurvedLatticeElementFactor = 8.0;
constexpr double kCurvedDofPerElement = 4.6;

double cad_curved_area_fraction(const geom::CadTopology* topology) {
    if (topology == nullptr || topology->faces.empty()) {
        return 0.0;
    }
    double total_area = 0.0;
    double curved_area = 0.0;
    for (const auto& face : topology->faces) {
        total_area += face.area;
        if (face.kind != geom::CadSurfaceKind::kPlane) {
            curved_area += face.area;
        }
    }
    return total_area > 0.0 ? curved_area / total_area : 0.0;
}

} // namespace

ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg, std::size_t max_elems,
                                   std::size_t max_dof, bool curved_geometry,
                                   const geom::CadTopology* cad_topology) {
    ResolvedMeshSize out;
    out.element_ceiling = max_elems == 0 ? kDefaultMaxMeshElems : max_elems;
    out.dof_ceiling = max_dof == 0 ? kDefaultMaxMeshDof : max_dof;
    if (requested_h > 0.0) {
        out.h = requested_h;
        out.auto_chosen = false;
        out.predicted_elements = predict_mesh_elements(model, out.h);
        out.note = std::format("h={:.4g} m (user, predicted {:.0f} elems)", out.h,
                               out.predicted_elements);
        return out;
    }

    const Eigen::Vector3d extent_vec = model.bbox_max - model.bbox_min;
    const double extent = extent_vec.maxCoeff();
    const double diagonal = extent_vec.norm();
    // Primary scale: max edge of AABB / 16. Secondary: diagonal / 28 — keeps
    // long thin parts from exploding along the short axes while still
    // resolving the long span.
    double h_geom = extent / 16.0;
    if (diagonal > 0.0) {
        h_geom = std::min(h_geom, diagonal / 28.0);
    }
    if (!(h_geom > 0.0) || !std::isfinite(h_geom)) {
        h_geom = 0.05; // last-resort fallback for degenerate bbox
    }

    const auto edges = geom::detect_sharp_edges(model.surface, sharp_angle_deg);
    out.n_sharp_edges = edges.size();
    // Prefer geometric feature scale over STL facet edge length: a faceted hole
    // has hundreds of short creases that would otherwise drive global h down.
    double min_feature = std::numeric_limits<double>::infinity();
    for (const auto& e : edges) {
        const double len =
            (model.surface.vertices[e.v0] - model.surface.vertices[e.v1]).norm();
        if (len > 0.02 * extent) { // ignore facet-scale creases
            min_feature = std::min(min_feature, len);
        }
    }
    // Curvature radius proxy: R ≈ 1/κ for high-κ verts (holes/fillets).
    double r_curv = std::numeric_limits<double>::infinity();
    {
        const auto curv = geom::estimate_vertex_curvature(model.surface);
        for (double k : curv.kappa) {
            if (k > 1e-9) {
                r_curv = std::min(r_curv, 1.0 / k);
            }
        }
    }
    // Thickness: thin plates need a few elements through thickness, not global flood.
    double t_min = std::numeric_limits<double>::infinity();
    {
        const auto thick = geom::estimate_local_thickness(model.surface);
        for (double t : thick.thickness) {
            if (geom::has_finite_thickness(t) && t > 1e-12) {
                t_min = std::min(t_min, t);
            }
        }
    }
    if (!std::isfinite(min_feature)) {
        min_feature = 0.0;
    }
    out.min_feature_length = min_feature;

    // Mild density tweak only; facet count must not drive global h.
    double density_scale = 1.0;
    if (out.n_sharp_edges > 40 && out.n_sharp_edges <= 120) {
        density_scale = 0.92;
    } else if (out.n_sharp_edges > 120) {
        density_scale = 0.88; // faceted curves: keep bulk coarse; local LEB refines
    }

    double h0 = h_geom * density_scale;
    // Resolve feature geometry with local multi-level LEB, not global h collapse.
    // Aim ~5–6 bulk cells across characteristic R so L2 (~h/4) yields a smooth hole.
    if (std::isfinite(r_curv) && r_curv > 0.0) {
        // ~6 bulk cells across characteristic radius; L2 LEB densifies the rim further.
        const double h_r = r_curv / 6.0;
        if (h_r < h0) {
            h0 = std::max(h_r, h_geom * 0.28);
        }
    }
    if (std::isfinite(t_min) && t_min > 0.0) {
        const double h_t = t_min / 2.0;
        if (h_t < h0 && h_t > h_geom * 0.2) {
            h0 = std::min(h0, std::max(h_t, h_geom * 0.35));
        }
    }
    if (min_feature > 0.0) {
        const double h_feat = 0.35 * min_feature;
        if (h_feat < h0) {
            h0 = std::max(h_feat, h_geom * 0.4);
        }
    }

    // BRep edge lengths (ADR-0020 / V1c): prefer retained Model::cad; fall back
    // to reloading source_path for surface-only models that still have a CAD path.
    double cad_min_edge = std::numeric_limits<double>::infinity();
    if (geom::occ_enabled()) {
        try {
            std::optional<geom::CadModel> cad_owned;
            const geom::CadModel* cad_ptr = nullptr;
            if (model.cad && !model.cad->empty()) {
                cad_ptr = &(*model.cad);
            } else if (!model.source_path.empty()) {
                cad_owned = geom::load_cad(model.source_path);
                if (cad_owned && !cad_owned->empty()) {
                    cad_ptr = &(*cad_owned);
                }
            }
            if (cad_ptr != nullptr) {
                const geom::CadTopology topo = geom::extract_topology(*cad_ptr, 4);
                for (const auto& e : topo.edges) {
                    if (e.length > 1e-12 && e.length > 0.02 * extent) {
                        cad_min_edge = std::min(cad_min_edge, e.length);
                    }
                }
                // Hole / fillet arcs often appear as single short edges relative
                // to bbox; still honor them if longer than 1% extent.
                if (!std::isfinite(cad_min_edge)) {
                    for (const auto& e : topo.edges) {
                        if (e.length > 0.01 * extent) {
                            cad_min_edge = std::min(cad_min_edge, e.length);
                        }
                    }
                }
            }
        } catch (...) {
            // Surface-only auto-h fallback.
        }
    }
    if (std::isfinite(cad_min_edge) && cad_min_edge > 0.0) {
        out.min_feature_length = (out.min_feature_length > 0.0)
                                     ? std::min(out.min_feature_length, cad_min_edge)
                                     : cad_min_edge;
        const double h_cad = 0.3 * cad_min_edge;
        if (h_cad < h0) {
            h0 = std::max(h_cad, h_geom * 0.35);
        }
    }

    // Absolute clamps keep interactive meshes within [diag/80, diag/6].
    if (diagonal > 0.0) {
        h0 = std::clamp(h0, diagonal / 80.0, diagonal / 6.0);
    }
    const double h_before_ceiling = h0;
    // The ADR-0023 bbox estimator intentionally ignores feature/transition
    // amplification (hybrid auto meshes reach ~3.1× N_pred); 4× headroom makes
    // the ceiling bind before the fill rather than after it.
    constexpr double kAutoPredictionSafety = 4.0;
    // Curved CAD geometry costs more per requested h: a curvature-dominated
    // BRep is filled on the half-size lattice and every cell carries mid-edge
    // nodes. Auto sizing has to spend that up front or the interactive ceiling
    // is exceeded by ~8× cells and ~12× DOF after the fact.
    const double lattice_factor =
        curved_geometry && cad_curved_area_fraction(cad_topology) >= kCurvedAreaLatticeFraction
            ? kCurvedLatticeElementFactor
            : 1.0;
    const double dof_per_element = curved_geometry ? kCurvedDofPerElement : 3.0;
    const std::size_t dof_elem_ceiling = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(out.dof_ceiling) /
                                    (dof_per_element * lattice_factor)));
    const std::size_t elem_ceiling_scaled = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(static_cast<double>(out.element_ceiling) / lattice_factor));
    const std::size_t effective_elem_ceiling = std::min(elem_ceiling_scaled, dof_elem_ceiling);
    const Eigen::Vector3d positive_extent = extent_vec.cwiseMax(0.0);
    const double bbox_volume =
        std::max(positive_extent[0] * positive_extent[1] * positive_extent[2], 0.0);
    const double h_ceiling = std::cbrt(6.0 * kAutoPredictionSafety * bbox_volume /
                                       static_cast<double>(effective_elem_ceiling));
    if (h_ceiling > h0 && std::isfinite(h_ceiling)) {
        h0 = std::nextafter(h_ceiling, std::numeric_limits<double>::infinity());
        out.ceiling_clamped = true;
    }
    out.h = h0;
    out.auto_chosen = true;
    out.predicted_elements = kAutoPredictionSafety * predict_mesh_elements(model, out.h);
    const std::string detail = std::format(
        "extent/16∩diag/28, prediction safety×4, n_sharp={}, min_feat={:.3g} m, "
        "dens×{:.2f}{}{}",
        out.n_sharp_edges, out.min_feature_length, density_scale,
        std::isfinite(r_curv) ? std::format(", Rκ≈{:.3g}", r_curv) : std::string{},
        std::isfinite(cad_min_edge) ? std::format(", CAD_edge≈{:.3g}", cad_min_edge)
                                    : std::string{});
    if (out.ceiling_clamped) {
        if (effective_elem_ceiling == elem_ceiling_scaled) {
            out.note = std::format(
                "auto h clamped from {:.4g} to {:.4g} m (element ceiling {}) | "
                "predicted {:.0f} elems ({})",
                h_before_ceiling, out.h, out.element_ceiling, out.predicted_elements, detail);
        } else {
            out.note =
                std::format("auto h clamped from {:.4g} to {:.4g} m (DOF ceiling {}) | "
                            "predicted {:.0f} elems / {:.0f} DOF ({})",
                            h_before_ceiling, out.h, out.dof_ceiling, out.predicted_elements,
                            3.0 * out.predicted_elements, detail);
        }
    } else {
        out.note = std::format("auto h={:.4g} m (predicted {:.0f} elems ≤ ceiling {}; {})",
                               out.h, out.predicted_elements, out.element_ceiling, detail);
    }
    return out;
}

} // namespace polymesh::pipeline
