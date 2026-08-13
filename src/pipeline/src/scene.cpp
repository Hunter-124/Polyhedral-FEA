// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/hp_driver.hpp"
#include "adapt/loop.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/p_elevate.hpp"
#include "fea/quadrature.hpp"
#include "fea/poly_to_vem.hpp"
#include "fea/shape.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"
#include "fea/vem.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_geometry_features.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/cvt_export.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/cvt_sites.hpp"
#include "mesh/geogram_clip.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/hex_fill.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/octa_fill.hpp"
#include "mesh/prism_fill.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "mesh/transition_fill.hpp"
#include "mesh/varyhedron_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <queue>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace polymesh::pipeline {
namespace adapt = polymesh::adapt;

namespace {

Eigen::Vector3d triangle_normal(const geom::TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    return ab.cross(ac).normalized();
}

} // namespace

Model Model::load(const std::string& path, double sharp_angle_deg) {
    Model model;
    const auto lower = [&] {
        std::string s = path;
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    model.source_path = path;
    const auto slash = path.find_last_of("/\\");
    model.name = slash == std::string::npos ? path : path.substr(slash + 1);

    // CAD-only inputs (ADR-0020): STEP/BREP retain the live CadModel; the
    // tessellation is derived for regions, viewport, and legacy hybrid fill.
    // STL is no longer an accepted input — provide a STEP/BREP CAD file.
    if (lower.ends_with(".step") || lower.ends_with(".stp")) {
        model.cad = geom::CadModel::load_step(path);
        model.surface = model.cad->tessellate();
        model.bbox_min = model.cad->bbox_min();
        model.bbox_max = model.cad->bbox_max();
    } else if (lower.ends_with(".brep") || lower.ends_with(".brp")) {
        model.cad = geom::CadModel::load_brep(path);
        model.surface = model.cad->tessellate();
        model.bbox_min = model.cad->bbox_min();
        model.bbox_max = model.cad->bbox_max();
    } else {
        throw std::runtime_error(
            std::format("unsupported input '{}': only CAD files are accepted "
                        "(.step, .stp, .brep, .brp). STL inputs are no longer supported.",
                        path));
    }
    model.surface.validate();

    // CAD-style face regions: grow across edges whose dihedral angle is
    // below the sharp threshold.
    const std::size_t n_tris = model.surface.triangles.size();
    std::vector<Eigen::Vector3d> normals(n_tris);
    for (std::size_t t = 0; t < n_tris; ++t) {
        normals[t] = triangle_normal(model.surface, t);
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edge_tris;
    for (std::size_t t = 0; t < n_tris; ++t) {
        const auto& tri = model.surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                         tri[static_cast<std::size_t>((e + 1) % 3)]);
            edge_tris[key].push_back(static_cast<std::uint32_t>(t));
        }
    }
    const double cos_sharp = std::cos(sharp_angle_deg * std::numbers::pi / 180.0);
    model.triangle_region.assign(n_tris, -1);
    for (std::size_t seed = 0; seed < n_tris; ++seed) {
        if (model.triangle_region[seed] >= 0) {
            continue;
        }
        const int region = model.region_count++;
        std::queue<std::uint32_t> frontier;
        frontier.push(static_cast<std::uint32_t>(seed));
        model.triangle_region[seed] = region;
        while (!frontier.empty()) {
            const auto t = frontier.front();
            frontier.pop();
            const auto& tri = model.surface.triangles[t];
            for (int e = 0; e < 3; ++e) {
                const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                             tri[static_cast<std::size_t>((e + 1) % 3)]);
                for (const auto other : edge_tris.at(key)) {
                    if (model.triangle_region[other] >= 0) {
                        continue;
                    }
                    if (normals[t].dot(normals[other]) > cos_sharp) {
                        model.triangle_region[other] = region;
                        frontier.push(other);
                    }
                }
            }
        }
    }
    return model;
}

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

ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg, std::size_t max_elems,
                                   std::size_t max_dof) {
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
    // Primary scale: max edge of AABB / 16 (practical zero-tune; former CLI
    // mesh default). Secondary: diagonal / 28 — keeps long thin parts from
    // exploding along the short axes while still resolving the long span.
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
    // has hundreds of short creases that used to drive global h → million-elem floods.
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

    // Mild density tweak only — never the old dens×0.55 from facet count.
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

    // Absolute clamps: coarser floor than before so interactive meshes stay sane.
    if (diagonal > 0.0) {
        h0 = std::clamp(h0, diagonal / 80.0, diagonal / 6.0);
    }
    const double h_before_ceiling = h0;
    // The ADR-0023 bbox estimator intentionally ignores feature/transition
    // amplification. Measured public hybrid auto meshes reach ~3.1× N_pred;
    // use 4× headroom so the ceiling binds before fill rather than after it.
    constexpr double kAutoPredictionSafety = 4.0;
    const std::size_t dof_elem_ceiling = std::max<std::size_t>(1, out.dof_ceiling / 3);
    const std::size_t effective_elem_ceiling = std::min(out.element_ceiling, dof_elem_ceiling);
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
        if (effective_elem_ceiling == out.element_ceiling) {
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

CaseFeatures extract_case_features(const Model& model,
                                  std::span<const RefineRegion> fix_regions,
                                  std::span<const RefineRegion> load_regions,
                                  const Eigen::Vector3d& load_dir, double poisson) {
    CaseFeatures out;
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseMax(0.0);
    const double bbox_diag = ext.norm();
    const double inv_diag =
        bbox_diag > 0.0 && std::isfinite(bbox_diag) ? 1.0 / bbox_diag : 0.0;
    out.bbox_dx = ext.x() * inv_diag;
    out.bbox_dy = ext.y() * inv_diag;
    out.bbox_dz = ext.z() * inv_diag;
    out.diag = inv_diag > 0.0 ? 1.0 : 0.0;
    out.n_faces = model.surface.triangles.size();
    out.poisson = std::isfinite(poisson) ? poisson : 0.0;

    // Exact-BRep descriptors for the advisor's OOD test. Same
    // retained-cad-then-reload-source_path pattern as resolve_mesh_size above,
    // guarded so a surface-only or no-OCC build simply reports them unavailable
    // instead of contributing zeros to a Mahalanobis distance.
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
                const geom::GeometryDescriptors geo =
                    geom::compute_geometry_descriptors(*cad_ptr);
                if (geo.available) {
                    out.geo_available = true;
                    out.geo_curved_area_frac = geo.curved_area_frac;
                    out.geo_cyl_area_frac = geo.cyl_area_frac;
                    out.geo_plane_area_frac = geo.plane_area_frac;
                    out.geo_other_area_frac = geo.other_area_frac;
                    out.geo_min_curv_radius_rel = geo.min_curv_radius_rel;
                    out.geo_log_curv_radius_mean = geo.log_curv_radius_mean;
                    out.geo_log_curv_radius_std = geo.log_curv_radius_std;
                    out.geo_n_faces = geo.n_faces;
                    out.geo_n_edges = geo.n_edges;
                    out.geo_face_area_cv = geo.face_area_cv;
                    out.geo_aspect_max = geo.aspect_max;
                    out.geo_aspect_mid = geo.aspect_mid;
                    out.geo_volume_frac = geo.volume_frac;
                    out.geo_area_over_v23 = geo.area_over_v23;
                    out.geo_min_face_size_rel = geo.min_face_size_rel;
                }
            }
        } catch (...) {
            out.geo_available = false;
        }
    }

    double surface_area_m2 = 0.0;
    double signed_volume_m3 = 0.0;
    struct RegionMeasure {
        std::size_t n_faces = 0;
        double area = 0.0;
        Eigen::Vector3d area_centroid = Eigen::Vector3d::Zero();
    };
    RegionMeasure fix;
    RegionMeasure load;
    const auto contains = [](const RefineRegion& region, const Eigen::Vector3d& point) {
        return (region.lo.array() <= region.hi.array()).all() &&
               (point.array() >= region.lo.array()).all() &&
               (point.array() <= region.hi.array()).all();
    };
    const auto selected = [&](std::span<const RefineRegion> regions,
                              const Eigen::Vector3d& point) {
        return std::any_of(regions.begin(), regions.end(),
                           [&](const RefineRegion& region) { return contains(region, point); });
    };
    for (const auto& tri : model.surface.triangles) {
        if (tri[0] >= model.surface.vertices.size() ||
            tri[1] >= model.surface.vertices.size() ||
            tri[2] >= model.surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
        const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
        const double area = 0.5 * (b - a).cross(c - a).norm();
        if (!(area > 0.0) || !std::isfinite(area)) {
            continue;
        }
        const Eigen::Vector3d centroid = (a + b + c) / 3.0;
        surface_area_m2 += area;
        signed_volume_m3 += a.dot(b.cross(c)) / 6.0;
        if (selected(fix_regions, centroid)) {
            ++fix.n_faces;
            fix.area += area;
            fix.area_centroid += area * centroid;
        }
        if (selected(load_regions, centroid)) {
            ++load.n_faces;
            load.area += area;
            load.area_centroid += area * centroid;
        }
    }
    const double volume_m3 = std::abs(signed_volume_m3);
    out.surface_area = surface_area_m2 * inv_diag * inv_diag;
    out.volume = volume_m3 * inv_diag * inv_diag * inv_diag;
    if (out.volume > 0.0) {
        out.sa_over_v23 = out.surface_area / std::pow(out.volume, 2.0 / 3.0);
    }
    out.n_fix_faces = fix.n_faces;
    out.n_load_faces = load.n_faces;
    if (surface_area_m2 > 0.0) {
        out.fix_area_frac = fix.area / surface_area_m2;
        out.load_area_frac = load.area / surface_area_m2;
    }

    try {
        const auto sharp = geom::detect_sharp_edges(model.surface, 30.0);
        out.n_sharp_edges = sharp.size();
        double min_sharp = std::numeric_limits<double>::infinity();
        for (const auto& edge : sharp) {
            if (edge.v0 >= model.surface.vertices.size() ||
                edge.v1 >= model.surface.vertices.size()) {
                continue;
            }
            const double length =
                (model.surface.vertices[edge.v1] - model.surface.vertices[edge.v0]).norm();
            if (length > 0.0 && std::isfinite(length)) {
                out.sharp_edge_len_total += length * inv_diag;
                min_sharp = std::min(min_sharp, length);
            }
        }
        if (std::isfinite(min_sharp)) {
            out.min_feature_h = min_sharp * inv_diag;
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    try {
        const auto curvature = geom::estimate_vertex_curvature(model.surface);
        double sum = 0.0;
        std::size_t n = 0;
        std::size_t curved = 0;
        for (const double kappa : curvature.kappa) {
            if (!(kappa >= 0.0) || !std::isfinite(kappa)) {
                continue;
            }
            const double scaled = kappa * bbox_diag;
            out.kappa_max_h = std::max(out.kappa_max_h, scaled);
            sum += scaled;
            ++n;
            if (scaled > 1e-8) {
                ++curved;
            }
        }
        if (n > 0) {
            out.kappa_mean_h = sum / static_cast<double>(n);
            out.curved_frac = static_cast<double>(curved) / static_cast<double>(n);
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    try {
        const auto thickness = geom::estimate_local_thickness(model.surface);
        std::vector<double> finite;
        finite.reserve(thickness.thickness.size());
        for (const double value : thickness.thickness) {
            if (geom::has_finite_thickness(value) && value >= 0.0) {
                finite.push_back(value * inv_diag);
            }
        }
        if (!finite.empty()) {
            std::sort(finite.begin(), finite.end());
            out.thin_min_over_diag = finite.front();
            const std::size_t p10 =
                static_cast<std::size_t>(0.1 * static_cast<double>(finite.size() - 1));
            out.thin_p10_over_diag = finite[p10];
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    Eigen::Vector3d direction = Eigen::Vector3d::Zero();
    if (load_dir.allFinite() && load_dir.norm() > 0.0) {
        direction = load_dir.normalized();
    }
    out.load_dir_x = direction.x();
    out.load_dir_y = direction.y();
    out.load_dir_z = direction.z();
    if (fix.area > 0.0 && load.area > 0.0) {
        const Eigen::Vector3d delta =
            load.area_centroid / load.area - fix.area_centroid / fix.area;
        out.fix_load_dist_over_diag = delta.norm() * inv_diag;
        if (delta.norm() > 0.0 && direction.norm() > 0.0) {
            out.load_axis_alignment = std::abs(direction.dot(delta.normalized()));
        }
    }
    return out;
}

namespace {

/// Finest-wins spatial decimation: keep one source (min h) per cubic cell of
/// side `cell`. Preserves the size field (where the mesh must be fine) while
/// capping seed count, so the gradient limiter and ball-grading meshers do not
/// choke on ~1 seed per surface vertex (tens of thousands on a real CAD part).
std::vector<adapt::SizeSource> decimate_sources(std::vector<adapt::SizeSource> src,
                                                double cell) {
    if (!(cell > 0.0) || src.size() < 2) {
        return src;
    }
    struct Key {
        long x, y, z;
        bool operator==(const Key& o) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            std::size_t h = std::hash<long>{}(k.x);
            h ^= std::hash<long>{}(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= std::hash<long>{}(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<Key, std::size_t, KeyHash> best;
    best.reserve(src.size());
    const double inv = 1.0 / cell;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const Key k{static_cast<long>(std::floor(src[i].x.x() * inv)),
                    static_cast<long>(std::floor(src[i].x.y() * inv)),
                    static_cast<long>(std::floor(src[i].x.z() * inv))};
        auto [it, inserted] = best.try_emplace(k, i);
        if (!inserted && src[i].h < src[it->second].h) {
            it->second = i;
        }
    }
    std::vector<adapt::SizeSource> out;
    out.reserve(best.size());
    for (const auto& [key, idx] : best) {
        out.push_back(src[idx]);
    }
    std::sort(out.begin(), out.end(), [](const adapt::SizeSource& a,
                                         const adapt::SizeSource& b) {
        if (a.x.x() != b.x.x()) {
            return a.x.x() < b.x.x();
        }
        if (a.x.y() != b.x.y()) {
            return a.x.y() < b.x.y();
        }
        if (a.x.z() != b.x.z()) {
            return a.x.z() < b.x.z();
        }
        return a.h < b.h;
    });
    return out;
}

} // namespace

RefinementPlan build_refinement_plan(const Model& model, double h_coarse,
                                     std::span<const RefineRegion> regions,
                                     bool use_geometry) {
    RefinementPlan plan;
    if (!(h_coarse > 0.0) || !std::isfinite(h_coarse)) {
        return plan;
    }
    std::vector<adapt::SizeSource> sources;

    // Geometry a-priori: curvature + thin-wall surface sources finer than the
    // bulk h. Flat, thick regions emit nothing, so the source set stays sparse.
    if (use_geometry) {
        const double h_min_geo = 0.15 * h_coarse; // floor: avoid runaway fine
        auto geo = adapt::geometry_size_sources(model.surface, h_min_geo, h_coarse);
        // ~1 seed per vertex on a real CAD part is far more than the grading
        // needs; keep the finest per half-h cell (field preserved, count bounded).
        geo = decimate_sources(std::move(geo), 0.5 * h_coarse);
        plan.n_geometry_seeds = geo.size();
        sources.insert(sources.end(), geo.begin(), geo.end());
    }

    // Boundary-condition / load a-priori: surface-face centroids inside each
    // selection box, at target_fraction * h_coarse (loads finest). This is what
    // makes the mesh grade toward the simulation setup, not just the geometry.
    const auto& surf = model.surface;
    for (const auto& reg : regions) {
        std::vector<Eigen::Vector3d> pts;
        for (const auto& t : surf.triangles) {
            const Eigen::Vector3d c =
                (surf.vertices[t[0]] + surf.vertices[t[1]] + surf.vertices[t[2]]) / 3.0;
            if ((c.array() >= reg.lo.array()).all() && (c.array() <= reg.hi.array()).all()) {
                pts.push_back(c);
            }
        }
        if (pts.empty()) {
            continue;
        }
        const double h_target = std::max(0.1 * h_coarse, reg.target_fraction * h_coarse);
        const auto bc = adapt::point_size_sources(pts, h_target);
        plan.n_bc_seeds += bc.size();
        sources.insert(sources.end(), bc.begin(), bc.end());
    }

    if (sources.empty()) {
        return plan;
    }
    const auto sp = adapt::seed_plan(sources, h_coarse, /*band_frac=*/1.5);
    plan.size_field =
        adapt::size_field_from_sources(sources, sp.h_fine, h_coarse, /*beta=*/1.0);
    plan.refine_seeds = std::move(sp.refine_seeds);
    plan.seed_band = sp.seed_band;
    plan.h_min = sp.h_fine;
    plan.h_fine = sp.h_fine;
    return plan;
}

ElementTendencyPlan resolve_element_tendency(VolumeMesher base, double tendency,
                                             int skin_layers) {
    ElementTendencyPlan plan;
    plan.tendency = std::clamp(tendency, -1.0, 1.0);
    plan.skin_layers = std::max(1, skin_layers);
    plan.mesher = base;
    plan.native_poly_transitions = (base == VolumeMesher::kHybridVem);
    plan.remapped = false;

    const auto label_for = [](VolumeMesher m) -> const char* {
        switch (m) {
        case VolumeMesher::kHexFill:
            return "hex";
        case VolumeMesher::kHexVem:
            return "hex-vem";
        case VolumeMesher::kHybrid:
            return "hybrid-fan";
        case VolumeMesher::kHybridVem:
            return "hybrid-vem";
        case VolumeMesher::kGradedTet:
            return "graded-tet";
        case VolumeMesher::kVaryhedron:
            return "varyhedron";
        case VolumeMesher::kCvtPoly:
            return "cvt_poly";
        case VolumeMesher::kTetFill:
            return "tet";
        case VolumeMesher::kHexPyramid:
            return "hex-pyramid";
        case VolumeMesher::kPrismSweep:
            return "prism";
        case VolumeMesher::kOctahedral:
            return "octahedral";
        }
        return "unknown";
    };
    plan.label = label_for(base);

    // Exact zero (campaign default / SimSetup default) preserves the base
    // mesher so kHybrid and kHybridVem product paths stay unchanged.
    if (std::abs(plan.tendency) < 1e-12) {
        return plan;
    }

    const auto is_hybrid_family = [](VolumeMesher m) {
        return m == VolumeMesher::kHybrid || m == VolumeMesher::kHybridVem;
    };
    const auto is_hex_family = [](VolumeMesher m) {
        return m == VolumeMesher::kHexFill || m == VolumeMesher::kHexVem;
    };
    const auto is_tet_family = [](VolumeMesher m) {
        return m == VolumeMesher::kTetFill || m == VolumeMesher::kGradedTet ||
               m == VolumeMesher::kVaryhedron;
    };

    // Shape dial for hybrid / hex / tet families. Prism / octa / hexpyr keep
    // their explicit base (no continuous remap yet).
    VolumeMesher effective = base;
    if (is_hybrid_family(base) || is_hex_family(base) || is_tet_family(base)) {
        if (plan.tendency <= -0.5) {
            effective = VolumeMesher::kHexFill;
        } else if (plan.tendency <= 0.25) {
            effective = VolumeMesher::kHybrid;
        } else if (plan.tendency <= 0.75) {
            effective = VolumeMesher::kHybridVem;
        } else {
            effective = VolumeMesher::kGradedTet;
        }
    }

    plan.mesher = effective;
    plan.native_poly_transitions = (effective == VolumeMesher::kHybridVem);
    plan.label = label_for(effective);
    plan.remapped = (effective != base);

    // Skin treatment: hex bias on hybrid → thinner free-surface skin (more
    // bulk hex); strong tet bias → one extra graded skin hop.
    if (effective == VolumeMesher::kHybrid && plan.tendency < 0.0) {
        const int thinned = std::max(1, skin_layers - 1);
        if (thinned != plan.skin_layers) {
            plan.skin_layers = thinned;
            plan.remapped = true;
        }
    } else if (effective == VolumeMesher::kGradedTet && plan.tendency > 0.75) {
        plan.skin_layers = skin_layers + 1;
        plan.remapped = true;
    }

    return plan;
}

namespace {

struct QuadraticBoundaryMid {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t mid = 0;
};

std::vector<QuadraticBoundaryMid>
quadratic_boundary_mids(const fea::NodalMesh& nodal_mesh) {
    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5},
         {2, 6}, {3, 7}}};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edge_mids;
    for (const auto& element : nodal_mesh.elements) {
        const bool tet10 = element.type == fea::ElementType::kTet10;
        const bool hex20 = element.type == fea::ElementType::kHex20;
        if (!tet10 && !hex20) {
            continue;
        }
        const std::size_t n_corner = tet10 ? 4 : 8;
        const std::size_t n_mid = tet10 ? 6 : 12;
        if (element.nodes.size() < n_corner + n_mid) {
            continue;
        }
        for (std::size_t edge_index = 0; edge_index < n_mid; ++edge_index) {
            const auto& edge =
                tet10 ? kTetEdges[edge_index] : kHexEdges[edge_index];
            const auto a = element.nodes[static_cast<std::size_t>(edge[0])];
            const auto b = element.nodes[static_cast<std::size_t>(edge[1])];
            edge_mids[std::minmax(a, b)] = element.nodes[n_corner + edge_index];
        }
    }

    static constexpr std::array<std::array<int, 2>, 3> kTriEdges{
        {{0, 1}, {1, 2}, {0, 2}}};
    static constexpr std::array<std::array<int, 2>, 4> kQuadEdges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
    std::map<std::uint32_t, QuadraticBoundaryMid> by_mid;
    for (const auto& face : fea::boundary_surface_faces(nodal_mesh)) {
        const bool tri6 = face.type == fea::FaceType::kTri6;
        const bool quad8 = face.type == fea::FaceType::kQuad8;
        if (!tri6 && !quad8) {
            continue;
        }
        const std::size_t n_edges = tri6 ? kTriEdges.size() : kQuadEdges.size();
        for (std::size_t edge_index = 0; edge_index < n_edges; ++edge_index) {
            const auto& edge =
                tri6 ? kTriEdges[edge_index] : kQuadEdges[edge_index];
            const auto a = face.nodes[static_cast<std::size_t>(edge[0])];
            const auto b = face.nodes[static_cast<std::size_t>(edge[1])];
            const auto it = edge_mids.find(std::minmax(a, b));
            if (it != edge_mids.end()) {
                by_mid.try_emplace(it->second, QuadraticBoundaryMid{a, b, it->second});
            }
        }
    }

    std::vector<QuadraticBoundaryMid> result;
    result.reserve(by_mid.size());
    for (const auto& [mid, edge] : by_mid) {
        (void)mid;
        result.push_back(edge);
    }
    return result;
}

bool quadratic_incident_valid(const fea::NodalMesh& nodal_mesh,
                              const fea::NodalElement& element,
                              std::uint32_t moved_node,
                              const Eigen::Vector3d& saved_position,
                              double volume_epsilon,
                              double jacobian_epsilon) {
    const bool tet10 = element.type == fea::ElementType::kTet10;
    const bool hex20 = element.type == fea::ElementType::kHex20;
    if (!tet10 && !hex20) {
        return false;
    }
    if (element.nodes.size() != (tet10 ? 10 : 20)) {
        return false;
    }

    if (tet10) {
        const auto& a = nodal_mesh.nodes[element.nodes[0]];
        const auto& b = nodal_mesh.nodes[element.nodes[1]];
        const auto& c = nodal_mesh.nodes[element.nodes[2]];
        const auto& d = nodal_mesh.nodes[element.nodes[3]];
        if (!(mesh::validity::tet_signed_volume(a, b, c, d) > volume_epsilon)) {
            return false;
        }
    } else {
        std::array<Eigen::Vector3d, 8> corners{};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            corners[i] = nodal_mesh.nodes[element.nodes[i]];
        }
        for (const auto& corner : mesh::validity::kHexCornerTriples) {
            if (!(mesh::validity::tet_signed_volume(
                      corners[static_cast<std::size_t>(corner[0])],
                      corners[static_cast<std::size_t>(corner[1])],
                      corners[static_cast<std::size_t>(corner[2])],
                      corners[static_cast<std::size_t>(corner[3])]) >
                  volume_epsilon)) {
                return false;
            }
        }
    }

    Eigen::Matrix<double, Eigen::Dynamic, 3> after(element.nodes.size(), 3);
    Eigen::Matrix<double, Eigen::Dynamic, 3> before(element.nodes.size(), 3);
    for (std::size_t i = 0; i < element.nodes.size(); ++i) {
        const auto node = element.nodes[i];
        after.row(static_cast<Eigen::Index>(i)) = nodal_mesh.nodes[node].transpose();
        before.row(static_cast<Eigen::Index>(i)) =
            (node == moved_node ? saved_position : nodal_mesh.nodes[node]).transpose();
    }

    // Use exactly the integration points consumed by element_stiffness. Keeping
    // the rule selection in fea::default_rule makes a future quadrature change
    // update both the acceptance gate and the solver together.
    static const auto tet10_rule = fea::default_rule(fea::ElementType::kTet10);
    static const auto hex20_rule = fea::default_rule(fea::ElementType::kHex20);
    const auto& rule = tet10 ? tet10_rule : hex20_rule;
    for (const auto& qp : rule) {
        const auto shape = fea::eval_shape(element.type, qp.xi);
        const double det_before = (shape.dn.transpose() * before).determinant();
        const double det_after = (shape.dn.transpose() * after).determinant();
        if (!std::isfinite(det_before) || !std::isfinite(det_after) ||
            !(det_before > 0.0) || !(det_after > jacobian_epsilon)) {
            return false;
        }
    }
    return true;
}


} // namespace

bool make_boundary_projection(const geom::CadModel& cad, double h,
                              mesh::BoundaryProjectionContext* ctx,
                              std::vector<mesh::BoundarySupport>* provenance) {
    if (ctx == nullptr || provenance == nullptr || cad.empty()) {
        if (ctx != nullptr) {
            *ctx = {};
        }
        return false;
    }

    auto topology = std::make_shared<geom::CadTopology>(geom::extract_topology(cad, 10));
    const geom::CadModel* cad_ptr = &cad;
    ctx->provenance = provenance;
    ctx->target =
        [cad_ptr, topology = std::move(topology),
         h](const Eigen::Vector3d& p,
            mesh::BoundarySupport& owner) -> std::optional<mesh::BoundaryTarget> {
        std::optional<geom::ProjectResult> exact;
        if (owner.kind == mesh::BoundarySupportKind::kCadVertex) {
            exact = geom::project_point_on_vertex(*cad_ptr, owner.id, p);
        } else if (owner.kind == mesh::BoundarySupportKind::kCadEdge) {
            exact = geom::project_point_on_edge(*cad_ptr, owner.id, p);
        } else if (owner.kind == mesh::BoundarySupportKind::kCadFace) {
            exact = geom::project_point_on_face(*cad_ptr, owner.id, p);
        } else {
            exact = geom::project_point_on_surface(*cad_ptr, p);
            if (!exact) {
                return std::nullopt;
            }

            const double feature_slack = 0.08 * h;
            bool chose_vertex = false;
            const geom::CadVertex* nearest_vertex = nullptr;
            double vertex_distance = std::numeric_limits<double>::infinity();
            for (const auto& vertex : topology->vertices) {
                const double distance = (p - vertex.position).norm();
                if (distance < vertex_distance) {
                    nearest_vertex = &vertex;
                    vertex_distance = distance;
                }
            }
            if (nearest_vertex != nullptr && vertex_distance <= 0.40 * h &&
                vertex_distance <= exact->distance + feature_slack) {
                auto vertex_exact =
                    geom::project_point_on_vertex(*cad_ptr, nearest_vertex->id, p);
                if (vertex_exact) {
                    exact = std::move(vertex_exact);
                    chose_vertex = true;
                    owner = {mesh::BoundarySupportKind::kCadVertex, nearest_vertex->id};
                }
            }

            if (!chose_vertex && owner.kind == mesh::BoundarySupportKind::kUnknown) {
                const auto nearest_edge = geom::closest_edge(*topology, p, true);
                if (nearest_edge && nearest_edge->distance <= 0.55 * h &&
                    nearest_edge->distance <= exact->distance + feature_slack) {
                    auto edge_exact =
                        geom::project_point_on_edge(*cad_ptr, nearest_edge->edge_id, p);
                    if (edge_exact) {
                        exact = std::move(edge_exact);
                        owner = {mesh::BoundarySupportKind::kCadEdge,
                                 nearest_edge->edge_id};
                    }
                }
            }

            if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                exact->support_kind == geom::CadSupportKind::kVertex) {
                owner = {mesh::BoundarySupportKind::kCadVertex, exact->support_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                       exact->support_kind == geom::CadSupportKind::kEdge &&
                       exact->support_id < topology->edges.size() &&
                       topology->edges[exact->support_id].feature ==
                           geom::CadEdgeFeature::kSharp) {
                owner = {mesh::BoundarySupportKind::kCadEdge, exact->support_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown &&
                       exact->face_id != geom::kInvalidCadSupportId) {
                owner = {mesh::BoundarySupportKind::kCadFace, exact->face_id};
            } else if (owner.kind == mesh::BoundarySupportKind::kUnknown) {
                return std::nullopt;
            }
        }
        if (!exact) {
            return std::nullopt;
        }
        return mesh::BoundaryTarget{exact->point, exact->distance};
    };
    return true;
}

std::size_t project_quadratic_boundary_mids(
    fea::NodalMesh& nodal_mesh, const geom::CadModel& cad,
    mesh::BoundaryProjectionContext* projection, double h,
    std::vector<std::uint32_t>* reverted_nodes,
    std::vector<std::uint32_t>* partial_nodes) {
    if (reverted_nodes != nullptr) {
        reverted_nodes->clear();
    }
    if (partial_nodes != nullptr) {
        partial_nodes->clear();
    }
    if (projection == nullptr || !projection->target || cad.empty() || !(h > 0.0)) {
        return 0;
    }
    const auto boundary_mids = quadratic_boundary_mids(nodal_mesh);
    if (boundary_mids.empty()) {
        return 0;
    }

    std::unordered_map<std::uint32_t, std::vector<std::size_t>> incident;
    incident.reserve(boundary_mids.size());
    for (const auto& edge : boundary_mids) {
        incident.try_emplace(edge.mid);
    }
    for (std::size_t element_index = 0; element_index < nodal_mesh.elements.size();
         ++element_index) {
        for (const auto node : nodal_mesh.elements[element_index].nodes) {
            if (auto it = incident.find(node); it != incident.end()) {
                it->second.push_back(element_index);
            }
        }
    }

    const auto owner_of = [&](std::uint32_t node) {
        if (projection->provenance != nullptr && node < projection->provenance->size()) {
            return (*projection->provenance)[node];
        }
        return mesh::BoundarySupport{};
    };
    const auto commit_owner = [&](std::uint32_t node, mesh::BoundarySupport owner) {
        if (projection->provenance == nullptr) {
            return;
        }
        if (projection->provenance->size() <= node) {
            projection->provenance->resize(static_cast<std::size_t>(node) + 1);
        }
        (*projection->provenance)[node] = owner;
    };

    const double volume_epsilon = 1e-14 * h * h * h;
    const double jacobian_epsilon = 1e-8 * h * h * h;
    std::size_t projected = 0;
    for (const auto& edge : boundary_mids) {
        if (edge.a >= nodal_mesh.nodes.size() || edge.b >= nodal_mesh.nodes.size() ||
            edge.mid >= nodal_mesh.nodes.size()) {
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(edge.mid);
            }
            continue;
        }
        const Eigen::Vector3d saved = nodal_mesh.nodes[edge.mid];
        const auto owner_a = owner_of(edge.a);
        const auto owner_b = owner_of(edge.b);
        std::optional<geom::ProjectResult> exact;
        mesh::BoundarySupport direct_owner;
        bool direct = owner_a.kind != mesh::BoundarySupportKind::kUnknown &&
                      owner_a.kind == owner_b.kind && owner_a.id == owner_b.id;
        if (direct) {
            direct_owner = owner_a;
            if (owner_a.kind == mesh::BoundarySupportKind::kCadEdge) {
                exact = geom::project_point_on_edge(cad, owner_a.id, saved);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadFace) {
                exact = geom::project_point_on_face(cad, owner_a.id, saved);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadVertex) {
                exact = geom::project_point_on_vertex(cad, owner_a.id, saved);
            } else {
                direct = false;
            }
        }

        std::optional<mesh::BoundaryTarget> target;
        if (direct) {
            if (exact) {
                commit_owner(edge.mid, direct_owner);
                target = mesh::BoundaryTarget{exact->point, exact->distance};
            }
        } else {
            target = mesh::owned_boundary_projection_target(saved, edge.mid, projection);
        }
        if (!target) {
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(edge.mid);
            }
            continue;
        }

        const auto incident_valid = [&]() {
            if (const auto it = incident.find(edge.mid); it != incident.end()) {
                for (const auto element_index : it->second) {
                    if (!quadratic_incident_valid(
                            nodal_mesh, nodal_mesh.elements[element_index], edge.mid, saved,
                            volume_epsilon, jacobian_epsilon)) {
                        return false;
                    }
                }
            }
            return true;
        };

        nodal_mesh.nodes[edge.mid] = target->point;
        if (incident_valid()) {
            ++projected;
            continue;
        }

        double valid_fraction = 0.0;
        double invalid_fraction = 1.0;
        const Eigen::Vector3d displacement = target->point - saved;
        for (int step = 0; step < 6; ++step) {
            const double fraction = 0.5 * (valid_fraction + invalid_fraction);
            nodal_mesh.nodes[edge.mid] = saved + fraction * displacement;
            if (incident_valid()) {
                valid_fraction = fraction;
            } else {
                invalid_fraction = fraction;
            }
        }
        if (valid_fraction > 0.0) {
            nodal_mesh.nodes[edge.mid] = saved + valid_fraction * displacement;
            const auto surface_projection =
                geom::project_point_on_surface(cad, nodal_mesh.nodes[edge.mid]);
            if (surface_projection && surface_projection->distance <= 0.02 * h) {
                ++projected;
            } else if (partial_nodes != nullptr) {
                partial_nodes->push_back(edge.mid);
            }
            continue;
        }

        nodal_mesh.nodes[edge.mid] = saved;
        if (reverted_nodes != nullptr) {
            reverted_nodes->push_back(edge.mid);
        }
        continue;
    }
    return projected;
}

namespace {

// The mesh volume measure lives in `fea::element_volume` / `fea::mesh_volume`
// (fea/cell_quality.hpp). It used to be duplicated here; the copy in
// `fea::element_centroid_stresses` had drifted into a wrong one, so the rule is
// now defined once and every caller shares it.

const char* geometry_volume_band(double relative_error) {
    if (relative_error > kGeometryVolumeHardLimit) {
        return "egregious";
    }
    if (relative_error >= kGeometryVolumeTruthLimit) {
        return "degraded";
    }
    return "clean";
}

std::string geometry_volume_note(std::string_view stage,
                                 const GeometryVolumeAssessment& assessment) {
    return std::format("geometry_{}_volume mesh={:.6g} cad={:.6g} rel_err={:.4g} band={}",
                       stage, assessment.mesh_volume, assessment.cad_volume,
                       assessment.relative_error,
                       geometry_volume_band(assessment.relative_error));
}

void replace_geometry_volume_note(std::string& note, std::string_view stage,
                                  const GeometryVolumeAssessment& assessment) {
    const std::string needle = std::format("geometry_{}_volume ", stage);
    const std::string replacement = geometry_volume_note(stage, assessment);
    const std::size_t token = note.find(needle);
    if (token == std::string::npos) {
        note += std::format(" | {}", replacement);
        return;
    }
    const std::size_t end = note.find(" | ", token);
    note.replace(token, end == std::string::npos ? std::string::npos : end - token,
                 replacement);
}

void enforce_feature_resolution(const Model& model, const VolumeMeshOutput& output,
                                double requested_h, double delivered_h) {
    if (!model.cad || model.cad->empty() || output.boundary_quads.empty() ||
        !(requested_h > 0.0) || !(delivered_h > 0.0)) {
        return;
    }
    constexpr std::size_t kSamplesPerFace = 64;
    constexpr double kNormalMinDot = 0.5;
    const auto inspection = geom::inspect_brep(*model.cad);
    if (!inspection.available || inspection.face_count == 0) {
        return;
    }
    const auto topology = geom::extract_topology(*model.cad, 8);
    const auto samples =
        geom::sample_brep_surface(*model.cad, kSamplesPerFace * inspection.face_count);

    geom::TriSurface boundary;
    boundary.vertices = output.mesh.nodes;
    boundary.triangles.reserve(2 * output.boundary_quads.size());
    for (const auto& face : output.boundary_quads) {
        boundary.triangles.push_back({face[0], face[1], face[2]});
        if (face[3] != face[2]) {
            boundary.triangles.push_back({face[0], face[2], face[3]});
        }
    }

    const double limit = kGeometryFeatureResolutionOverH * requested_h;
    const double small_face_area = requested_h * requested_h;
    std::vector<bool> feature_face(inspection.face_count, false);
    for (std::size_t face_id = 0;
         face_id < inspection.face_count && face_id < topology.faces.size(); ++face_id) {
        feature_face[face_id] =
            topology.faces[face_id].kind != geom::CadSurfaceKind::kPlane ||
            topology.faces[face_id].area <= small_face_area;
    }

    // A small/curved CAD face is present only when the delivered mesh contains
    // an actual nearby boundary patch with a compatible normal. Point distance
    // alone cannot detect a vanished through-hole in a thin plate: its bore is
    // still close to the plate's top/bottom faces, whose normals are orthogonal.
    std::vector<bool> face_has_patch(inspection.face_count, false);
    for (const auto& face : output.boundary_quads) {
        const int n = face[3] == face[2] ? 3 : 4;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int i = 0; i < n; ++i) {
            centroid += output.mesh.nodes[face[static_cast<std::size_t>(i)]];
        }
        centroid /= static_cast<double>(n);
        Eigen::Vector3d normal =
            (output.mesh.nodes[face[1]] - output.mesh.nodes[face[0]])
                .cross(output.mesh.nodes[face[2]] - output.mesh.nodes[face[0]]);
        const double normal_norm = normal.norm();
        if (!(normal_norm > 0.0) || !std::isfinite(normal_norm)) {
            continue;
        }
        normal /= normal_norm;
        for (std::size_t face_id = 0; face_id < feature_face.size(); ++face_id) {
            if (!feature_face[face_id] || face_has_patch[face_id]) {
                continue;
            }
            const auto exact = geom::project_point_on_face(
                *model.cad, static_cast<std::uint32_t>(face_id), centroid);
            if (exact && exact->distance <= limit &&
                std::abs(normal.dot(exact->normal)) >= kNormalMinDot) {
                face_has_patch[face_id] = true;
            }
        }
    }

    std::vector<double> face_distance(inspection.face_count, 0.0);
    std::vector<bool> sampled(inspection.face_count, false);
    for (std::size_t i = 0; i < samples.points.size(); ++i) {
        if (i >= samples.face_ids.size() || samples.face_ids[i] >= face_distance.size()) {
            continue;
        }
        const auto face_id = samples.face_ids[i];
        const double distance =
            mesh::closest_on_surface(boundary, samples.points[i]).distance;
        face_distance[face_id] = std::max(face_distance[face_id], distance);
        sampled[face_id] = true;
    }

    double worst_distance = 0.0;
    std::uint32_t worst_face = 0;
    bool unresolved = false;
    for (std::size_t face_id = 0; face_id < feature_face.size(); ++face_id) {
        if (!feature_face[face_id] || face_has_patch[face_id]) {
            continue;
        }
        unresolved = true;
        if (sampled[face_id] && face_distance[face_id] >= worst_distance) {
            worst_distance = face_distance[face_id];
            worst_face = static_cast<std::uint32_t>(face_id);
        }
    }
    if (!unresolved) {
        return;
    }
    const auto& volume = output.fill_geometry_volume;
    throw GeometryVolumeLimitError(
        std::format(
            "feature unresolved at h={:.6g} m: CAD face {} has no aligned delivered "
            "boundary patch within {:.6g} m (sampled CAD-to-mesh max distance "
            "{:.6g} m); mesh/BRep volume relative error is {:.4g}. This Cartesian "
            "grid fill supports one local h/2 level, so a hole/void smaller than "
            "that level can disappear; reduce -h to <= {:.6g} m or raise "
            "--max-elems/--max-dof",
            requested_h, worst_face, limit, worst_distance, volume.relative_error,
            0.6 * requested_h),
        volume, false);
}

} // namespace

GeometryVolumeAssessment measure_geometry_volume(const Model& model,
                                                 const fea::NodalMesh& nodal) {
    GeometryVolumeAssessment out;
    if (!model.cad || model.cad->empty()) {
        return out;
    }
    const auto completeness =
        mesh::evaluate_geometry_completeness(*model.cad, fea::mesh_volume(nodal));
    if (!completeness.available) {
        return out;
    }
    out.available = true;
    out.mesh_volume = completeness.mesh_volume;
    out.cad_volume = completeness.brep_volume;
    out.relative_error = completeness.relative_volume_error;
    return out;
}

void update_solved_geometry_volume(const Model& model, VolumeMeshOutput& output) {
    output.solved_geometry_volume = measure_geometry_volume(model, output.mesh);
    if (!output.solved_geometry_volume.available) {
        return;
    }
    replace_geometry_volume_note(output.mesher_note, "solved",
                                 output.solved_geometry_volume);
    if (output.solved_geometry_volume.relative_error > kGeometryVolumeHardLimit) {
        throw GeometryVolumeLimitError(
            std::format("geometry solved-stage guard failed: mesh/BRep volume relative error "
                        "{:.4g} exceeds hard limit {:.4g}; solved geometry is incomplete | {}",
                        output.solved_geometry_volume.relative_error,
                        kGeometryVolumeHardLimit, output.mesher_note),
            output.solved_geometry_volume, true);

    }
}


// Internal: the public `volume_mesh` below wraps this to convert a zero-interior-cell
// fill into a resolution refusal. Nothing else about its behaviour changes.
static VolumeMeshOutput volume_mesh_impl(const Model& model, double h, VolumeMesher mesher,
                                         int skin_layers, bool feature_refine,
                                         std::span<const Eigen::Vector3d> refine_seeds,
                                         double seed_band, double element_tendency,
                                         std::size_t max_elems, std::size_t max_dof,
                                         int auto_retry_budget,
                                         const std::function<void()>& cancel_check,
                                         const mesh::SizeFieldFn& size_field) {
    const auto poll_cancel = [&] {
        if (cancel_check) {
            cancel_check();
        }
    };
    poll_cancel();
    const double predicted_elems = predict_mesh_elements(model, h);
    const double predicted_dof = 3.0 * predicted_elems;
    if (max_elems > 0 && predicted_elems > static_cast<double>(max_elems)) {
        throw std::runtime_error(std::format(
            "mesh element ceiling {} exceeded: predicted {:.0f} elements at h={:.6g} m; "
            "increase -h or raise --max-elems",
            max_elems, std::ceil(predicted_elems), h));
    }
    if (max_dof > 0 && predicted_dof > static_cast<double>(max_dof)) {
        throw std::runtime_error(std::format(
            "mesh DOF ceiling {} exceeded: predicted {:.0f} DOF ({:.0f} elements) at "
            "h={:.6g} m; increase -h or raise --max-dof",
            max_dof, std::ceil(predicted_dof), std::ceil(predicted_elems), h));
    }
    VolumeMeshOutput out;
    const VolumeMesher requested_mesher = mesher;
    const int requested_skin_layers = skin_layers;
    double fill_h = h;
    const auto tendency_plan = resolve_element_tendency(mesher, element_tendency, skin_layers);
    mesher = tendency_plan.mesher;
    skin_layers = tendency_plan.skin_layers;

    // One exact BRep oracle and one compact owner slot per eventual mesh node.
    // Unknown nodes classify on their first snap; known owners remain immutable.
    std::vector<mesh::BoundarySupport> boundary_provenance;
    mesh::BoundaryProjectionContext projection_context;
    mesh::BoundaryProjectionContext* projection = nullptr;
    if (model.cad && !model.cad->empty() &&
        (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem ||
         mesher == VolumeMesher::kGradedTet || mesher == VolumeMesher::kVaryhedron)) {
        try {
            if (make_boundary_projection(*model.cad, h, &projection_context,
                                         &boundary_provenance)) {
                projection = &projection_context;
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::format("exact BRep projection setup failed: {}", e.what()));
        } catch (...) {
            throw std::runtime_error("exact BRep projection setup failed");
        }
    }
    // Per-cell turning-angle refinement threshold (feature_refine paths): refine
    // where the surface turns more than this per bulk cell (h·κ). Angle-based,
    // so gentle curves / big bores stay coarse and flats never refine — replaces
    // the capped seed-ball scheme (coarse rings mid-bore, fine islands on flats).
    constexpr double kCurvatureTurnDeg = 15.0;
    if (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem) {
        // SPEC hybrid zoo: hex bulk @ h + 2:1 fine @ h/2 on feature/curvature
        // bands + conforming transitions.
        // kHybrid: product FE expands hex→pyramids (ADR-0012 / ADR-0013).
        // kHybridVem: keep hex as FE + unsplit transition polyhedra as VEM
        // (ADR-0019 fe-vem-assembly); no fan-split, no hex→pyramid expand.
        const bool native_poly = (mesher == VolumeMesher::kHybridVem);
        std::vector<geom::SharpEdge> edges;
        std::vector<Eigen::Vector3d> adapt_seeds(refine_seeds.begin(), refine_seeds.end());
        double feat_band = 0.0;
        double s_band = seed_band;
        double turn_deg = 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Feature band ~2 bulk cells so hole rims get a clear h/2 shell.
                feat_band = 2.0 * h;
            }
            turn_deg = kCurvatureTurnDeg;
        }
        // A-posteriori adapt seeds (caller) keep their ball semantics.
        if (s_band <= 0.0 && !adapt_seeds.empty()) {
            s_band = 2.0 * h;
        }
        if (adapt_seeds.empty()) {
            s_band = 0.0;
        }
        // Build lattice without snap first; product FE snaps after hex→pyramid
        // expand so free-surface Jacobian is pyramid-based. Native-poly path
        // keeps hex FE + poly VEM and snaps on that mesh.
        auto raw = mesh::mixed_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h,
            std::max(1, skin_layers), edges, feat_band, adapt_seeds, s_band,
            /*snap_boundary=*/false, turn_deg, native_poly, cancel_check, size_field,
            /*local_surface_classification=*/projection != nullptr);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_raw = raw.n_pyramid;
        const std::size_t n_poly_raw = raw.n_poly;
        // ADR-0013: the hex→pyramid product expansion exists so an
        // isoparametric hex8 never shares a face with a tet-split pyramid5 /
        // tet4 (the mixed-zoo patch test fails otherwise). A lattice that came
        // out pure hex — no 2:1 interface, so no fans — has no such face, and
        // expanding it only multiplies the element count by six while
        // downgrading hex8 to split-pyramid accuracy.
        const bool pure_hex_lattice = raw.n_pyramid == 0 && raw.n_tet == 0 && raw.n_poly == 0;
        auto fill = (native_poly || pure_hex_lattice)
                        ? std::move(raw)
                        : mesh::expand_mixed_hex_to_pyramids(raw);
        poll_cancel();
        fill_h = fill.h;
        // Post-expand free-surface snap (boundary quads from lattice).
        if (!fill.boundary_quads.empty()) {
            std::set<std::uint32_t> bset;
            for (const auto& q : fill.boundary_quads) {
                bset.insert(q.begin(), q.end());
            }
            std::vector<std::uint32_t> bnodes(bset.begin(), bset.end());
            const double h_snap = fill.h > 0.0 ? fill.h : h;
            const double vol_eps = 1e-14 * h_snap * h_snap * h_snap;
            // Fan tets must keep a usable shape: the snap used to flatten them
            // unchecked (only pyramids were guarded), leaving zero-aspect
            // boundary tets in the product mesh (M6 → 0 at fine h).
            const double kMinTetAspect = mesh::validity::kCellShapeFloor;
            const double kMinShape = mesh::validity::kCellShapeFloor;
            const auto tet_aspect_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                const double v = (b - a).dot((c - a).cross(d - a)) / 6.0;
                if (v <= vol_eps) {
                    return false;
                }
                const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                              (b - c).norm(), (b - d).norm(), (c - d).norm()});
                if (emax <= 0.0) {
                    return false;
                }
                return 6.0 * 1.4142135623730951 * v / (emax * emax * emax) >= kMinTetAspect;
            };
            // A pyramid is a single cell, not two independently quality-scored
            // tetrahedra. Require both halves of the conformity-safe assembly
            // split to stay positively oriented, then apply the shared shape
            // floor to VTK/PyVista's signed pyramid volume (the mean of both
            // base-diagonal volume sums). Using the minimum split-tet aspect
            // here made a healthy asymmetric pyramid fail and forced boundary
            // nodes back off the surface.
            const auto pyramid_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& p0 = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& p1 = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& p2 = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& p3 = fill.nodes[cell.nodes[3]];
                const Eigen::Vector3d& p4 = fill.nodes[cell.nodes[4]];
                const double v02a = mesh::validity::tet_signed_volume(p0, p1, p2, p4);
                const double v02b = mesh::validity::tet_signed_volume(p0, p2, p3, p4);
                const double v13a = mesh::validity::tet_signed_volume(p1, p2, p3, p4);
                const double v13b = mesh::validity::tet_signed_volume(p1, p3, p0, p4);
                double v0 = v02a;
                double v1 = v02b;
                if (mesh::validity::pyramid_split_diagonal(p0, p1, p2, p3) == 1) {
                    v0 = v13a;
                    v1 = v13b;
                }
                if (v0 <= vol_eps || v1 <= vol_eps) {
                    return false;
                }
                const double mean_edge =
                    ((p1 - p0).norm() + (p2 - p1).norm() + (p3 - p2).norm() +
                     (p0 - p3).norm() + (p4 - p0).norm() + (p4 - p1).norm() +
                     (p4 - p2).norm() + (p4 - p3).norm()) /
                    8.0;
                constexpr double kRegularPyramidVolumeRatio = 0.23570226039551587;
                const double volume = 0.5 * (v02a + v02b + v13a + v13b);
                const double collapse = mean_edge > 0.0
                                            ? volume / (mean_edge * mean_edge * mean_edge) /
                                                  kRegularPyramidVolumeRatio
                                            : 0.0;
                return volume > vol_eps && collapse >= kMinShape;
            };
            // The hex8 check `cell_valid` used to only promise: min detJ over
            // the centre and the 2×2×2 Gauss points the assembly integrates at.
            const auto hex_ok = [&](const mesh::MixedCell& cell) {
                std::array<Eigen::Vector3d, 8> x{};
                for (std::size_t i = 0; i < 8; ++i) {
                    x[i] = fill.nodes[cell.nodes[i]];
                }
                return mesh::validity::hex8_min_jacobian(x) > 0.0 &&
                       mesh::validity::hex8_shape_quality(x) >= kMinShape;
            };
            // One canonical snap predicate feeds both the global audit and the
            // per-node incident-cell query below. Keeping these identical is
            // what lets surface_project avoid full-mesh scans during a trial
            // move without weakening the final global validity sweep.
            const auto snap_cell_valid = [&](const mesh::MixedCell& cell) {
                if (cell.kind == mesh::MixedCellKind::kTet4) {
                    // Volume only: a fan tet flattened by a full snap is peeled
                    // right after (apex coplanar), cheaper than unsnapping wall.
                    const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                    const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                    const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                    const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                    return (b - a).dot((c - a).cross(d - a)) >= 0.0;
                }
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    std::vector<Eigen::Vector3d> coords;
                    coords.reserve(cell.poly_nodes.size());
                    for (const auto g : cell.poly_nodes) {
                        coords.push_back(fill.nodes[g]);
                    }
                    return fea::poly_volume(coords, cell.poly_faces) > vol_eps;
                }
                if (cell.kind == mesh::MixedCellKind::kHex8) {
                    return hex_ok(cell);
                }
                return cell.kind != mesh::MixedCellKind::kPyramid5 || pyramid_ok(cell);
            };

            // Boundary-node → incident-cell map. Trial line-search checks are
            // O(local degree), not O(all 47k cells). The global collector still
            // runs before/after bounded rounds to prove whole-mesh validity.
            std::vector<std::vector<std::size_t>> snap_node_cells(fill.nodes.size());
            std::vector<char> is_snap_node(fill.nodes.size(), 0);
            for (const auto ni : bnodes) {
                if (ni < is_snap_node.size()) {
                    is_snap_node[ni] = 1;
                }
            }
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                const auto& cell = fill.cells[ci];
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    for (const auto ni : cell.poly_nodes) {
                        if (ni < is_snap_node.size() && is_snap_node[ni]) {
                            snap_node_cells[ni].push_back(ci);
                        }
                    }
                } else {
                    for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                        const auto ni = cell.nodes[m];
                        if (ni < is_snap_node.size() && is_snap_node[ni]) {
                            snap_node_cells[ni].push_back(ci);
                        }
                    }
                }
            }

            std::size_t validity_poll = 0;
            const auto collect_snap_offenders = [&](std::set<std::uint32_t>& offenders) {
                for (const auto& cell : fill.cells) {
                    if ((validity_poll++ & 255U) == 0U) {
                        poll_cancel();
                    }
                    if (snap_cell_valid(cell)) {
                        continue;
                    }
                    if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                        offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                    } else {
                        offenders.insert(cell.nodes.begin(),
                                         cell.nodes.begin() + cell.n_nodes);
                    }
                }
            };
            const auto snap_node_offends = [&](std::uint32_t ni) {
                if (ni >= snap_node_cells.size()) {
                    return false;
                }
                for (const auto ci : snap_node_cells[ni]) {
                    if (!snap_cell_valid(fill.cells[ci])) {
                        return true;
                    }
                }
                return false;
            };
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, bnodes, h_snap, collect_snap_offenders,
                    /*max_move_frac=*/1.25, /*passes=*/8, edges,
                    [&] { mesh::repair_mixed_fan_apices(fill, kMinShape); }, snap_node_offends,
                    /*defer_coupled=*/fill.n_pyramid > 0 || fill.n_tet > 0, projection)
                    .max_residual;
            poll_cancel();
            // Peel snap-flattened fan tets: a full wall snap can pull all three
            // free nodes of a transition fan tet into the apex plane (aspect →
            // 0). The apex is then coplanar with the wall, so deleting the tet
            // exposes conforming faces with ~zero residual — better than
            // unsnapping the wall to save a degenerate element.
            {
                struct TriKey {
                    std::uint32_t a, b, c;
                    bool operator==(const TriKey& o) const {
                        return a == o.a && b == o.b && c == o.c;
                    }
                };
                struct TriHash {
                    std::size_t operator()(const TriKey& f) const noexcept {
                        std::size_t s = f.a;
                        s ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL + (s << 6) +
                             (s >> 2);
                        s ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL + (s << 6) +
                             (s >> 2);
                        return s;
                    }
                };
                const auto tkey = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    std::array<std::uint32_t, 3> v{{x, y, z}};
                    std::sort(v.begin(), v.end());
                    return TriKey{v[0], v[1], v[2]};
                };
                static constexpr int kTetTris[4][3] = {
                    {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
                static constexpr int kPyrTris[4][3] = {
                    {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
                const auto count_tris = [&]() {
                    std::unordered_map<TriKey, int, TriHash> tri_count;
                    tri_count.reserve(fill.cells.size() * 2);
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            ++tri_count[tkey(cell.nodes[static_cast<std::size_t>(tt[f][0])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][1])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][2])])];
                        }
                    }
                    return tri_count;
                };
                bool peeled_any = false;
                for (int pass = 0; pass < 3; ++pass) {
                    const auto tri_count = count_tris();
                    std::vector<char> kill(fill.cells.size(), 0);
                    std::size_t n_kill = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        const auto& cell = fill.cells[ci];
                        if (cell.kind != mesh::MixedCellKind::kTet4 || tet_aspect_ok(cell)) {
                            continue;
                        }
                        for (const auto& f : kTetTris) {
                            const auto it = tri_count.find(
                                tkey(cell.nodes[static_cast<std::size_t>(f[0])],
                                     cell.nodes[static_cast<std::size_t>(f[1])],
                                     cell.nodes[static_cast<std::size_t>(f[2])]));
                            if (it != tri_count.end() && it->second == 1) {
                                kill[ci] = 1;
                                ++n_kill;
                                break;
                            }
                        }
                    }
                    if (n_kill == 0 || n_kill >= fill.cells.size()) {
                        break;
                    }
                    std::size_t w = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        if (!kill[ci]) {
                            fill.cells[w++] = fill.cells[ci];
                        }
                    }
                    fill.cells.resize(w);
                    fill.n_tet -= n_kill;
                    peeled_any = true;
                }
                if (peeled_any) {
                    // Rebuild tri-encoded boundary entries from the surviving
                    // cells (true quads — pyramid bases — are unaffected by a
                    // tet peel and are kept as-is).
                    std::erase_if(fill.boundary_quads,
                                  [](const auto& q) { return q[2] == q[3]; });
                    const auto tri_count = count_tris();
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            const auto n0 = cell.nodes[static_cast<std::size_t>(tt[f][0])];
                            const auto n1 = cell.nodes[static_cast<std::size_t>(tt[f][1])];
                            const auto n2 = cell.nodes[static_cast<std::size_t>(tt[f][2])];
                            const auto it = tri_count.find(tkey(n0, n1, n2));
                            if (it != tri_count.end() && it->second == 1) {
                                fill.boundary_quads.push_back({{n0, n1, n2, n2}});
                            }
                        }
                    }
                }
            }
            // Per-node outlier re-projection (mirror of graded S3): residual
            // stragglers get a full/partial projection accepted only when every
            // incident cell stays valid. After expand all cells are pyramid/tet.
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> node_cells;
            poll_cancel();
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                if ((ci & 255U) == 0U) {
                    poll_cancel();
                }
                const auto& cell = fill.cells[ci];
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    for (const auto g : cell.poly_nodes) {
                        node_cells[g].push_back(ci);
                    }
                } else {
                    for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                        node_cells[cell.nodes[m]].push_back(ci);
                    }
                }
            }
            const auto cell_valid = [&](const mesh::MixedCell& cell) {
                if (cell.kind == mesh::MixedCellKind::kTet4) {
                    return tet_aspect_ok(cell);
                }
                if (cell.kind == mesh::MixedCellKind::kPyramid5) {
                    return pyramid_ok(cell);
                }
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    std::vector<Eigen::Vector3d> coords;
                    coords.reserve(cell.poly_nodes.size());
                    for (const auto g : cell.poly_nodes) {
                        coords.push_back(fill.nodes[g]);
                    }
                    return fea::poly_volume(coords, cell.poly_faces) > vol_eps;
                }
                if (cell.kind == mesh::MixedCellKind::kHex8) {
                    return hex_ok(cell);
                }
                return true;
            };
            const double thr = 0.08 * h_snap;
            double max_resid = 0.0;
            std::size_t reprojection_poll = 0;
            for (const auto ni : bnodes) {
                if ((reprojection_poll++ & 63U) == 0U) {
                    poll_cancel();
                }
                if (ni >= fill.nodes.size()) {
                    continue;
                }
                const auto target = mesh::boundary_projection_target(
                    model.surface, fill.nodes[ni], ni, projection);
                if (!target) {
                    continue;
                }
                double resid = target->distance;
                if (resid > thr && resid <= 2.5 * h_snap) {
                    const Eigen::Vector3d saved = fill.nodes[ni];
                    static constexpr double kFracs[] = {1.0, 0.6, 0.35};
                    for (const double frac : kFracs) {
                        fill.nodes[ni] = saved + frac * (target->point - saved);
                        bool ok = true;
                        const auto it = node_cells.find(ni);
                        if (it != node_cells.end()) {
                            for (const auto ci : it->second) {
                                if (!cell_valid(fill.cells[ci])) {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (ok) {
                            resid = (1.0 - frac) * resid;
                            break;
                        }
                        fill.nodes[ni] = saved;
                    }
                }
                max_resid = std::max(max_resid, resid);
            }
            fill.boundary_max_distance = max_resid;
            poll_cancel();
            // Tangential smoothing: even out lattice-stair spacing on curved
            // walls / hole rims (crease nodes relax along the crease). Moves
            // are re-projected so the residual cannot grow; any move that
            // invalidates a cell is reverted.
            validity_poll = 0;
            const auto smooth_st = mesh::smooth_boundary_nodes(
                model.surface, fill.nodes, fill.boundary_quads, h_snap,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : fill.cells) {
                        if ((validity_poll++ & 255U) == 0U) {
                            poll_cancel();
                        }
                        if (cell_valid(cell)) {
                            continue;
                        }
                        if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                            offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                        } else {
                            for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                                offenders.insert(cell.nodes[m]);
                            }
                        }
                    }
                },
                /*passes=*/3, /*relax=*/0.5, edges, projection);
            poll_cancel();
            if (smooth_st.n_moved > 0) {
                fill.boundary_max_distance = smooth_st.max_residual;
            }
            // The mixed-cell merge survey never collapsed a node on any STEP
            // fixture, so no topology-repair path remains. This post-smoothing
            // projection round is active, however: without it icecream_cone at
            // h_rel=.20 measured exact-BRep p99/h=.191; with it, .0594.
            std::set<std::uint32_t> final_boundary_set;
            for (const auto& face : fill.boundary_quads) {
                final_boundary_set.insert(face.begin(), face.end());
            }
            const std::vector<std::uint32_t> final_boundary_nodes(
                final_boundary_set.begin(), final_boundary_set.end());
            const auto final_node_offends = [&](std::uint32_t node) {
                const auto it = node_cells.find(node);
                if (it == node_cells.end()) {
                    return false;
                }
                for (const auto cell_index : it->second) {
                    if (!cell_valid(fill.cells[cell_index])) {
                        return true;
                    }
                }
                return false;
            };
            validity_poll = 0;
            const auto collect_final_offenders =
                [&](std::set<std::uint32_t>& offenders) {
                for (const auto& cell : fill.cells) {
                    if ((validity_poll++ & 255U) == 0U) {
                        poll_cancel();
                    }
                    if (cell_valid(cell)) {
                        continue;
                    }
                    if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                        offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                    } else {
                        offenders.insert(cell.nodes.begin(),
                                         cell.nodes.begin() + cell.n_nodes);
                    }
                }
            };
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, final_boundary_nodes, h_snap,
                    collect_final_offenders, /*max_move_frac=*/1.25, /*passes=*/4, edges,
                    [&] { mesh::repair_mixed_fan_apices(fill, kMinShape); },
                    final_node_offends, /*defer_coupled=*/true, projection)
                    .max_residual;
            poll_cancel();

        }
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        std::size_t conversion_poll = 0;
        for (const auto& cell : fill.cells) {
            if ((conversion_poll++ & 255U) == 0U) {
                poll_cancel();
            }
            if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                out.mesh.elements.emplace_back(fea::ElementType::kPolyVem, cell.poly_nodes,
                                               cell.poly_faces);
            } else if (cell.kind == mesh::MixedCellKind::kPyramid5) {
                std::array<std::uint32_t, 5> p{{cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                                cell.nodes[3], cell.nodes[4]}};
                // Normalize winding at the final coordinates: snap rollback and
                // smoothing happen after mixed-cell emission, so a pre-existing
                // all-negative winding can otherwise survive as an offender
                // with no moved node to restore. VTK's signed pyramid volume is
                // the mean of the two base-diagonal tet-volume sums.
                const auto& x0 = out.mesh.nodes[p[0]];
                const auto& x1 = out.mesh.nodes[p[1]];
                const auto& x2 = out.mesh.nodes[p[2]];
                const auto& x3 = out.mesh.nodes[p[3]];
                const auto& xa = out.mesh.nodes[p[4]];
                const double vtk_volume =
                    0.5 * (mesh::validity::tet_signed_volume(x0, x1, x2, xa) +
                           mesh::validity::tet_signed_volume(x0, x2, x3, xa) +
                           mesh::validity::tet_signed_volume(x1, x2, x3, xa) +
                           mesh::validity::tet_signed_volume(x1, x3, x0, xa));
                if (vtk_volume < 0.0) {
                    std::swap(p[1], p[3]);
                }
                // VTK/PyVista triangulate pyramid5 along local 0-2. Rotate the
                // cyclic base so it is the conformity-safe assembly diagonal.
                if (mesh::validity::pyramid_split_diagonal(
                        out.mesh.nodes[p[0]], out.mesh.nodes[p[1]], out.mesh.nodes[p[2]],
                        out.mesh.nodes[p[3]]) == 1) {
                    std::rotate(p.begin(), p.begin() + 1, p.begin() + 4);
                }
                out.mesh.elements.push_back(fea::NodalElement{fea::ElementType::kPyramid5,
                                                              {p[0], p[1], p[2], p[3], p[4]}});
            } else if (cell.kind == mesh::MixedCellKind::kHex8) {
                out.mesh.elements.push_back(fea::NodalElement{
                    fea::ElementType::kHex8,
                    {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4],
                     cell.nodes[5], cell.nodes[6], cell.nodes[7]}});
            } else {
                out.mesh.elements.push_back(fea::NodalElement{
                    fea::ElementType::kTet4,
                    {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]}});
            }
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.local_child_boundary_quads = std::move(fill.local_child_boundary_quads);
        if (native_poly) {
            out.mesher_note = std::format(
                "hybrid-VEM zoo (hex FE bulk@h + 2:1 fine@h/2 + native poly VEM "
                "transitions; not Delaunay): {} hex8 + {} polyVEM ({} pyr raw unused), "
                "{} nodes, h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} "
                "feature={}, snap max|d|={:.3g} m{}",
                fill.n_hex, fill.n_poly, n_pyr_raw, out.mesh.nodes.size(), fill.h,
                fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
            (void)n_hex_lattice;
            (void)n_poly_raw;
        } else {
            out.mesher_note = std::format(
                "hybrid zoo v4 (hex bulk@h + 2:1 fine@h/2 + conforming fan transition; "
                "not Delaunay): {} hex + {} pyr raw → {} pyramid5 + {} tet4, {} nodes, "
                "h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} feature={}, "
                "snap max|d|={:.3g} m{}",
                n_hex_lattice, n_pyr_raw, fill.n_pyramid, fill.n_tet, out.mesh.nodes.size(),
                fill.h, fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
        }
        if (size_field) {
            out.mesher_note += std::format(
                " | size_field h_min={:.4g} h_max={:.4g} m, levels L0={} L1={}{}",
                fill.field_h_min, fill.field_h_max, fill.n_level0_cells, fill.n_level1_cells,
                fill.n_field_budget_clamped > 0
                    ? std::format(", {} cells clamped at budget floor",
                                  fill.n_field_budget_clamped)
                    : std::string{});
        }
        if (fill.classification_refinement_levels > 0) {
            out.mesher_note += std::format(
                " | feature-aware classify L{} volume_err={:.4g}",
                fill.classification_refinement_levels, fill.classification_volume_error);
        }
    } else if (mesher == VolumeMesher::kHexFill || mesher == VolumeMesher::kHexVem) {
        auto fill = mesh::hex_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.hexes.size());
        for (const auto& hx : fill.hexes) {
            fea::NodalElement el{fea::ElementType::kHex8,
                                 {hx[0], hx[1], hx[2], hx[3], hx[4], hx[5], hx[6], hx[7]}};
            if (mesher == VolumeMesher::kHexVem) {
                auto poly = fea::hex8_as_poly(el);
                el.type = fea::ElementType::kPolyVem;
                el.faces = std::move(poly.faces);
            }
            out.mesh.elements.push_back(std::move(el));
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "{} grid fill v1 (Cartesian, not CAD-fitted): {} cells, {} nodes, h={:.4g} m, "
            "snap max|d|={:.3g} m",
            mesher == VolumeMesher::kHexVem ? "hex-VEM" : "hex", out.mesh.elements.size(),
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kHexPyramid) {
        // Topology: hex core + pyramid skin (ADR-0013). Product FE path expands
        // each interior hex to six pyramids (centroid apex) so face diagonals
        // match the tet-split pyramid skin — constant-strain patch exact.
        auto raw =
            mesh::transition_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_skin = raw.n_pyramid;
        auto fill = mesh::expand_hex_core_to_pyramids(raw);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPyramid5,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "hex+pyramid product FE (Cartesian grid, not Delaunay; all-pyramid expand): "
            "{} lattice hex → {} pyramids ({} skin + {} from hex), {} nodes, h={:.4g} m, "
            "boundary max|d|={:.3g} m",
            n_hex_lattice, fill.n_pyramid, n_pyr_skin, fill.n_pyramid - n_pyr_skin,
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kPrismSweep) {
        // Cartesian prism6 wedges along the longest bbox axis (ROADMAP C3).
        // Not CAD extrusion detection — same grid-fill honesty as tet/hex (ADR-0015).
        auto fill = mesh::prism_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.prisms.size());
        for (const auto& pr : fill.prisms) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPrism6, {pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        static constexpr const char* kAxis[] = {"x", "y", "z"};
        const char* axis_name =
            (fill.sweep_axis >= 0 && fill.sweep_axis < 3) ? kAxis[fill.sweep_axis] : "?";
        out.mesher_note =
            std::format("prism sweep grid fill v1 (Cartesian, not CAD extrusion; sweep={}): "
                        "{} prism6, {} nodes, h={:.4g} m, snap max|d|={:.3g} m",
                        axis_name, out.mesh.elements.size(), out.mesh.nodes.size(), fill_h,
                        fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kGradedTet) {
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        // Caller a-posteriori adapt seeds keep ball semantics; curvature is now
        // the per-cell turning-angle criterion inside the fill (no caps, no
        // stray islands). Thin walls (t < 2.5 h) still seed *locally* — but a
        // globally thin part (most of the surface thin) is an h problem, not a
        // local feature: seeding it would just scatter capped fine islands.
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = 0.0;
        std::size_t n_thin_seeds = 0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Crease band ~ two bulk cells so hole rims get a clear L1/L2 shell.
                feature_band = 2.0 * h;
            }
            turn_deg = kCurvatureTurnDeg;
            const auto thick = geom::estimate_local_thickness(model.surface);
            std::vector<Eigen::Vector3d> thin_pts;
            std::size_t n_finite = 0;
            for (std::size_t i = 0; i < model.surface.vertices.size(); ++i) {
                if (i >= thick.thickness.size() ||
                    !geom::has_finite_thickness(thick.thickness[i])) {
                    continue;
                }
                ++n_finite;
                if (thick.thickness[i] < 2.5 * h) {
                    thin_pts.push_back(model.surface.vertices[i]);
                }
            }
            const bool globally_thin =
                n_finite > 0 &&
                thin_pts.size() * 3 > model.surface.vertices.size(); // > ~1/3 thin
            if (!globally_thin && !thin_pts.empty()) {
                if (band <= 0.0) {
                    band = 1.6 * h;
                }
                // Spatial thinning: min sep 0.75 h, capped, caller seeds first.
                constexpr std::size_t kMaxGeoSeeds = 256;
                const double min_sep2 = (0.75 * h) * (0.75 * h);
                for (const auto& p : thin_pts) {
                    if (seeds.size() >= kMaxGeoSeeds) {
                        break;
                    }
                    bool far = true;
                    for (const auto& q : seeds) {
                        if ((p - q).squaredNorm() < min_sep2) {
                            far = false;
                            break;
                        }
                    }
                    if (far) {
                        seeds.push_back(p);
                        ++n_thin_seeds;
                    }
                }
            }
            // Flat solids: free-surface skin alone is enough (no fake seed flood).
            if (band > 0.0 && seeds.empty()) {
                band = 0.0;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * h;
        }
        auto graded = mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, seeds, band, turn_deg, projection, size_field);
        fill_h = graded.h_fine;
        out.mesh.nodes = std::move(graded.mesh.nodes);
        out.mesh.elements.reserve(graded.mesh.tets.size());
        for (const auto& tet : graded.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        // Prefer true exterior faces after LEB (includes mid-edge free-surface nodes).
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(graded.mesh.boundary_quads);
        }
        std::vector<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(bnodes.end(), q.begin(), q.end());
        }
        std::sort(bnodes.begin(), bnodes.end());
        bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, bnodes);
        const char* budget_note =
            (graded.h_fine > (h / static_cast<double>(graded.subdivision)) * 1.05)
                ? ", h raised to cell budget"
                : "";
        out.mesher_note = std::format(
            "graded tet v6 (multi-level LEB geo + cap collapse/void carve): {} tets ({} bulk, "
            "{} refined L1/L2, "
            "{} feature, {} seed), h_bulk={:.4g}/h_L2~{:.4g} m (L0/L1/L2), "
            "snap max|d|={:.3g} m mean|d|={:.3g} m"
            "{}{}{}",
            out.mesh.elements.size(), graded.n_coarse_cells, graded.n_fine_cells,
            graded.n_feature_cells, graded.n_seed_cells, graded.h_coarse, graded.h_fine,
            conf.max_distance, conf.mean_distance, budget_note,
            turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg) : std::string{},
            n_thin_seeds > 0 ? std::format(", thin_seeds={}", n_thin_seeds) : std::string{});
        if (size_field) {
            out.mesher_note += std::format(
                " | size_field h_min={:.4g} h_max={:.4g} m, levels L0={} L1={} L2={}{}",
                graded.field_h_min, graded.field_h_max, graded.n_level0_cells,
                graded.n_level1_cells, graded.n_level2_cells,
                graded.n_field_budget_clamped > 0
                    ? std::format(", {} cells clamped at budget floor",
                                  graded.n_field_budget_clamped)
                    : std::string{});
        }
        if (graded.classification_refinement_levels > 0) {
            out.mesher_note += std::format(
                " | feature-aware classify L{} volume_err={:.4g}",
                graded.classification_refinement_levels,
                graded.classification_volume_error);
        }
    } else if (mesher == VolumeMesher::kOctahedral) {
        // Experimental BCC octahedra → tet4 (ADR-0019). Not a product claim.
        auto fill = mesh::octa_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.mesh.boundary_quads);
        out.mesher_note =
            std::format("octahedral experimental (BCC face-octa → tet4; not product): "
                        "{} tets ({} octa + {} bdy pyr), {} nodes, h={:.4g} m",
                        out.mesh.elements.size(), fill.n_octahedra, fill.n_boundary_pyramids,
                        out.mesh.nodes.size(), fill_h);
    } else if (mesher == VolumeMesher::kVaryhedron) {
        // ADR-0021 v1: CAD edge seeds + graded scaffold + edge-profile snap.
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = feature_refine ? 15.0 : 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                feature_band = 2.0 * h;
            }
            if (band <= 0.0 && !seeds.empty()) {
                band = 1.6 * h;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * h;
        }

        // Prefer retained Model::cad (ADR-0020 / V1c); fall back to reloading
        // the source CAD path when the model was surface-only (legacy).
        std::optional<geom::CadModel> cad_reload;
        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            try {
                topo = geom::extract_topology(*model.cad, 10);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        } else if (geom::occ_enabled()) {
            std::vector<std::string> candidates;
            if (!model.source_path.empty()) {
                candidates.push_back(model.source_path);
            }
            if (!model.name.empty()) {
                candidates.push_back(model.name);
                candidates.push_back(std::string("tests/fixtures/parts/") + model.name);
                candidates.push_back(std::string("tests/fixtures/") + model.name);
            }
            for (const auto& cand : candidates) {
                try {
                    std::string low = cand;
                    for (char& c : low) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (low.ends_with(".step") || low.ends_with(".stp")) {
                        cad_reload = geom::CadModel::load_step(cand);
                    } else if (low.ends_with(".brep") || low.ends_with(".brp")) {
                        cad_reload = geom::CadModel::load_brep(cand);
                    } else {
                        continue;
                    }
                    if (cad_reload && !cad_reload->empty()) {
                        topo = geom::extract_topology(*cad_reload, 10);
                        topo_ptr = &(*topo);
                        break;
                    }
                } catch (...) {
                    cad_reload.reset();
                    topo.reset();
                    topo_ptr = nullptr;
                }
            }
        }

        // Live BRep for M10 wall free-slide + OCC re-project (optional).
        const geom::CadModel* cad_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            cad_ptr = &(*model.cad);
        } else if (cad_reload && !cad_reload->empty()) {
            cad_ptr = &(*cad_reload);
        }

        auto fill = mesh::varyhedron_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, seeds, band, turn_deg, topo_ptr, cad_ptr,
            /*wall_smooth_iters=*/4, projection);
        fill_h = fill.h_fine;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(fill.mesh.boundary_quads);
        }
        // Packing-seed engine (ADR-0023): sharp-only protect; dual deferred; CVT path later.
        // Protect radii sized by min(α h, β lfs) (CDS / advisor Q6).
        // M10: wall free-slide + OCC surface re-project after sharp snap.
        out.mesher_note = std::format(
            "varyhedron packing (sharp-only edge protect + interior bubble seeds + graded "
            "scaffold + sharp snap + wall OCC project; dual deferred; CVT target): "
            "{} tets, edge_seeds={}, r_protect=[{:.3g},{:.3g}], "
            "sharp/smooth/seam={}/{}/{}, vol_seeds={}, pack_relax={}, "
            "pack_fill={:.3g}, h_bulk={:.4g}/h_fine={:.4g} m, edge_Hd={:.3g} m "
            "(rel={:.3g}, /h={:.3g}, e_chord={:.3g}), wall_nodes={}/moved={}/iters={}{}",
            out.mesh.elements.size(), fill.n_edge_seeds, fill.min_protect_radius,
            fill.max_protect_radius, fill.n_sharp_edges, fill.n_smooth_edges,
            fill.n_seam_edges, fill.n_volume_seeds, fill.n_pack_relax_iters,
            fill.pack_fill_frac, fill.h_coarse, fill.h_fine, fill.edge_profile_hausdorff_max,
            fill.edge_profile_rel, fill.edge_hausdorff_over_h,
            fill.edge_chordal_efficiency_max, fill.n_wall_nodes, fill.n_wall_moved,
            fill.n_wall_iters,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=surface_only");
    } else if (mesher == VolumeMesher::kCvtPoly) {
        // G1–G4 product poly path: constrained restricted CVT → clipped cells → VEM.
        // Free sites at surface-interior cell centres (not full AABB lattice).
        // M5: export clips cells to local surface halfspaces so boundary faces
        // sit on the solid (not the bbox). Free sites stay strictly interior —
        // projecting onto the surface creates zero-thickness domain clips.
        if (!mesh::geogram_available()) {
            throw std::runtime_error(
                "cvt_poly mesher requires POLYMESH_WITH_GEOGRAM (Geogram ConvexCell)");
        }
        mesh::ClipBox box;
        box.min = model.bbox_min;
        box.max = model.bbox_max;
        const Eigen::Vector3d ext = (box.max - box.min).cwiseMax(1e-30);
        const double diag = ext.norm();

        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            try {
                topo = geom::extract_topology(*model.cad, 8);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        }

        // Sharp fixed protectors + free sites from interior grid cells.
        mesh::ConstrainedSiteSeedParams seed_p;
        seed_p.interior_n_side = 0; // no full-AABB lattice; we inject interior free sites
        seed_p.sharp_sample_stride = 1;
        // Slightly denser sharp protectors for hole SCF (0.20 vs 0.25); 0.16
        // raised plate residual.
        seed_p.sharp_min_sep_frac = 0.20 * h / diag;
        auto seeded = mesh::seed_constrained_cvt_sites(box, topo_ptr, seed_p);

        // Plate-like (high aspect) gets hole-local densify for SCF.
        const Eigen::Vector3d extent = (box.max - box.min).cwiseMax(1e-30);
        const double aspect = extent.maxCoeff() / extent.minCoeff();
        const bool plate_like = aspect > 5.0; // plate_hole ~10–20; cylinder ~ few
        const double h_site = std::max(0.9 * h, 1e-9);
        mesh::CartesianGrid grid =
            mesh::make_bbox_grid(model.bbox_min, model.bbox_max, h_site);
        const auto inside = mesh::classify_cells_inside(model.surface, grid);
        const double min_sep = seed_p.sharp_min_sep_frac * diag;
        const double min_sep2 = min_sep * min_sep;
        // Spatial hash over site positions so min-separation and nearest-sharp
        // queries are O(1) per grid cell, not O(N). The frame has ~16k sharp
        // sites; the old linear scans made cvt_poly seeding O(N^2) → it hung.
        const double hg = std::max(0.35 * h, 1e-12);
        const double inv_hg = 1.0 / hg;
        struct BKey {
            long long a, b, c;
            bool operator==(const BKey& o) const { return a == o.a && b == o.b && c == o.c; }
        };
        struct BKeyHash {
            std::size_t operator()(const BKey& k) const noexcept {
                std::size_t h = static_cast<std::size_t>(k.a) * 73856093ull;
                h ^= static_cast<std::size_t>(k.b) * 19349663ull + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                h ^= static_cast<std::size_t>(k.c) * 83492791ull + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                return h;
            }
        };
        auto bkey = [&](const Eigen::Vector3d& p) -> BKey {
            return BKey{static_cast<long long>(std::floor(p.x() * inv_hg)),
                        static_cast<long long>(std::floor(p.y() * inv_hg)),
                        static_cast<long long>(std::floor(p.z() * inv_hg))};
        };
        std::unordered_map<BKey, std::vector<Eigen::Vector3d>, BKeyHash> site_hash;
        // Sharp hash uses a coarser cell (≈ the densify band) so nearest-sharp is
        // a fixed 27-bucket lookup, not a deep ring search in sharp-free regions.
        const double sg = std::max(3.2 * h, 1e-12);
        const double inv_sg = 1.0 / sg;
        auto skey = [&](const Eigen::Vector3d& p) -> BKey {
            return BKey{static_cast<long long>(std::floor(p.x() * inv_sg)),
                        static_cast<long long>(std::floor(p.y() * inv_sg)),
                        static_cast<long long>(std::floor(p.z() * inv_sg))};
        };
        std::unordered_map<BKey, std::vector<Eigen::Vector3d>, BKeyHash> sharp_hash;
        std::vector<Eigen::Vector3d> sharp_pos;
        for (const auto& s : seeded.sites) {
            site_hash[bkey(s.pos)].push_back(s.pos);
            if (s.fixed) {
                sharp_hash[skey(s.pos)].push_back(s.pos);
                sharp_pos.push_back(s.pos);
            }
        }
        auto site_far = [&](const Eigen::Vector3d& p, double sep2) -> bool {
            const int R = std::max(1, static_cast<int>(std::ceil(std::sqrt(sep2) * inv_hg)));
            const BKey k0 = bkey(p);
            for (int dz = -R; dz <= R; ++dz) {
                for (int dy = -R; dy <= R; ++dy) {
                    for (int dx = -R; dx <= R; ++dx) {
                        auto it = site_hash.find(BKey{k0.a + dx, k0.b + dy, k0.c + dz});
                        if (it == site_hash.end()) {
                            continue;
                        }
                        for (const auto& q : it->second) {
                            if ((q - p).squaredNorm() < sep2) {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        };
        auto add_site = [&](const mesh::CvtSite& s) {
            seeded.sites.push_back(s);
            site_hash[bkey(s.pos)].push_back(s.pos);
        };
        auto dist_to_sharp = [&](const Eigen::Vector3d& p) -> double {
            // Coarse cell = band, so all sharps within the densify band lie in the
            // 3×3×3 neighbourhood — a fixed 27-bucket scan, exact for d ≤ band.
            const BKey k0 = skey(p);
            double best2 = 1e300;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto it = sharp_hash.find(BKey{k0.a + dx, k0.b + dy, k0.c + dz});
                        if (it == sharp_hash.end()) {
                            continue;
                        }
                        for (const auto& q : it->second) {
                            best2 = std::min(best2, (q - p).squaredNorm());
                        }
                    }
                }
            }
            return std::sqrt(best2);
        };

        // Infer hole radius from sharp fixed sites near the plate mid-plane
        // (top/bottom circular rims). Fallback 0.01 matches plate_hole fixture.
        double hole_r = 0.01;
        if (plate_like && !sharp_pos.empty()) {
            double r_sum = 0.0;
            std::size_t r_n = 0;
            const double z_mid = 0.5 * (box.min.z() + box.max.z());
            const double z_band = 0.35 * extent.z();
            for (const auto& q : sharp_pos) {
                if (std::abs(q.z() - z_mid) > z_band) {
                    continue;
                }
                const double r = q.head<2>().norm();
                if (r > 1e-6 && r < 0.4 * extent.head<2>().minCoeff()) {
                    r_sum += r;
                    ++r_n;
                }
            }
            if (r_n >= 4) {
                hole_r = r_sum / static_cast<double>(r_n);
            }
        }

        std::size_t n_interior_free = 0;
        std::size_t n_feature_free = 0;
        for (int k = 0; k < grid.nz; ++k) {
            for (int j = 0; j < grid.ny; ++j) {
                for (int i = 0; i < grid.nx; ++i) {
                    if (!inside[grid.index(i, j, k)]) {
                        continue;
                    }
                    const Eigen::Vector3d c = grid.cell_center(i, j, k);
                    if (!site_far(c, min_sep2)) {
                        continue;
                    }
                    mesh::CvtSite s;
                    s.pos = c;
                    s.fixed = false;
                    add_site(s);
                    ++n_interior_free;

                    // Plate-like only: mild in-plane densify near sharp features
                    // (grid half-offsets). Aggressive packing / graded size fields
                    // lowered measured SCF — keep this light.
                    if (!plate_like) {
                        continue;
                    }
                    const double d_sharp = dist_to_sharp(c);
                    const double r_xy = c.head<2>().norm();
                    const bool near_hole = r_xy < hole_r + 2.5 * h;
                    const double band = near_hole ? 3.2 * h : 2.6 * h;
                    if (d_sharp < band && d_sharp > 0.18 * h) {
                        static constexpr double kOff[][2] = {
                            {1, 0},  {-1, 0},  {0, 1},   {0, -1},   {1, 1},   {1, -1},
                            {-1, 1}, {-1, -1}, {0.5, 0}, {-0.5, 0}, {0, 0.5}, {0, -0.5},
                        };
                        const double sep = near_hole ? 0.28 * h : 0.33 * h;
                        const double local_sep2 = sep * sep;
                        const double scale = near_hole ? 0.36 : 0.40;
                        for (const auto& o : kOff) {
                            Eigen::Vector3d p = c;
                            p[0] += scale * grid.cell[0] * o[0];
                            p[1] += scale * grid.cell[1] * o[1];
                            if (std::abs(p[0] - c[0]) > 0.48 * grid.cell[0] ||
                                std::abs(p[1] - c[1]) > 0.48 * grid.cell[1]) {
                                continue;
                            }
                            if (!site_far(p, local_sep2)) {
                                continue;
                            }
                            mesh::CvtSite fs;
                            fs.pos = p;
                            fs.fixed = false;
                            add_site(fs);
                            ++n_feature_free;
                            ++n_interior_free;
                        }
                    }
                }
            }
        }

        // Cylinder: free sites on an inset cylindrical shell (smooth curved wall
        // is not sharp-protected). Improves SE without wall-pull stiffening.
        if (!plate_like && topo_ptr) {
            const double z_lo = box.min.z() + 0.08 * extent.z();
            const double z_hi = box.max.z() - 0.08 * extent.z();
            // Outer radius from sharp rims (top/bottom circles).
            double R_out = 0.5 * extent.head<2>().minCoeff();
            {
                double r_sum = 0.0;
                std::size_t r_n = 0;
                for (const auto& q : sharp_pos) {
                    const double r = q.head<2>().norm();
                    if (r > 0.2 * R_out) {
                        r_sum += r;
                        ++r_n;
                    }
                }
                if (r_n >= 4) {
                    R_out = r_sum / static_cast<double>(r_n);
                }
            }
            const double shell_r = std::max(R_out - 0.35 * h, 0.5 * R_out);
            const double shell_sep = 0.32 * h;
            const double shell_sep2 = shell_sep * shell_sep;
            const int n_ang =
                std::max(16, static_cast<int>(std::ceil(2.0 * 3.141592653589793 * shell_r /
                                                        std::max(0.30 * h, 1e-9))));
            const int n_z = std::max(
                4, static_cast<int>(std::ceil((z_hi - z_lo) / std::max(0.40 * h, 1e-9))));
            // A genuine inset-cylinder shell is modest; a bogus cylinder fit on a
            // non-cylinder part (e.g. a frame) or an over-fine h explodes n_ang·n_z
            // into millions of shell candidates. Skip when it isn't cylinder-scale.
            const long long shell_budget =
                static_cast<long long>(n_ang) * static_cast<long long>(n_z + 1);
            if (shell_budget <= 200000) {
                for (int iz = 0; iz <= n_z; ++iz) {
                    const double z = z_lo + (z_hi - z_lo) * static_cast<double>(iz) /
                                                static_cast<double>(std::max(n_z, 1));
                    for (int a = 0; a < n_ang; ++a) {
                        const double th = 2.0 * 3.141592653589793 * static_cast<double>(a) /
                                          static_cast<double>(n_ang);
                        Eigen::Vector3d p(shell_r * std::cos(th), shell_r * std::sin(th), z);
                        if (!site_far(p, shell_sep2)) {
                            continue;
                        }
                        const int gi = static_cast<int>(
                            std::floor((p.x() - grid.origin.x()) / grid.cell[0]));
                        const int gj = static_cast<int>(
                            std::floor((p.y() - grid.origin.y()) / grid.cell[1]));
                        const int gk = static_cast<int>(
                            std::floor((p.z() - grid.origin.z()) / grid.cell[2]));
                        if (gi < 0 || gj < 0 || gk < 0 || gi >= grid.nx || gj >= grid.ny ||
                            gk >= grid.nz) {
                            continue;
                        }
                        if (!inside[grid.index(gi, gj, gk)]) {
                            continue;
                        }
                        mesh::CvtSite fs;
                        fs.pos = p;
                        fs.fixed = false;
                        add_site(fs);
                        ++n_feature_free;
                        ++n_interior_free;
                    }
                }
            }
        }

        seeded.n_interior_free = n_interior_free;

        mesh::CvtLloydParams lloyd;
        // Slightly more Lloyd for free-site equilibrium (helps SE a little).
        lloyd.max_iters = 32;
        lloyd.move_tol_rel = 7e-4;
        // Geometry-graded density (ADR-0021): curvature / thin-wall drive cell
        // size and count; gradient-limited (β=1) so grading is smooth. Flat
        // surfaces emit no source ⇒ falls back to uniform h.
        auto poly_size = std::make_shared<adapt::GradedSizing>(
            adapt::geometry_size_sources(model.surface, 0.35 * h, h), 0.35 * h, h, 1.0);
        lloyd.size_at = [poly_size](const Eigen::Vector3d& p) {
            return poly_size->size_at(p);
        };

        auto sites = seeded.sites;
        const auto lr = mesh::lloyd_cvt(box, sites, lloyd);
        for (std::size_t i = 0; i < sites.size() && i < lr.positions.size(); ++i) {
            if (!sites[i].fixed) {
                sites[i].pos = lr.positions[i];
            }
        }
        // Soft wall pull: plate needs interior inset for RVD; cylinder keeps
        // sites where Lloyd left them (better SE — wall pull stiffened response).
        const geom::CadModel* cad_ptr =
            (model.cad && !model.cad->empty()) ? &(*model.cad) : nullptr;
        if (cad_ptr && plate_like) {
            const double wall_band = 0.12 * diag;
            const double inset = std::max(0.15 * h, 1e-4 * diag);
            const double sharp_guard = 0.04 * diag;
            for (auto& s : sites) {
                if (s.fixed) {
                    continue;
                }
                if (topo_ptr && sharp_guard > 0.0) {
                    if (auto q = geom::closest_edge(*topo_ptr, s.pos, /*sharp_only=*/true)) {
                        if (q->distance < sharp_guard) {
                            continue;
                        }
                    }
                }
                const auto pr = geom::project_point_on_surface(*cad_ptr, s.pos);
                if (!pr || pr->distance > wall_band) {
                    continue;
                }
                Eigen::Vector3d n = pr->normal;
                const double nn = n.norm();
                if (!(nn > 1e-14)) {
                    continue;
                }
                n /= nn;
                s.pos = pr->point - inset * n;
            }
        }

        std::vector<Eigen::Vector3d> positions;
        positions.reserve(sites.size());
        for (const auto& s : sites) {
            positions.push_back(s.pos);
        }

        // RVD ∩ tet scaffold for all solids (holes need it; prismatic SE is
        // competitive with load_area trim). AABB-only reintroduces bad SE when
        // free-site counts stay modest.
        const std::string clip_mode = "rvd_tet";
        mesh::ClippedVoronoiExport exp;
        std::size_t n_domain_tets = 0;
        double scaffold_tet_volume = 0.0;
        try {
            const double h_tet = std::max(h, 1e-9);
            auto tet_fill =
                mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h_tet,
                                       /*snap_boundary=*/true);
            std::vector<mesh::DomainTet> dtets;
            dtets.reserve(tet_fill.tets.size());
            for (const auto& t : tet_fill.tets) {
                mesh::DomainTet d;
                d.v0 = tet_fill.nodes[t[0]];
                d.v1 = tet_fill.nodes[t[1]];
                d.v2 = tet_fill.nodes[t[2]];
                d.v3 = tet_fill.nodes[t[3]];
                d.centroid = 0.25 * (d.v0 + d.v1 + d.v2 + d.v3);
                const double tet_volume = mesh::tet_signed_volume(d.v0, d.v1, d.v2, d.v3);
                if (!(tet_volume > 0.0)) {
                    continue;
                }
                scaffold_tet_volume += tet_volume;
                dtets.push_back(d);
            }
            n_domain_tets = dtets.size();
            const double R = std::max(2.5 * h, 0.08 * diag);
            exp = mesh::export_rvd_tet_clipped(box, positions, dtets, R);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                std::format("cvt_poly RVD construction failed: {}", error.what()));
        }
        const double volume_scale =
            std::max({std::abs(scaffold_tet_volume), std::abs(exp.stats.sum_cell_volume),
                      std::numeric_limits<double>::min()});
        const double coverage_tol =
            (1e-9 + 128.0 * std::numeric_limits<double>::epsilon()) * volume_scale;
        if (!std::isfinite(scaffold_tet_volume) || !std::isfinite(exp.stats.sum_cell_volume) ||
            std::abs(exp.stats.sum_cell_volume - scaffold_tet_volume) > coverage_tol) {
            throw std::runtime_error(std::format(
                "cvt_poly RVD raw clip volume {:.17g} does not cover positive scaffold tetra "
                "volume {:.17g} within scale-aware tolerance {:.3e}",
                exp.stats.sum_cell_volume, scaffold_tet_volume, coverage_tol));
        }
        const std::size_t min_admitted_sites =
            std::min(positions.size(), std::max<std::size_t>(1, positions.size() / 4));
        const std::size_t n_admitted_sites = static_cast<std::size_t>(std::count_if(
            exp.site_to_cell.begin(), exp.site_to_cell.end(),
            [](std::size_t cell) { return cell != static_cast<std::size_t>(-1); }));
        if (n_admitted_sites < min_admitted_sites) {
            throw std::runtime_error(
                std::format("cvt_poly RVD admitted only {} sites; at least {} are required",
                            n_admitted_sites, min_admitted_sites));
        }

        if (exp.stats.n_invalid_face_claims != 0) {
            throw std::runtime_error(
                std::format("cvt_poly RVD is nonmanifold: {} exact face claims have invalid "
                            "multiplicity, site ownership, or winding",
                            exp.stats.n_invalid_face_claims));
        }
        if (exp.stats.n_unpaired_bisector_faces != 0) {
            throw std::runtime_error(
                std::format("cvt_poly RVD is nonconforming: {} Voronoi interface fragments "
                            "lack an opposite owner",
                            exp.stats.n_unpaired_bisector_faces));
        }
        if (exp.stats.n_unpaired_scaffold_faces != 0) {
            throw std::runtime_error(std::format(
                "cvt_poly RVD is incomplete: {} internal scaffold-cut faces lack an "
                "opposite owner",
                exp.stats.n_unpaired_scaffold_faces));
        }
        // Admit the exact exported polygons before triangulation.  Otherwise a
        // warped source n-gon could be replaced by valid triangles and escape
        // the original geometry contract.
        exp.mesh.check_validity();
        exp.mesh.check_geometry();
        // Any face incident to a projected exterior vertex is triangulated
        // first. Independent CAD projection can then preserve face planarity
        // while deep-interior coalesced Voronoi n-gons remain intact.
        exp.mesh.triangulate_boundary_incident_faces();
        exp.mesh.check_validity();
        exp.mesh.check_geometry();
        const std::vector<Eigen::Vector3d> pre_projection_vertices = exp.mesh.vertices;

        // Light boundary polish onto the surface (helps residual staircasing).
        std::size_t n_bnd_snapped = 0;
        if (!model.surface.triangles.empty() && !exp.mesh.faces.empty()) {
            std::vector<char> is_bnd(exp.mesh.vertices.size(), 0);
            for (const auto& f : exp.mesh.faces) {
                if (f.neighbour) {
                    continue;
                }
                for (auto v : f.vertices) {
                    if (v < is_bnd.size()) {
                        is_bnd[v] = 1;
                    }
                }
            }
            const double snap_budget = (clip_mode == "rvd_tet") ? 0.4 * h : 1.35 * h;
            for (std::size_t vi = 0; vi < exp.mesh.vertices.size(); ++vi) {
                if (!is_bnd[vi]) {
                    continue;
                }
                // Prefer OCC surface project when available (curved walls /
                // cylinder SE); fall back to STL closest.
                bool snapped = false;
                if (cad_ptr) {
                    if (const auto pr =
                            geom::project_point_on_surface(*cad_ptr, exp.mesh.vertices[vi])) {
                        if (pr->distance > 0.0 && pr->distance <= snap_budget) {
                            exp.mesh.vertices[vi] = pr->point;
                            ++n_bnd_snapped;
                            snapped = true;
                        }
                    }
                }
                if (!snapped) {
                    const auto cp =
                        mesh::closest_on_surface(model.surface, exp.mesh.vertices[vi]);
                    if (cp.distance > 0.0 && cp.distance <= snap_budget) {
                        exp.mesh.vertices[vi] = cp.point;
                        ++n_bnd_snapped;
                    }
                }
            }
        }

        // Projection is all-or-nothing. In addition to local shell validity,
        // preserve the authoritative positive scaffold volume. A 1e-4 relative
        // allowance is large compared with summation/welding roundoff but small
        // enough to reject a coherent CAD-projection shrink or expansion.
        constexpr double kProjectionVolumeRelTol = 1e-4;
        const auto convert_and_measure = [&]() {
            fea::NodalMesh converted = fea::poly_mesh_to_vem(exp.mesh);
            if (converted.elements.size() != exp.mesh.cells.size()) {
                throw std::runtime_error(std::format(
                    "cvt_poly VEM conversion changed admitted cell count from {} to {}",
                    exp.mesh.cells.size(), converted.elements.size()));
            }
            double volume = 0.0;
            for (const fea::NodalElement& element : converted.elements) {
                const Eigen::Vector3d origin = converted.nodes[element.nodes.front()];
                for (const auto& face : element.faces) {
                    if (face.size() < 3) {
                        continue;
                    }
                    const Eigen::Vector3d a = converted.nodes[element.nodes[face[0]]] - origin;
                    for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                        const Eigen::Vector3d b =
                            converted.nodes[element.nodes[face[i]]] - origin;
                        const Eigen::Vector3d c =
                            converted.nodes[element.nodes[face[i + 1]]] - origin;
                        volume += a.dot(b.cross(c)) / 6.0;
                    }
                }
            }
            return std::pair{std::move(converted), volume};
        };
        const auto admitted_volume_matches_scaffold = [&](double volume) {
            const double scale = std::max({std::abs(scaffold_tet_volume), std::abs(volume),
                                           std::numeric_limits<double>::min()});
            return std::isfinite(volume) &&
                   std::abs(volume - scaffold_tet_volume) <= kProjectionVolumeRelTol * scale;
        };

        double snap_scale = 0.0;
        double post_admission_vem_volume = 0.0;
        std::string projection_outcome = "not_applied";
        fea::NodalMesh admitted_mesh;
        if (n_bnd_snapped == 0) {
            exp.mesh.check_geometry();
            auto [converted, volume] = convert_and_measure();
            if (!admitted_volume_matches_scaffold(volume)) {
                throw std::runtime_error(std::format(
                    "cvt_poly admitted VEM volume {:.17g} differs from scaffold volume "
                    "{:.17g} beyond projection-relative tolerance {:.3e}",
                    volume, scaffold_tet_volume, kProjectionVolumeRelTol));
            }
            admitted_mesh = std::move(converted);
            post_admission_vem_volume = volume;
        } else {
            try {
                exp.mesh.check_geometry();
                auto [projected, projected_volume] = convert_and_measure();
                if (!admitted_volume_matches_scaffold(projected_volume)) {
                    throw mesh::ValidityError(std::format(
                        "CAD projection changed VEM volume from scaffold {:.17g} to {:.17g}",
                        scaffold_tet_volume, projected_volume));
                }
                admitted_mesh = std::move(projected);
                post_admission_vem_volume = projected_volume;
                snap_scale = 1.0;
                projection_outcome = "accepted";
            } catch (const mesh::ValidityError&) {
                exp.mesh.vertices = pre_projection_vertices;
                exp.mesh.check_geometry();
                auto [restored, restored_volume] = convert_and_measure();
                if (!admitted_volume_matches_scaffold(restored_volume)) {
                    throw std::runtime_error(std::format(
                        "cvt_poly restored VEM volume {:.17g} differs from scaffold volume "
                        "{:.17g} beyond projection-relative tolerance {:.3e}",
                        restored_volume, scaffold_tet_volume, kProjectionVolumeRelTol));
                }
                admitted_mesh = std::move(restored);
                post_admission_vem_volume = restored_volume;
                n_bnd_snapped = 0;
                projection_outcome = "rolled_back";
            }
        }

        out.mesh = std::move(admitted_mesh);
        const std::size_t post_admission_face_count = exp.mesh.faces.size();
        out.mesh.compact_unused_nodes();
        fill_h = h;
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        out.mesher_note = std::format(
            "cvt_poly RVD (interior sites + {} tets + clip={} + projection={} + "
            "bnd_snap={}@{:.4g} → kPolyVem): {} post_polys, {} post_nodes, post_faces={}, "
            "sites={}/fixed={}/interior={}/feat={}, lloyd_iters={}, "
            "raw_split_components={}, raw_unpaired_bisectors={}, "
            "raw_unpaired_scaffold={}, raw_invalid_face_claims={}, "
            "raw_coalesced_faces/fragments={}/{}, raw_clip_volume={:.17g}, "
            "scaffold_volume={:.17g}, post_vem_volume={:.17g}, raw_domain_clips={}, "
            "grid={}x{}x{}, h={:.4g} m{}",
            n_domain_tets, clip_mode, projection_outcome, n_bnd_snapped, snap_scale,
            out.mesh.elements.size(), out.mesh.nodes.size(), post_admission_face_count,
            sites.size(), seeded.n_sharp_fixed, n_interior_free - n_feature_free,
            n_feature_free, lr.stats.n_iters, exp.stats.n_split_site_components,
            exp.stats.n_unpaired_bisector_faces, exp.stats.n_unpaired_scaffold_faces,
            exp.stats.n_invalid_face_claims, exp.stats.n_coalesced_faces,
            exp.stats.n_coalesced_face_fragments, exp.stats.sum_cell_volume,
            scaffold_tet_volume, post_admission_vem_volume, exp.stats.n_domain_plane_clips,
            grid.nx, grid.ny, grid.nz, h,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=surface_class");
    } else {
        auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.tets.size());
        for (const auto& tet : fill.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        std::vector<std::array<std::uint32_t, 4>> tet_ids;
        tet_ids.reserve(out.mesh.elements.size());
        for (const auto& el : out.mesh.elements) {
            if (el.nodes.size() == 4) {
                tet_ids.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
            }
        }
        const auto q = mesh::summarize_tet4_quality(out.mesh.nodes, tet_ids);
        // Snap residual from boundary nodes (snap already ran inside tet_fill).
        std::vector<std::uint32_t> btmp;
        for (const auto& qf : out.boundary_quads) {
            btmp.insert(btmp.end(), qf.begin(), qf.end());
        }
        std::sort(btmp.begin(), btmp.end());
        btmp.erase(std::unique(btmp.begin(), btmp.end()), btmp.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, btmp);
        out.mesher_note = std::format(
            "tet grid fill v1 (Cartesian, not Delaunay): {} tet4, {} nodes, h={:.4g} m, "
            "minQ={:.3f}, meanQ={:.3f}, slivers={}, snap max|d|={:.3g} m mean|d|={:.3g} m",
            out.mesh.elements.size(), out.mesh.nodes.size(), fill_h, q.min_aspect,
            q.mean_aspect, q.n_sliver, conf.max_distance, conf.mean_distance);
    }

    // Prefer true element exterior faces for display/region skin so tet/prism
    // previews show element triangles (incl. Kuhn diagonals), not only lattice quads.
    {
        auto exterior = fea::extract_boundary_faces(out.mesh);
        if (!exterior.empty()) {
            out.boundary_quads = std::move(exterior);
        }
    }

    // CAD-face → boundary-node map. Fixtures / loads are picked on CAD faces,
    // so every mesh handed to the solver has to carry this map; rebuild it from
    // scratch whenever the node numbering changes below.
    const auto& surf = model.surface;
    auto map_boundary_regions = [&] {
        out.boundary_node_region.clear();
        std::size_t boundary_poll = 0;
        std::set<std::uint32_t> boundary_nodes;
        for (const auto& quad : out.boundary_quads) {
            boundary_nodes.insert(quad.begin(), quad.end());
        }
        for (const auto node : boundary_nodes) {
            if ((boundary_poll++ & 255U) == 0U) {
                poll_cancel();
            }
            const auto cp = mesh::closest_on_surface(surf, out.mesh.nodes[node]);
            if (cp.distance <= 1.5 * fill_h && cp.triangle < model.triangle_region.size()) {
                out.boundary_node_region[node] = model.triangle_region[cp.triangle];
            }
        }
    };
    map_boundary_regions();
    if (std::abs(tendency_plan.tendency) >= 1e-12 || tendency_plan.remapped) {
        out.mesher_note = std::format("{} | element_tendency={:.3g}→{}{}", out.mesher_note,
                                      tendency_plan.tendency, tendency_plan.label,
                                      tendency_plan.remapped ? " (remapped)" : "");
    }
    // Graded LEB / packing can leave orphan node slots (no element refs) that
    // inject zero-stiffness free DOFs and singular K — drop them always.
    const std::size_t n_orphans = out.mesh.compact_unused_nodes();
    if (n_orphans > 0) {
        out.mesher_note += std::format(" | compact_orphans={}", n_orphans);
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        // compact_unused_nodes() renumbered the nodes, so the map built above
        // keys on dead ids. Re-map onto the compacted mesh — dropping it here
        // silently stripped every fixture/load from the solve ("no fixtures:
        // fix at least one face before solving" with faces plainly assigned).
        map_boundary_regions();
    }
    out.fill_geometry_volume = measure_geometry_volume(model, out.mesh);
    out.solved_geometry_volume = out.fill_geometry_volume;
    if (out.fill_geometry_volume.available) {
        replace_geometry_volume_note(out.mesher_note, "fill", out.fill_geometry_volume);
        if (out.fill_geometry_volume.relative_error > kGeometryVolumeHardLimit) {
            // Name the remedy, like the two sibling refusals do
            // (`enforce_feature_resolution` and `refuse_unresolvable_h`). A bare
            // ratio is a dead end, and here it is worse than silent: the error
            // does not fall smoothly with h, so the obvious reading of "0.34
            // exceeds 0.1" walks the user down an asymptote. Measured on
            // channel_s0.step: 0.3351 at h=0.015 m, 0.3372 at h=0.012 m -- a 20%
            // reduction in h made it WORSE -- and 1.102e-14 at h=0.0075 m. The
            // recommended halving is that verified jump, not an extrapolation.
            //
            // RETRACTED (see ADR-0030): this guard used to split on the presence
            // of pyramid cells and blame the 2:1 conforming fan transition for
            // "dropping the volume" on mixed-level fills, recommending
            // --mesher graded_tet. That diagnosis was wrong. The fan was
            // conforming and exact; `fea::pyramid_rule` integrated it over the
            // wrong parametric domain and measured every pyramid at 0.6x its
            // true volume, so the guard was reporting a defect in its own
            // measuring stick. With the rule fixed, every mixed-level fill that
            // used to be refused here passes at ~1e-13, and no case is known
            // where the transition costs volume. Recommending graded_tet for a
            // cause that does not exist would send users away from the better
            // mesher on the strength of a retracted finding, so the branch is
            // gone rather than reworded.
            const std::size_t n_pyramid = static_cast<std::size_t>(std::count_if(
                out.mesh.elements.begin(), out.mesh.elements.end(),
                [](const fea::NodalElement& e) {
                    return e.type == fea::ElementType::kPyramid5;
                }));
            // Both h values, because they differ: the fill snaps the requested
            // size to a whole number of cells, and a user told only the snapped
            // one cannot match it to the -h they typed.
            throw GeometryVolumeLimitError(
                std::format("geometry fill-stage guard failed at h={:.6g} m (requested -h "
                            "{:.6g} m): mesh/BRep volume relative error {:.4g} exceeds hard "
                            "limit {:.4g}; the lattice does not resolve the solid, so parts "
                            "of it are missing from the fill. Reducing -h a little does NOT "
                            "fix this -- the error is set by which features the lattice "
                            "straddles, not by resolution in the small. Reduce -h to <= "
                            "{:.6g} m, or raise --max-elems/--max-dof to afford it. (Fill "
                            "census: {} cells, {} of them pyramid5 2:1 transition cells; the "
                            "transition is conforming and volume-exact and is not the cause.)"
                            " | {}",
                            fill_h, h, out.fill_geometry_volume.relative_error,
                            kGeometryVolumeHardLimit, 0.5 * h, out.mesh.elements.size(),
                            n_pyramid, out.mesher_note),
                out.fill_geometry_volume, false);
        }
    }
    if (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem ||
        mesher == VolumeMesher::kGradedTet) {
        enforce_feature_resolution(model, out, h, fill_h);
    }
    poll_cancel();
    const std::size_t actual_elems = out.mesh.elements.size();
    const std::size_t actual_dof =
        out.mesh.nodes.size() > std::numeric_limits<std::size_t>::max() / 3
            ? std::numeric_limits<std::size_t>::max()
            : 3 * out.mesh.nodes.size();
    const bool elem_over = max_elems > 0 && actual_elems > max_elems;
    const bool dof_over = max_dof > 0 && actual_dof > max_dof;
    if ((elem_over || dof_over) && auto_retry_budget > 0) {
        double scale = 1.0;
        if (elem_over) {
            scale = std::max(scale, std::cbrt(static_cast<double>(actual_elems) /
                                              static_cast<double>(max_elems)));
        }
        if (dof_over) {
            scale = std::max(scale, std::cbrt(static_cast<double>(actual_dof) /
                                              static_cast<double>(max_dof)));
        }
        const double retry_h =
            std::nextafter(h * scale * 1.05, std::numeric_limits<double>::infinity());
        auto retry = volume_mesh(model, retry_h, requested_mesher, requested_skin_layers,
                                 feature_refine, refine_seeds, seed_band, element_tendency,
                                 max_elems, max_dof, auto_retry_budget - 1, cancel_check,
                                 size_field);
        const std::string ceiling_note =
            elem_over ? std::format("element ceiling {}, actual {}", max_elems, actual_elems)
                      : std::format("DOF ceiling {}, actual {}", max_dof, actual_dof);
        retry.mesher_note = std::format("auto h clamped from {:.4g} to {:.4g} m ({}) | {}", h,
                                        retry_h, ceiling_note, retry.mesher_note);
        return retry;
    }
    if (elem_over) {
        throw std::runtime_error(
            std::format("mesh element ceiling {} exceeded after fill: actual {} elements "
                        "(predicted {:.0f}); increase -h or raise --max-elems",
                        max_elems, actual_elems, predicted_elems));
    }
    if (dof_over) {
        throw std::runtime_error(
            std::format("mesh DOF ceiling {} exceeded after fill: actual {} DOF / {} elements "
                        "(predicted {:.0f} DOF); increase -h or raise --max-dof",
                        max_dof, actual_dof, actual_elems, predicted_dof));
    }
    if (max_elems > 0 || max_dof > 0) {
        out.mesher_note +=
            std::format(" | budget predicted={:.0f} elems/{:.0f} DOF ceilings={}/{}",
                        predicted_elems, predicted_dof, max_elems, max_dof);
    }
    out.mesh.check_validity();
    return out;
}

/// A fill that yields zero interior cells means h could not represent the part at
/// all -- typically h exceeds a wall thickness -- which is a resolution refusal,
/// not a generic validity failure. Report it in the same actionable shape as
/// `enforce_feature_resolution`, and keep the three refusal causes textually
/// distinct: "feature unresolved at h=", "geometry fill-stage guard failed:",
/// and "resolution refused at h=".
[[noreturn]] static void refuse_unresolvable_h(const Model& model, double h,
                                               const mesh::ValidityError& cause) {
    // Only reached on a failure path, so an otherwise-expensive measurement is
    // free here -- and it is the one number that makes the refusal actionable.
    double thinnest = 0.0;
    try {
        const auto thickness = geom::estimate_local_thickness(model.surface);
        for (const double t : thickness.thickness) {
            if (geom::has_finite_thickness(t) && t > 0.0 && (thinnest == 0.0 || t < thinnest)) {
                thinnest = t;
            }
        }
    } catch (...) {
        thinnest = 0.0; // fall through to the honest "cannot derive" message
    }
    if (thinnest > 0.0) {
        // Two cells across the thinnest wall is the minimum that can represent it.
        throw GeometryVolumeLimitError(
            std::format("resolution refused at h={:.6g} m: the fill produced no interior "
                        "cells, so this h cannot represent the part at all; thinnest wall "
                        "is {:.6g} m and needs at least two cells across, so reduce -h to "
                        "<= {:.6g} m (or raise --max-elems/--max-dof to afford it) | {}",
                        h, thinnest, 0.5 * thinnest, cause.what()),
            GeometryVolumeAssessment{}, false);
    }
    throw GeometryVolumeLimitError(
        std::format("resolution refused at h={:.6g} m: the fill produced no interior cells, "
                    "so this h cannot represent the part at all. A recommended h could not "
                    "be derived here (no finite local thickness sample on this surface), so "
                    "reduce -h and retry rather than trusting a number this guard never "
                    "measured | {}",
                    h, cause.what()),
        GeometryVolumeAssessment{}, false);
}

VolumeMeshOutput volume_mesh(const Model& model, double h, VolumeMesher mesher,
                             int skin_layers, bool feature_refine,
                             std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                             double element_tendency, std::size_t max_elems,
                             std::size_t max_dof, int auto_retry_budget,
                             const std::function<void()>& cancel_check,
                             const mesh::SizeFieldFn& size_field) {
    try {
        return volume_mesh_impl(model, h, mesher, skin_layers, feature_refine, refine_seeds,
                                seed_band, element_tendency, max_elems, max_dof,
                                auto_retry_budget, cancel_check, size_field);
    } catch (const mesh::ValidityError& e) {
        // ONLY this cause is reclassified. Every other validity failure propagates
        // unchanged, so no existing campaign row status shifts.
        if (std::string(e.what()).find("no interior cells") == std::string::npos) {
            throw;
        }
        refuse_unresolvable_h(model, h, e);
    }
}

VolumeMeshOutput voxel_mesh(const Model& model, double h) {
    return volume_mesh(model, h, VolumeMesher::kTetFill, 2);
}

namespace {
struct JobCancelled : std::runtime_error {
    JobCancelled() : std::runtime_error("cancelled") {}
};
} // namespace

void SolveJob::set_status(const std::string& s) {
    const std::lock_guard lock(status_mutex_);
    status_ = s;
}

void SolveJob::set_progress(const std::string& phase, double phase_frac, int pass,
                            int pass_count) {
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
            .count();
    const std::lock_guard lock(status_mutex_);
    progress_.phase = phase;
    progress_.phase_frac = std::clamp(phase_frac, 0.0, 1.0);
    progress_.elapsed_ms = ms;
    progress_.pass = pass;
    progress_.pass_count = pass_count;
}

void SolveJob::report(const std::string& phase, double phase_frac,
                      const std::string& status_msg, int pass, int pass_count) {
    const auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
            .count();
    const std::lock_guard lock(status_mutex_);
    status_ = status_msg;
    progress_.phase = phase;
    progress_.phase_frac = std::clamp(phase_frac, 0.0, 1.0);
    progress_.elapsed_ms = ms;
    progress_.pass = pass;
    progress_.pass_count = pass_count;
}

void SolveJob::publish_live_mesh(const VolumeMeshOutput& vol) {
    // Boundary-focused copy for the viewport. Keep elements so face type colors
    // work; this runs only at mesh/adapt phase boundaries (not mid-fill).
    {
        const std::lock_guard lock(live_mesh_mutex_);
        live_mesh_ = vol;
    }
    live_mesh_gen_.fetch_add(1, std::memory_order_release);
    note_mesh_stats(vol);
}

void SolveJob::note_mesh_stats(const VolumeMeshOutput& vol) {
    const std::lock_guard lock(status_mutex_);
    progress_.n_elems = vol.mesh.elements.size();
    progress_.n_nodes = vol.mesh.nodes.size();
}

fea::SolveOptions SolveJob::solve_options_with_progress(int pass, int pass_count) {
    // Keep the shared default method/tolerance policy. The per-run memory cap
    // is enforced during solve preflight; four-iteration callbacks make the
    // same hook a low-latency cooperative pause/cancel point without restarting
    // the PCG recurrence.
    fea::SolveOptions opt;
    opt.max_mem_gb = active_max_mem_gb_;
    opt.on_note = [this](std::string_view note) { set_status(std::string(note)); };
    opt.on_progress = [this, pass, pass_count](int iter, int max_iters, double resid) {
        checkpoint();
        const double frac =
            max_iters > 0
                ? std::clamp(static_cast<double>(iter) / static_cast<double>(max_iters), 0.0,
                             1.0)
                : 0.0;
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
                .count();
        const std::lock_guard lock(status_mutex_);
        progress_.phase = "solve";
        progress_.phase_frac = frac;
        progress_.elapsed_ms = ms;
        progress_.pass = pass;
        progress_.pass_count = pass_count;
        progress_.cg_iter = iter;
        progress_.cg_resid = resid;
        status_ = std::format("solving… CG {}/{}  resid {:.3g}", iter, max_iters, resid);
    };
    return opt;
}

std::optional<VolumeMeshOutput> SolveJob::poll_live_mesh(std::uint64_t& seen_gen) const {
    const auto gen = live_mesh_gen_.load(std::memory_order_acquire);
    if (gen == 0 || gen == seen_gen) {
        return std::nullopt;
    }
    const std::lock_guard lock(live_mesh_mutex_);
    if (!live_mesh_) {
        return std::nullopt;
    }
    seen_gen = gen;
    return *live_mesh_;
}

void SolveJob::checkpoint() {
    bool announced = false;
    std::string resume_phase;
    while (pause_.load(std::memory_order_relaxed) &&
           !cancel_.load(std::memory_order_relaxed)) {
        {
            const std::lock_guard lock(status_mutex_);
            if (!announced) {
                resume_phase = progress_.phase.empty() ? "solve" : progress_.phase;
                if (status_.rfind("paused", 0) != 0) {
                    status_ = std::format("paused — {}", status_);
                }
                progress_.phase = "paused";
                announced = true;
            }
            progress_.elapsed_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0_)
                                       .count();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (cancel_.load(std::memory_order_relaxed)) {
        throw JobCancelled{};
    }
    if (announced) {
        const std::lock_guard lock(status_mutex_);
        // Restore phase token so the UI leaves the paused bar.
        if (progress_.phase == "paused") {
            progress_.phase = resume_phase.empty() ? "solve" : resume_phase;
        }
        if (status_.rfind("paused — ", 0) == 0) {
            status_ = status_.substr(std::string("paused — ").size());
        }
    }
}

void SolveJob::reset_control_flags() {
    cancel_.store(false, std::memory_order_relaxed);
    pause_.store(false, std::memory_order_relaxed);
    t0_ = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(status_mutex_);
        progress_ = JobProgress{};
    }
    {
        const std::lock_guard lock(live_mesh_mutex_);
        live_mesh_.reset();
    }
    live_mesh_gen_.store(0, std::memory_order_relaxed);
}

std::string SolveJob::status_text() const {
    const std::lock_guard lock(status_mutex_);
    return status_;
}

JobProgress SolveJob::progress() const {
    // Recompute wall-clock on every UI poll. `report()` only stamps phase
    // boundaries; mesh/assemble/solve can run for minutes without another
    // report, which made the progress panel look frozen while CPU was busy.
    JobProgress p;
    {
        const std::lock_guard lock(status_mutex_);
        p = progress_;
    }
    const auto st = state_.load(std::memory_order_relaxed);
    if (st == State::kMeshing || st == State::kSolving) {
        p.elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
                .count();
    }
    return p;
}

void SolveJob::request_cancel() {
    cancel_.store(true, std::memory_order_relaxed);
    // Wake a paused worker so it can observe cancel.
    pause_.store(false, std::memory_order_relaxed);
}

void SolveJob::request_pause() {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        pause_.store(true, std::memory_order_relaxed);
    }
}

void SolveJob::request_resume() { pause_.store(false, std::memory_order_relaxed); }

void SolveJob::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SolveJob::clear_failure() {
    if (state_ == State::kFailed || state_ == State::kCancelled) {
        join_worker();
        state_ = State::kIdle;
        reset_control_flags();
        set_status("idle");
    }
}

void SolveJob::start_mesh(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    reset_control_flags();
    state_ = State::kMeshing;
    report("mesh", 0.0, "meshing…");
    worker_ = std::thread([this, model, setup] {
        try {
            // Global scalar h from D5 only. Do NOT min-sample geometry sizing at
            // sharp corners for uniform meshers — that forced h→h_min on every
            // CAD box and ~8× DOF, which freezes interactive "mesh only".
            // Feature grading is applied as feature_refine (graded skin / bands).
            const auto resolved = resolve_mesh_size(model, setup.mesh_size, 30.0,
                                                    setup.max_elems, setup.max_dof);
            const double h = resolved.h;
            report("mesh", 0.15, std::format("meshing… ({}, h={:.4g} m)", resolved.note, h));
            checkpoint();
            const std::function<void()> mesh_cancel_check = [this] { checkpoint(); };
            const auto refinement =
                build_refinement_plan(model, h, {}, setup.use_feature_grading);
            mesh_only_ = volume_mesh(
                model, h, setup.mesher, setup.skin_layers, setup.use_feature_grading,
                refinement.refine_seeds, refinement.seed_band, setup.element_tendency,
                resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0,
                mesh_cancel_check, refinement.size_field);
            publish_live_mesh(mesh_only_);
            checkpoint();
            mesh_only_.mesher_note =
                std::format("{} | {}", resolved.note, mesh_only_.mesher_note);
            report("done", 1.0,
                   std::format("mesh ready — {} elems, {} nodes | {}",
                               mesh_only_.mesh.elements.size(), mesh_only_.mesh.nodes.size(),
                               mesh_only_.mesher_note));
            state_ = State::kMeshDone;
        } catch (const JobCancelled&) {
            report("cancelled", 0.0, "mesh cancelled");
            state_ = State::kCancelled;
        } catch (const std::exception& e) {
            report("done", 0.0, std::format("mesh failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

namespace {
void fill_result_fields(SolveResult& r, const fea::ZzRecovery& zz, const Eigen::VectorXd& u) {
    r.displacement = u;
    r.global_eta = zz.global_eta;
    r.element_eta = zz.element_eta;
    const auto n_nodes = r.volume_mesh.nodes.size();
    r.nodal_eta.assign(n_nodes, 0.0);
    std::vector<int> counts(n_nodes, 0);
    for (std::size_t e = 0; e < r.volume_mesh.elements.size() && e < zz.element_eta.size();
         ++e) {
        for (auto n : r.volume_mesh.elements[e].nodes) {
            r.nodal_eta[n] += zz.element_eta[e];
            ++counts[n];
        }
    }
    r.max_nodal_eta = 0.0;
    for (std::size_t i = 0; i < n_nodes; ++i) {
        if (counts[i] > 0) {
            r.nodal_eta[i] /= static_cast<double>(counts[i]);
        }
        r.max_nodal_eta = std::max(r.max_nodal_eta, r.nodal_eta[i]);
    }
    const auto& stress = zz.nodal_stress;
    r.von_mises.resize(stress.size());
    r.u_magnitude.resize(stress.size());
    r.max_von_mises = 0.0;
    r.max_displacement = 0.0;
    for (std::size_t i = 0; i < stress.size(); ++i) {
        r.von_mises[i] = fea::von_mises(stress[i]);
        r.u_magnitude[i] = u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
        r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
        r.max_displacement = std::max(r.max_displacement, r.u_magnitude[i]);
    }
}

PassTrace make_pass_trace(int pass, const fea::NodalMesh& mesh,
                          const fea::ZzRecovery& recovery,
                          const adapt::HpDriverPlan& plan, double mesh_ms,
                          double solve_ms) {
    PassTrace trace;
    trace.pass = pass;
    trace.n_elems = mesh.elements.size();
    trace.n_nodes = mesh.nodes.size();
    trace.n_dof = 3 * mesh.nodes.size();
    trace.global_eta = recovery.global_eta;
    std::vector<double> eta;
    eta.reserve(recovery.element_eta.size());
    for (const double value : recovery.element_eta) {
        if (std::isfinite(value)) {
            eta.push_back(value);
        }
    }
    if (!eta.empty()) {
        std::sort(eta.begin(), eta.end());
        const auto percentile = [&](double q) {
            const std::size_t index = static_cast<std::size_t>(
                q * static_cast<double>(eta.size() - 1));
            return eta[index];
        };
        trace.eta_p50 = percentile(0.5);
        trace.eta_p90 = percentile(0.9);
        trace.eta_max = eta.back();
    }
    trace.n_h_mark = plan.h_mark.size();
    trace.n_p_mark = plan.p_mark.size();
    trace.n_shape_mark = plan.shape_mark.size();
    trace.global_shape = static_cast<int>(plan.global_shape);
    trace.predicted_dof_factor = plan.predicted_dof_factor;
    trace.mesh_ms = mesh_ms;
    trace.solve_ms = solve_ms;
    return trace;
}
} // namespace

void SolveJob::start(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    reset_control_flags();
    active_max_mem_gb_ = setup.max_mem_gb;
    state_ = State::kMeshing;
    const int pass_count = std::max(0, setup.adapt_passes);
    report("mesh", 0.0, "meshing…", 0, pass_count);
    // Copy inputs by value into the worker.
    const auto pass_callback = on_pass;
    worker_ = std::thread([this, model, setup, pass_count, pass_callback] {
        try {
            // Global h from D5 only (same as start_mesh). Feature grading is
            // feature_refine on graded fills — not global h→h_min at corners.
            const auto resolved = resolve_mesh_size(model, setup.mesh_size, 30.0,
                                                    setup.max_elems, setup.max_dof);
            const double h = resolved.h;
            double h_use = h;
            report("mesh", 0.15, std::format("meshing… ({}, h={:.4g} m)", resolved.note, h), 0,
                   pass_count);
            checkpoint();
            const std::function<void()> mesh_cancel_check = [this] { checkpoint(); };
            std::vector<Eigen::Vector3d> adapt_seeds;
            double adapt_seed_band = 0.0;
            mesh::SizeFieldFn adapt_size_field;
            std::vector<adapt::SizeSource> src;
            // A-priori BC grading (ADR-0021): refine near loaded / fixed faces
            // before the first solve. Loads get the finest target (stress
            // concentrates under applied load); fixtures a moderate one.
            if (setup.bc_grading) {
                std::vector<Eigen::Vector3d> load_pts, fix_pts;
                const auto& surf = model.surface;
                for (std::size_t ti = 0; ti < surf.triangles.size(); ++ti) {
                    const int rg =
                        ti < model.triangle_region.size() ? model.triangle_region[ti] : -1;
                    const auto& t = surf.triangles[ti];
                    const Eigen::Vector3d c =
                        (surf.vertices[t[0]] + surf.vertices[t[1]] + surf.vertices[t[2]]) /
                        3.0;
                    if (setup.loads.count(rg)) {
                        load_pts.push_back(c);
                    } else if (setup.fixtures.count(rg)) {
                        fix_pts.push_back(c);
                    }
                }
                src = adapt::point_size_sources(load_pts, 0.25 * h);
                const auto fix_src = adapt::point_size_sources(fix_pts, 0.5 * h);
                src.insert(src.end(), fix_src.begin(), fix_src.end());
            }
            // Geometry and BC sources share one continuous min-plus field.
            if (setup.use_feature_grading) {
                auto geo = adapt::geometry_size_sources(model.surface, 0.15 * h, h);
                geo = decimate_sources(std::move(geo), 0.5 * h);
                src.insert(src.end(), geo.begin(), geo.end());
            }
            if (!src.empty()) {
                const auto plan = adapt::seed_plan(src, h, /*band_frac=*/1.5);
                adapt_size_field =
                    adapt::size_field_from_sources(src, plan.h_fine, h, /*beta=*/1.0);
                adapt_seeds = plan.refine_seeds;
                adapt_seed_band = plan.seed_band;
            }
            // D4: Dörfler element indices for optional local LEB before remesh.
            std::vector<std::size_t> adapt_marked;
            const auto initial_mesh_t0 = std::chrono::steady_clock::now();
            auto vol = volume_mesh(
                model, h_use, setup.mesher, setup.skin_layers, setup.use_feature_grading,
                adapt_seeds, adapt_seed_band, setup.element_tendency, resolved.element_ceiling,
                resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, mesh_cancel_check,
                adapt_size_field);
            double pass_mesh_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - initial_mesh_t0)
                                      .count();
            publish_live_mesh(vol);
            checkpoint();
            // Keep resolved-h note on mesher_note for solve mesh_note (GUI/CLI).
            vol.mesher_note = std::format("{} | {}", resolved.note, vol.mesher_note);
            report("assemble", 0.0,
                   std::format("assembling… ({} elements, {} nodes)", vol.mesh.elements.size(),
                               vol.mesh.nodes.size()),
                   0, pass_count);
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
            Eigen::VectorXd loads;
            bool bc_provenance_noted = false;
            const fea::Material material{.youngs_modulus = setup.youngs_modulus,
                                         .poissons_ratio = setup.poissons_ratio};

            std::vector<mesh::BoundarySupport> solve_boundary_provenance;
            mesh::BoundaryProjectionContext solve_projection_context;
            mesh::BoundaryProjectionContext* solve_projection = nullptr;
            if (model.cad &&
                make_boundary_projection(*model.cad, h, &solve_projection_context,
                                         &solve_boundary_provenance)) {
                solve_projection = &solve_projection_context;
            }
            // Legacy region ids are grown from the display tessellation. Keep
            // them as the primary path, but also remember the exact trimmed
            // BRep faces they represent so an empty legacy selection can be
            // recovered without guessing from a second nearest-triangle pass.
            std::map<int, std::set<std::uint32_t>> cad_faces_by_region;
            std::map<int, double> tess_area_by_region;
            std::map<int, double> cad_area_by_region;
            if (model.cad && solve_projection != nullptr) {
                const auto topology = geom::extract_topology(*model.cad, 4);
                for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
                    if (ti >= model.triangle_region.size() || model.triangle_region[ti] < 0) {
                        continue;
                    }
                    const auto& tri = model.surface.triangles[ti];
                    const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
                    const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
                    const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
                    const int region = model.triangle_region[ti];
                    tess_area_by_region[region] += 0.5 * (b - a).cross(c - a).norm();
                    const auto exact =
                        geom::project_point_on_surface(*model.cad, (a + b + c) / 3.0);
                    if (exact && exact->face_id != geom::kInvalidCadSupportId) {
                        cad_faces_by_region[region].insert(exact->face_id);
                    }
                }
                for (const auto& [region, face_ids] : cad_faces_by_region) {
                    for (const auto face_id : face_ids) {
                        const auto it = std::find_if(
                            topology.faces.begin(), topology.faces.end(),
                            [&](const geom::CadFace& face) { return face.id == face_id; });
                        if (it != topology.faces.end()) {
                            cad_area_by_region[region] += it->area;
                        }
                    }
                }
            }

            auto assign_boundary_regions = [&](double band) {
                vol.boundary_node_region.clear();
                const auto& surf = model.surface;
                for (std::uint32_t node = 0;
                     node < static_cast<std::uint32_t>(vol.mesh.nodes.size()); ++node) {
                    const auto cp = mesh::closest_on_surface(surf, vol.mesh.nodes[node]);
                    if (cp.distance <= band && cp.triangle < model.triangle_region.size()) {
                        vol.boundary_node_region[node] = model.triangle_region[cp.triangle];
                    }
                }
            };
            std::map<int, std::vector<fea::SurfaceFace>> exact_faces_by_region;
            auto recover_missing_regions_from_cad = [&]() {
                exact_faces_by_region.clear();
                if (!model.cad || solve_projection == nullptr || !solve_projection->target) {
                    return;
                }
                std::set<int> missing;
                for (const int region : setup.fixtures) {
                    if (!region_nodes.contains(region) || region_nodes[region].empty()) {
                        missing.insert(region);
                    }
                }
                for (const auto& [region, load] : setup.loads) {
                    (void)load;
                    if (!region_nodes.contains(region) || region_nodes[region].empty()) {
                        missing.insert(region);
                    }
                }
                if (missing.empty()) {
                    return;
                }

                const auto all_faces = fea::boundary_surface_faces(vol.mesh);
                solve_boundary_provenance.assign(vol.mesh.nodes.size(), {});
                std::set<std::uint32_t> boundary_nodes;
                for (const auto& face : all_faces) {
                    boundary_nodes.insert(face.nodes.begin(), face.nodes.end());
                }
                for (const auto node : boundary_nodes) {
                    mesh::BoundarySupport owner;
                    (void)solve_projection->target(vol.mesh.nodes[node], owner);
                    solve_boundary_provenance[node] = owner;
                }

                for (const int region : missing) {
                    const auto ids_it = cad_faces_by_region.find(region);
                    if (ids_it == cad_faces_by_region.end() || ids_it->second.empty()) {
                        continue;
                    }
                    std::set<std::uint32_t> nodes;
                    auto& recovered_faces = exact_faces_by_region[region];
                    for (const auto& face : all_faces) {
                        std::size_t selected_votes = 0;
                        std::size_t other_face_votes = 0;
                        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                        for (const auto node : face.nodes) {
                            centroid += vol.mesh.nodes[node];
                            const auto& owner = solve_boundary_provenance[node];
                            if (owner.kind != mesh::BoundarySupportKind::kCadFace) {
                                continue;
                            }
                            if (ids_it->second.contains(owner.id)) {
                                ++selected_votes;
                            } else {
                                ++other_face_votes;
                            }
                        }
                        centroid /= static_cast<double>(face.nodes.size());
                        bool keep =
                            selected_votes > 0 && selected_votes >= other_face_votes;
                        if (!keep) {
                            const auto exact =
                                geom::project_point_on_surface(*model.cad, centroid);
                            keep = exact &&
                                   exact->face_id != geom::kInvalidCadSupportId &&
                                   ids_it->second.contains(exact->face_id);
                        }
                        if (!keep) {
                            continue;
                        }
                        recovered_faces.push_back(face);
                        nodes.insert(face.nodes.begin(), face.nodes.end());
                    }
                    if (nodes.empty()) {
                        exact_faces_by_region.erase(region);
                    } else {
                        region_nodes[region] =
                            std::vector<std::uint32_t>(nodes.begin(), nodes.end());
                    }
                }
            };

            // Collect the CAD-face node sets and the Dirichlet set from whatever
            // face map the current mesh carries.
            auto collect_bcs = [&]() {
                bc = fea::Dirichlet{};
                region_nodes.clear();
                for (const auto& [node, region] : vol.boundary_node_region) {
                    region_nodes[region].push_back(node);
                }
                for (const int region : setup.fixtures) {
                    if (const auto it = region_nodes.find(region); it != region_nodes.end()) {
                        for (const auto node : it->second) {
                            bc.fix_node(node);
                        }
                    }
                }
            };

            auto apply_bcs = [&]() {
                if (setup.boundary_builder) {
                    // The caller owns selection outright: no region fallback,
                    // no CAD recovery, no resultant redistribution. Those exist
                    // to rescue stale region ids across a remesh; a builder
                    // re-selects on the mesh in hand and cannot go stale.
                    BoundaryConditions built = setup.boundary_builder(vol.mesh);
                    const Eigen::Index expected_dofs =
                        3 * static_cast<Eigen::Index>(vol.mesh.nodes.size());
                    if (built.dirichlet.dof_values.empty()) {
                        throw fea::FeaError(
                            "boundary_builder returned no Dirichlet DOFs; refusing to solve "
                            "an unconstrained system");
                    }
                    if (built.loads.size() != expected_dofs) {
                        throw fea::FeaError(std::format(
                            "boundary_builder returned a {}-entry load vector for a mesh of "
                            "{} nodes (expected {})",
                            built.loads.size(), vol.mesh.nodes.size(), expected_dofs));
                    }
                    if (!built.loads.allFinite()) {
                        throw fea::FeaError(
                            "boundary_builder returned a non-finite load vector");
                    }
                    if (!(built.loads.norm() > 0.0)) {
                        throw fea::FeaError(
                            "boundary_builder returned a zero load vector; refusing to solve "
                            "an unloaded system");
                    }
                    bc = std::move(built.dirichlet);
                    loads = std::move(built.loads);
                    region_nodes.clear(); // region bookkeeping is unused on this path
                    if (!bc_provenance_noted) {
                        vol.mesher_note +=
                            std::format(" | BCs: caller boundary_builder ({} fixed DOFs, "
                                        "|f|={:.6g} N)",
                                        bc.dof_values.size(), loads.norm());
                        bc_provenance_noted = true;
                    }
                    return;
                }
                collect_bcs();
                // Fixtures and loads live on CAD faces and outlive every mesh:
                // when the mesh in hand cannot show them (a remesh renumbered
                // nodes, a mesher dropped its face map), re-snap the faces onto
                // the current mesh instead of reporting an empty setup.
                const bool fixtures_lost = !setup.fixtures.empty() && bc.dof_values.empty();
                const bool loads_lost =
                    std::any_of(setup.loads.begin(), setup.loads.end(), [&](const auto& kv) {
                        return !region_nodes.contains(kv.first);
                    });
                if (fixtures_lost || loads_lost) {
                    assign_boundary_regions(1.5 * h_use);
                    collect_bcs();
                }
                recover_missing_regions_from_cad();
                for (const int region : setup.fixtures) {
                    if (const auto it = region_nodes.find(region); it != region_nodes.end()) {
                        for (const auto node : it->second) {
                            bc.fix_node(node);
                        }
                    }
                }
                if (bc.dof_values.empty()) {
                    throw fea::FeaError("no fixtures: fix at least one face before solving");
                }
                loads = Eigen::VectorXd::Zero(
                    3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
                // Consistent (energy-conjugate) load application: the region's
                // boundary faces carry a uniform traction whose resultant is the
                // requested force. Even splitting over region nodes made the
                // distribution mesh-density dependent on graded faces.
                const auto all_faces = fea::boundary_surface_faces(vol.mesh);
                for (const auto& [region, load] : setup.loads) {
                    const auto it = region_nodes.find(region);
                    if (it == region_nodes.end() || it->second.empty()) {
                        throw fea::FeaError(
                            std::format("load on region {} has no boundary nodes", region));
                    }
                    auto exact_faces_it = exact_faces_by_region.find(region);
                    auto faces = exact_faces_it != exact_faces_by_region.end()
                                     ? exact_faces_it->second
                                     : fea::faces_within(all_faces, it->second);
                    if (faces.empty()) {
                        // CAD region boundaries rarely coincide with a coarse
                        // volume-mesh face. Prefer a complete face when a
                        // majority of its nodes maps to the region; if even that
                        // does not exist, the resultant-preserving node fallback
                        // below handles the legitimate sub-face load region.
                        for (const auto& face : all_faces) {
                            const std::size_t in_region = static_cast<std::size_t>(
                                std::count_if(face.nodes.begin(), face.nodes.end(),
                                              [&](std::uint32_t node) {
                                                  return std::find(it->second.begin(),
                                                                   it->second.end(),
                                                                   node) != it->second.end();
                                              }));
                            if (2 * in_region >= face.nodes.size() && in_region > 0) {
                                faces.push_back(face);
                            }
                        }
                    }
                    Eigen::Vector3d requested_force = load.force;
                    if (exact_faces_it != exact_faces_by_region.end()) {
                        const double tess_area = tess_area_by_region[region];
                        const double cad_area = cad_area_by_region[region];
                        if (tess_area > 0.0 && cad_area > 0.0) {
                            requested_force *= cad_area / tess_area;
                        }
                    }
                    if (exact_faces_it != exact_faces_by_region.end()) {
                        vol.mesher_note += std::format(
                            " | exact CAD region {} fallback={} faces", region, faces.size());
                    }
                    const auto applied =
                        fea::consistent_face_load(vol.mesh, faces, requested_force);
                    if (applied.area > 0.0) {
                        if (applied.conservation_error > 1e-9) {
                            throw fea::FeaError(std::format(
                                "load on region {}: traction assembly lost {:.3g} N of the "
                                "requested {:.6g} N resultant",
                                region, applied.conservation_error, requested_force.norm()));
                        }
                        loads += applied.loads;
                        continue;
                    }
                    const Eigen::Vector3d per_node =
                        requested_force / static_cast<double>(it->second.size());
                    Eigen::Vector3d fallback_sum = Eigen::Vector3d::Zero();
                    for (const auto node : it->second) {
                        loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
                        fallback_sum += per_node;
                    }
                    const double conservation_error = (fallback_sum - requested_force).norm();
                    if (conservation_error > 1e-9) {
                        throw fea::FeaError(std::format(
                            "load on region {}: nodal fallback lost {:.3g} N of the "
                            "requested {:.6g} N resultant",
                            region, conservation_error, requested_force.norm()));
                    }
                    vol.mesher_note += std::format(
                        " | load region {} node fallback={} Σf=({:.6g},{:.6g},{:.6g}) N",
                        region, it->second.size(), fallback_sum.x(), fallback_sum.y(),
                        fallback_sum.z());
                }
                if (!bc_provenance_noted) {
                    vol.mesher_note += std::format(
                        " | BCs: region selection ({} fixed DOFs, {} load regions, "
                        "|f|={:.6g} N)",
                        bc.dof_values.size(), setup.loads.size(), loads.norm());
                    bc_provenance_noted = true;
                }
            };
            apply_bcs();

            auto element_centroids = [&](const fea::NodalMesh& m) {
                std::vector<Eigen::Vector3d> cents;
                cents.reserve(m.elements.size());
                for (const auto& el : m.elements) {
                    Eigen::Vector3d c = Eigen::Vector3d::Zero();
                    for (auto n : el.nodes) {
                        c += m.nodes[n];
                    }
                    cents.push_back(c / static_cast<double>(el.nodes.size()));
                }
                return cents;
            };

            // D3: p-elevate smooth linear elems after last h-pass (or single solve).
            // Explicit flag or auto when adapt_passes > 0 — but never on huge meshes
            // (tet10 ~3–4× DOF; was a common OOM path with graded+feature floods).
            constexpr std::size_t kPElevateMaxNodes = 40000;
            const bool want_p_elevate = setup.p_elevate || setup.adapt_passes > 0;
            auto p_elevate_allowed = [&]() {
                return want_p_elevate && vol.mesh.nodes.size() <= kPElevateMaxNodes;
            };
            const bool do_p_elevate = want_p_elevate; // gate per-call via p_elevate_allowed
            fea::LinearConstraints p_constraints;
            const auto active_p_constraints = [&]() -> const fea::LinearConstraints* {
                return p_constraints.empty() ? nullptr : &p_constraints;
            };
            const auto remove_slave_dirichlet = [&]() {
                for (const auto& constraint : p_constraints.entries()) {
                    bc.dof_values.erase(
                        static_cast<Eigen::Index>(constraint.slave_dof));
                }
            };

            // After mid-edge insertion, assign boundary regions to new nodes that
            // sit between two existing boundary nodes of the same region so
            // fixtures/loads still cover face mid-edge DOFs.
            auto extend_boundary_regions = [&]() {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edge_mid;
                for (const auto& el : vol.mesh.elements) {
                    if (el.type != fea::ElementType::kTet10 &&
                        el.type != fea::ElementType::kHex20) {
                        continue;
                    }
                    const int n_corner = (el.type == fea::ElementType::kTet10) ? 4 : 8;
                    const int n_mid = (el.type == fea::ElementType::kTet10) ? 6 : 12;
                    // Canonical edge order matches p_elevate mid-edge append order.
                    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
                        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
                    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
                                                                                   {1, 2},
                                                                                   {2, 3},
                                                                                   {3, 0},
                                                                                   {4, 5},
                                                                                   {5, 6},
                                                                                   {6, 7},
                                                                                   {7, 4},
                                                                                   {0, 4},
                                                                                   {1, 5},
                                                                                   {2, 6},
                                                                                   {3, 7}}};
                    for (int m = 0; m < n_mid; ++m) {
                        const auto& epair = (el.type == fea::ElementType::kTet10)
                                                ? kTetEdges[static_cast<std::size_t>(m)]
                                                : kHexEdges[static_cast<std::size_t>(m)];
                        const auto a = el.nodes[static_cast<std::size_t>(epair[0])];
                        const auto b = el.nodes[static_cast<std::size_t>(epair[1])];
                        const auto mid = el.nodes[static_cast<std::size_t>(n_corner + m)];
                        edge_mid[std::minmax(a, b)] = mid;
                    }
                }
                for (const auto& [ab, mid] : edge_mid) {
                    if (vol.boundary_node_region.contains(mid)) {
                        continue;
                    }
                    const auto ita = vol.boundary_node_region.find(ab.first);
                    const auto itb = vol.boundary_node_region.find(ab.second);
                    if (ita != vol.boundary_node_region.end() &&
                        itb != vol.boundary_node_region.end() && ita->second == itb->second) {
                        vol.boundary_node_region[mid] = ita->second;
                    }
                }
            };

            // Joint (h,p,shape) driver policy (ADR-0019 §4). Seed fixed for
            // deterministic product runs; campaign-fitted costs land later.
            // h_min is set once h_adapt_floor is known (below).
            adapt::HpDriverPolicy hp_policy;
            hp_policy.seed = 0x48504452ull; // 'HPDR'
            hp_policy.h_refine_factor = 0.75;

            // Surface geometry attributes for a-priori h gate (once per solve).
            std::vector<double> surf_kappa;
            std::vector<double> surf_thickness;
            if (setup.use_feature_grading && !model.surface.vertices.empty()) {
                try {
                    surf_kappa = geom::estimate_vertex_curvature(model.surface).kappa;
                    surf_thickness = geom::estimate_local_thickness(model.surface).thickness;
                } catch (...) {
                    surf_kappa.clear();
                    surf_thickness.clear();
                }
            }

            auto build_hp_signals = [&](const std::vector<Eigen::Vector3d>& cents,
                                        const std::vector<double>& element_eta) {
                const auto n = element_eta.size();
                std::vector<double> h_loc(n, h_use);
                std::vector<double> kappa(n, 0.0);
                std::vector<double> thick(n, 0.0);
                std::vector<int> p_ord(n, 1);
                for (std::size_t e = 0; e < n && e < vol.mesh.elements.size(); ++e) {
                    const auto& el = vol.mesh.elements[e];
                    if (el.type == fea::ElementType::kTet10 ||
                        el.type == fea::ElementType::kHex20) {
                        p_ord[e] = 2;
                    }
                    if (e < cents.size() && !surf_kappa.empty()) {
                        const auto vi = geom::nearest_vertex_index(model.surface, cents[e]);
                        if (vi < surf_kappa.size()) {
                            kappa[e] = surf_kappa[vi];
                        }
                        if (vi < surf_thickness.size() &&
                            geom::has_finite_thickness(surf_thickness[vi])) {
                            thick[e] = surf_thickness[vi];
                        }
                    }
                }
                // Empty surplus → driver estimates from ZZ ranking.
                return adapt::make_hp_signals(h_loc, kappa, thick, element_eta, {}, p_ord, {},
                                              {}, {}, hp_policy);
            };

            auto maybe_p_elevate = [&](const std::vector<std::size_t>& elevate_idx,
                                       std::string& note_suffix) {
                if (!do_p_elevate || elevate_idx.empty()) {
                    if (do_p_elevate && elevate_idx.empty()) {
                        note_suffix = " p-elev=0";
                    }
                    return false;
                }
                if (!p_elevate_allowed()) {
                    note_suffix = std::format(" p-elev skipped (nodes {}>{} budget)",
                                              vol.mesh.nodes.size(), kPElevateMaxNodes);
                    return false;
                }
                // Only elevate still-linear zoo elements (driver may re-list p=2).
                std::vector<std::size_t> linear;
                linear.reserve(elevate_idx.size());
                for (auto e : elevate_idx) {
                    if (e >= vol.mesh.elements.size()) {
                        continue;
                    }
                    const auto t = vol.mesh.elements[e].type;
                    if (t == fea::ElementType::kTet4 || t == fea::ElementType::kHex8) {
                        linear.push_back(e);
                    }
                }
                if (linear.empty()) {
                    note_suffix = " p-elev=0";
                    return false;
                }
                const auto before = vol.mesh.nodes.size();
                auto elevated = fea::p_elevate_with_constraints(vol.mesh, linear);
                vol.mesh = std::move(elevated.mesh);
                p_constraints = std::move(elevated.constraints);
                extend_boundary_regions();
                apply_bcs();
                if (solve_projection != nullptr && model.cad) {
                    std::vector<std::uint32_t> reverted;
                    std::vector<std::uint32_t> partial;
                    const std::size_t projected = project_quadratic_boundary_mids(
                        vol.mesh, *model.cad, solve_projection, h, &reverted, &partial);
                    vol.mesher_note += std::format(
                        " | mids projected={} partial={} reverted={}", projected,
                        partial.size(), reverted.size());
                }
                remove_slave_dirichlet();
                const auto counts = fea::count_element_types(vol.mesh);
                note_suffix = std::format(
                    " p-elev={} rejected={} n+{} constrained-mid={} (tet10={} hex20={})",
                    elevated.n_promoted, elevated.n_rejected,
                    vol.mesh.nodes.size() - before, elevated.n_constrained_midside,
                    counts.tet10, counts.hex20);
                report("recover", 0.5,
                       std::format("p-elevate… ({} accepted, {} rejected)",
                                   elevated.n_promoted, elevated.n_rejected),
                       /*pass=*/0, pass_count);
                return true;
            };

            // Geometry-driven hp hybrid (pre-solve): elevate bulk elements whose
            // centroids sit deep inside, away from the free surface / curved skin.
            // Leaves linear tets near features (high κ / boundary) for cheaper
            // resolution of gradients while bulk gets p=2 economy.
            auto maybe_geo_p_elevate = [&]() {
                if (!do_p_elevate || !setup.use_feature_grading) {
                    return;
                }
                if (!p_elevate_allowed()) {
                    vol.mesher_note =
                        std::format("{} | geo-hp skipped (nodes {}>{} budget)",
                                    vol.mesher_note, vol.mesh.nodes.size(), kPElevateMaxNodes);
                    return;
                }
                if (setup.mesher != VolumeMesher::kGradedTet &&
                    setup.mesher != VolumeMesher::kVaryhedron &&
                    setup.mesher != VolumeMesher::kTetFill &&
                    setup.mesher != VolumeMesher::kHybrid &&
                    setup.mesher != VolumeMesher::kHybridVem) {
                    return;
                }
                const double bulk_band = 1.75 * h_use;
                const auto cents = element_centroids(vol.mesh);
                std::vector<std::size_t> bulk;
                bulk.reserve(cents.size() / 2);
                for (std::size_t e = 0; e < cents.size(); ++e) {
                    const auto& el = vol.mesh.elements[e];
                    if (el.type != fea::ElementType::kTet4 &&
                        el.type != fea::ElementType::kHex8) {
                        continue;
                    }
                    const double d =
                        geom::distance_to_surface_vertices(model.surface, cents[e]);
                    if (d >= bulk_band) {
                        bulk.push_back(e);
                    }
                }
                if (bulk.empty()) {
                    return;
                }
                // Cap: never elevate more than 70% of linear elems in one shot.
                const std::size_t cap = std::max<std::size_t>(1, (cents.size() * 7) / 10);
                if (bulk.size() > cap) {
                    bulk.resize(cap);
                }
                const auto before = vol.mesh.nodes.size();
                auto elevated = fea::p_elevate_with_constraints(vol.mesh, bulk);
                vol.mesh = std::move(elevated.mesh);
                p_constraints = std::move(elevated.constraints);
                extend_boundary_regions();
                apply_bcs();
                if (solve_projection != nullptr && model.cad) {
                    std::vector<std::uint32_t> reverted;
                    std::vector<std::uint32_t> partial;
                    const std::size_t projected = project_quadratic_boundary_mids(
                        vol.mesh, *model.cad, solve_projection, h, &reverted, &partial);
                    vol.mesher_note += std::format(
                        " | mids projected={} partial={} reverted={}", projected,
                        partial.size(), reverted.size());
                }
                remove_slave_dirichlet();
                const auto counts = fea::count_element_types(vol.mesh);
                vol.mesher_note = std::format(
                    "{} | geo-hp: p-elev bulk {} rejected={} (tet10={} n+{})",
                    vol.mesher_note, elevated.n_promoted, elevated.n_rejected,
                    counts.tet10, vol.mesh.nodes.size() - before);
                report("recover", 0.3,
                       std::format("geo hp-elevate… ({} accepted, {} rejected)",
                                   elevated.n_promoted, elevated.n_rejected),
                       0, pass_count);
            };
            maybe_geo_p_elevate();
            checkpoint();

            auto mesh_is_all_tet4 = [](const fea::NodalMesh& m) {
                if (m.elements.empty()) {
                    return false;
                }
                for (const auto& el : m.elements) {
                    if (el.type != fea::ElementType::kTet4 || el.nodes.size() != 4) {
                        return false;
                    }
                }
                return true;
            };

            /// ADR-0016: one Rivara LEB wave on marked tets; returns true if mesh grew.
            auto try_local_leb_once = [&](std::span<const std::size_t> marks) -> bool {
                if (marks.empty() || !mesh_is_all_tet4(vol.mesh)) {
                    return false;
                }
                std::vector<std::array<std::uint32_t, 4>> tets;
                tets.reserve(vol.mesh.elements.size());
                for (const auto& el : vol.mesh.elements) {
                    tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
                }
                const std::size_t n0 = tets.size();
                try {
                    mesh::LocalRefineStats st;
                    auto refined =
                        mesh::local_refine_tets(vol.mesh.nodes, std::move(tets), marks, &st);
                    if (refined.tets.size() <= n0) {
                        return false;
                    }
                    vol.mesh.nodes = std::move(refined.nodes);
                    vol.mesh.elements.clear();
                    vol.mesh.elements.reserve(refined.tets.size());
                    for (const auto& tet : refined.tets) {
                        vol.mesh.elements.push_back(fea::NodalElement{
                            fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
                    }
                    vol.mesh.check_validity();
                    // Lattice quads invalid after midpoints — rebuild true exterior faces.
                    vol.boundary_quads = fea::extract_boundary_faces(vol.mesh);
                    assign_boundary_regions(1.5 * h_use);
                    vol.mesher_note = std::format(
                        "{} | local LEB (ADR-0016): +{} tets, +{} nodes, {} bisections",
                        vol.mesher_note, refined.tets.size() - n0, st.n_new_nodes,
                        st.n_bisections);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            };

            /// Multi-wave LEB: re-mark by proximity to adapt seeds between waves
            /// so high-error regions deepen without a full remesh each time.
            auto try_local_leb = [&](std::span<const std::size_t> marks) -> bool {
                const int waves = std::clamp(setup.adapt_leb_waves, 1, 4);
                std::vector<std::size_t> current(marks.begin(), marks.end());
                bool any = false;
                int waves_done = 0;
                for (int w = 0; w < waves; ++w) {
                    if (current.empty()) {
                        break;
                    }
                    if (!try_local_leb_once(current)) {
                        break;
                    }
                    any = true;
                    ++waves_done;
                    if (w + 1 >= waves || adapt_seeds.empty()) {
                        break;
                    }
                    // Next wave: tets whose centroid falls in a seed ball.
                    const auto cents = element_centroids(vol.mesh);
                    const double band =
                        adapt_seed_band > 0.0 ? adapt_seed_band : (1.5 * h_use);
                    const double r2 = band * band;
                    current.clear();
                    current.reserve(cents.size() / 4 + 8);
                    for (std::size_t e = 0; e < cents.size(); ++e) {
                        for (const auto& s : adapt_seeds) {
                            if ((cents[e] - s).squaredNorm() <= r2) {
                                current.push_back(e);
                                break;
                            }
                        }
                    }
                    // Cap so a single pass cannot explode DOF (≤ 30% of mesh).
                    const std::size_t cap =
                        std::max<std::size_t>(8, (cents.size() * 3) / 10); // ≤ 30% of mesh
                    if (current.size() > cap) {
                        current.resize(cap);
                    }
                }
                if (any && waves_done > 1) {
                    vol.mesher_note =
                        std::format("{} | LEB waves={}", vol.mesher_note, waves_done);
                }
                return any;
            };

            // Grid budget floor: graded is always 2:1 (fine lattice ≈ h/2).
            const int grid_sub = (setup.mesher == VolumeMesher::kGradedTet ||
                                  setup.mesher == VolumeMesher::kVaryhedron)
                                     ? 2
                                     : 1;
            const double h_grid_floor = mesh::min_h_for_cell_budget(
                model.bbox_min, model.bbox_max, mesh::kDefaultMaxGridCells, grid_sub);
            const double h_adapt_floor = std::max(h * 0.35, h_grid_floor);
            hp_policy.h_min = h_adapt_floor;

            // Prefer mesher matching the last shape vote (mesher-tendency will own the
            // continuous dial; here we only flip discrete product meshers when the
            // driver majority-votes).
            auto mesher_for_tendency = [&](VolumeMesher base,
                                           adapt::ShapeTendency t) -> VolumeMesher {
                switch (t) {
                case adapt::ShapeTendency::kPreferTet:
                    if (base == VolumeMesher::kHybrid || base == VolumeMesher::kHybridVem ||
                        base == VolumeMesher::kHexFill || base == VolumeMesher::kHexVem) {
                        return VolumeMesher::kGradedTet;
                    }
                    break;
                case adapt::ShapeTendency::kPreferPoly:
                    if (base == VolumeMesher::kHybrid) {
                        return VolumeMesher::kHybridVem;
                    }
                    break;
                case adapt::ShapeTendency::kPreferHex:
                    if (base == VolumeMesher::kTetFill || base == VolumeMesher::kGradedTet) {
                        return VolumeMesher::kHybrid;
                    }
                    break;
                case adapt::ShapeTendency::kKeep:
                default:
                    break;
                }
                return base;
            };
            adapt::ShapeTendency last_shape_vote = adapt::ShapeTendency::kKeep;

            for (int pass = 0; pass <= setup.adapt_passes; ++pass) {
                checkpoint();
                // Overall progress across adapt passes (pass 0..N).
                const double pass_base =
                    static_cast<double>(pass) / static_cast<double>(pass_count + 1);
                const double pass_span = 1.0 / static_cast<double>(pass_count + 1);
                if (pass > 0) {
                    const auto adapt_mesh_t0 = std::chrono::steady_clock::now();
                    // D4: prefer true local LEB on tet meshes when marks exist.
                    const bool tet_path = setup.mesher == VolumeMesher::kTetFill ||
                                          setup.mesher == VolumeMesher::kGradedTet;
                    bool did_local = false;
                    if (tet_path && !adapt_marked.empty()) {
                        did_local = try_local_leb(adapt_marked);
                    }
                    if (!did_local) {
                        // Prefer graded mesher when local seeds are available so
                        // a posteriori balls can refine without global h→0.
                        auto mesher_adapt = (!adapt_seeds.empty() &&
                                             (setup.mesher == VolumeMesher::kTetFill ||
                                              setup.mesher == VolumeMesher::kGradedTet))
                                                ? VolumeMesher::kGradedTet
                                                : setup.mesher;
                        mesher_adapt = mesher_for_tendency(mesher_adapt, last_shape_vote);
                        // Remesh: graded always uses ÷2 lattice budget (fine ≈ h/2).
                        const int remesh_sub =
                            (!adapt_seeds.empty() || mesher_adapt == VolumeMesher::kGradedTet)
                                ? 2
                                : 1;
                        const double h_remesh =
                            std::max(h_use, mesh::min_h_for_cell_budget(
                                                model.bbox_min, model.bbox_max,
                                                mesh::kDefaultMaxGridCells, remesh_sub));
                        report("mesh", pass_base,
                               std::format("adapt remesh {}… ({} seeds)", pass,
                                           adapt_seeds.size()),
                               pass, pass_count);
                        checkpoint();
                        vol = volume_mesh(model, h_remesh, mesher_adapt, setup.skin_layers,
                                          setup.use_feature_grading, adapt_seeds,
                                          adapt_seed_band, setup.element_tendency,
                                          resolved.element_ceiling, resolved.dof_ceiling,
                                          resolved.auto_chosen ? 3 : 0, mesh_cancel_check,
                                          adapt_size_field);
                        p_constraints = {};
                        publish_live_mesh(vol);
                        h_use = std::max(h_use, h_remesh);
                    } else {
                        publish_live_mesh(vol); // local LEB also changes connectivity
                    }
                    apply_bcs();
                    // Re-apply geometry hp after remesh so bulk stays quadratic.
                    if (!did_local) {
                        maybe_geo_p_elevate();
                    }
                    pass_mesh_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - adapt_mesh_t0)
                                       .count();
                    report("solve", pass_base + 0.15 * pass_span,
                           std::format("adapt pass {}… ({} elems, {} seeds{})", pass,
                                       vol.mesh.elements.size(), adapt_seeds.size(),
                                       did_local ? ", local LEB" : ""),
                           pass, pass_count);
                } else {
                    report("solve", pass_base + 0.2 * pass_span,
                           std::format("solving… ({} elements, {} nodes)",
                                       vol.mesh.elements.size(), vol.mesh.nodes.size()),
                           pass, pass_count);
                }
                checkpoint();
                const auto solve_opt = solve_options_with_progress(pass, pass_count);
                const auto solve_t0 = std::chrono::steady_clock::now();
                update_solved_geometry_volume(model, vol);

                auto u_try = fea::solve_elastostatics(
                    vol.mesh, material, bc, loads, solve_opt, active_p_constraints());
                const double pass_solve_ms = std::chrono::duration<double, std::milli>(
                                                 std::chrono::steady_clock::now() - solve_t0)
                                                 .count();
                report("recover", pass_base + 0.7 * pass_span,
                       std::format("recovering stress… (pass {}/{})", pass, pass_count), pass,
                       pass_count);
                checkpoint();
                auto zz_try = fea::recover_zz(vol.mesh, material, u_try);
                const auto cents = element_centroids(vol.mesh);
                const auto signals = build_hp_signals(cents, zz_try.element_eta);
                const auto hp_plan = adapt::drive_hp(signals, hp_policy, cents, h_use);
                last_shape_vote = hp_plan.global_shape;
                const std::string hp_note = adapt::summarize_hp_plan(hp_plan);
                if (setup.adapt_passes > 0 && pass_callback) {
                    pass_callback(make_pass_trace(pass, vol.mesh, zz_try, hp_plan,
                                                  pass_mesh_ms, pass_solve_ms));
                }

                // D2: global η target — stop when η is small enough (0 = disabled).
                if (setup.eta_target > 0.0 && zz_try.global_eta <= setup.eta_target) {
                    std::string pnote;
                    if (maybe_p_elevate(hp_plan.p_mark, pnote)) {
                        publish_live_mesh(vol);
                        update_solved_geometry_volume(model, vol);

                        u_try = fea::solve_elastostatics(
                            vol.mesh, material, bc, loads,
                            solve_options_with_progress(pass, pass_count),
                            active_p_constraints());
                        zz_try = fea::recover_zz(vol.mesh, material, u_try);
                    }
                    SolveResult r;
                    r.mesh_note = std::format(
                        "{} | {} | eta-target stop η={:.4g}≤{:.4g} pass={}/{} h={:.4g}{}",
                        vol.mesher_note, hp_note, zz_try.global_eta, setup.eta_target, pass,
                        setup.adapt_passes, h_use, pnote);
                    r.volume_mesh = std::move(vol.mesh);
                    r.boundary_quads = std::move(vol.boundary_quads);
                    fill_result_fields(r, zz_try, u_try);
                    r.fill_geometry_volume = vol.fill_geometry_volume;
                    r.solved_geometry_volume = vol.solved_geometry_volume;

                    result_ = std::move(r);
                    break;
                }
                if (pass < setup.adapt_passes) {
                    const double growth = std::max(1.0, hp_plan.predicted_dof_factor);
                    const double next_elems =
                        std::ceil(growth * static_cast<double>(vol.mesh.elements.size()));
                    const double next_dof =
                        std::ceil(growth * 3.0 * static_cast<double>(vol.mesh.nodes.size()));

                    const bool elem_cap =
                        next_elems > static_cast<double>(resolved.element_ceiling);
                    const bool dof_cap = next_dof > static_cast<double>(resolved.dof_ceiling);
                    if (elem_cap || dof_cap) {
                        const std::string reason =
                            elem_cap && dof_cap
                                ? std::format("adapt growth cap stop: next pass predicted "
                                              "{:.0f} elems / "
                                              "{:.0f} DOF exceeds element ceiling {} and DOF "
                                              "ceiling {}",
                                              next_elems, next_dof, resolved.element_ceiling,
                                              resolved.dof_ceiling)
                            : elem_cap
                                ? std::format(
                                      "adapt growth cap stop: next pass predicted {:.0f} "
                                      "elems exceeds element ceiling {}",
                                      next_elems, resolved.element_ceiling)
                                : std::format(
                                      "adapt growth cap stop: next pass predicted {:.0f} "
                                      "DOF exceeds DOF ceiling {}",
                                      next_dof, resolved.dof_ceiling);
                        SolveResult r;
                        r.mesh_note =
                            std::format("{} | {} | {}", vol.mesher_note, hp_note, reason);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        fill_result_fields(r, zz_try, u_try);
                        r.fill_geometry_volume = vol.fill_geometry_volume;
                        r.solved_geometry_volume = vol.solved_geometry_volume;

                        result_ = std::move(r);
                        break;
                    }
                    const auto& sug = hp_plan.h_suggestion;
                    // Early stop only when neither h nor p wants work.
                    if (sug.n_marked == 0 && sug.h_next >= h_use * 0.98 &&
                        hp_plan.p_mark.empty()) {
                        std::string pnote;

                        // Still try mark_smooth fallback if driver was silent on p
                        // but residual remains (legacy complement path).
                        auto p_idx = hp_plan.p_mark;
                        if (p_idx.empty()) {
                            p_idx = adapt::mark_smooth(zz_try.element_eta, 0.3);
                        }
                        if (maybe_p_elevate(p_idx, pnote)) {
                            publish_live_mesh(vol);
                            update_solved_geometry_volume(model, vol);

                            u_try = fea::solve_elastostatics(
                                vol.mesh, material, bc, loads,
                                solve_options_with_progress(pass, pass_count),
                                active_p_constraints());
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                        }
                        SolveResult r;
                        r.mesh_note = std::format("{} | {} | adapt early-stop h={:.4g}{}",
                                                  vol.mesher_note, hp_note, h_use, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        fill_result_fields(r, zz_try, u_try);
                        r.fill_geometry_volume = vol.fill_geometry_volume;
                        r.solved_geometry_volume = vol.solved_geometry_volume;

                        result_ = std::move(r);
                        break;
                    }
                    // Mid-loop p-raise when driver prefers p and marks few h cells.
                    if (!hp_plan.p_mark.empty() &&
                        hp_plan.h_mark.size() * 4 < hp_plan.p_mark.size()) {
                        std::string pnote;
                        if (maybe_p_elevate(hp_plan.p_mark, pnote)) {
                            publish_live_mesh(vol);
                            update_solved_geometry_volume(model, vol);

                            u_try = fea::solve_elastostatics(
                                vol.mesh, material, bc, loads,
                                solve_options_with_progress(pass, pass_count),
                                active_p_constraints());
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                            vol.mesher_note =
                                std::format("{} | mid-loop{}", vol.mesher_note, pnote);
                        }
                    }
                    h_use = std::max(sug.h_next, h_adapt_floor);
                    adapt_seeds = sug.refine_seeds;
                    adapt_seed_band = sug.seed_band;
                    adapt_marked = hp_plan.h_mark.empty()
                                       ? adapt::dorfler_mark(zz_try.element_eta, 0.3)
                                       : hp_plan.h_mark;
                    continue;
                }
                std::string pnote;
                auto p_idx = hp_plan.p_mark;
                if (p_idx.empty() && do_p_elevate) {
                    p_idx = adapt::mark_smooth(zz_try.element_eta, 0.3);
                }
                if (maybe_p_elevate(p_idx, pnote)) {
                    publish_live_mesh(vol);
                    update_solved_geometry_volume(model, vol);

                    u_try = fea::solve_elastostatics(
                        vol.mesh, material, bc, loads,
                        solve_options_with_progress(pass, pass_count),
                        active_p_constraints());
                    zz_try = fea::recover_zz(vol.mesh, material, u_try);
                }
                SolveResult r;
                r.mesh_note = std::format("{} | {} | adapt_passes={} h={:.4g} seeds={}{}",
                                          vol.mesher_note, hp_note, setup.adapt_passes, h_use,
                                          adapt_seeds.size(), pnote);
                r.volume_mesh = std::move(vol.mesh);
                r.boundary_quads = std::move(vol.boundary_quads);
                fill_result_fields(r, zz_try, u_try);
                r.fill_geometry_volume = vol.fill_geometry_volume;
                r.solved_geometry_volume = vol.solved_geometry_volume;

                result_ = std::move(r);
            } // adapt passes
            report("done", 1.0,
                   std::format("done — max von Mises {:.4g} MPa, max deflection {:.4g} mm",
                               result_.max_von_mises / 1e6, result_.max_displacement * 1e3),
                   pass_count, pass_count);
            state_ = State::kDone;
        } catch (const JobCancelled&) {
            report("cancelled", 0.0, "solve cancelled");
            state_ = State::kCancelled;
        } catch (const std::exception& e) {
            report("done", 0.0, std::format("solve failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

std::optional<SolveResult> SolveJob::take_result() {
    if (state_ != State::kDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(result_);
}

std::optional<VolumeMeshOutput> SolveJob::take_mesh() {
    if (state_ != State::kMeshDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(mesh_only_);
}

SolveJob::~SolveJob() { join_worker(); }

} // namespace polymesh::pipeline
