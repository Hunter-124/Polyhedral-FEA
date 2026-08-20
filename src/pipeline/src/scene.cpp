// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/hp_driver.hpp"
#include "adapt/loop.hpp"
#include "adapt/spectral_sizing.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/element_validity.hpp"
#include "fea/p_elevate.hpp"
#include "fea/quadrature.hpp"
#include "fea/poly_to_vem.hpp"
#include "fea/shape.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"
#include "fea/vem.hpp"
#include "fea/vtu.hpp"
#include "mesh/local_refine.hpp"
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
    // Reflection symmetry of the exact geometry, once per load. Detected from the
    // BRep when there is one; the tessellation path is for OCC-disabled builds,
    // where the tessellation IS the geometry.
    model.mirror = model.cad && !model.cad->empty()
                       ? mesh::detect_mirror_frame(*model.cad, model.bbox_min,
                                                   model.bbox_max)
                       : mesh::detect_mirror_frame(model.surface);

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

// Curved-geometry cost policy, shared by the mesher and the auto-h budget so
// interactive sizing cannot be blown by a factor the resolver never saw.
// A curvature-dominated BRep is meshed on a half-size lattice (8× the cells),
// and tet10/hex20 promotion carries mid-edge nodes, i.e. well over 3 DOF per
// element. Measured 3·nodes/elements at auto h on the current fixtures:
//   sphere 4.44, icecream_cone 4.41, cylinder 4.36, plate_hole 4.51,
//   cantilever 4.47
// so 4.6 keeps the worst measured case inside the ceiling with ~2% margin. It
// must stay above the measured maximum: the resolver only gets one shot before
// the fill runs, and 4.4 let the sphere ship 1.9% over an interactive DOF cap.
constexpr double kCurvedAreaLatticeFraction = 0.25;
constexpr double kCurvedLatticeScale = 0.5;
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
    // Curved CAD geometry costs more per requested h: a curvature-dominated
    // BRep is filled on the half-size lattice and every cell carries mid-edge
    // nodes. Auto sizing has to spend that up front or the interactive ceiling
    // is exceeded by ~8× cells and ~12× DOF after the fact.
    const double lattice_factor =
        curved_geometry && cad_curved_area_fraction(cad_topology) >=
                               kCurvedAreaLatticeFraction
            ? kCurvedLatticeElementFactor
            : 1.0;
    const double dof_per_element = curved_geometry ? kCurvedDofPerElement : 3.0;
    const std::size_t dof_elem_ceiling = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(out.dof_ceiling) /
                                    (dof_per_element * lattice_factor)));
    const std::size_t elem_ceiling_scaled = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(out.element_ceiling) /
                                    lattice_factor));
    const std::size_t effective_elem_ceiling =
        std::min(elem_ceiling_scaled, dof_elem_ceiling);
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
///
/// Buckets are anchored on the source set's own bbox centre, not on world zero.
/// A world-anchored lattice puts an arbitrary cell wall somewhere in the part, so
/// a source and its exact mirror can fall in cells of different widths relative
/// to the symmetry plane and one of the pair survives decimation alone. The
/// centre-anchored partition maps onto itself under reflection about any bbox
/// mid-plane, and min-h is commutative, so the surviving set is mirror-symmetric
/// whenever the input is.
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
    Eigen::Vector3d lo = src.front().x;
    Eigen::Vector3d hi = src.front().x;
    for (const auto& s : src) {
        lo = lo.cwiseMin(s.x);
        hi = hi.cwiseMax(s.x);
    }
    const Eigen::Vector3d anchor = 0.5 * (lo + hi);
    std::unordered_map<Key, std::size_t, KeyHash> best;
    best.reserve(src.size());
    const double inv = 1.0 / cell;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const Eigen::Vector3d d = (src[i].x - anchor) * inv;
        const Key k{static_cast<long>(std::floor(d.x())),
                    static_cast<long>(std::floor(d.y())),
                    static_cast<long>(std::floor(d.z()))};
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

/// Spectral truncation keeps the modes carrying this fraction of the spectral
/// energy; the remainder (noise, sub-seed oscillation) is merged into the
/// surrounding field. 0.995 is aggressive enough to trim isolated seed
/// artifacts and conservative enough that any spatially extended feature
/// survives verbatim.
constexpr double kSpectralEnergyFraction = 0.995;

/// Chordal sagitta rule numerator. A segment of length ℓ = c/κ on geometry of
/// curvature κ = 1/R has sagitta d = ℓ²κ/8 = c²R/8, i.e. a *relative* sag
/// d/R = c²/8 that is independent of R. c = 0.25 sets that at 0.78% of the
/// local radius of curvature, which is the value the surface-vertex rule
/// (adapt::curvature_size_sources' curvature_fraction) has always used; edge
/// and face sizing share it so a curved edge and the curved face it bounds ask
/// for the same size instead of fighting at their shared boundary.
constexpr double kCurvatureSagittaFraction = 0.25;

/// uv / arc-length sampling density for the exact-BRep sizing reads. 32 gives
/// 34 stations per curve and a 32×32 uv grid per non-planar face — enough that
/// the FFT edge denoise has a usable spectrum, and cheap because
/// extract_topology skips the grid on planar faces entirely.
constexpr int kCadSizingSamples = 32;

/// BRep topology for the sizing reads, or an empty topology when the model has
/// no CAD (STL / .msh input) or OCC cannot walk it. Walking the BRep once and
/// sharing the result keeps face-curvature and edge-curvature sizing on the
/// same sample stations.
geom::CadTopology cad_sizing_topology(const Model& model) {
    if (!model.cad || model.cad->empty()) {
        return {};
    }
    try {
        return geom::extract_topology(*model.cad, kCadSizingSamples);
    } catch (...) {
        return {};
    }
}

/// Chordal size sources along curved CAD edges with FFT-denoised curvature
/// (ADR-0034). OCC BRepLProp κ samples carry parameterization noise; the
/// energy-truncated inverse FFT recovers the smooth κ(s), and the emitted
/// source size follows the constant-relative-sag rule h = c/κ.
/// Flat edge runs (κ below the noise floor after denoise) emit nothing.
std::vector<adapt::SizeSource> spectral_edge_sources(const geom::CadTopology& topo,
                                                     double h_min_geo, double h_coarse,
                                                     SpectralSizingReport& report) {
    std::vector<adapt::SizeSource> out;
    if (!(h_coarse > 0.0)) {
        return out;
    }
    for (const auto& edge : topo.edges) {
        if (edge.feature == geom::CadEdgeFeature::kSeam) {
            continue; // parameterization artifact, not geometry
        }
        const auto& pts = edge.samples;
        const auto& kappa = edge.kappa_samples;
        if (pts.size() < 8 || kappa.size() != pts.size()) {
            continue;
        }
        std::vector<double> stations(pts.size(), 0.0);
        for (std::size_t i = 1; i < pts.size(); ++i) {
            stations[i] = stations[i - 1] + (pts[i] - pts[i - 1]).norm();
        }
        adapt::spectral::FilterReport edge_report;
        const auto smooth =
            adapt::spectral::lowpass_signal(stations, kappa, kSpectralEnergyFraction,
                                            &edge_report);
        if (smooth.size() != pts.size()) {
            continue;
        }
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const double k = smooth[i];
            if (!(k > 1e-9) || !std::isfinite(k)) {
                continue; // denoised-flat run
            }
            const double h_edge =
                std::clamp(kCurvatureSagittaFraction / k, h_min_geo, h_coarse);
            if (h_edge < h_coarse) {
                out.push_back({pts[i], h_edge});
            }
        }
    }
    report.n_edge_curve_seeds = out.size();
    return out;
}

/// Curvature size sources read from the **exact BRep faces**: the same
/// constant-relative-sag rule h = c/κ as spectral_edge_sources, applied to
/// `geom::CadFace::kappa_samples` (max |principal curvature| on a uv grid
/// inside the trim) instead of to a discrete per-vertex estimate on the
/// triangulation.
///
/// This is the whole point of ADR-0036 §6: OCC's tessellation of a
/// mirror-symmetric part is not mirror-symmetric (sphere: 0.00% of tessellation
/// vertices have an exact mirror partner across x, 1.33% across z; plate_hole
/// 5.97% across x), so a size field seeded from tessellation curvature cannot
/// be mirror-symmetric and neither can the element pattern it drives. A uv grid
/// on the analytic surface has no seam or pole bias — measured on sphere.step
/// the exact samples give κ = 20.000 1/m at every one of 1024 stations
/// (max/min = 1.0) where the tessellation estimate spans 17.70…145.87 1/m
/// (max/min = 8.24).
///
/// No FFT denoise here: an analytic κ has no parameterization noise to remove
/// (that is what the edge path is compensating for), and filtering would
/// re-introduce a dependence on the sample ordering.
std::vector<adapt::SizeSource> cad_face_curvature_sources(const geom::CadTopology& topo,
                                                          double h_min_geo, double h_coarse) {
    std::vector<adapt::SizeSource> out;
    if (!(h_coarse > 0.0)) {
        return out;
    }
    for (const auto& face : topo.faces) {
        if (face.kappa_samples.size() != face.samples.size()) {
            continue;
        }
        for (std::size_t i = 0; i < face.samples.size(); ++i) {
            const double k = face.kappa_samples[i];
            if (!(k > 1e-9) || !std::isfinite(k)) {
                continue; // exactly flat: nothing to resolve
            }
            const double h_face =
                std::clamp(kCurvatureSagittaFraction / k, h_min_geo, h_coarse);
            if (h_face < h_coarse) {
                out.push_back({face.samples[i], h_face});
            }
        }
    }
    return out;
}

/// Spectral wrap of a fused size field (ADR-0034): sample on a Cartesian grid,
/// energy-truncate the spectrum (insignificant fine bands merge), re-impose
/// the geometry-only demand (elementwise min — trimming can never blur a real
/// feature), then optionally land the predicted element count on `budget`
/// with one uniform h scale. `geo_field` must be the geometry-sources-only
/// sizing (no BC/error seeds); pass an empty fn when no floor is wanted.
mesh::SizeFieldFn apply_spectral_sizing(const Model& model,
                                        const mesh::SizeFieldFn& field,
                                        const mesh::SizeFieldFn& geo_field,
                                        double h_fine, std::size_t budget,
                                        SpectralSizingReport& report) {
    if (!field || !(h_fine > 0.0)) {
        return field;
    }
    const double target_spacing = 0.5 * h_fine;
    adapt::spectral::Grid3d grid = adapt::spectral::sample_field_grid(
        field, model.bbox_min, model.bbox_max, target_spacing);
    report.predicted_before = adapt::spectral::predict_element_count(grid);
    const double h_entry_min = grid.min_value();
    const double h_entry_max = grid.max_value();

    const auto filter =
        adapt::spectral::lowpass_grid_energy(grid, kSpectralEnergyFraction);
    report.modes_total = filter.modes_total;
    report.modes_kept = filter.modes_kept;
    report.energy_kept = filter.energy_total > 0.0
                             ? filter.energy_kept / filter.energy_total
                             : 1.0;

    if (geo_field) {
        // Geometry cap: the filter may raise h inside a weak-but-real feature;
        // the geometry-only field (denoised curvature / thin-wall demand) is
        // the authority there. min() can only refine, never coarsen.
        for (int k = 0; k < grid.dims[2]; ++k) {
            for (int j = 0; j < grid.dims[1]; ++j) {
                for (int i = 0; i < grid.dims[0]; ++i) {
                    const Eigen::Vector3d p =
                        grid.origin +
                        Eigen::Vector3d(static_cast<double>(i) * grid.spacing.x(),
                                        static_cast<double>(j) * grid.spacing.y(),
                                        static_cast<double>(k) * grid.spacing.z());
                    double& v = grid.at(i, j, k);
                    const double geo_h = geo_field(p);
                    if (geo_h > 0.0 && std::isfinite(geo_h)) {
                        v = std::min(v, geo_h);
                    }
                }
            }
        }
    }

    if (budget > 0) {
        const double predicted = adapt::spectral::predict_element_count(grid);
        if (predicted > static_cast<double>(budget)) {
            report.h_scale =
                std::cbrt(predicted / static_cast<double>(budget));
            for (double& v : grid.values) {
                v = std::clamp(v * report.h_scale, h_entry_min, h_entry_max);
            }
        }
    }
    report.predicted_after = adapt::spectral::predict_element_count(grid);
    report.budget_met =
        budget == 0 || report.predicted_after <= static_cast<double>(budget) * 1.001;
    report.applied = true;

    auto shared = std::make_shared<adapt::spectral::GridSizingField>(std::move(grid));
    return [shared](const Eigen::Vector3d& p) { return shared->size_at(p); };
}

} // namespace

RefinementPlan build_refinement_plan(const Model& model, double h_coarse,
                                     std::span<const RefineRegion> regions,
                                     bool use_geometry, bool spectral,
                                     std::size_t spectral_budget) {
    RefinementPlan plan;
    if (!(h_coarse > 0.0) || !std::isfinite(h_coarse)) {
        return plan;
    }
    std::vector<adapt::SizeSource> sources;
    std::vector<adapt::SizeSource> geo_only; // spectral geometry floor

    // Geometry a-priori: curvature + thin-wall surface sources finer than the
    // bulk h. Flat, thick regions emit nothing, so the source set stays sparse.
    if (use_geometry) {
        const double h_min_geo = 0.15 * h_coarse; // floor: avoid runaway fine

        // One extraction serves both exact-BRep reads (faces for curvature,
        // edges for the FFT-denoised chordal rule) so the BRep is walked once.
        const geom::CadTopology topo = cad_sizing_topology(model);

        std::vector<adapt::SizeSource> geo;
        if (!topo.faces.empty()) {
            // Exact BRep available: curvature comes from the analytic faces, so
            // the sizing plan no longer inherits the tessellation's broken
            // mirror symmetry (ADR-0036 §6). Thin-wall demand has no exact
            // analogue — local thickness is a ray cast through the closed
            // surface — so it keeps coming from the tessellation.
            plan.geometry_curvature_from_brep = true;
            geo = cad_face_curvature_sources(topo, h_min_geo, h_coarse);
            auto thick = adapt::thickness_size_sources(model.surface, h_min_geo, h_coarse);
            geo.insert(geo.end(), thick.begin(), thick.end());
        } else {
            geo = adapt::geometry_size_sources(model.surface, h_min_geo, h_coarse);
        }
        // ~1 seed per vertex on a real CAD part is far more than the grading
        // needs; keep the finest per half-h cell (field preserved, count bounded).
        geo = decimate_sources(std::move(geo), 0.5 * h_coarse);
        if (spectral) {
            // FFT-denoised CAD-edge chordal sources join the geometry set.
            auto edge = spectral_edge_sources(topo, h_min_geo, h_coarse, plan.spectral);
            edge = decimate_sources(std::move(edge), 0.5 * h_coarse);
            plan.spectral.n_edge_curve_seeds = edge.size();
            geo.insert(geo.end(), edge.begin(), edge.end());
            geo_only = geo; // copy: the spectral floor excludes BC seeds
        }
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
    if (spectral && plan.size_field) {
        // Geometry-only floor: denoised curvature / thin-wall demand without
        // BC seeds, so spectral trimming can never blur a real feature.
        mesh::SizeFieldFn geo_field;
        if (!geo_only.empty()) {
            const auto geo_sp = adapt::seed_plan(geo_only, h_coarse, 1.5);
            geo_field = adapt::size_field_from_sources(geo_only, geo_sp.h_fine,
                                                       h_coarse, /*beta=*/1.0);
        }
        // Budget scale is deliberately NOT driven from the element ceiling
        // here: the lattice meshers' real element counts diverge from the
        // Σvol/h³ density model by part-dependent factors (fine bands, skin
        // cells), so the pre-flight resolve + measured auto-retry remain the
        // cap authority. The spectral budget API serves callers whose mesher
        // honors the CVT density contract directly (ADR-0024 Q10 #4).
        plan.size_field =
            apply_spectral_sizing(model, plan.size_field, geo_field, sp.h_fine,
                                  spectral_budget, plan.spectral);
        if (plan.spectral.applied) {
            // Seeds force fine balls regardless of the field; drop the ones
            // the trimmed field no longer wants, or they silently defeat the
            // trim on the ball-grading meshers. Keep the seed's own h demand:
            // a seed survives where the filtered field still asks for < 3/4
            // of the bulk size.
            std::vector<Eigen::Vector3d> kept;
            kept.reserve(plan.refine_seeds.size());
            for (const auto& seed : plan.refine_seeds) {
                if (plan.size_field(seed) < 0.75 * h_coarse) {
                    kept.push_back(seed);
                }
            }
            plan.refine_seeds = std::move(kept);
        }
    }
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
    const double quality = fea::cell_quality(nodal_mesh, element);
    if (!std::isfinite(quality) || quality < mesh::validity::kCellShapeFloor) {
        return false;
    }
    return true;
}


} // namespace

bool make_boundary_projection(const geom::CadModel& cad, double h,
                              mesh::BoundaryProjectionContext* ctx,
                              std::vector<mesh::BoundarySupport>* provenance,
                              std::shared_ptr<const geom::CadTopology>* topology_out) {
    if (ctx == nullptr || provenance == nullptr || cad.empty()) {
        if (ctx != nullptr) {
            *ctx = {};
        }
        return false;
    }

    // 32 interior samples per edge, not 10: the polyline is the capture test
    // and the arclength parameterization for the feature pin (ADR-0035), and
    // a 10-sample circle has a 4.9%·R chord sag — a fifth of a cell on a small
    // bore, enough to mis-capture a crease node. The pin target itself is
    // always the exact OCC curve projection, so this only sharpens the
    // classification, never the geometry.
    auto topology = std::make_shared<geom::CadTopology>(geom::extract_topology(cad, 32));
    if (topology_out != nullptr) {
        *topology_out = topology;
    }
    const geom::CadModel* cad_ptr = &cad;
    ctx->provenance = provenance;
    ctx->topology = topology;
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
            // Ownership exists so a node cannot drift across a trimmed face or
            // a sharp edge, not so a misclassification can pin it in mid-air.
            // A node that latched a face it cannot reach — classified at its
            // raw lattice site, then found on the far side of a bore rim — is
            // projected onto that face's nearest trimmed point forever and
            // ships O(h) off the solid (measured on plate_hole tet at
            // h = 3 mm: 4 nodes stuck 0.22 h out, drawn as flaps standing off
            // the hole). When the owned projection is more than half a cell
            // away and the free whole-shape projection is materially closer,
            // the classification was wrong: adopt the better face and record
            // it. Edges and vertices stay immutable — those owners are
            // features, not conveniences.
            if (exact && exact->distance > 0.5 * h) {
                if (auto free_target = geom::project_point_on_surface(*cad_ptr, p);
                    free_target && free_target->distance < 0.5 * exact->distance &&
                    free_target->face_id != geom::kInvalidCadSupportId) {
                    owner = {mesh::BoundarySupportKind::kCadFace, free_target->face_id};
                    exact = std::move(free_target);
                }
            }
        } else {
            exact = geom::project_point_on_surface(*cad_ptr, p);
            if (!exact) {
                return std::nullopt;
            }

            const double feature_slack = 0.08 * h;
            // The crease preference exists to capture lattice stair nodes that
            // sit O(h) off the surface near a sharp rim. It must not claim a
            // node whose home face is decisively closer than the crease:
            // measured on icecream_cone at h = 8 mm, a cone-wall edge midpoint
            // 4 um from its face was pinned to the foot rim 580 um away
            // (580 <= 4 + 0.08 h passed), and the quadratic patch through it
            // dipped 140 um below the base plane. The absolute slack alone
            // cannot separate "on the face next to the rim" from "stair node
            // of the rim"; the ratio can — a genuine crease node has the two
            // distances within a small multiple of each other.
            const auto crease_competes = [&](double crease_distance) {
                return crease_distance <= exact->distance + feature_slack &&
                       crease_distance <= 4.0 * exact->distance + 1e-3 * h;
            };
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
                crease_competes(vertex_distance)) {
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
                    crease_competes(nearest_edge->distance)) {
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
    std::vector<std::uint32_t>* partial_nodes, const mesh::MirrorFrame* mirror) {
    if (reverted_nodes != nullptr) {
        reverted_nodes->clear();
    }
    if (partial_nodes != nullptr) {
        partial_nodes->clear();
    }
    if (projection == nullptr || !projection->target || cad.empty() || !(h > 0.0)) {
        return 0;
    }
    auto boundary_mids = quadratic_boundary_mids(nodal_mesh);
    if (boundary_mids.empty()) {
        return 0;
    }
    // Mirror-canonical visit order. Each mid is projected and then line-searched
    // back against its own incident cells, and those cells are shared, so the
    // order decides which mids keep how much of their projection. Ascending id
    // does not mirror (ADR-0036 Section 9).
    {
        const mesh::MirrorKeyFrame mkey = mesh::mirror_key_frame(nodal_mesh.nodes);
        std::stable_sort(boundary_mids.begin(), boundary_mids.end(),
                         [&](const auto& x, const auto& y) {
                             if (x.mid >= nodal_mesh.nodes.size() ||
                                 y.mid >= nodal_mesh.nodes.size()) {
                                 return x.mid < y.mid;
                             }
                             const auto kx = mkey.key(nodal_mesh.nodes[x.mid]);
                             const auto ky = mkey.key(nodal_mesh.nodes[y.mid]);
                             return kx != ky ? kx < ky : x.mid < y.mid;
                         });
    }
    std::unordered_map<std::uint32_t, Eigen::Vector3d> saved_positions;
    saved_positions.reserve(boundary_mids.size());
    for (const auto& edge : boundary_mids) {
        if (edge.mid < nodal_mesh.nodes.size()) {
            saved_positions.try_emplace(edge.mid, nodal_mesh.nodes[edge.mid]);
        }
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
            // Folded query, unfolded answer: the owner id these corners agree on
            // is the canonical-octant entity (ADR-0036 Section 9.2).
            const Eigen::Vector3d query = mesh::mirror_fold(mirror, saved);
            if (owner_a.kind == mesh::BoundarySupportKind::kCadEdge) {
                exact = geom::project_point_on_edge(cad, owner_a.id, query);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadFace) {
                exact = geom::project_point_on_face(cad, owner_a.id, query);
            } else if (owner_a.kind == mesh::BoundarySupportKind::kCadVertex) {
                exact = geom::project_point_on_vertex(cad, owner_a.id, query);
            } else {
                direct = false;
            }
            if (exact) {
                exact->point = mesh::mirror_unfold(mirror, exact->point, saved);
                exact->distance = (exact->point - saved).norm();
            }
        }

        std::optional<mesh::BoundaryTarget> target;
        if (direct) {
            if (exact) {
                commit_owner(edge.mid, direct_owner);
                target = mesh::BoundaryTarget{exact->point, exact->distance};
            }
        } else {
            // Endpoints that both sit on the same sharp edge own the mid
            // jointly even when their provenance KINDS differ: a rim node is
            // legitimately on the edge and on a face at once, so kind
            // disagreement says nothing. Without this the chord mid of a rim
            // edge free-classified to the face (distance ~0, the chord is in
            // the face plane) and bowed off the rim — measured on
            // icecream_cone at h = 8 mm: a 1.9 mm foot-rim edge's mid sat
            // 74 um inside the rim circle.
            if (projection->topology != nullptr) {
                const double tol = 1e-3 * h;
                const Eigen::Vector3d qa =
                    mesh::mirror_fold(mirror, nodal_mesh.nodes[edge.a]);
                const Eigen::Vector3d qb =
                    mesh::mirror_fold(mirror, nodal_mesh.nodes[edge.b]);
                double best = std::numeric_limits<double>::infinity();
                std::uint32_t best_id = 0;
                bool found = false;
                for (const auto& topo_edge : projection->topology->edges) {
                    if (topo_edge.feature != geom::CadEdgeFeature::kSharp) {
                        continue;
                    }
                    const auto pa = geom::project_point_on_edge(cad, topo_edge.id, qa);
                    const auto pb = geom::project_point_on_edge(cad, topo_edge.id, qb);
                    if (!pa || !pb) {
                        continue;
                    }
                    const double both = std::max(pa->distance, pb->distance);
                    if (both <= tol && both < best) {
                        best = both;
                        best_id = topo_edge.id;
                        found = true;
                    }
                }
                if (found) {
                    const Eigen::Vector3d query = mesh::mirror_fold(mirror, saved);
                    if (auto on_edge = geom::project_point_on_edge(cad, best_id, query)) {
                        on_edge->point = mesh::mirror_unfold(mirror, on_edge->point, saved);
                        on_edge->distance = (on_edge->point - saved).norm();
                        commit_owner(edge.mid,
                                     mesh::BoundarySupport{
                                         mesh::BoundarySupportKind::kCadEdge, best_id});
                        target = mesh::BoundaryTarget{on_edge->point, on_edge->distance};
                    }
                }
            }
            if (!target) {
                target = mesh::owned_boundary_projection_target(saved, edge.mid, projection,
                                                                mirror);
            }
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
    // Midpoint moves share cells. A sequence can pass every local line search
    // and still make their combined curved mapping unacceptable, so close with
    // a whole-cell rollback and feed those edges to the caller's h-refinement
    // fallback.
    for (int round = 0; round < 4; ++round) {
        std::set<std::uint32_t> rollback;
        for (const auto& element : nodal_mesh.elements) {
            const double quality = fea::cell_quality(nodal_mesh, element);
            if (fea::element_jacobians_positive(nodal_mesh, element) &&
                std::isfinite(quality) &&
                quality >= mesh::validity::kCellShapeFloor) {
                continue;
            }
            for (const auto node : element.nodes) {
                if (saved_positions.contains(node)) {
                    rollback.insert(node);
                }
            }
        }
        if (rollback.empty()) {
            break;
        }
        bool changed = false;
        for (const auto node : rollback) {
            const auto& saved = saved_positions.at(node);
            if ((nodal_mesh.nodes[node] - saved).squaredNorm() > 0.0) {
                nodal_mesh.nodes[node] = saved;
                changed = true;
            }
            if (reverted_nodes != nullptr) {
                reverted_nodes->push_back(node);
            }
        }
        if (!changed) {
            break;
        }
    }
    if (reverted_nodes != nullptr) {
        std::sort(reverted_nodes->begin(), reverted_nodes->end());
        reverted_nodes->erase(
            std::unique(reverted_nodes->begin(), reverted_nodes->end()),
            reverted_nodes->end());
    }
    if (partial_nodes != nullptr) {
        std::sort(partial_nodes->begin(), partial_nodes->end());
        partial_nodes->erase(
            std::unique(partial_nodes->begin(), partial_nodes->end()),
            partial_nodes->end());
    }
    return projected;
}

CurvedGeometryResult curve_volume_geometry(const Model& model,
                                           const fea::NodalMesh& source, double h) {
    CurvedGeometryResult result;
    result.mesh.nodes = source.nodes;
    result.mesh.elements.reserve(source.elements.size());
    for (const auto& element : source.elements) {
        if (element.type != fea::ElementType::kPyramid5 || element.nodes.size() != 5) {
            result.mesh.elements.push_back(element);
            continue;
        }
        const auto& p = element.nodes;
        const int diagonal = mesh::validity::pyramid_split_diagonal(
            source.nodes[p[0]], source.nodes[p[1]], source.nodes[p[2]],
            source.nodes[p[3]]);
        const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            result.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kTet4, {a, b, c, p[4]}});
        };
        if (diagonal == 1) {
            emit(p[1], p[2], p[3]);
            emit(p[1], p[3], p[0]);
        } else {
            emit(p[0], p[1], p[2]);
            emit(p[0], p[2], p[3]);
        }
        ++result.n_pyramids_split;
    }

    fea::NodalMesh linear_mesh = result.mesh;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<std::size_t> promote;
        promote.reserve(linear_mesh.elements.size());
        for (std::size_t i = 0; i < linear_mesh.elements.size(); ++i) {
            const auto type = linear_mesh.elements[i].type;
            if (type == fea::ElementType::kTet4 || type == fea::ElementType::kHex8) {
                promote.push_back(i);
            }
        }
        auto elevated = fea::p_elevate_with_constraints(linear_mesh, promote);
        result.mesh = std::move(elevated.mesh);
        const fea::NodalMesh straight_mesh = result.mesh;
        result.constraints = std::move(elevated.constraints);
        result.n_promoted = elevated.n_promoted;

        std::vector<std::uint32_t> reverted;
        std::vector<std::uint32_t> partial;
        std::map<std::uint32_t, std::array<std::uint32_t, 2>> parents;
        for (const auto& edge : quadratic_boundary_mids(result.mesh)) {
            parents.try_emplace(edge.mid, std::array{edge.a, edge.b});
        }
        if (model.cad && h > 0.0) {
            std::vector<mesh::BoundarySupport> provenance;
            mesh::BoundaryProjectionContext projection;
            // The linear mesh handed here is exactly mirror-symmetric on a
            // symmetric part (ADR-0036 §9); the curved promotion must not undo
            // that, so the mid projection is folded like every other one.
            const mesh::MirrorFrame* mirror = model.mirror.any() ? &model.mirror : nullptr;
            if (make_boundary_projection(*model.cad, h, &projection, &provenance)) {
                result.n_projected = project_quadratic_boundary_mids(
                    result.mesh, *model.cad, &projection, h, &reverted, &partial, mirror);
            }
            const auto topology = geom::extract_topology(*model.cad);
            std::unordered_map<std::uint32_t, std::vector<std::size_t>>
                edge_mid_incidence;
            edge_mid_incidence.reserve(parents.size());
            for (std::size_t ei = 0; ei < result.mesh.elements.size(); ++ei) {
                for (const auto node : result.mesh.elements[ei].nodes) {
                    if (parents.contains(node)) {
                        edge_mid_incidence[node].push_back(ei);
                    }
                }
            }
            // One topology query and one OCC projection per boundary corner —
            // not per quadratic edge. A corner is shared by ~6 boundary edges,
            // so caching its exact sharp-edge owner is what keeps this pass off
            // the mesher's critical path.
            std::unordered_map<std::uint32_t, std::optional<std::uint32_t>>
                sharp_owner;
            const auto exact_sharp_owner =
                [&](std::uint32_t node) -> std::optional<std::uint32_t> {
                    const auto cached = sharp_owner.find(node);
                    if (cached != sharp_owner.end()) {
                        return cached->second;
                    }
                    std::optional<std::uint32_t> owner;
                    // Folded, so a corner and its mirror image latch the SAME
                    // canonical sharp edge and the mid projection below reproduces
                    // mirrored points from it (ADR-0036 Section 9).
                    const Eigen::Vector3d query =
                        mesh::mirror_fold(mirror, result.mesh.nodes[node]);
                    if (const auto near = geom::closest_edge(topology, query, true)) {
                        if (near->distance <= h) {
                            if (const auto exact = geom::project_point_on_edge(
                                    *model.cad, near->edge_id, query)) {
                                if (exact->distance <= 1e-6 * h) {
                                    owner = near->edge_id;
                                }
                            }
                        }
                    }
                    sharp_owner.emplace(node, owner);
                    return owner;
                };
            // Mirror-canonical visit order: each pin is accepted against the
            // shared node array, so the order decides which ones survive.
            auto sharp_mids = quadratic_boundary_mids(result.mesh);
            {
                const mesh::MirrorKeyFrame mkey =
                    mesh::mirror_key_frame(result.mesh.nodes);
                std::stable_sort(sharp_mids.begin(), sharp_mids.end(),
                                 [&](const auto& x, const auto& y) {
                                     const auto kx = mkey.key(result.mesh.nodes[x.mid]);
                                     const auto ky = mkey.key(result.mesh.nodes[y.mid]);
                                     return kx != ky ? kx < ky : x.mid < y.mid;
                                 });
            }
            for (const auto& edge : sharp_mids) {
                const auto owner_a = exact_sharp_owner(edge.a);
                if (!owner_a) {
                    continue;
                }
                const auto owner_b = exact_sharp_owner(edge.b);
                if (!owner_b || *owner_a != *owner_b) {
                    continue;
                }
                const Eigen::Vector3d mid_query =
                    mesh::mirror_fold(mirror, result.mesh.nodes[edge.mid]);
                const auto exact =
                    geom::project_point_on_edge(*model.cad, *owner_a, mid_query);
                if (!exact) {
                    continue;
                }
                const Eigen::Vector3d saved_mid = result.mesh.nodes[edge.mid];
                result.mesh.nodes[edge.mid] =
                    mesh::mirror_unfold(mirror, exact->point, saved_mid);
                bool valid = true;
                for (const auto ei : edge_mid_incidence[edge.mid]) {
                    const auto& element = result.mesh.elements[ei];
                    const double quality = fea::cell_quality(result.mesh, element);
                    const double target = std::min(
                        mesh::validity::kCellShapeFloor,
                        fea::cell_quality(straight_mesh, straight_mesh.elements[ei]));
                    valid = valid &&
                            fea::element_jacobians_positive(result.mesh, element) &&
                            std::isfinite(quality) && quality >= target;
                }
                if (!valid) {
                    result.mesh.nodes[edge.mid] = saved_mid;
                    continue;
                }
                ++result.n_projected;
            }
        }
        // Curvature must never make a cell worse, but it also must not be held
        // to a floor the straight promotion already misses: `cell_quality` on a
        // p2 cell is not the corner ratio the mesher gates on, so a cell the
        // mesher shipped at 0.0201 can read 0.0182 straight. Compare each curved
        // cell against its own straight baseline and the shared floor.
        std::vector<double> straight_quality(straight_mesh.elements.size(), 0.0);
        for (std::size_t i = 0; i < straight_mesh.elements.size(); ++i) {
            straight_quality[i] =
                fea::cell_quality(straight_mesh, straight_mesh.elements[i]);
        }
        bool curved_mesh_valid = true;
        for (std::size_t i = 0; i < result.mesh.elements.size(); ++i) {
            const auto& element = result.mesh.elements[i];
            const double quality = fea::cell_quality(result.mesh, element);
            const double target =
                std::min(mesh::validity::kCellShapeFloor,
                         i < straight_quality.size() ? straight_quality[i] : 0.0);
            curved_mesh_valid = curved_mesh_valid &&
                                fea::element_jacobians_positive(result.mesh, element) &&
                                std::isfinite(quality) && quality >= target;
        }
        if (!curved_mesh_valid) {
            result.mesh = straight_mesh;
            partial.clear();
            reverted.clear();
            for (const auto& [mid, edge] : parents) {
                (void)edge;
                reverted.push_back(mid);
            }
        }
        result.n_partial = partial.size();
        result.n_reverted = reverted.size();
        if ((partial.empty() && reverted.empty()) || attempt == 3) {
            break;
        }

        bool pure_linear_tet = true;
        for (const auto& element : linear_mesh.elements) {
            pure_linear_tet =
                pure_linear_tet && element.type == fea::ElementType::kTet4;
        }
        if (!pure_linear_tet) {
            break;
        }
        std::set<std::array<std::uint32_t, 2>> offending_edges;
        for (const auto node : partial) {
            if (const auto found = parents.find(node); found != parents.end()) {
                auto edge = found->second;
                std::sort(edge.begin(), edge.end());
                offending_edges.insert(edge);
            }
        }
        for (const auto node : reverted) {
            if (const auto found = parents.find(node); found != parents.end()) {
                auto edge = found->second;
                std::sort(edge.begin(), edge.end());
                offending_edges.insert(edge);
            }
        }
        mesh::TetFillOutput linear;
        linear.nodes = linear_mesh.nodes;
        linear.tets.reserve(linear_mesh.elements.size());
        std::vector<std::size_t> marked;
        for (std::size_t i = 0; i < linear_mesh.elements.size(); ++i) {
            const auto& nodes = linear_mesh.elements[i].nodes;
            std::array<std::uint32_t, 4> tet{
                nodes[0], nodes[1], nodes[2], nodes[3]};
            linear.tets.push_back(tet);
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    auto edge = std::array{
                        tet[static_cast<std::size_t>(a)],
                        tet[static_cast<std::size_t>(b)]};
                    std::sort(edge.begin(), edge.end());
                    if (offending_edges.contains(edge)) {
                        marked.push_back(i);
                    }
                }
            }
        }
        std::sort(marked.begin(), marked.end());
        marked.erase(std::unique(marked.begin(), marked.end()), marked.end());
        if (marked.empty()) {
            break;
        }
        // The fallback is opportunistic: if conformal refinement cannot clear
        // these marks (LEB closure can fail on an already-graded 2:1 front), the
        // straight-edged promotion above is still a correct mesh, so abandon the
        // retry instead of failing the whole mesh/solve.
        mesh::LocalRefineStats refine_stats;
        mesh::TetFillOutput refined;
        try {
            refined = mesh::local_refine_tets(linear, marked, &refine_stats,
                                              &model.surface);
        } catch (const std::exception&) {
            break;
        }
        if (refine_stats.n_new_nodes == 0) {
            break;
        }
        bool refined_shape_ok = true;
        for (const auto& tet : refined.tets) {
            refined_shape_ok =
                refined_shape_ok &&
                mesh::validity::tet_shape_quality(
                    refined.nodes[tet[0]], refined.nodes[tet[1]],
                    refined.nodes[tet[2]], refined.nodes[tet[3]]) >=
                    mesh::validity::kCellShapeFloor;
        }
        if (!refined_shape_ok) {
            break;
        }
        result.n_h_refined += refine_stats.n_bisections;
        linear_mesh.nodes = std::move(refined.nodes);
        linear_mesh.elements.clear();
        linear_mesh.elements.reserve(refined.tets.size());
        for (const auto& tet : refined.tets) {
            linear_mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4,
                                  {tet[0], tet[1], tet[2], tet[3]}});
        }
    }
    result.mesh.check_validity();
    // Integrability is absolute: a non-integrable cell here would be a bug in
    // the promotion/projection above, not a mesh the caller can use.
    for (const auto& element : result.mesh.elements) {
        if (!fea::element_jacobians_positive(result.mesh, element)) {
            throw std::runtime_error(std::format(
                "curve_volume_geometry: {} cell is not integrable after curving",
                fea::element_type_name(element.type)));
        }
    }
    return result;
}

namespace {
// The mesh volume measure lives in `fea::element_volume` / `fea::mesh_volume`
// (fea/cell_quality.hpp). It used to be duplicated here; the copy in
// `fea::element_centroid_stresses` had drifted into a wrong one, so the rule is
// now defined once and every caller shares it.

/// Final ship gate: relax the cells that are actually being emitted.
///
/// Every mesher gates its OWN cells during snap, with its own predicate over
/// its own intermediate zoo. What ships is `fea::NodalMesh`, and the measure
/// the product reports is `fea::cell_quality` — the pyramid a hybrid fill
/// gated as a pyramid may ship as two assembly tets, an LEB child may be
/// carved after its last gate, and neither is re-measured. This sweep closes
/// that gap the way ADR-0033 requires: measure the cell that ships, and if it
/// is below the floor, give it room by relaxing its INTERIOR nodes only
/// (boundary nodes carry the exact-BRep placement and must not move).
///
/// Returns the number of cells still below the floor, which the caller reports
/// rather than hides.
std::size_t relax_cells_below_shape_floor(fea::NodalMesh& mesh,
                                          std::span<const std::array<std::uint32_t, 4>>
                                              boundary_faces,
                                          double floor_value, int rounds = 4) {
    if (mesh.elements.empty() || mesh.nodes.empty()) {
        return 0;
    }
    std::vector<char> on_boundary(mesh.nodes.size(), 0);
    for (const auto& face : boundary_faces) {
        for (const auto ni : face) {
            if (ni < on_boundary.size()) {
                on_boundary[ni] = 1;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> incident(mesh.nodes.size());
    std::vector<std::vector<std::uint32_t>> neighbours(mesh.nodes.size());
    for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
        const auto& nodes = mesh.elements[ei].nodes;
        for (const auto ni : nodes) {
            if (ni >= incident.size()) {
                continue;
            }
            incident[ni].push_back(static_cast<std::uint32_t>(ei));
            for (const auto other : nodes) {
                if (other != ni && other < mesh.nodes.size()) {
                    neighbours[ni].push_back(other);
                }
            }
        }
    }
    for (auto& list : neighbours) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    // Accept a relaxation when it raises the WORST cell of the moved node's
    // star. Demanding the whole star clear the floor rejects every move on a
    // star that is below the floor to begin with — which is the only star this
    // sweep is ever called on.
    const auto star_min_quality = [&](std::uint32_t ni) {
        double worst = std::numeric_limits<double>::infinity();
        for (const auto ei : incident[ni]) {
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (std::isfinite(q)) {
                worst = std::min(worst, q);
            }
        }
        return worst;
    };

    std::size_t remaining = 0;
    for (int round = 0; round < rounds; ++round) {
        // Ascending element index, then ascending node id: the acceptance test
        // reads the shared node array, so visit order is mutation state.
        std::vector<std::uint32_t> targets;
        for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (!std::isfinite(q) || q >= floor_value) {
                continue;
            }
            for (const auto ni : mesh.elements[ei].nodes) {
                if (ni < on_boundary.size() && on_boundary[ni] == 0 &&
                    !neighbours[ni].empty()) {
                    targets.push_back(ni);
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (targets.empty()) {
            break;
        }
        bool moved_any = false;
        for (const auto ni : targets) {
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            for (const auto other : neighbours[ni]) {
                centroid += mesh.nodes[other];
            }
            centroid /= static_cast<double>(neighbours[ni].size());
            const Eigen::Vector3d saved = mesh.nodes[ni];
            const double before = star_min_quality(ni);
            mesh.nodes[ni] = saved + 0.5 * (centroid - saved);
            const double after = star_min_quality(ni);
            // Improvement is not enough: a move from -1e-3 to -9e-11 is an
            // improvement and still ships an inverted cell, which the solver
            // then refuses with "non-positive Jacobian". Never leave a star
            // non-positive, and never make a positive star worse.
            if (!(after > before) || !(after > 0.0)) {
                mesh.nodes[ni] = saved;
            } else {
                moved_any = true;
            }
        }
        if (!moved_any) {
            break;
        }
    }
    for (const auto& element : mesh.elements) {
        const double q = fea::cell_quality(mesh, element);
        if (std::isfinite(q) && q < floor_value) {
            ++remaining;
        }
    }
    return remaining;
}

/// Boundary conformity on the mesh that actually ships (ADR-0035).
///
/// Every mesher snaps *its own* boundary set — the lattice skin it built. What
/// ships is `fea::extract_boundary_faces(out.mesh)`, the true element exterior,
/// and the two are not the same set: a fan tet peeled after the snap, a pyramid
/// shipped as two assembly tets, or an LEB child carved late can expose a node
/// that was interior when the snap ran and is on the free surface when the mesh
/// leaves. Those nodes were never candidates for projection, so they keep their
/// raw lattice position — measured on sphere/hybrid at h = 8 mm: the branch's
/// own worst boundary node sat 0.016 h off the exact BRep while the shipped
/// mesh carried nodes 0.081 h off, visible as isolated flaps on the render.
///
/// This closes the loop for every mesher at once: project the true exterior
/// through the same owner-aware oracle, accept a move only when every incident
/// cell keeps `fea::cell_quality` at or above the shared floor (the measure the
/// product reports, not each mesher's internal predicate), and open room by
/// relaxing interior star nodes when the first attempt is refused.
///
/// The acceptance test is deliberately absolute rather than "improves": an
/// earlier version of this pass accepted a cell at ~1e-11 quality because the
/// move improved it, and the solver then refused the p-elevated element with
/// "non-positive Jacobian". A node that cannot be placed above the floor stays
/// where it is and is counted.
struct ExteriorConformStats {
    std::size_t n_candidates = 0;
    std::size_t n_moved = 0;
    std::size_t n_relax_rescued = 0;
    std::size_t n_hex_fanned = 0; // hexes fanned into pyramids to free a node
    std::size_t n_left = 0;       // still off the BRep by more than 1e-9 h
    std::size_t n_edge_pinned = 0;
    std::size_t n_edge_chains = 0;
    std::size_t n_pin_rejected = 0;
    std::size_t n_connected_edges = 0;
    std::string connected_edge_census;
    double edge_pass_ms = 0.0; // cost of the exact sharp-edge recovery pass
    std::size_t n_kink_relieved = 0; // face nodes slid to lower a facet kink
    double worst_residual = 0.0;
    std::uint32_t worst_node = 0;
    Eigen::Vector3d worst_position = Eigen::Vector3d::Zero();
    bool reverted = false; // whole pass rolled back by the exit invariant
};

ExteriorConformStats
conform_true_exterior(fea::NodalMesh& mesh,
                      std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                      mesh::BoundaryProjectionContext* projection,
                      const mesh::BoundaryFit* fit, double h, double floor_value,
                      const mesh::MirrorFrame* mirror) {
    ExteriorConformStats stats;
    if (mesh.elements.empty() || mesh.nodes.empty() || projection == nullptr || !(h > 0.0)) {
        return stats;
    }
    // Reflection orbit over the node set this pass receives. Every mesher stage
    // upstream is exactly mirror-symmetric by construction (mesh/mirror.hpp), and
    // this pass then moved nodes one at a time under a quality gate, which is
    // order-dependent: measured on cylinder.step at h = 8 mm the mesher handed the
    // pipeline a 100/100/100% mirrored mesh and the shipped VTU read
    // 98.96/98.79/99.61%. So each accepted move here is applied to a whole orbit
    // or to none of it.
    const mesh::MirrorNodeOrbit orbit(mirror != nullptr ? *mirror : mesh::MirrorFrame{},
                                      mesh.nodes, [&] {
                                          const mesh::MirrorKeyFrame frame =
                                              mesh::mirror_key_frame(mesh.nodes);
                                          return frame.inv_quantum > 0.0
                                                     ? 1.0 / frame.inv_quantum
                                                     : 0.0;
                                      }());
    // Orbit copies of `node`, itself included, or empty when any copy is missing.
    const auto orbit_of = [&](std::uint32_t node) {
        std::vector<std::uint32_t> group{node};
        if (!orbit.active()) {
            return group;
        }
        for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
            const std::uint32_t other = orbit.reflected(node, mask);
            if (other == mesh::MirrorNodeOrbit::npos) {
                group.clear();
                return group;
            }
            if (std::find(group.begin(), group.end(), other) == group.end()) {
                group.push_back(other);
            }
        }
        return group;
    };
    std::vector<char> on_boundary(mesh.nodes.size(), 0);
    for (const auto& face : boundary_faces) {
        for (const auto ni : face) {
            if (ni < on_boundary.size()) {
                on_boundary[ni] = 1;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> incident;
    std::vector<std::vector<std::uint32_t>> neighbours;
    const auto rebuild_incidence = [&] {
        on_boundary.resize(mesh.nodes.size(), 0);
        incident.assign(mesh.nodes.size(), {});
        neighbours.assign(mesh.nodes.size(), {});
        for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
            const auto& nodes = mesh.elements[ei].nodes;
            for (const auto ni : nodes) {
                if (ni >= incident.size()) {
                    continue;
                }
                incident[ni].push_back(static_cast<std::uint32_t>(ei));
                for (const auto other : nodes) {
                    if (other != ni && other < mesh.nodes.size()) {
                        neighbours[ni].push_back(other);
                    }
                }
            }
        }
        for (auto& list : neighbours) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
    };
    rebuild_incidence();
    const auto star_min_quality = [&](std::uint32_t ni) {
        double worst = std::numeric_limits<double>::infinity();
        for (const auto ei : incident[ni]) {
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (std::isfinite(q)) {
                worst = std::min(worst, q);
            }
        }
        return worst;
    };
    // Two conditions, both necessary. `cell_quality` keeps the mesh solvable in
    // the shape sense the product reports; `element_jacobians_positive` is the
    // assembly's own integrability test, and it is the one that catches the
    // near-degenerate acceptances a quality floor lets through (measured: a
    // quality-accepted move shipped det J = -6.085e-09 on icecream_cone/graded).
    const auto star_ok = [&](std::uint32_t ni) {
        const double q = star_min_quality(ni);
        if (std::isfinite(q) && q < floor_value) {
            return false;
        }
        return fea::star_jacobians_positive(mesh, incident[ni]);
    };
    // Room for one refused node. Interior star neighbours carry no geometry
    // constraint, so moving them costs no fidelity; the boundary neighbours may
    // only slide ALONG their own owner geometry, which changes spacing and not
    // placement. Both tiers are validated with the same rule the node move
    // uses — quality never worse, and always integrable, because a Laplacian
    // nudge that only watched `cell_quality` shipped det J = -1.911e-09 on
    // icecream_cone/varyhedron.
    // One node's Laplacian nudge. `try_nudge_orbit` below is what callers use:
    // a nudge accepted on one side of a symmetric part and refused on the other is
    // itself an asymmetry, and this pass runs on nodes the snap could not place,
    // which is exactly where a symmetric part has symmetric trouble.
    const auto try_nudge_one = [&](std::uint32_t ni, bool tangential) {
        if (neighbours[ni].empty()) {
            return false;
        }
        const double cap = 0.25 * h;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (const auto other : neighbours[ni]) {
            centroid += mesh.nodes[other];
        }
        centroid /= static_cast<double>(neighbours[ni].size());
        const Eigen::Vector3d saved = mesh.nodes[ni];
        const Eigen::Vector3d step = 0.5 * (centroid - saved);
        const double len = step.norm();
        const double before = star_min_quality(ni);
        Eigen::Vector3d moved = saved + (len > cap ? step * (cap / len) : step);
        if (tangential) {
            const auto back =
                mesh::owned_boundary_projection_target(moved, ni, projection, mirror);
            if (!back || (back->point - saved).norm() > cap) {
                return false;
            }
            moved = back->point;
        }
        mesh.nodes[ni] = moved;
        const double after = star_min_quality(ni);
        const bool ok = (!std::isfinite(after) || after >= std::min(before, floor_value)) &&
                        (std::isfinite(after) ? after > 0.0 : true) &&
                        fea::star_jacobians_positive(mesh, incident[ni]);
        if (!ok) {
            mesh.nodes[ni] = saved;
            return false;
        }
        return (mesh.nodes[ni] - saved).squaredNorm() > 0.0;
    };
    const auto try_nudge = [&](std::uint32_t ni, bool tangential) {
        const auto group = orbit_of(ni);
        if (group.empty()) {
            return false; // incomplete orbit: leave the node alone
        }
        std::vector<Eigen::Vector3d> saved;
        saved.reserve(group.size());
        for (const auto node : group) {
            saved.push_back(mesh.nodes[node]);
        }
        bool all_moved = true;
        for (const auto node : group) {
            if (!try_nudge_one(node, tangential)) {
                all_moved = false;
                break;
            }
        }
        if (!all_moved) {
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                mesh.nodes[group[gi]] = saved[gi];
            }
            return false;
        }
        return true;
    };
    const auto open_room = [&](std::uint32_t seed) {
        bool moved_any = false;
        std::vector<std::uint32_t> ring = neighbours[seed];
        mesh::sort_mirror_canonical(mesh.nodes, ring);
        for (const auto ni : ring) {
            if (on_boundary[ni] == 0) {
                moved_any = try_nudge(ni, /*tangential=*/false) || moved_any;
            }
        }
        if (moved_any) {
            return true;
        }
        // A hex-blocked node has no interior corner to give — measured on
        // sphere/hybrid, `open_room` rescued none of 34 stragglers through the
        // interior tier alone. The star's other WALL nodes are the only degrees
        // of freedom left, and sliding them along the surface is free.
        for (const auto ni : ring) {
            if (ni != seed && on_boundary[ni] != 0) {
                moved_any = try_nudge(ni, /*tangential=*/true) || moved_any;
            }
        }
        return moved_any;
    };

    std::vector<std::uint32_t> exterior;
    for (std::uint32_t ni = 0; ni < mesh.nodes.size(); ++ni) {
        if (on_boundary[ni] != 0 && !incident[ni].empty()) {
            exterior.push_back(ni);
        }
    }
    // Mirror-canonical order: the march below moves nodes and opens room in their
    // neighbourhoods, so an earlier node decides what a later one can do.
    mesh::sort_mirror_canonical(mesh.nodes, exterior);
    const double eps = 1e-9 * h;
    // Exact resolution only. `boundary_projection_target` falls back to the
    // TESSELLATION when a node has no latched owner, and that fallback is
    // exactly how one visible defect survived every earlier pass: on
    // sphere/hybrid, 34 nodes reported a ~0 residual against the triangulation
    // while sitting 0.085 h off the true sphere, because OCC's facets are that
    // far off it at this deflection. A node with no owner is therefore
    // projected freely onto the BRep instead, which also latches an owner.
    const auto resolve = [&](std::uint32_t ni) -> std::optional<mesh::BoundaryTarget> {
        // Folded: owners recorded by the pin are the CANONICAL entity, so an
        // unfolded query against them answers in the wrong octant (ADR-0036 §9.2).
        auto target =
            mesh::owned_boundary_projection_target(mesh.nodes[ni], ni, projection, mirror);
        if (!target && fit != nullptr && fit->cad != nullptr) {
            if (const auto free_projection =
                    geom::project_point_on_surface(*fit->cad, mesh.nodes[ni])) {
                target = mesh::BoundaryTarget{free_projection->point, free_projection->distance};
            }
        }
        return target;
    };
    // Constrained march instead of one all-or-nothing jump: take the largest
    // legal fraction of the remaining gap, re-project onto the same owner, and
    // repeat. Every accepted step strictly reduces the distance to the owner's
    // exact geometry, and the acceptance rule is the ship gate's — never make
    // the worst incident cell worse, never leave a star non-positive or below
    // the floor it started above. Returns the residual left.
    // The owner projection is not the measured quantity. A node whose latched
    // owner is a far face can be walked toward a point that IS on the BRep and
    // still end up FARTHER from the nearest surface, because the straight
    // segment leaves the local patch — measured on icecream_cone/cvt_poly,
    // where an unguarded march took the worst node from 0.503 h to 1.799 h. So
    // every step must also not increase the free distance to the shape, which
    // is exactly what the fidelity metric reports.
    const auto free_distance = [&](std::uint32_t ni) {
        if (fit == nullptr || fit->cad == nullptr) {
            return 0.0;
        }
        const auto projected = geom::project_point_on_surface(*fit->cad, mesh.nodes[ni]);
        return projected ? projected->distance : 0.0;
    };
    // The march advances a whole reflection orbit in lockstep: one step fraction
    // for every member, accepted only when every member's own gate accepts it.
    // Marching each node separately is not equivalent — the members' gates read
    // quality values that tie across a mirror pair, so their fraction ladders can
    // stop at different rungs and place mirrored nodes differently (the same
    // failure mode the collapse round showed, ADR-0036 Section 9.2).
    const auto march_group = [&](const std::vector<std::uint32_t>& group, bool* relaxed) {
        for (int step = 0; step < 6; ++step) {
            std::vector<mesh::BoundaryTarget> now;
            now.reserve(group.size());
            bool have_targets = true;
            for (const auto node : group) {
                const auto target = resolve(node);
                if (!target || target->distance <= eps) {
                    have_targets = false;
                    break;
                }
                now.push_back(*target);
            }
            if (!have_targets) {
                break;
            }
            std::vector<Eigen::Vector3d> saved;
            std::vector<double> floor_here;
            std::vector<double> free_before;
            saved.reserve(group.size());
            floor_here.reserve(group.size());
            free_before.reserve(group.size());
            for (const auto node : group) {
                saved.push_back(mesh.nodes[node]);
                floor_here.push_back(std::min(star_min_quality(node), floor_value));
                free_before.push_back(free_distance(node));
            }
            bool advanced = false;
            for (const double frac : {1.0, 0.5, 0.25, 0.125}) {
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    mesh.nodes[group[gi]] = saved[gi] + frac * (now[gi].point - saved[gi]);
                }
                bool step_ok = true;
                for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                    const auto node = group[gi];
                    const double after = star_min_quality(node);
                    step_ok = (!std::isfinite(after) || after >= floor_here[gi]) &&
                              fea::star_jacobians_positive(mesh, incident[node]) &&
                              free_distance(node) <= free_before[gi];
                }
                if (step_ok) {
                    advanced = true;
                    break;
                }
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    mesh.nodes[group[gi]] = saved[gi];
                }
            }
            if (advanced) {
                continue;
            }
            bool opened = false;
            if (relaxed != nullptr && !*relaxed) {
                for (const auto node : group) {
                    opened = open_room(node) || opened;
                }
            }
            if (opened) {
                *relaxed = true;
                continue;
            }
            break;
        }
        return free_distance(group.front());
    };
    const auto march = [&](std::uint32_t ni, bool* relaxed) {
        const auto group = orbit_of(ni);
        if (group.empty()) {
            return free_distance(ni); // incomplete orbit: no move is symmetric
        }
        return march_group(group, relaxed);
    };

    // Whole-pass insurance. Every individual acceptance above is local — it
    // proves its own star did not get worse — and locality is not the same
    // promise as "the shipped mesh did not get worse". On cvt_poly, whose cells
    // are already degenerate (quality ~1e-14), the local rule let the count of
    // sub-floor cells drift up while the minimum stayed put. So the pass is also
    // judged as a whole, against the two numbers the product reports, and
    // reverted wholesale if either moved the wrong way.
    const auto mesh_quality_census = [&] {
        std::pair<double, std::size_t> census{std::numeric_limits<double>::infinity(), 0};
        for (const auto& element : mesh.elements) {
            const double q = fea::cell_quality(mesh, element);
            if (!std::isfinite(q)) {
                continue;
            }
            census.first = std::min(census.first, q);
            if (q < floor_value) {
                ++census.second;
            }
        }
        return census;
    };
    const auto entry_nodes = mesh.nodes;
    const auto entry_elements = mesh.elements;
    const auto entry_census = mesh_quality_census();

    std::vector<std::uint32_t> stuck;
    for (const auto ni : exterior) {
        const double start = free_distance(ni);
        if (start <= eps) {
            continue;
        }
        ++stats.n_candidates;
        bool relaxed_here = false;
        const double left = march(ni, &relaxed_here);
        if (left < start) {
            ++stats.n_moved;
            if (relaxed_here) {
                ++stats.n_relax_rescued;
            }
        }
        if (left > eps) {
            stuck.push_back(ni);
        }
    }

    // Conforming hex relief. What is left is blocked by a hexahedron already
    // saturated at the shape floor (measured on sphere/hybrid: every straggler
    // had exactly one incident hex at quality 0.020081, four e-5 above the
    // floor, so even a 0.125 step took it under). A hex has no interior corner
    // and its neighbours are hexes too, so no amount of relaxation helps.
    //
    // Fanning the hex into six pyramids over its own six quad faces changes no
    // face — the pyramid bases ARE the hex faces — so it is conforming with
    // every neighbour and the boundary shell is untouched. The apex is a new
    // interior node with full freedom, and a pyramid tolerates the wall motion
    // a hex refuses. The pipeline already ships pyramids from this mesher, so
    // nothing downstream is new.
    // The whole phase is judged as one unit and rolled back if it does not pay:
    // on icecream_cone/hex it fanned 20 hexes, moved no node at all, and left
    // 29 cells below the floor. A relief that buys nothing must cost nothing.
    if (!stuck.empty()) {
        const auto mesh_worst_quality = [&] {
            double worst = std::numeric_limits<double>::infinity();
            for (const auto& element : mesh.elements) {
                const double q = fea::cell_quality(mesh, element);
                if (std::isfinite(q)) {
                    worst = std::min(worst, q);
                }
            }
            return worst;
        };
        const auto count_below_floor = [&] {
            std::size_t n = 0;
            for (const auto& element : mesh.elements) {
                const double q = fea::cell_quality(mesh, element);
                if (std::isfinite(q) && q < floor_value) {
                    ++n;
                }
            }
            return n;
        };
        const auto saved_nodes = mesh.nodes;
        const auto saved_elements = mesh.elements;
        const double quality_before = mesh_worst_quality();
        const std::size_t below_before = count_below_floor();
        double residual_before = 0.0;
        for (const auto ni : stuck) {
            residual_before += free_distance(ni);
        }
        static constexpr int kHexFaces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                                {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        std::set<std::uint32_t> hexes;
        for (const auto ni : stuck) {
            for (const auto ei : incident[ni]) {
                if (mesh.elements[ei].type == fea::ElementType::kHex8 &&
                    mesh.elements[ei].nodes.size() == 8) {
                    hexes.insert(ei);
                }
            }
        }
        for (const auto ei : hexes) {
            const auto corners = mesh.elements[ei].nodes;
            Eigen::Vector3d centre = Eigen::Vector3d::Zero();
            for (const auto ni : corners) {
                centre += mesh.nodes[ni];
            }
            centre /= 8.0;
            const auto apex = static_cast<std::uint32_t>(mesh.nodes.size());
            mesh.nodes.push_back(centre);
            std::array<fea::NodalElement, 6> fan{};
            bool ok = true;
            for (int f = 0; f < 6 && ok; ++f) {
                std::array<std::uint32_t, 4> base{
                    {corners[static_cast<std::size_t>(kHexFaces[f][0])],
                     corners[static_cast<std::size_t>(kHexFaces[f][1])],
                     corners[static_cast<std::size_t>(kHexFaces[f][2])],
                     corners[static_cast<std::size_t>(kHexFaces[f][3])]}};
                // Orientation is decided by measurement, not by trusting a face
                // table: whichever winding gives the pyramid a positive split
                // volume is the one that ships.
                if (mesh::validity::pyramid_min_split_volume(
                        mesh.nodes[base[0]], mesh.nodes[base[1]], mesh.nodes[base[2]],
                        mesh.nodes[base[3]], centre) <= 0.0) {
                    std::swap(base[1], base[3]);
                }
                if (mesh::validity::pyramid_min_split_volume(
                        mesh.nodes[base[0]], mesh.nodes[base[1]], mesh.nodes[base[2]],
                        mesh.nodes[base[3]], centre) <= 0.0) {
                    ok = false;
                    break;
                }
                fan[static_cast<std::size_t>(f)] = fea::NodalElement{
                    fea::ElementType::kPyramid5,
                    {base[0], base[1], base[2], base[3], apex}};
                // A positive volume is not enough. Fanning an already tight hex
                // produced pyramids far under the floor and shipped 71 cells
                // below it, quality_min -0.159, on sphere/hybrid — a worse mesh
                // bought with a better boundary. Every child must clear the
                // floor and be integrable, measured before anything is
                // committed (`cell_quality` reads only `mesh.nodes`, and the
                // apex is already in place).
                const auto& child = fan[static_cast<std::size_t>(f)];
                const double q = fea::cell_quality(mesh, child);
                ok = std::isfinite(q) && q >= floor_value &&
                     fea::element_jacobians_positive(mesh, child);
            }
            if (!ok) {
                mesh.nodes.pop_back();
                continue;
            }
            mesh.elements[ei] = fan[0];
            for (std::size_t f = 1; f < fan.size(); ++f) {
                mesh.elements.push_back(fan[f]);
            }
            ++stats.n_hex_fanned;
        }
        if (stats.n_hex_fanned > 0) {
            rebuild_incidence();
        }
        std::size_t moved_here = 0;
        double residual_after = 0.0;
        for (const auto ni : stuck) {
            const double before = free_distance(ni);
            bool relaxed_here = false;
            const double left = march(ni, &relaxed_here);
            residual_after += left;
            if (left < before) {
                ++moved_here;
            }
        }
        std::size_t nonintegrable = 0;
        for (const auto& element : mesh.elements) {
            if (!fea::element_jacobians_positive(mesh, element)) {
                ++nonintegrable;
            }
        }
        const bool paid = moved_here > 0 && residual_after < residual_before;
        // On a mesh that is already degenerate (cvt_poly ships quality ~1e-14)
        // a "worst quality did not drop" test is toothless — it let the count of
        // sub-floor cells grow 807 → 855 while the minimum stayed put. Count is
        // therefore part of the contract too.
        const bool kept_shape = mesh_worst_quality() >= std::min(quality_before, floor_value) &&
                                nonintegrable == 0 && count_below_floor() <= below_before;
        if (!paid || !kept_shape) {
            mesh.nodes = saved_nodes;
            mesh.elements = saved_elements;
            stats.n_hex_fanned = 0;
            rebuild_incidence();
        } else {
            stats.n_moved += moved_here;
        }
        for (const auto ni : stuck) {
            const double left = free_distance(ni);
            if (left > eps) {
                ++stats.n_left;
                if (left > stats.worst_residual) {
                    stats.worst_residual = left;
                    stats.worst_node = ni;
                    stats.worst_position = mesh.nodes[ni];
                }
            }
        }
    }

    // Features last, on the same shipped node set and under the same gate: a
    // node exposed late can be a crease node nobody pinned.
    if (fit != nullptr && fit->can_pin()) {
        const auto node_offends = [&](std::uint32_t ni) { return !star_ok(ni); };
        const auto pin = mesh::pin_feature_nodes(
            *fit->cad, *fit->topo, mesh.nodes, exterior, h, node_offends,
            projection->provenance, mirror);
        stats.n_edge_pinned = pin.edge_pinned;
        stats.n_edge_chains = pin.chains;
        stats.n_pin_rejected = pin.rejected;
        const auto edge_pass_t0 = std::chrono::steady_clock::now();
        std::set<std::pair<std::uint32_t, std::uint32_t>> boundary_edges;
        for (const auto& face : boundary_faces) {
            const int n = face[3] == face[2] ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                const auto a = face[static_cast<std::size_t>(i)];
                const auto b = face[static_cast<std::size_t>((i + 1) % n)];
                if (a != b) {
                    boundary_edges.insert(std::minmax(a, b));
                }
            }
        }
        const auto repair_edge = [&](std::uint32_t a, std::uint32_t b,
                                     const Eigen::Vector3d& target_a,
                                     const Eigen::Vector3d& target_b,
                                     std::vector<std::pair<std::uint32_t, Eigen::Vector3d>>*
                                         undo) {
            const Eigen::Vector3d saved_a = mesh.nodes[a];
            const Eigen::Vector3d saved_b = mesh.nodes[b];
            mesh.nodes[a] = target_a;
            mesh.nodes[b] = target_b;
            std::vector<std::uint32_t> candidates;
            std::vector<std::size_t> affected(incident[a].begin(), incident[a].end());
            affected.insert(affected.end(), incident[b].begin(), incident[b].end());
            std::sort(affected.begin(), affected.end());
            affected.erase(std::unique(affected.begin(), affected.end()),
                           affected.end());
            for (const auto ei : affected) {
                for (const auto node : mesh.elements[ei].nodes) {
                    if (on_boundary[node] == 0) {
                        candidates.push_back(node);
                    }
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            // The pattern search below is a Gauss-Seidel sweep over these interior
            // nodes, so their visit order decides where each one lands. Ascending
            // node id does not mirror; the mirror key does.
            mesh::sort_mirror_canonical(mesh.nodes, candidates);
            std::vector<Eigen::Vector3d> saved;
            saved.reserve(candidates.size());
            for (const auto node : candidates) {
                saved.push_back(mesh.nodes[node]);
                affected.insert(affected.end(), incident[node].begin(),
                                incident[node].end());
            }
            std::sort(affected.begin(), affected.end());
            affected.erase(std::unique(affected.begin(), affected.end()),
                           affected.end());
            // Branch-and-bound worst-quality: a candidate move can only be
            // accepted if it beats `bound`, so stop the scan the moment a cell
            // is at or below it. This is what keeps a per-edge pattern search
            // affordable on a full boundary (measured 22.7 s -> 2.6 s on
            // plate_hole h = 6 mm).
            const auto objective = [&](double bound) {
                double worst = std::numeric_limits<double>::infinity();
                for (const auto ei : affected) {
                    const auto& element = mesh.elements[ei];
                    double quality = fea::cell_quality(mesh, element);
                    if (!std::isfinite(quality)) {
                        return -std::numeric_limits<double>::infinity();
                    }
                    if (!fea::element_jacobians_positive(mesh, element)) {
                        quality = -std::abs(quality);
                    }
                    worst = std::min(worst, quality);
                    if (worst <= bound) {
                        return worst;
                    }
                }
                return worst;
            };
            if (objective(floor_value) < floor_value) {
                for (const double step_size : {0.5 * h, 0.25 * h, 0.125 * h}) {
                    for (int sweep = 0; sweep < 8; ++sweep) {
                        bool changed = false;
                        for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                            const auto node = candidates[ci];
                            double best_quality =
                                objective(-std::numeric_limits<double>::infinity());
                            Eigen::Vector3d best_position = mesh.nodes[node];
                            // Trial directions are taken in the node's OWN folded
                            // frame: a node in a high octant tries the reflected
                            // step first, so a node and its mirror image walk
                            // mirrored paths through this greedy search. Fixed
                            // ±axis order would hand them different first
                            // improvements and place them asymmetrically.
                            Eigen::Vector3d fold_sign = Eigen::Vector3d::Ones();
                            if (mirror != nullptr) {
                                for (int axis = 0; axis < 3; ++axis) {
                                    if (mirror->plane[static_cast<std::size_t>(axis)] &&
                                        mesh.nodes[node][axis] > mirror->center[axis]) {
                                        fold_sign[axis] = -1.0;
                                    }
                                }
                            }
                            for (int axis = 0; axis < 3; ++axis) {
                                for (const double sign : {-1.0, 1.0}) {
                                    Eigen::Vector3d trial = best_position;
                                    trial[axis] += sign * fold_sign[axis] * step_size;
                                    if (mirror != nullptr) {
                                        // A node on a plane may only move within
                                        // it; the normal step cancels and the
                                        // trial is a no-op.
                                        trial = mirror->clamp_to_planes(trial,
                                                                        mesh.nodes[node]);
                                    }
                                    mesh.nodes[node] = trial;
                                    const double quality = objective(best_quality);
                                    if (quality > best_quality + 1e-14) {
                                        best_quality = quality;
                                        best_position = trial;
                                        changed = true;
                                    }
                                    mesh.nodes[node] = best_position;
                                }
                            }
                        }
                        if (!changed) {
                            break;
                        }
                        if (objective(floor_value) >= floor_value) {
                            break;
                        }
                    }
                    if (objective(floor_value) >= floor_value) {
                        break;
                    }
                }
            }
            if (objective(floor_value) >= floor_value) {
                if (undo != nullptr) {
                    undo->emplace_back(a, saved_a);
                    undo->emplace_back(b, saved_b);
                    for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                        undo->emplace_back(candidates[ci], saved[ci]);
                    }
                }
                return true;
            }
            mesh.nodes[a] = saved_a;
            mesh.nodes[b] = saved_b;
            for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                mesh.nodes[candidates[ci]] = saved[ci];
            }
            return false;
        };
        std::map<std::size_t, std::size_t> connected_by_edge;
        // Per-node target cache: a boundary node belongs to ~6 boundary edges,
        // and both the topology query and the OCC curve projection are far more
        // expensive than the quality gate they feed.
        struct EdgeTarget {
            std::uint32_t edge_id = 0;
            Eigen::Vector3d point = Eigen::Vector3d::Zero();
            bool valid = false;
        };
        std::unordered_map<std::uint32_t, EdgeTarget> edge_target;
        const auto target_of = [&](std::uint32_t node) {
            const auto cached = edge_target.find(node);
            if (cached != edge_target.end()) {
                return cached->second;
            }
            EdgeTarget target;
            if (const auto near =
                    geom::closest_edge(*fit->topo, mesh.nodes[node], true)) {
                if (near->distance <= 4.0 * h) {
                    if (const auto exact = geom::project_point_on_edge(
                            *fit->cad, near->edge_id, mesh.nodes[node])) {
                        if ((exact->point - mesh.nodes[node]).norm() <= 4.0 * h) {
                            target = EdgeTarget{near->edge_id, exact->point, true};
                        }
                    }
                }
            }
            edge_target.emplace(node, target);
            return target;
        };
        // Edges are visited in mirror-canonical order and repaired in whole
        // orbits: `repair_edge` moves interior nodes to buy the quality its own
        // gate demands, so a repair accepted on one side and refused on the other
        // leaves the two sides of a symmetric part genuinely different.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edge_order(boundary_edges.begin(),
                                                                       boundary_edges.end());
        {
            const mesh::MirrorKeyFrame ekey = mesh::mirror_key_frame(mesh.nodes);
            std::sort(edge_order.begin(), edge_order.end(),
                      [&](const auto& x, const auto& y) {
                          const auto kx = ekey.key(0.5 * (mesh.nodes[x.first] +
                                                          mesh.nodes[x.second]));
                          const auto ky = ekey.key(0.5 * (mesh.nodes[y.first] +
                                                          mesh.nodes[y.second]));
                          return kx != ky ? kx < ky : x < y;
                      });
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> repaired;
        for (const auto& [a, b] : edge_order) {
            if (repaired.count(std::minmax(a, b)) != 0) {
                continue;
            }
            // Orbit copies of this edge, as node pairs.
            std::vector<std::pair<std::uint32_t, std::uint32_t>> copies{{a, b}};
            bool orbit_complete = true;
            if (orbit.active()) {
                for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
                    const std::uint32_t a2 = orbit.reflected(a, mask);
                    const std::uint32_t b2 = orbit.reflected(b, mask);
                    if (a2 == mesh::MirrorNodeOrbit::npos ||
                        b2 == mesh::MirrorNodeOrbit::npos) {
                        orbit_complete = false;
                        break;
                    }
                    if (boundary_edges.count(std::minmax(a2, b2)) == 0) {
                        orbit_complete = false;
                        break;
                    }
                    if (std::find(copies.begin(), copies.end(),
                                  std::pair<std::uint32_t, std::uint32_t>{a2, b2}) ==
                        copies.end()) {
                        copies.emplace_back(a2, b2);
                    }
                }
            }
            if (!orbit_complete) {
                continue;
            }
            bool all_valid = true;
            for (const auto& [a2, b2] : copies) {
                const auto ta = target_of(a2);
                const auto tb = target_of(b2);
                if (!ta.valid || !tb.valid || ta.edge_id != tb.edge_id) {
                    all_valid = false;
                    break;
                }
            }
            if (!all_valid) {
                continue;
            }
            std::vector<std::pair<std::uint32_t, Eigen::Vector3d>> undo;
            bool all_repaired = true;
            for (const auto& [a2, b2] : copies) {
                const auto ta = target_of(a2);
                const auto tb = target_of(b2);
                if (!repair_edge(a2, b2, ta.point, tb.point, &undo)) {
                    all_repaired = false;
                    break;
                }
            }
            if (!all_repaired) {
                for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                    mesh.nodes[it->first] = it->second;
                }
                continue;
            }
            if (projection->provenance->size() < mesh.nodes.size()) {
                projection->provenance->resize(mesh.nodes.size());
            }
            for (const auto& [a2, b2] : copies) {
                const std::uint32_t edge_id = target_of(a2).edge_id;
                (*projection->provenance)[a2] =
                    mesh::BoundarySupport{mesh::BoundarySupportKind::kCadEdge, edge_id};
                (*projection->provenance)[b2] =
                    mesh::BoundarySupport{mesh::BoundarySupportKind::kCadEdge, edge_id};
                stats.n_edge_pinned += 2;
                ++connected_by_edge[edge_id];
                ++stats.n_connected_edges;
                repaired.insert(std::minmax(a2, b2));
            }
        }
        stats.edge_pass_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - edge_pass_t0)
                .count();
        for (const auto& [edge_id, count] : connected_by_edge) {
            if (!stats.connected_edge_census.empty()) {
                stats.connected_edge_census += ",";
            }
            stats.connected_edge_census +=
                std::format("{}:{}", edge_id, count);
        }
    }

    // Facet-kink relief.
    //
    // With every boundary node exactly on the BRep the surface can still LOOK
    // wrong, and this is the defect a user actually sees: the shipped exterior
    // of the showcase cone (graded, h = 10 mm) carries adjacent facet pairs
    // whose planes differ by up to 77.8°, with a mean of 6.0°, because the
    // grading transitions leave needle facets next to bulk ones (adjacent area
    // ratios up to 9.8). A kink between two exact facets is not a placement
    // error, it is a *spacing* error, and spacing is the one degree of freedom
    // a node on a face still has.
    //
    // So: find the kinked boundary edges, slide the free-face nodes around them
    // along their own surface (re-projected through the owner oracle, so the
    // placement never changes), and keep only moves that lower the worst kink
    // in the node's own boundary neighbourhood. Crease and corner nodes are
    // never touched — their owner is an edge or a vertex, and sliding them is
    // exactly what would blunt the feature the pinning pass just made exact.
    {
        struct Facet {
            std::array<std::uint32_t, 4> nodes{};
            int count = 3;
        };
        std::vector<Facet> facets;
        facets.reserve(boundary_faces.size());
        for (const auto& face : boundary_faces) {
            Facet f;
            f.nodes = face;
            f.count = (face[3] == face[2]) ? 3 : 4;
            bool valid = true;
            for (int i = 0; i < f.count; ++i) {
                valid = valid && f.nodes[static_cast<std::size_t>(i)] < mesh.nodes.size();
            }
            if (valid) {
                facets.push_back(f);
            }
        }
        const auto facet_normal = [&](const Facet& f) {
            Eigen::Vector3d n = (mesh.nodes[f.nodes[1]] - mesh.nodes[f.nodes[0]])
                                    .cross(mesh.nodes[f.nodes[2]] - mesh.nodes[f.nodes[0]]);
            const double norm = n.norm();
            return norm > 1e-18 ? Eigen::Vector3d(n / norm) : Eigen::Vector3d::Zero().eval();
        };
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edge_facets;
        std::vector<std::vector<std::size_t>> node_facets(mesh.nodes.size());
        for (std::size_t fi = 0; fi < facets.size(); ++fi) {
            const auto& f = facets[fi];
            for (int i = 0; i < f.count; ++i) {
                const auto a = f.nodes[static_cast<std::size_t>(i)];
                const auto b = f.nodes[static_cast<std::size_t>((i + 1) % f.count)];
                if (a != b) {
                    edge_facets[{std::min(a, b), std::max(a, b)}].push_back(fi);
                }
                node_facets[a].push_back(fi);
            }
        }
        for (auto& list : node_facets) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        // Worst plane-to-plane angle among the manifold boundary edges of the
        // facets around one node — the quantity a move has to lower.
        const auto node_worst_kink = [&](std::uint32_t ni) {
            double worst = 0.0;
            for (const auto fi : node_facets[ni]) {
                const auto& f = facets[fi];
                for (int i = 0; i < f.count; ++i) {
                    const auto a = f.nodes[static_cast<std::size_t>(i)];
                    const auto b = f.nodes[static_cast<std::size_t>((i + 1) % f.count)];
                    if (a == b) {
                        continue;
                    }
                    const auto it = edge_facets.find({std::min(a, b), std::max(a, b)});
                    if (it == edge_facets.end() || it->second.size() != 2) {
                        continue;
                    }
                    const Eigen::Vector3d n0 = facet_normal(facets[it->second[0]]);
                    const Eigen::Vector3d n1 = facet_normal(facets[it->second[1]]);
                    if (n0.isZero() || n1.isZero()) {
                        worst = std::numbers::pi; // degenerate facet: always worse
                        continue;
                    }
                    worst = std::max(worst,
                                     std::acos(std::clamp(std::abs(n0.dot(n1)), 0.0, 1.0)));
                }
            }
            return worst;
        };
        const auto* provenance = projection->provenance;
        // Face-owned nodes slide; edge- and vertex-owned nodes never do, because
        // sliding them is what would blunt the crease the pinning pass just made
        // exact. Broadening this to unowned nodes as well was measured and
        // changed nothing on any fixture, so it is not carried.
        const auto slidable = [&](std::uint32_t ni) {
            if (provenance == nullptr || ni >= provenance->size()) {
                return false;
            }
            return (*provenance)[ni].kind == mesh::BoundarySupportKind::kCadFace;
        };
        const double kink_threshold = 25.0 * std::numbers::pi / 180.0;
        for (int round = 0; round < 4; ++round) {
            std::vector<std::uint32_t> candidates;
            for (const auto& [edge, owners] : edge_facets) {
                if (owners.size() != 2) {
                    continue;
                }
                const Eigen::Vector3d n0 = facet_normal(facets[owners[0]]);
                const Eigen::Vector3d n1 = facet_normal(facets[owners[1]]);
                if (n0.isZero() || n1.isZero() ||
                    std::acos(std::clamp(std::abs(n0.dot(n1)), 0.0, 1.0)) >= kink_threshold) {
                    for (const auto fi : owners) {
                        const auto& f = facets[fi];
                        for (int i = 0; i < f.count; ++i) {
                            candidates.push_back(f.nodes[static_cast<std::size_t>(i)]);
                        }
                    }
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            // Mirror-canonical visit order: this is a Gauss-Seidel sweep on the
            // shared node array under a quality gate, so an accepted slide decides
            // whether the next one is legal.
            mesh::sort_mirror_canonical(mesh.nodes, candidates);
            std::size_t moved = 0;
            std::vector<char> done(mesh.nodes.size(), 0);
            for (const auto ni : candidates) {
                if (!slidable(ni) || done[ni] != 0) {
                    continue;
                }
                // The whole orbit slides together or not at all. A slide is a
                // tangential move under a local quality gate, so accepting it on
                // one side and refusing it on the other is exactly the asymmetry
                // this pass used to introduce.
                const auto group = orbit_of(ni);
                if (group.empty()) {
                    continue;
                }
                bool orbit_slidable = true;
                for (const auto node : group) {
                    orbit_slidable = orbit_slidable && slidable(node);
                }
                if (!orbit_slidable) {
                    continue;
                }
                std::vector<Eigen::Vector3d> group_saved;
                group_saved.reserve(group.size());
                for (const auto node : group) {
                    group_saved.push_back(mesh.nodes[node]);
                }
                // One relax value for the whole orbit, tested on every member
                // before any of them is kept. Letting each member walk its own
                // 0.5/0.25/0.125 ladder is not equivalent: the acceptance test
                // compares kink and quality values that tie in exact arithmetic
                // across a mirror pair, so the ladders could stop at different
                // rungs and slide mirrored nodes by different amounts (measured on
                // icecream_cone at h = 12 mm without feature refinement: 10 nodes
                // and 144 tets lost their mirror image about y).
                std::vector<Eigen::Vector3d> centroid(group.size());
                std::vector<double> kink_before(group.size());
                std::vector<double> quality_before(group.size());
                bool have_centroids = true;
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    const auto node = group[gi];
                    // Umbrella centroid over the boundary neighbours only: an
                    // interior neighbour would pull the node off its own face.
                    Eigen::Vector3d sum = Eigen::Vector3d::Zero();
                    std::size_t n_used = 0;
                    for (const auto fi : node_facets[node]) {
                        const auto& f = facets[fi];
                        for (int i = 0; i < f.count; ++i) {
                            const auto other = f.nodes[static_cast<std::size_t>(i)];
                            if (other != node) {
                                sum += mesh.nodes[other];
                                ++n_used;
                            }
                        }
                    }
                    if (n_used == 0) {
                        have_centroids = false;
                        break;
                    }
                    centroid[gi] = sum / static_cast<double>(n_used);
                    kink_before[gi] = node_worst_kink(node);
                    quality_before[gi] = star_min_quality(node);
                }
                if (!have_centroids) {
                    continue;
                }
                bool all_accepted = false;
                for (const double relax : {0.5, 0.25, 0.125}) {
                    bool step_ok = true;
                    for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                        const auto node = group[gi];
                        const Eigen::Vector3d slid =
                            group_saved[gi] + relax * (centroid[gi] - group_saved[gi]);
                        const auto back = mesh::owned_boundary_projection_target(
                            slid, node, projection, mirror);
                        if (!back) {
                            step_ok = false;
                            break;
                        }
                        mesh.nodes[node] = back->point;
                    }
                    if (step_ok) {
                        for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                            const auto node = group[gi];
                            const double after = star_min_quality(node);
                            step_ok = node_worst_kink(node) < kink_before[gi] &&
                                      (!std::isfinite(after) ||
                                       after >= std::min(quality_before[gi], floor_value)) &&
                                      fea::star_jacobians_positive(mesh, incident[node]);
                        }
                    }
                    if (step_ok) {
                        all_accepted = true;
                        break;
                    }
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        mesh.nodes[group[gi]] = group_saved[gi];
                    }
                }
                if (!all_accepted) {
                    continue;
                }
                for (const auto node : group) {
                    done[node] = 1;
                    ++moved;
                }
            }
            stats.n_kink_relieved += moved;
            if (moved == 0) {
                break;
            }
        }
    }

    std::size_t nonintegrable_exit = 0;
    for (const auto& element : mesh.elements) {
        if (!fea::element_jacobians_positive(mesh, element)) {
            ++nonintegrable_exit;
        }
    }
    const auto exit_census = mesh_quality_census();
    if (nonintegrable_exit > 0 || exit_census.second > entry_census.second ||
        exit_census.first < std::min(entry_census.first, floor_value)) {
        mesh.nodes = entry_nodes;
        mesh.elements = entry_elements;
        stats.reverted = true;
    }
    return stats;
}

struct BoundaryShellTopology {
    std::size_t n_edges = 0;
    /// Used by one face: a hole in the shell.
    std::size_t n_open = 0;
    /// Used by three or more: two boundary patches occupying the same place.
    std::size_t n_nonmanifold = 0;
};

/// Edge-use census over the free-face set. A closed 2-manifold uses every edge
/// exactly twice; anything else is a tear or a duplicated skin, and neither
/// shows up in a volume comparison.
BoundaryShellTopology boundary_shell_topology(
    const std::vector<std::array<std::uint32_t, 4>>& faces) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> use;
    for (const auto& face : faces) {
        // Triangles arrive as the degenerate quad (a,b,c,c).
        const std::size_t n = face[3] == face[2] ? 3 : 4;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % n];
            if (a == b) {
                continue;
            }
            ++use[a < b ? std::pair{a, b} : std::pair{b, a}];
        }
    }
    BoundaryShellTopology out;
    out.n_edges = use.size();
    for (const auto& [edge, count] : use) {
        (void)edge;
        if (count == 1) {
            ++out.n_open;
        } else if (count > 2) {
            ++out.n_nonmanifold;
        }
    }
    return out;
}

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
    //
    // Every CAD-backed mesher gets this, not just the four that used to be
    // listed: plain tet and hex fill were snapping to the tessellation with no
    // crease awareness at all, which is why their sharp edges came out
    // chamfered and their curved walls carried lattice sawtooth (ADR-0035).
    std::vector<mesh::BoundarySupport> boundary_provenance;
    mesh::BoundaryProjectionContext projection_context;
    mesh::BoundaryProjectionContext* projection = nullptr;
    std::shared_ptr<const geom::CadTopology> cad_topology;
    if (model.cad && !model.cad->empty()) {
        try {
            if (make_boundary_projection(*model.cad, h, &projection_context,
                                         &boundary_provenance, &cad_topology)) {
                projection = &projection_context;
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::format("exact BRep projection setup failed: {}", e.what()));
        } catch (...) {
            throw std::runtime_error("exact BRep projection setup failed");
        }
    }
    mesh::BoundaryFit boundary_fit;
    boundary_fit.cad = model.cad ? &*model.cad : nullptr;
    boundary_fit.topo = cad_topology.get();
    boundary_fit.projection = projection;
    const mesh::BoundaryFit* fit = projection != nullptr ? &boundary_fit : nullptr;
    // Verified reflection symmetry of the geometry (mesh/mirror.hpp). Detected
    // from the exact BRep when there is one, and from the tessellation itself
    // when the tessellation IS the geometry (STL input, OCC-disabled build).
    //
    // This is what makes a symmetric part come out with a symmetric element
    // pattern: without it every mesher decision is read off a tessellation that
    // is not mirror-symmetric (sphere x: 0.00% of tessellation vertices have an
    // exact mirror partner), so a cell and its mirror image genuinely disagree.
    // Detection is dense and tight — every exact face sample is reflected and
    // must land back on the solid — so an asymmetric part simply gets no frame
    // and no fold.
    const mesh::MirrorFrame& mirror_frame = model.mirror;
    const mesh::MirrorFrame* mirror = mirror_frame.any() ? &mirror_frame : nullptr;
    const auto mirror_note = [&]() -> std::string {
        if (mirror == nullptr) {
            return " | mirror=none";
        }
        std::string axes;
        for (int a = 0; a < 3; ++a) {
            if (mirror_frame.plane[static_cast<std::size_t>(a)]) {
                axes += "xyz"[a];
            }
        }
        return std::format(" | mirror={} (reflected-sample residual {:.2g}·diag)", axes,
                           mirror_frame.max_residual_over_diag);
    };
    // Per-cell turning-angle refinement threshold for local curvature grading.
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
            // floor to the normalized signed pyramid volume. The collapse term is
            // shared with `fea::cell_quality` now
            // (mesh::validity::pyramid_volume_collapse) instead of being spelled
            // out a second time here; the numbers are identical, since the mean of
            // both base-diagonal volume sums IS the centroid-fanned volume.
            //
            // What this gate deliberately does NOT test is the base-corner scaled
            // Jacobian: a fold there is cured for free at conversion, by shipping
            // the cell as the two tets the assembly already builds from it, and
            // testing it here would instead retreat the wall (measured: hybrid
            // sphere M1max 1.7e-16 -> 0.037 at h=0.15*extent).
            const auto pyramid_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& p0 = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& p1 = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& p2 = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& p3 = fill.nodes[cell.nodes[3]];
                const Eigen::Vector3d& p4 = fill.nodes[cell.nodes[4]];
                if (mesh::validity::pyramid_min_split_volume(p0, p1, p2, p3, p4) <= vol_eps) {
                    return false;
                }
                return mesh::validity::pyramid_volume_collapse(p0, p1, p2, p3, p4) >= kMinShape;
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
            // Interior room for the snap, the mechanism tet_fill/hex_fill
            // already use (ADR-0035). Without it a boundary node whose star is
            // a stair fold retreats to its raw lattice site: measured on
            // sphere/hybrid at h = 8 mm, isolated flaps up to 0.081 h off the
            // exact BRep while every other conforming mesher reached machine
            // precision. This needs adjacency for EVERY node, not just the
            // boundary ones `snap_node_cells` tracks, because the nodes being
            // opened up are interior.
            std::vector<std::vector<std::size_t>> all_node_cells(fill.nodes.size());
            std::vector<std::vector<std::uint32_t>> nbrs(fill.nodes.size());
            {
                std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
                const auto link = [&](std::uint32_t u, std::uint32_t v) {
                    if (u == v) {
                        return;
                    }
                    const auto key = std::minmax(u, v);
                    if (!seen.insert({key.first, key.second}).second) {
                        return;
                    }
                    nbrs[u].push_back(v);
                    nbrs[v].push_back(u);
                };
                for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                    const auto& cell = fill.cells[ci];
                    std::span<const std::uint32_t> members =
                        cell.kind == mesh::MixedCellKind::kPolyVem
                            ? std::span<const std::uint32_t>(cell.poly_nodes)
                            : std::span<const std::uint32_t>(cell.nodes.data(), cell.n_nodes);
                    for (const auto ni : members) {
                        all_node_cells[ni].push_back(ci);
                    }
                    for (std::size_t a = 0; a < members.size(); ++a) {
                        for (std::size_t b = a + 1; b < members.size(); ++b) {
                            link(members[a], members[b]);
                        }
                    }
                }
            }
            const auto any_node_offends = [&](std::uint32_t ni) {
                if (ni >= all_node_cells.size()) {
                    return false;
                }
                for (const auto ci : all_node_cells[ni]) {
                    if (!snap_cell_valid(fill.cells[ci])) {
                        return true;
                    }
                }
                return false;
            };
            const auto reproject_node = [&](std::uint32_t ni, const Eigen::Vector3d& p) {
                const auto target =
                    mesh::boundary_projection_target(model.surface, p, ni, projection);
                return target ? target->point : p;
            };
            const auto relax_neighborhood = [&](std::uint32_t seed) {
                if (seed >= all_node_cells.size()) {
                    return false;
                }
                std::vector<std::uint32_t> ring;
                std::vector<std::uint32_t> wall;
                for (const auto ci : all_node_cells[seed]) {
                    const auto& cell = fill.cells[ci];
                    std::span<const std::uint32_t> members =
                        cell.kind == mesh::MixedCellKind::kPolyVem
                            ? std::span<const std::uint32_t>(cell.poly_nodes)
                            : std::span<const std::uint32_t>(cell.nodes.data(), cell.n_nodes);
                    for (const auto ni : members) {
                        if (ni == seed || nbrs[ni].empty()) {
                            continue;
                        }
                        (is_snap_node[ni] == 0 ? ring : wall).push_back(ni);
                    }
                }
                const auto dedup = [](std::vector<std::uint32_t>& v) {
                    std::sort(v.begin(), v.end());
                    v.erase(std::unique(v.begin(), v.end()), v.end());
                };
                dedup(ring);
                dedup(wall);
                bool moved_any = false;
                const double cap = 0.25 * h_snap;
                const auto nudge = [&](std::uint32_t ni, bool tangential) {
                    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                    for (const auto other : nbrs[ni]) {
                        centroid += fill.nodes[other];
                    }
                    centroid /= static_cast<double>(nbrs[ni].size());
                    const Eigen::Vector3d saved = fill.nodes[ni];
                    const Eigen::Vector3d step = 0.5 * (centroid - saved);
                    const double len = step.norm();
                    Eigen::Vector3d moved = saved + (len > cap ? step * (cap / len) : step);
                    if (tangential) {
                        // A wall node may only slide ALONG the surface: it is
                        // re-projected through the same owner-aware oracle, so
                        // it stays on its own face/edge and only its spacing
                        // changes. A projection that runs away is abandoned.
                        moved = reproject_node(ni, moved);
                        if ((moved - saved).norm() > cap) {
                            return;
                        }
                    }
                    fill.nodes[ni] = moved;
                    if (any_node_offends(ni)) {
                        fill.nodes[ni] = saved;
                    } else if ((fill.nodes[ni] - saved).squaredNorm() > 0.0) {
                        moved_any = true;
                    }
                };
                for (const auto ni : ring) {
                    nudge(ni, /*tangential=*/false);
                }
                if (!moved_any) {
                    for (const auto ni : wall) {
                        nudge(ni, /*tangential=*/true);
                    }
                }
                return moved_any;
            };
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, bnodes, h_snap, collect_snap_offenders,
                    /*max_move_frac=*/1.25, /*passes=*/8, edges,
                    [&] { mesh::repair_mixed_fan_apices(fill, kMinShape); }, snap_node_offends,
                    /*defer_coupled=*/fill.n_pyramid > 0 || fill.n_tet > 0, projection,
                    relax_neighborhood)
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
            // A boundary node the exact oracle cannot give a target for is not
            // moved AND was not counted, so `snap max|d|` could not see it — the
            // mesher's own fidelity figure was blind to precisely the nodes that
            // failed. Count them, and count the tail left above 0.2 h, and report
            // both. Measured 2026-08-15 on ellipsoid_boss_s1 (h_rel 0.03, hybrid):
            // 23 of 5974 boundary nodes sit 0.30-0.48 h INSIDE the boss, all on
            // the one face OCC represents as a BSplineSurface rather than an
            // analytic sphere (sphere_box, which this code meshes cleanly, gets
            // GeomAbs_Sphere), and refinement does not shrink the fraction.
            std::size_t n_no_target = 0;
            std::size_t n_residual_tail = 0;
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
                    ++n_no_target;
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
                if (resid > 0.2 * h_snap) {
                    ++n_residual_tail;
                }
                max_resid = std::max(max_resid, resid);
            }
            fill.boundary_max_distance = max_resid;
            fill.n_boundary_no_target = n_no_target;
            fill.n_boundary_residual_tail = n_residual_tail;
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
                    final_node_offends, /*defer_coupled=*/true, projection, relax_neighborhood)
                    .max_residual;
            poll_cancel();
            // Hard-pin CAD vertices and sharp edge curves, exactly as the
            // tet/hex/graded fills do. Without this the mixed fill was the one
            // CAD-backed path with no pinning at all: it reached the exact
            // surface but reconstructed a 90° crease as whatever chamfer the
            // lattice happened to cut. Pin, even out the free surface, pin
            // again — smoothing can slide a chain node a little off its curve,
            // and the second pass is what makes the crease exact.
            if (fit != nullptr && fit->can_pin()) {
                std::vector<mesh::BoundarySupport>* pin_provenance =
                    projection != nullptr ? projection->provenance : nullptr;
                mesh::pin_feature_nodes(*fit->cad, *fit->topo, fill.nodes, final_boundary_nodes,
                                        h_snap, final_node_offends, pin_provenance);
                poll_cancel();
                mesh::smooth_boundary_nodes(model.surface, fill.nodes, fill.boundary_quads,
                                            h_snap, collect_final_offenders, /*passes=*/3,
                                            /*relax=*/0.5, /*feature_edges=*/{}, projection);
                poll_cancel();
                const auto pin = mesh::pin_feature_nodes(*fit->cad, *fit->topo, fill.nodes,
                                                         final_boundary_nodes, h_snap,
                                                         final_node_offends, pin_provenance);
                fill.boundary_max_distance =
                    std::max(fill.boundary_max_distance, pin.worst_node_distance);
                poll_cancel();
            }

        }
        // Corner-fold decomposition floor: the same normalized cell-shape floor
        // every mesher gate uses, so "folded" means one thing in this codebase.
        constexpr double kMinShapeConvert = mesh::validity::kCellShapeFloor;
        std::size_t n_pyramid_split_to_tets = 0;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());

        // Which pyramid5 cells ship as their two assembly tets. A base quad is
        // shared by the fans of the two lattice cells across it, so triangulating
        // it on one side only leaves the shared face non-conforming (measured:
        // the boundary-shell guard reports 2152 edges used by three or more
        // faces). The split diagonal depends only on the base quad, so splitting
        // BOTH sides is conforming; the mark therefore propagates from a folded
        // cell to whoever shares its base. Side faces are triangles already, so
        // this closes in one round — no cascade.
        std::vector<char> split_pyramid(fill.cells.size(), 0);
        {
            // Same winding normalization the emission below applies, so the fold
            // is measured on the cell that actually ships (the corner Jacobian
            // is sign-sensitive: an inverted stored winding would otherwise read
            // as folded).
            const auto oriented = [&](const mesh::MixedCell& cell) {
                std::array<std::uint32_t, 5> p{{cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                                cell.nodes[3], cell.nodes[4]}};
                const auto& xa = out.mesh.nodes[p[4]];
                const double vtk_volume =
                    0.5 * (mesh::validity::tet_signed_volume(out.mesh.nodes[p[0]],
                                                             out.mesh.nodes[p[1]],
                                                             out.mesh.nodes[p[2]], xa) +
                           mesh::validity::tet_signed_volume(out.mesh.nodes[p[0]],
                                                             out.mesh.nodes[p[2]],
                                                             out.mesh.nodes[p[3]], xa) +
                           mesh::validity::tet_signed_volume(out.mesh.nodes[p[1]],
                                                             out.mesh.nodes[p[2]],
                                                             out.mesh.nodes[p[3]], xa) +
                           mesh::validity::tet_signed_volume(out.mesh.nodes[p[1]],
                                                             out.mesh.nodes[p[3]],
                                                             out.mesh.nodes[p[0]], xa));
                if (vtk_volume < 0.0) {
                    std::swap(p[1], p[3]);
                }
                return p;
            };
            std::map<std::array<std::uint32_t, 4>, std::vector<std::size_t>> base_owners;
            std::vector<char> folded(fill.cells.size(), 0);
            std::vector<char> splittable(fill.cells.size(), 0);
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                const auto& cell = fill.cells[ci];
                if (cell.kind != mesh::MixedCellKind::kPyramid5) {
                    continue;
                }
                const auto p = oriented(cell);
                std::array<std::uint32_t, 4> key{{p[0], p[1], p[2], p[3]}};
                std::sort(key.begin(), key.end());
                base_owners[key].push_back(ci);
                const auto& x0 = out.mesh.nodes[p[0]];
                const auto& x1 = out.mesh.nodes[p[1]];
                const auto& x2 = out.mesh.nodes[p[2]];
                const auto& x3 = out.mesh.nodes[p[3]];
                const auto& x4 = out.mesh.nodes[p[4]];
                // Splittable = both assembly tets have positive volume. The bar
                // is validity, NOT the shape floor: two positive tets are
                // strictly better than one folded pyramid whatever their aspect,
                // and a floor here refused splits whose tets were no worse than
                // cells the mesh already ships (measured on cylinder_prism.stl
                // h=0.12·extent, where one -0.082 pyramid survived beside
                // shipped 0.005 tets).
                splittable[ci] = static_cast<char>(
                    mesh::validity::pyramid_min_split_volume(x0, x1, x2, x3, x4) > 0.0);
                folded[ci] = static_cast<char>(
                    mesh::validity::pyramid_corner_folded(x0, x1, x2, x3, x4, kMinShapeConvert));
            }
            for (const auto& [key, owners] : base_owners) {
                (void)key;
                const bool any_folded = std::any_of(
                    owners.begin(), owners.end(), [&](std::size_t ci) { return folded[ci] != 0; });
                const bool all_splittable =
                    std::all_of(owners.begin(), owners.end(),
                                [&](std::size_t ci) { return splittable[ci] != 0; });
                if (!any_folded || !all_splittable) {
                    continue; // nothing folded here, or a partner could not be split safely
                }
                for (const auto ci : owners) {
                    split_pyramid[ci] = 1;
                }
            }
        }
        std::size_t conversion_poll = 0;
        for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
            const auto& cell = fill.cells[ci];
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
                // A base corner folded by the boundary snap makes the kPyramid5
                // isoparametric map turn inside out there, even with both
                // assembly split tets healthy — `fea::cell_quality` reports the
                // cell inverted and every consumer that trusts the map is
                // wrong. Ship it as the two tets the assembly would have built
                // from it (`element_stiffness` splits kPyramid5 along exactly
                // this diagonal): identical geometry, identical stiffness,
                // conforming (the diagonal depends only on the shared base
                // quad), and no folded cell leaves the mesher. Unsnapping the
                // wall instead costs real fidelity — measured on icecream_cone
                // h=0.008, exact-BRep p99/h 0.019 → 0.107.
                const int diagonal = mesh::validity::pyramid_split_diagonal(
                    out.mesh.nodes[p[0]], out.mesh.nodes[p[1]], out.mesh.nodes[p[2]],
                    out.mesh.nodes[p[3]]);
                if (split_pyramid[ci] != 0) {
                    const auto emit = [&](std::size_t a, std::size_t b, std::size_t c) {
                        out.mesh.elements.push_back(fea::NodalElement{
                            fea::ElementType::kTet4, {p[a], p[b], p[c], p[4]}});
                    };
                    if (diagonal == 1) {
                        emit(1, 2, 3);
                        emit(1, 3, 0);
                    } else {
                        emit(0, 1, 2);
                        emit(0, 2, 3);
                    }
                    ++n_pyramid_split_to_tets;
                    continue;
                }
                // VTK/PyVista triangulate pyramid5 along local 0-2. Rotate the
                // cyclic base so it is the conformity-safe assembly diagonal.
                if (diagonal == 1) {
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
        if (fill.n_boundary_no_target > 0 || fill.n_boundary_residual_tail > 0) {
            out.mesher_note += std::format(
                " | boundary tail: {} node(s) with no exact CAD target, {} left >0.2 h off",
                fill.n_boundary_no_target, fill.n_boundary_residual_tail);
        }
        if (n_pyramid_split_to_tets > 0) {
            out.mesher_note += std::format(
                " | {} corner-folded pyramid5 shipped as their 2 assembly tets",
                n_pyramid_split_to_tets);
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
        auto fill = mesh::hex_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                           /*snap_boundary=*/true, fit);
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
        double graded_h = h;
        const double curved_area_fraction =
            cad_curved_area_fraction(cad_topology ? cad_topology.get() : nullptr);
        if (curved_area_fraction >= kCurvedAreaLatticeFraction) {
            graded_h = kCurvedLatticeScale * h;
        }
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
                feature_band = 2.0 * graded_h;
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
                if (thick.thickness[i] < 2.5 * graded_h) {
                    thin_pts.push_back(model.surface.vertices[i]);
                }
            }
            const bool globally_thin =
                n_finite > 0 &&
                thin_pts.size() * 3 > model.surface.vertices.size(); // > ~1/3 thin
            if (!globally_thin && !thin_pts.empty()) {
                if (band <= 0.0) {
                    band = 1.6 * graded_h;
                }
                // Spatial thinning: min sep 0.75 h, capped, caller seeds first.
                constexpr std::size_t kMaxGeoSeeds = 256;
                const double min_sep2 =
                    (0.75 * graded_h) * (0.75 * graded_h);
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
            band = 1.6 * graded_h;
        }
        auto graded = mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, graded_h,
            std::max(1, skin_layers), edges, feature_band, seeds, band, turn_deg,
            fit, size_field, mirror);
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
            (graded.h_fine >
             (graded_h / static_cast<double>(graded.subdivision)) * 1.05)
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
        if (graded_h < h) {
            out.mesher_note += std::format(
                " | curved-area={:.1f}% accuracy lattice h={:.4g} m "
                "(0.5x requested)",
                100.0 * curved_area_fraction, graded_h);
        }
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
        out.mesher_note += mirror_note();
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
                                       /*snap_boundary=*/true, fit, mirror);
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
        auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                           /*snap_boundary=*/true, fit, mirror);
        const auto owner_name = [](mesh::BoundarySupportKind k) {
            switch (k) {
            case mesh::BoundarySupportKind::kCadVertex:
                return "cad_vertex";
            case mesh::BoundarySupportKind::kCadEdge:
                return "cad_edge";
            case mesh::BoundarySupportKind::kCadFace:
                return "cad_face";
            case mesh::BoundarySupportKind::kUnknown:
                break;
            }
            return "unknown";
        };
        const std::string conformity_note = std::format(
            " | conformity snap_moved={} unsnapped={} relax_rescued={} "
            "pin_edge={} pin_vertex={} pin_chains={} pin_rejected={} pin_res={:.3g} m "
            "worst_node={} d={:.3g} m owner={}",
            fill.snap.n_moved, fill.snap.n_unsnapped, fill.snap.n_relax_rescued,
            fill.pin.edge_pinned, fill.pin.vertex_pinned, fill.pin.chains,
            fill.pin.rejected, fill.pin.max_edge_residual, fill.pin.worst_node,
            fill.pin.worst_node_distance, owner_name(fill.pin.worst_node_owner));
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
        out.mesher_note += conformity_note;
        out.mesher_note += mirror_note();
    }

    // Prefer true element exterior faces for display/region skin so tet/prism
    // previews show element triangles (incl. Kuhn diagonals), not only lattice quads.
    {
        auto exterior = fea::extract_boundary_faces(out.mesh);
        if (!exterior.empty()) {
            out.boundary_quads = std::move(exterior);
        }
    }

    // Exterior conformity gate (ADR-0035): the snap ran on each mesher's own
    // lattice skin; the mesh ships the true element exterior extracted just
    // above. Conform THAT set, so a node exposed by a late peel/split is not
    // shipped at its raw lattice site.
    if (projection != nullptr) {
        const auto ext = conform_true_exterior(out.mesh, out.boundary_quads, projection, fit,
                                               fill_h > 0.0 ? fill_h : h,
                                               mesh::validity::kCellShapeFloor, mirror);
        if (ext.n_candidates > 0 || ext.n_edge_pinned > 0 ||
            ext.n_kink_relieved > 0) {
            out.mesher_note += std::format(
                " | exterior_gate cand={} moved={} relax_rescued={} hex_fanned={} left={} "
                "worst_node={} worst_xyz=({:.6g},{:.6g},{:.6g}) "
                "worst|d|={:.3g} m kink_relieved={} edge_pinned={} connected={} [{}] "
                "edge_pass={:.0f} ms chains={} pin_rejected={}{}",
                ext.n_candidates, ext.n_moved, ext.n_relax_rescued, ext.n_hex_fanned,
                ext.n_left, ext.worst_node, ext.worst_position.x(),
                ext.worst_position.y(), ext.worst_position.z(), ext.worst_residual,
                ext.n_kink_relieved, ext.n_edge_pinned, ext.n_connected_edges,
                ext.connected_edge_census, ext.edge_pass_ms, ext.n_edge_chains,
                ext.n_pin_rejected,
                ext.reverted ? " REVERTED" : "");
        }
    }

    // Ship gate (ADR-0035): the cells that leave this function are measured
    // with the product's own `fea::cell_quality`, not with whatever predicate
    // each mesher used internally over its own intermediate zoo. Cells under
    // the floor get interior room; whatever is left is reported, never hidden.
    {
        const std::size_t below_floor = relax_cells_below_shape_floor(
            out.mesh, out.boundary_quads, mesh::validity::kCellShapeFloor);
        out.n_cells_below_shape_floor = below_floor;
        if (below_floor > 0) {
            out.mesher_note +=
                std::format(" | ship_gate {} cells below shape floor {:.3g}", below_floor,
                            mesh::validity::kCellShapeFloor);
        }
        // Integrability is a separate question from shape, and it is the one the
        // solver actually asks: `element_stiffness` refuses any element with a
        // non-positive Jacobian at a quadrature point. Counting it here means a
        // mesh that would abort the solve says so in its own note instead of
        // failing later with a bare error.
        std::size_t nonintegrable = 0;
        for (const auto& element : out.mesh.elements) {
            if (!fea::element_jacobians_positive(out.mesh, element)) {
                ++nonintegrable;
            }
        }
        if (nonintegrable > 0) {
            out.mesher_note +=
                std::format(" | ship_gate {} non-integrable cells (det J <= 0 at a quadrature "
                            "point)",
                            nonintegrable);
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
    // A volume check cannot see a duplicated boundary skin: two coincident
    // patches with opposite orientation cancel in the divergence sum, so the
    // mesh measures right and is still torn. Measured on graded_tet, four of
    // the seven meshes carrying real non-manifold edges reported a "clean"
    // volume band -- box_hole at h=0.00278 sits at rel_err 2.4e-4 with three
    // multiplicity-4 edges on the bore. The shell is the thing the solver's
    // traction integral and the renderer both consume, so check the shell.
    {
        const auto shell = boundary_shell_topology(out.boundary_quads);
        out.mesher_note +=
            std::format(" | boundary_shell edges={} open={} nonmanifold={}",
                        shell.n_edges, shell.n_open, shell.n_nonmanifold);
        if (shell.n_open > 0 || shell.n_nonmanifold > 0) {
            throw GeometryVolumeLimitError(
                std::format(
                    "geometry fill-stage guard failed at h={:.6g} m (requested -h {:.6g} m): "
                    "the boundary is not a closed surface -- {} edge(s) are used by one face "
                    "(a hole) and {} by three or more (two skins in the same place), out of "
                    "{}. A volume check cannot catch this, because coincident skins cancel: "
                    "this mesh's volume error is {:.4g}. This is a mesher defect, not a "
                    "resolution one, and refining does not clear it. Retry with the default "
                    "--mesher hybrid, which is watertight on every part measured | {}",
                    fill_h, h, shell.n_open, shell.n_nonmanifold, shell.n_edges,
                    out.fill_geometry_volume.available
                        ? out.fill_geometry_volume.relative_error
                        : std::numeric_limits<double>::quiet_NaN(),
                    out.mesher_note),
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
    out.geometry_h = fill_h > 0.0 ? fill_h : h;
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
            const bool curved_geometry =
                setup.p_elevate && model.cad && !model.cad->empty();
            std::optional<geom::CadTopology> auto_topology;
            if (curved_geometry) {
                try {
                    auto_topology = geom::extract_topology(*model.cad);
                } catch (...) {
                    auto_topology.reset();
                }
            }
            const auto resolved = resolve_mesh_size(
                model, setup.mesh_size, 30.0, setup.max_elems, setup.max_dof,
                curved_geometry, auto_topology ? &*auto_topology : nullptr);
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
            if (setup.p_elevate) {
                auto curved = curve_volume_geometry(model, mesh_only_.mesh, h);
                mesh_only_.mesh = std::move(curved.mesh);
                mesh_only_.boundary_quads = fea::extract_boundary_faces(mesh_only_.mesh);
                mesh_only_.mesher_note += std::format(
                    " | curved_volume promoted={} pyramid_split={} projected={} "
                    "partial={} reverted={}",
                    curved.n_promoted, curved.n_pyramids_split, curved.n_projected,
                    curved.n_partial, curved.n_reverted);
            }
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
            const bool curved_geometry =
                setup.p_elevate && model.cad && !model.cad->empty();
            std::optional<geom::CadTopology> auto_topology;
            if (curved_geometry) {
                try {
                    auto_topology = geom::extract_topology(*model.cad);
                } catch (...) {
                    auto_topology.reset();
                }
            }
            const auto resolved = resolve_mesh_size(
                model, setup.mesh_size, 30.0, setup.max_elems, setup.max_dof,
                curved_geometry, auto_topology ? &*auto_topology : nullptr);
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
            SpectralSizingReport spectral_report;
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
            // The geometry-only subset is ALSO kept as its own field: it is
            // the coarsen gate's demand floor (ADR-0034). A-priori BC seeds
            // are heuristic demands that a-posteriori evidence may retire;
            // curvature / thin-wall demand may never be coarsened through.
            std::vector<adapt::SizeSource> geo_only;
            if (setup.use_feature_grading) {
                auto geo = adapt::geometry_size_sources(model.surface, 0.15 * h, h);
                geo = decimate_sources(std::move(geo), 0.5 * h);
                if (setup.spectral_smooth) {
                    const geom::CadTopology topo = cad_sizing_topology(model);
                    auto edge =
                        spectral_edge_sources(topo, 0.15 * h, h, spectral_report);
                    edge = decimate_sources(std::move(edge), 0.5 * h);
                    spectral_report.n_edge_curve_seeds = edge.size();
                    geo.insert(geo.end(), edge.begin(), edge.end());
                }
                geo_only = geo;
                src.insert(src.end(), geo.begin(), geo.end());
            }
            mesh::SizeFieldFn geo_size_field; // geometry-only demand field
            if (!geo_only.empty()) {
                const auto geo_sp = adapt::seed_plan(geo_only, h, 1.5);
                geo_size_field = adapt::size_field_from_sources(geo_only, geo_sp.h_fine,
                                                                h, /*beta=*/1.0);
            }
            if (!src.empty()) {
                const auto plan = adapt::seed_plan(src, h, /*band_frac=*/1.5);
                adapt_size_field =
                    adapt::size_field_from_sources(src, plan.h_fine, h, /*beta=*/1.0);
                adapt_seeds = plan.refine_seeds;
                adapt_seed_band = plan.seed_band;
                if (setup.spectral_smooth && adapt_size_field) {
                    // Trim only: the element ceiling stays with the measured
                    // resolve + auto-retry path (see build_refinement_plan).
                    adapt_size_field = apply_spectral_sizing(
                        model, adapt_size_field, geo_size_field, plan.h_fine,
                        /*budget=*/0, spectral_report);
                    if (spectral_report.applied) {
                        std::vector<Eigen::Vector3d> kept;
                        kept.reserve(adapt_seeds.size());
                        for (const auto& seed : adapt_seeds) {
                            if (adapt_size_field(seed) < 0.75 * h) {
                                kept.push_back(seed);
                            }
                        }
                        adapt_seeds = std::move(kept);
                    }
                }
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
            if (spectral_report.applied) {
                vol.mesher_note += std::format(
                    " | spectral {}/{} modes ({:.1f}% energy), N_pred {:.4g}→{:.4g}{}",
                    spectral_report.modes_kept, spectral_report.modes_total,
                    100.0 * spectral_report.energy_kept,
                    spectral_report.predicted_before, spectral_report.predicted_after,
                    spectral_report.budget_met
                        ? ""
                        : std::format(" (budget {} not met — geometry floor binds)",
                                      resolved.element_ceiling));
            }
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
            std::shared_ptr<const geom::CadTopology> solve_topology;
            if (model.cad &&
                make_boundary_projection(*model.cad, h, &solve_projection_context,
                                         &solve_boundary_provenance, &solve_topology)) {
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

            // Exact CAD-face membership for BC application. The legacy
            // one-region-per-node map (nearest display-triangle roulette)
            // hands a face's perimeter ring to whichever adjacent face wins
            // the coin flip — measured on plate_hole at h = 6 mm, a "fixed"
            // end face kept only 405 of its 657 nodes; the free rim ring
            // speckled the clamped-end stress (0.05…1.83 MPa where the smooth
            // reference is 0.28…0.54 MPa) and broke the part's mirror symmetry
            // (mirror-pair von Mises gap to 1.33 MPa on a symmetric tension
            // case). A node belongs to a CAD face when its exact BRep owner IS
            // the face, or is an edge/vertex bordering it — fixtures then cover
            // the perimeter ring and load facet sets close exactly. Owners are
            // classified fresh on the mesh in hand (never carried across a
            // remesh, where node ids are reused for different positions).
            std::map<int, std::vector<std::uint32_t>> exact_region_nodes;
            auto build_exact_membership = [&]() {
                exact_region_nodes.clear();
                if (setup.boundary_builder || solve_projection == nullptr ||
                    solve_topology == nullptr) {
                    return;
                }
                std::set<int> wanted;
                for (const int region : setup.fixtures) {
                    wanted.insert(region);
                }
                for (const auto& [region, load] : setup.loads) {
                    (void)load;
                    wanted.insert(region);
                }
                // Per-region border sets: the exact face ids, the edges
                // bounding them, and the vertices closing those edges.
                std::map<int, std::set<std::uint32_t>> region_faces;
                std::map<int, std::set<std::uint32_t>> region_edges;
                std::map<int, std::set<std::uint32_t>> region_vertices;
                for (const int region : wanted) {
                    const auto faces_it = cad_faces_by_region.find(region);
                    if (faces_it == cad_faces_by_region.end() || faces_it->second.empty()) {
                        continue; // no exact mapping — legacy roulette keeps it
                    }
                    auto& faces = region_faces[region];
                    auto& edges = region_edges[region];
                    auto& vertices = region_vertices[region];
                    for (const auto face_id : faces_it->second) {
                        faces.insert(face_id);
                        const auto face_it =
                            std::find_if(solve_topology->faces.begin(),
                                         solve_topology->faces.end(),
                                         [&](const geom::CadFace& face) {
                                             return face.id == face_id;
                                         });
                        if (face_it == solve_topology->faces.end()) {
                            continue;
                        }
                        for (const auto edge_id : face_it->edge_ids) {
                            edges.insert(edge_id);
                            if (edge_id < solve_topology->edges.size()) {
                                vertices.insert(solve_topology->edges[edge_id].v0);
                                vertices.insert(solve_topology->edges[edge_id].v1);
                            }
                        }
                    }
                }
                if (region_faces.empty()) {
                    return;
                }
                const auto all = fea::boundary_surface_faces(vol.mesh);
                std::set<std::uint32_t> boundary_nodes;
                for (const auto& face : all) {
                    boundary_nodes.insert(face.nodes.begin(), face.nodes.end());
                }
                std::vector<mesh::BoundarySupport> owners(vol.mesh.nodes.size());
                for (const auto node : boundary_nodes) {
                    auto& owner = owners[node];
                    (void)solve_projection->target(vol.mesh.nodes[node], owner);
                }
                for (const auto& [region, faces] : region_faces) {
                    const auto& edges = region_edges[region];
                    const auto& vertices = region_vertices[region];
                    std::set<std::uint32_t> members;
                    for (const auto node : boundary_nodes) {
                        const auto& owner = owners[node];
                        const bool member =
                            (owner.kind == mesh::BoundarySupportKind::kCadFace &&
                             faces.contains(owner.id)) ||
                            (owner.kind == mesh::BoundarySupportKind::kCadEdge &&
                             edges.contains(owner.id)) ||
                            (owner.kind == mesh::BoundarySupportKind::kCadVertex &&
                             vertices.contains(owner.id));
                        if (member) {
                            members.insert(node);
                        }
                    }
                    // A node the oracle cannot classify keeps whatever the
                    // roulette gave it, so exact coverage is never narrower
                    // than the legacy path it replaces.
                    for (const auto& [node, region_id] : vol.boundary_node_region) {
                        if (region_id == region && boundary_nodes.contains(node) &&
                            owners[node].kind == mesh::BoundarySupportKind::kUnknown) {
                            members.insert(node);
                        }
                    }
                    // A region the oracle resolves to nothing keeps its
                    // roulette set: exact coverage may supersede, never shrink.
                    if (!members.empty()) {
                        exact_region_nodes[region] =
                            std::vector<std::uint32_t>(members.begin(), members.end());
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
                // Exact BRep membership supersedes the roulette where resolved.
                for (const auto& [region, exact] : exact_region_nodes) {
                    region_nodes[region] = exact;
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
                build_exact_membership();
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
            const bool do_p_elevate = setup.p_elevate;
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
                // Exact per-element sizes (cube-root volume): the global
                // h_use is a stale proxy after any local refinement, and the
                // coarsen gate compares element size against the a-priori
                // demand — it only functions with measured sizes.
                std::vector<double> h_loc(n, h_use);
                std::vector<double> kappa(n, 0.0);
                std::vector<double> thick(n, 0.0);
                std::vector<int> p_ord(n, 1);
                // A-priori size demand per element (ADR-0034 coarsen gate):
                // the fused geometry+BC field at the centroid. Coarsening
                // reverts a-posteriori over-refinement (LEB children, seed
                // balls, the global-h ratchet) back to this demand — never
                // below it, so curvature / thin-wall / BC bands are
                // structurally protected. Where no field exists the demand is
                // the bulk h itself (flat geometry tolerates it).
                std::vector<double> h_geo(n, h);
                const bool have_h_geo = static_cast<bool>(adapt_size_field);
                for (std::size_t e = 0; e < n && e < vol.mesh.elements.size(); ++e) {
                    const auto& el = vol.mesh.elements[e];
                    const double velem = fea::element_volume(vol.mesh, el);
                    if (velem > 0.0 && std::isfinite(velem)) {
                        h_loc[e] = std::cbrt(velem);
                    }
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
                    if (have_h_geo && e < cents.size()) {
                        const double g = adapt_size_field(cents[e]);
                        if (g > 0.0 && std::isfinite(g)) {
                            h_geo[e] = g;
                        }
                    }
                }
                // Empty surplus → driver estimates from ZZ ranking.
                return adapt::make_hp_signals(h_loc, kappa, thick, element_eta, {}, p_ord, {},
                                              {}, {}, hp_policy, h_geo);
            };

            auto maybe_p_elevate = [&](const std::vector<std::size_t>&,
                                       std::string& note_suffix) {
                if (!do_p_elevate) {
                    return false;
                }
                const std::size_t before = vol.mesh.nodes.size();
                auto curved = curve_volume_geometry(model, vol.mesh, h_use);
                const bool changed =
                    curved.n_promoted > 0 || curved.n_pyramids_split > 0;
                p_constraints = std::move(curved.constraints);
                vol.mesh = std::move(curved.mesh);
                vol.boundary_quads = fea::extract_boundary_faces(vol.mesh);
                extend_boundary_regions();
                apply_bcs();
                remove_slave_dirichlet();
                const auto counts = fea::count_element_types(vol.mesh);
                note_suffix = std::format(
                    " curved-volume={} pyramid-split={} n+{} constrained-mid={} "
                    "(tet10={} hex20={} projected={} partial={} reverted={})",
                    curved.n_promoted, curved.n_pyramids_split,
                    vol.mesh.nodes.size() - before,
                    p_constraints.size() / 3, counts.tet10, counts.hex20,
                    curved.n_projected, curved.n_partial, curved.n_reverted);
                vol.mesher_note += note_suffix;
                report("recover", 0.5,
                       std::format("curving authoritative volume… ({} promoted)",
                                   curved.n_promoted),
                       /*pass=*/0, pass_count);
                return changed;
            };

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
            // Coarsen passes (ADR-0034) may raise the global h suggestion, but
            // never past the resolved a-priori size the user/campaign asked
            // for — derefinement reverts toward the baseline, not beyond it.
            const double h_adapt_ceiling = h;

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
                const auto hp_plan =
                    adapt::drive_hp(signals, hp_policy, cents, h_use, h_adapt_ceiling);
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
                        r.mesh_note = std::format("{} | {} | {}{}", vol.mesher_note,
                                                  hp_note, reason, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        fill_result_fields(r, zz_try, u_try);
                        r.fill_geometry_volume = vol.fill_geometry_volume;
                        r.solved_geometry_volume = vol.solved_geometry_volume;

                        result_ = std::move(r);
                        break;
                    }
                    const auto& sug = hp_plan.h_suggestion;
                    // Early stop only when neither h nor p wants work — and no
                    // coarsen pass is pending (a coarsen remesh must run, or
                    // over-resolved regions would stay fine forever).
                    if (sug.n_marked == 0 && sug.h_next >= h_use * 0.98 &&
                        hp_plan.p_mark.empty() && hp_plan.coarsen_mark.empty()) {
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
                    h_use = std::max(sug.h_next, h_adapt_floor);
                    adapt_seeds = sug.refine_seeds;
                    adapt_seed_band = sug.seed_band;
                    if (!hp_plan.h_mark.empty()) {
                        adapt_marked = hp_plan.h_mark;
                    } else if (!hp_plan.coarsen_mark.empty()) {
                        // Coarsen pass: LEB can only refine, so suppress the
                        // Dörfler fallback — the remesh path must run and it
                        // reverts unseeded regions to base + geometry sizing.
                        adapt_marked.clear();
                    } else {
                        adapt_marked = adapt::dorfler_mark(zz_try.element_eta, 0.3);
                    }
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
