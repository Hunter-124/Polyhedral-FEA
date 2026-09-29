// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"
#include "scene_internal.hpp"

#include "adapt/graded_sizing.hpp"
#include "adapt/spectral_sizing.hpp"
#include "geom/cad_topology.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {

/// Spectral truncation keeps the modes carrying this fraction of the spectral
/// energy; the remainder (noise, sub-seed oscillation) is merged into the
/// surrounding field. 0.995 is aggressive enough to trim isolated seed
/// artifacts and conservative enough that any spatially extended feature
/// survives verbatim.
constexpr double kSpectralEnergyFraction = 0.995;

/// Chordal sagitta rule numerator. A segment of length ℓ = c/κ on geometry of
/// curvature κ = 1/R has sagitta d = ℓ²κ/8 = c²R/8, i.e. a *relative* sag
/// d/R = c²/8 that is independent of R. c = 0.25 sets that at 0.78% of the
/// local radius of curvature, the value the surface-vertex rule
/// (adapt::curvature_size_sources' curvature_fraction) uses; edge
/// and face sizing share it so a curved edge and the curved face it bounds ask
/// for the same size instead of fighting at their shared boundary.
constexpr double kCurvatureSagittaFraction = 0.25;

/// uv / arc-length sampling density for the exact-BRep sizing reads. 32 gives
/// 34 stations per curve and a 32×32 uv grid per non-planar face — enough that
/// the FFT edge denoise has a usable spectrum, and cheap because
/// extract_topology skips the grid on planar faces entirely.
constexpr int kCadSizingSamples = 32;

/// Curvature size sources read from the **exact BRep faces**: the same
/// constant-relative-sag rule h = c/κ as spectral_edge_sources, applied to
/// `geom::CadFace::kappa_samples` (max |principal curvature| on a uv grid
/// inside the trim) instead of to a discrete per-vertex estimate on the
/// triangulation. OCC's tessellation of a mirror-symmetric part is not
/// mirror-symmetric, so only the exact read gives a mirror-symmetric size
/// field (ADR-0036 §6).
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

} // namespace

namespace detail {

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
        const Key k{static_cast<long>(std::floor(d.x())), static_cast<long>(std::floor(d.y())),
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
    std::sort(out.begin(), out.end(),
              [](const adapt::SizeSource& a, const adapt::SizeSource& b) {
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
        const auto smooth = adapt::spectral::lowpass_signal(
            stations, kappa, kSpectralEnergyFraction, &edge_report);
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

mesh::SizeFieldFn apply_spectral_sizing(const Model& model, const mesh::SizeFieldFn& field,
                                        const mesh::SizeFieldFn& geo_field, double h_fine,
                                        std::size_t budget, SpectralSizingReport& report) {
    if (!field || !(h_fine > 0.0)) {
        return field;
    }
    const double target_spacing = 0.5 * h_fine;
    adapt::spectral::Grid3d grid = adapt::spectral::sample_field_grid(
        field, model.bbox_min, model.bbox_max, target_spacing);
    report.predicted_before = adapt::spectral::predict_element_count(grid);
    const double h_entry_min = grid.min_value();
    const double h_entry_max = grid.max_value();

    const auto filter = adapt::spectral::lowpass_grid_energy(grid, kSpectralEnergyFraction);
    report.modes_total = filter.modes_total;
    report.modes_kept = filter.modes_kept;
    report.energy_kept =
        filter.energy_total > 0.0 ? filter.energy_kept / filter.energy_total : 1.0;

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
            report.h_scale = std::cbrt(predicted / static_cast<double>(budget));
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

} // namespace detail

RefinementPlan build_refinement_plan(const Model& model, double h_coarse,
                                     std::span<const RefineRegion> regions, bool use_geometry,
                                     bool spectral, std::size_t spectral_budget) {
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
        const geom::CadTopology topo = detail::cad_sizing_topology(model);

        std::vector<adapt::SizeSource> geo;
        if (!topo.faces.empty()) {
            // Exact BRep available: curvature comes from the analytic faces, so
            // the sizing plan does not inherit the tessellation's broken
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
        geo = detail::decimate_sources(std::move(geo), 0.5 * h_coarse);
        if (spectral) {
            // FFT-denoised CAD-edge chordal sources join the geometry set.
            auto edge =
                detail::spectral_edge_sources(topo, h_min_geo, h_coarse, plan.spectral);
            edge = detail::decimate_sources(std::move(edge), 0.5 * h_coarse);
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
            geo_field = adapt::size_field_from_sources(geo_only, geo_sp.h_fine, h_coarse,
                                                       /*beta=*/1.0);
        }
        // Budget scale is deliberately NOT driven from the element ceiling
        // here: the lattice meshers' real element counts diverge from the
        // Σvol/h³ density model by part-dependent factors (fine bands, skin
        // cells), so the pre-flight resolve + measured auto-retry remain the
        // cap authority. The spectral budget API serves callers whose mesher
        // honors the CVT density contract directly (ADR-0024 Q10 #4).
        plan.size_field = detail::apply_spectral_sizing(
            model, plan.size_field, geo_field, sp.h_fine, spectral_budget, plan.spectral);
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

} // namespace polymesh::pipeline
