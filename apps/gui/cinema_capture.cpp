// SPDX-License-Identifier: BSD-3-Clause
// Cinema capture: skeleton, sizing/spectral story and advisor explanation,
// computed once per take before the film plays.
#include "cinema.hpp"
#include "cinema_internal.hpp"

#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/signal_fft.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace polymesh::gui {

using detail::mesher_plain;

namespace {

/// Axis-aligned box of one tessellation region, in model coordinates (metres).
///
/// `pipeline::extract_case_features` takes fix/load context as
/// `RefineRegion` boxes, which is what the CLI's `--fix-box` / `--load-box`
/// supply by hand. The studio selects boundary conditions by face id instead,
/// so the box a GUI face contributes is the box of that face's own triangles.
/// Returns nullopt for a region with no triangles, so an empty selection
/// contributes nothing rather than a degenerate box at the origin.
std::optional<std::pair<Eigen::Vector3d, Eigen::Vector3d>>
region_box(const pipeline::Model& model, int region) {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
    bool any = false;
    const auto& tris = model.surface.triangles;
    for (std::size_t ti = 0; ti < tris.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        for (const std::uint32_t vi : tris[ti]) {
            const Eigen::Vector3d& p = model.surface.vertices[vi];
            if (any) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            } else {
                lo = p;
                hi = p;
                any = true;
            }
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return std::make_pair(lo, hi);
}

/// Area-weighted centroid of the exact tessellated region selected by the GUI.
/// Unlike the region AABB centre, this point lies on the selected surface in
/// the average-of-area sense and remains stable as tessellation density changes.
std::optional<Eigen::Vector3d> region_centroid(const pipeline::Model& model, int region) {
    Eigen::Vector3d weighted = Eigen::Vector3d::Zero();
    double area_sum = 0.0;
    for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        const auto& tri = model.surface.triangles[ti];
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
        const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
        const double area = 0.5 * (b - a).cross(c - a).norm();
        if (!(area > 0.0)) {
            continue;
        }
        weighted += area * (a + b + c) / 3.0;
        area_sum += area;
    }
    if (!(area_sum > 0.0)) {
        return std::nullopt;
    }
    return weighted / area_sum;
}

std::vector<Eigen::Vector3d> region_surface_points(const pipeline::Model& model, int region) {
    std::vector<Eigen::Vector3d> points;
    std::vector<std::uint8_t> used(model.surface.vertices.size(), 0);
    for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        for (const std::uint32_t node : model.surface.triangles[ti]) {
            if (node < model.surface.vertices.size() && used[node] == 0) {
                used[node] = 1;
                points.push_back(model.surface.vertices[node]);
            }
        }
    }
    return points;
}

/// Direction the closing act's field reveals travel in, and one sentence saying
/// how it was chosen.
///
/// The front starts at the loaded end: the axis is the resultant force
/// direction, signed by comparing the loaded regions' mean box centre with the
/// part's bounding-box midpoint (two measured numbers, no assumption about
/// which face is loaded). With no load the longest bounding-box axis is used
/// and the caption says it is a camera move, not physics.
std::pair<Eigen::Vector3f, std::string> resolve_sweep_axis(const pipeline::Model& model,
                                                           const pipeline::SimSetup& setup) {
    Eigen::Vector3d force = Eigen::Vector3d::Zero();
    Eigen::Vector3d load_centre = Eigen::Vector3d::Zero();
    int load_boxes = 0;
    for (const auto& [face, load] : setup.loads) {
        force += load.force;
        if (const auto box = region_box(model, face)) {
            load_centre += 0.5 * (box->first + box->second);
            ++load_boxes;
        }
    }
    const Eigen::Vector3d span = model.bbox_max - model.bbox_min;
    if (force.norm() <= 0.0 || load_boxes == 0) {
        int longest = 0;
        for (int i = 1; i < 3; ++i) {
            if (span[i] > span[longest]) {
                longest = i;
            }
        }
        Eigen::Vector3f axis = Eigen::Vector3f::Zero();
        axis[longest] = 1.0f;
        return {axis, "this take has no load case, so the reveal runs along the part's "
                      "longest axis — a camera move, not a direction of anything physical"};
    }
    Eigen::Vector3d axis = force.normalized();
    load_centre /= static_cast<double>(load_boxes);
    // Flip so that the loaded end is at the LOW end of the projection, which is
    // where the viewport's own sweep front starts.
    const double mid = 0.5 * axis.dot(model.bbox_min + model.bbox_max);
    if (axis.dot(load_centre) > mid) {
        axis = -axis;
    }
    return {axis.cast<float>(),
            "the reveal travels from the loaded faces toward the far end, along the "
            "resultant of every SimSetup::LoadSpec::force in this take"};
}

void capture_curve_spectrum(CinemaSizingStory& sizing) {
    sizing.curve_spectrum.clear();
    sizing.curve_mode_kept.clear();
    if (sizing.stations.size() < 3 || sizing.stations.size() != sizing.curvature_raw.size()) {
        return;
    }

    std::size_t n = 8;
    while (n < sizing.curvature_raw.size()) {
        n <<= 1;
    }
    const auto interpolate = [&](double station) {
        const auto it =
            std::upper_bound(sizing.stations.begin(), sizing.stations.end(), station);
        if (it == sizing.stations.begin()) {
            return sizing.curvature_raw.front();
        }
        if (it == sizing.stations.end()) {
            return sizing.curvature_raw.back();
        }
        const std::size_t hi = static_cast<std::size_t>(it - sizing.stations.begin());
        const std::size_t lo = hi - 1;
        const double t =
            (station - sizing.stations[lo]) / (sizing.stations[hi] - sizing.stations[lo]);
        return sizing.curvature_raw[lo] +
               t * (sizing.curvature_raw[hi] - sizing.curvature_raw[lo]);
    };

    // This is the exact even-reflection preparation used by
    // geom::lowpass_signal before its FFT.
    std::vector<std::complex<double>> spectrum(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        const double station = static_cast<double>(i) / static_cast<double>(n - 1);
        const std::complex<double> value{interpolate(station), 0.0};
        spectrum[i] = value;
        spectrum[2 * n - 1 - i] = value;
    }
    geom::fft_inplace(spectrum, false);
    std::vector<std::complex<double>> kept = spectrum;
    (void)geom::truncate_modes(kept, 0.995);

    // A real input has a conjugate-symmetric spectrum. Plot one half, including
    // DC and Nyquist, so frequency increases monotonically across the panel
    // instead of showing the mirrored half twice.
    const std::size_t visible_modes = n + 1;
    sizing.curve_spectrum.reserve(visible_modes);
    sizing.curve_mode_kept.reserve(visible_modes);
    for (std::size_t k = 0; k < visible_modes; ++k) {
        sizing.curve_spectrum.push_back(std::abs(spectrum[k]));
        sizing.curve_mode_kept.push_back(
            static_cast<std::uint8_t>(k == 0 || std::norm(kept[k]) > 0.0));
    }
}

void capture_curve_story(CinemaState& state, const geom::CadTopology& topology) {
    state.sizing.edge_id = 0;
    state.sizing.edge_length = 0.0;
    state.sizing.curve_modes_total = 0;
    state.sizing.curve_modes_kept = 0;
    state.sizing.curve_energy_fraction = 0.0;
    state.sizing.stations.clear();
    state.sizing.curvature_raw.clear();
    state.sizing.curvature_filtered.clear();
    state.sizing.curve_spectrum.clear();
    state.sizing.curve_mode_kept.clear();
    state.sizing.curve_points.clear();
    state.sizing.network_points.clear();
    state.sizing.network_curvature_raw.clear();
    state.sizing.network_curvature_filtered.clear();
    double best_score = -1.0;
    for (const auto& edge : topology.edges) {
        if (edge.samples.size() < 3 || edge.kappa_samples.size() != edge.samples.size()) {
            continue;
        }
        std::vector<double> stations(edge.samples.size(), 0.0);
        for (std::size_t i = 1; i < edge.samples.size(); ++i) {
            stations[i] = stations[i - 1] + (edge.samples[i] - edge.samples[i - 1]).norm();
        }
        if (!(stations.back() > 0.0)) {
            continue;
        }
        for (double& station : stations) {
            station /= stations.back();
        }
        geom::FilterReport report;
        std::vector<double> filtered =
            geom::lowpass_signal(stations, edge.kappa_samples, 0.995, &report);
        if (filtered.size() != edge.kappa_samples.size()) {
            continue;
        }
        state.sizing.network_points.insert(state.sizing.network_points.end(),
                                           edge.samples.begin(), edge.samples.end());
        state.sizing.network_curvature_raw.insert(state.sizing.network_curvature_raw.end(),
                                                  edge.kappa_samples.begin(),
                                                  edge.kappa_samples.end());
        state.sizing.network_curvature_filtered.insert(
            state.sizing.network_curvature_filtered.end(), filtered.begin(), filtered.end());
        const auto [lo, hi] =
            std::minmax_element(edge.kappa_samples.begin(), edge.kappa_samples.end());
        double mean = 0.0;
        for (const double kappa : edge.kappa_samples) {
            mean += std::fabs(kappa);
        }
        mean /= static_cast<double>(edge.kappa_samples.size());
        // Prefer a genuinely varying curvature trace; when every curved edge is
        // analytic-constant, still choose the strongest real curve rather than
        // drawing an invented spectrum.
        const double score = edge.length * (std::fabs(*hi - *lo) + 0.05 * mean);
        if (score <= best_score) {
            continue;
        }
        best_score = score;
        state.sizing.edge_id = edge.id;
        state.sizing.edge_length = edge.length;
        state.sizing.curve_modes_total = report.modes_total;
        state.sizing.curve_modes_kept = report.modes_kept;
        state.sizing.curve_energy_fraction =
            report.energy_total > 0.0 ? report.energy_kept / report.energy_total : 1.0;
        state.sizing.stations = std::move(stations);
        state.sizing.curvature_raw = edge.kappa_samples;
        state.sizing.curvature_filtered = std::move(filtered);
        state.sizing.curve_points = edge.samples;
    }
    capture_curve_spectrum(state.sizing);
}
} // namespace

void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model,
                           const pipeline::SimSetup& setup, Viewport& viewport) {
    std::vector<std::vector<Eigen::Vector3d>> polylines;
    state.skeleton_note.clear();
    state.skeleton_source = SkeletonSource::kNone;
    state.sizing = CinemaSizingStory{};
    state.subject_center = 0.5 * (model.bbox_min + model.bbox_max);
    state.model_diagonal = (model.bbox_max - model.bbox_min).norm();
    state.support_markers.clear();
    state.load_markers.clear();
    for (const int region : setup.fixtures) {
        if (const auto position = region_centroid(model, region)) {
            CinemaMechanicsMarker marker;
            marker.region = region;
            marker.position = *position;
            marker.surface_points = region_surface_points(model, region);
            state.support_markers.push_back(std::move(marker));
        }
    }
    for (const auto& [region, load] : setup.loads) {
        if (const auto position = region_centroid(model, region)) {
            CinemaMechanicsMarker marker;
            marker.region = region;
            marker.position = *position;
            marker.vector = load.force;
            marker.surface_points = region_surface_points(model, region);
            state.load_markers.push_back(std::move(marker));
        }
    }

    if (model.cad && !model.cad->empty()) {
        try {
            // 32 samples are the product sizing pass's own density. They keep
            // the outline smooth at 1080p and give the opening feature panel
            // the exact curvature trace the FFT denoiser receives.
            const geom::CadTopology topology = geom::extract_topology(*model.cad, 32);
            capture_curve_story(state, topology);
            polylines.reserve(topology.edges.size());
            for (const auto& edge : topology.edges) {
                if (edge.samples.size() >= 2) {
                    polylines.push_back(edge.samples);
                }
            }
            state.skeleton_source = SkeletonSource::kBrepEdges;
        } catch (const std::exception& e) {
            // A build without OpenCASCADE, or a BRep the extractor rejects.
            // Both are real conditions with a real message; neither licenses
            // falling back to the tessellation while claiming BRep edges.
            state.skeleton_source = SkeletonSource::kUnavailable;
            state.skeleton_note = e.what();
        }
    } else {
        try {
            // STL input carries no BRep. The crease network of the tessellation
            // is a different measurement of the same part; it is labelled as
            // such on screen and never called a BRep skeleton.
            const auto sharp = geom::detect_sharp_edges(model.surface, 30.0);
            polylines.reserve(sharp.size());
            for (const auto& edge : sharp) {
                if (edge.v0 < model.surface.vertices.size() &&
                    edge.v1 < model.surface.vertices.size()) {
                    polylines.push_back(
                        {model.surface.vertices[edge.v0], model.surface.vertices[edge.v1]});
                }
            }
            state.skeleton_source = SkeletonSource::kSharpEdges;
        } catch (const std::exception& e) {
            state.skeleton_source = SkeletonSource::kUnavailable;
            state.skeleton_note = e.what();
        }
    }

    state.skeleton_polylines = polylines.size();
    state.skeleton_points = 0;
    for (const auto& line : polylines) {
        state.skeleton_points += line.size();
    }
    viewport.set_skeleton(polylines);

    auto [axis, note] = resolve_sweep_axis(model, setup);
    state.sweep_axis = axis;
    state.sweep_note = std::move(note);
}

void prepare_cinema_features(CinemaState& state, const pipeline::Model& model,
                             const pipeline::SimSetup& setup) {
    std::vector<pipeline::RefineRegion> regions;
    if (setup.bc_grading) {
        regions.reserve(setup.fixtures.size() + setup.loads.size());
        for (const int face : setup.fixtures) {
            if (const auto box = region_box(model, face)) {
                regions.push_back({box->first, box->second, 0.5});
            }
        }
        for (const auto& [face, load] : setup.loads) {
            (void)load;
            if (const auto box = region_box(model, face)) {
                regions.push_back({box->first, box->second, 0.25});
            }
        }
    }

    state.uploaded_sizing_story = false;
    state.sizing.field_points.clear();
    state.sizing.field_h_before.clear();
    state.sizing.field_h_after.clear();
    state.sizing.sampled_h_before_min = 0.0;
    state.sizing.sampled_h_before_max = 0.0;
    state.sizing.sampled_h_after_min = 0.0;
    state.sizing.sampled_h_after_max = 0.0;
    try {
        double h = setup.mesh_size;
        if (!(h > 0.0)) {
            h = pipeline::resolve_mesh_size(model, h, 30.0, setup.max_elems, setup.max_dof,
                                            setup.p_elevate)
                    .h;
        }
        const pipeline::RefinementPlan plan = pipeline::build_refinement_plan(
            model, h, regions, setup.use_feature_grading, setup.spectral_smooth, 0);
        std::optional<pipeline::RefinementPlan> baseline_plan;
        if (setup.spectral_smooth && plan.spectral.applied) {
            baseline_plan = pipeline::build_refinement_plan(
                model, h, regions, setup.use_feature_grading, false, 0);
        }
        const pipeline::RefinementPlan& baseline = baseline_plan ? *baseline_plan : plan;

        state.sizing.prepared = true;
        state.sizing.brep_curvature = plan.geometry_curvature_from_brep;
        state.sizing.geometry_seeds = plan.n_geometry_seeds;
        state.sizing.bc_seeds = plan.n_bc_seeds;
        state.sizing.h_min = plan.h_min;
        state.sizing.spectral = plan.spectral;

        const auto valid_h = [](double value) { return std::isfinite(value) && value > 0.0; };
        const auto append_sample =
            [&](const Eigen::Vector3d& point, std::vector<Eigen::Vector3d>& points,
                std::vector<double>& before, std::vector<double>& after) {
                if (!baseline.size_field || !plan.size_field) {
                    return;
                }
                const double h_before = baseline.size_field(point);
                const double h_after = plan.size_field(point);
                if (!valid_h(h_before) || !valid_h(h_after)) {
                    return;
                }
                points.push_back(point);
                before.push_back(h_before);
                after.push_back(h_after);
            };

        // Bound the point pass while retaining deterministic coverage of the
        // whole tessellated surface. The selected BRep edge is stored
        // separately and always keeps all of its samples.
        constexpr std::size_t kMaxFieldSamples = 1800;
        const std::size_t stride = std::max<std::size_t>(
            1, (model.surface.vertices.size() + kMaxFieldSamples - 1) / kMaxFieldSamples);
        for (std::size_t i = 0; i < model.surface.vertices.size(); i += stride) {
            append_sample(model.surface.vertices[i], state.sizing.field_points,
                          state.sizing.field_h_before, state.sizing.field_h_after);
        }

        const auto range = [](const std::vector<double>& values) {
            if (values.empty()) {
                return std::pair{0.0, 0.0};
            }
            const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
            return std::pair{*lo, *hi};
        };
        std::tie(state.sizing.sampled_h_before_min, state.sizing.sampled_h_before_max) =
            range(state.sizing.field_h_before);
        std::tie(state.sizing.sampled_h_after_min, state.sizing.sampled_h_after_max) =
            range(state.sizing.field_h_after);
    } catch (const std::exception&) {
        // The worker remains authoritative and will report the actual failure.
        // The feature panel simply declines to draw a report it could not
        // compute; it never blocks the solve or substitutes plausible values.
        state.sizing.prepared = false;
        state.sizing.spectral = {};
        state.sizing.field_points.clear();
        state.sizing.field_h_before.clear();
        state.sizing.field_h_after.clear();
    }

    const auto& spectral = state.sizing.spectral;
    std::printf("cinema: spectral applied %d modes_total %zu modes_kept %zu "
                "energy_kept %.9g edge_seeds %zu predicted_before %.9g "
                "predicted_after %.9g geometry_seeds %zu bc_seeds %zu brep_curvature %d "
                "field_samples %zu h_before_min %.9g h_before_max %.9g "
                "h_after_min %.9g h_after_max %.9g\n",
                spectral.applied ? 1 : 0, spectral.modes_total, spectral.modes_kept,
                spectral.energy_kept, spectral.n_edge_curve_seeds, spectral.predicted_before,
                spectral.predicted_after, state.sizing.geometry_seeds, state.sizing.bc_seeds,
                state.sizing.brep_curvature ? 1 : 0, state.sizing.field_points.size(),
                state.sizing.sampled_h_before_min, state.sizing.sampled_h_before_max,
                state.sizing.sampled_h_after_min, state.sizing.sampled_h_after_max);
    std::fflush(stdout);
}

#ifdef POLYMESH_WITH_ADVISOR
namespace {

/// A high percentile of `values`, by partial selection. `values` is reordered.
float percentile(std::vector<float>& values, double q) {
    if (values.empty()) {
        return 1.0f;
    }
    const auto at = values.begin() +
                    static_cast<std::ptrdiff_t>(q * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), at, values.end());
    return *at > 0.0f ? *at : 1.0f;
}

/// Everything the network panel needs to draw one pass so that it can be told
/// apart from the other 108. Computed once per take: the ensemble is fixed the
/// moment `explain` returns, and none of it may be recomputed per frame from
/// the pass being drawn, which is precisely the mistake that made every pass
/// look alike.
CinemaState::AdvisorScale advisor_display_scale(const advisor::NetworkLayout& layout,
                                                const advisor::AdvisorExplanation& ex) {
    CinemaState::AdvisorScale scale;
    if (layout.layers.size() != 4 || ex.frames.empty()) {
        return scale;
    }
    const auto tap = [](const advisor::ActivationFrame& f,
                        std::size_t l) -> const std::vector<float>& {
        return l == 0 ? f.input : (l == 1 ? f.fc1 : (l == 2 ? f.fc2 : f.heads));
    };
    std::vector<float> pool;
    for (std::size_t l = 0; l < 4; ++l) {
        pool.clear();
        pool.reserve(ex.frames.size() * layout.layers[l].size);
        for (const auto& f : ex.frames) {
            for (const float v : tap(f, l)) {
                pool.push_back(std::fabs(v));
            }
        }
        scale.layer[l] = percentile(pool, 0.98);
    }

    // The same |w_ji * a_i| the panel ranks on, over every pass, so line
    // brightness compares across passes instead of being renormalised inside
    // each one.
    pool.clear();
    for (const auto& f : ex.frames) {
        for (std::size_t b = 0; b < layout.edges.size() && b + 1 < 4; ++b) {
            const auto& block = layout.edges[b];
            const auto& src = tap(f, b);
            if (block.weights.size() != block.rows * block.cols || block.cols != src.size()) {
                continue;
            }
            for (std::size_t j = 0; j < block.rows; ++j) {
                const float* row = block.weights.data() + j * block.cols;
                for (std::size_t i = 0; i < block.cols; ++i) {
                    pool.push_back(std::fabs(row[i] * src[i]));
                }
            }
        }
    }
    scale.contribution = percentile(pool, 0.98);

    // Which input columns the candidate grid actually moves. Measured across
    // the ensemble rather than read off a column-name list, so a re-exported
    // feature order cannot make this annotation a lie.
    const auto& first = ex.frames.front().input;
    for (std::size_t i = 0; i < first.size(); ++i) {
        for (const auto& f : ex.frames) {
            if (i < f.input.size() && std::fabs(f.input[i] - first[i]) > 1.0e-6f) {
                scale.action_columns.push_back(static_cast<int>(i));
                break;
            }
        }
    }

    bool any_ranked = false;
    for (const auto& f : ex.frames) {
        if (!f.ranked || !std::isfinite(f.score)) {
            continue;
        }
        const auto s = static_cast<float>(f.score);
        scale.score_min = any_ranked ? std::min(scale.score_min, s) : s;
        scale.score_max = any_ranked ? std::max(scale.score_max, s) : s;
        any_ranked = true;
    }

    // The candidate the chooser went on to recommend, identified by matching
    // the recommended pass's own action. This is a lookup, never a second
    // ranking: a re-ranked "winner" here could disagree with the shipped
    // decision, which is the one thing this surface must never do.
    const advisor::ActivationFrame* recommended = nullptr;
    for (const auto& f : ex.frames) {
        if (f.recommended) {
            recommended = &f;
        }
    }
    if (recommended != nullptr) {
        const auto& want = recommended->action;
        for (std::size_t n = 0; n < ex.frames.size(); ++n) {
            const auto& f = ex.frames[n];
            if (f.candidate < 0) {
                continue;
            }
            if (f.action.mesher == want.mesher && f.action.order == want.order &&
                f.action.adapt_passes == want.adapt_passes &&
                std::fabs(f.action.h_rel - want.h_rel) < 1.0e-9 &&
                std::fabs(f.action.eta_target - want.eta_target) < 1.0e-9) {
                scale.winner_frame = static_cast<int>(n);
                break;
            }
        }
    }
    scale.ready = true;
    return scale;
}

} // namespace
#endif

// ---- act 2: the network ---------------------------------------------------

bool load_cinema_advisor(CinemaState& state, const pipeline::Model& model,
                         pipeline::SimSetup& setup, const std::string& dir) {
    state.advisor_dir = dir;
    state.advisor_note.clear();
    state.advisor_ran = false;
    state.decision_vetoed = false;
    state.decision_unrecognized = false;
    state.decision_applied = false;
    state.decision_note.clear();

    auto unavailable = [&state, &dir](std::string why) {
        state.advisor_note = std::move(why);
        state.decision_note =
            "no decision was applied: the mesh act runs on the studio's own setup";
        std::printf("cinema: advisor unavailable %s: %s\n", dir.c_str(),
                    state.advisor_note.c_str());
        std::fflush(stdout);
        return false;
    };

#ifndef POLYMESH_WITH_ADVISOR
    (void)model;
    (void)setup;
    return unavailable("advisor inference was not compiled into this build");
#else
    state.explanation.reset();
    state.layout = advisor::NetworkLayout{};
    state.advisor_scale = CinemaState::AdvisorScale{};

    if (model.surface.triangles.empty()) {
        return unavailable(
            "no part is loaded, so there is no feature row to run the network on");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path{dir}, ec)) {
        return unavailable(std::format("no such model directory: {}", dir));
    }

    // The feature row is built exactly the way the product path builds it
    // (the CLI solve command): fix/load context as region boxes, then
    // pipeline::extract_case_features, then advisor::to_columns inside
    // Advisor::explain. The studio selects by face id rather than by box, so
    // each selected face contributes the box of its own triangles.
    std::vector<pipeline::RefineRegion> fix_regions;
    std::vector<pipeline::RefineRegion> load_regions;
    for (const int face : setup.fixtures) {
        if (const auto box = region_box(model, face)) {
            fix_regions.push_back({box->first, box->second, 0.5});
        }
    }
    Eigen::Vector3d load_dir = Eigen::Vector3d::Zero();
    for (const auto& [face, load] : setup.loads) {
        if (const auto box = region_box(model, face)) {
            load_regions.push_back({box->first, box->second, 0.25});
        }
        load_dir += load.force;
    }
    if (load_dir.norm() > 0.0) {
        load_dir.normalize();
    }

    try {
        const auto features = pipeline::extract_case_features(model, fix_regions, load_regions,
                                                              load_dir, setup.poissons_ratio);
        const advisor::Advisor advisor(dir);
        if (!advisor.has_activations()) {
            return unavailable("model dir exports no activation taps — re-export with "
                               "scripts/advisor/export_onnx.py");
        }
        state.layout = advisor.layout();
        state.explanation = advisor.explain(features, static_cast<double>(setup.max_dof));
        state.advisor_scale = advisor_display_scale(state.layout, *state.explanation);
    } catch (const std::exception& e) {
        return unavailable(e.what());
    }

    const auto& explanation = *state.explanation;
    const auto& decision = explanation.decision;
    state.advisor_ran = true;

    // Applying the decision is what makes the causal claim true: the mesher
    // must execute the action the network chose, or the video would be showing
    // two unrelated things side by side. A refusal is NOT applied -- it is a
    // real outcome and is shown as one.
    if (decision.vetoed) {
        state.decision_vetoed = true;
        const std::string why =
            decision.note.empty()
                ? (decision.budget_refusal ? "no candidate fit the degrees-of-freedom budget"
                                           : "the feasibility gate declined the prediction")
                : decision.note;
        state.decision_note = std::format(
            "advisor abstained: {}; the configured baseline below stays unchanged", why);
    } else if (const auto mesher = pipeline::mesher_from_name(decision.mesher)) {
        const double diag = (model.bbox_max - model.bbox_min).norm();
        setup.mesher = *mesher;
        setup.mesh_size = std::max(decision.h_rel * diag, 1e-9);
        setup.adapt_passes = decision.adapt_passes;
        setup.eta_target = decision.eta_target;
        // The solve path has one p-elevation step (tet4/hex8 -> tet10/hex20),
        // so an order above 2 is executed as quadratic. Same mapping the CLI
        // applies, and the HUD reports the executed order, not the asked one.
        setup.p_elevate = decision.p_elevate || decision.order >= 2;
        state.decision_applied = true;
        state.decision_note = std::format(
            "applied to the run below: {} at {:.3g} mm cells, {} refinement pass{}, "
            "order {}",
            mesher_plain(decision.mesher), setup.mesh_size * 1e3, setup.adapt_passes,
            setup.adapt_passes == 1 ? "" : "es", setup.p_elevate ? 2 : 1);
    } else {
        state.decision_unrecognized = true;
        // Same refusal the CLI makes: meshing something other than what was
        // recommended, while reporting the recommendation, is the failure mode
        // this whole surface exists to rule out.
        state.decision_note = std::format(
            "NOT applied: this build does not recognise the advised mesher '{}', so the mesh "
            "below is the studio's own setup rather than something else meshed silently",
            decision.mesher);
    }

    // One candidate per enumerated action, then the final re-score pass, so the
    // candidate count is the frame count minus that last pass.
    const std::size_t n_frames = explanation.frames.size();
    const std::size_t candidates = n_frames > 0 ? n_frames - 1 : 0;
    std::printf("cinema: advisor %s candidates %zu gate_threshold %.6g frames %zu decision %s "
                "h_rel %.6g order %d adapt_passes %d eta_target %.6g vetoed %d "
                "ood_distance %.6g applied %d\n",
                dir.c_str(), candidates, explanation.gate_threshold, n_frames,
                decision.mesher.c_str(), decision.h_rel, decision.order, decision.adapt_passes,
                decision.eta_target, decision.vetoed ? 1 : 0, decision.ood_distance,
                state.decision_applied ? 1 : 0);
    // What the network panel drew with, so the display scales and the strip's
    // range are auditable from the render log rather than taken on trust.
    const auto& scale = state.advisor_scale;
    std::printf(
        "cinema: advisor panel panel_action_columns %zu panel_case_columns %zu "
        "panel_winner_candidate %d panel_score_min %.6g panel_score_max %.6g "
        "panel_input_p98 %.6g panel_fc1_p98 %.6g panel_fc2_p98 %.6g "
        "panel_heads_p98 %.6g panel_contribution_p98 %.6g\n",
        scale.action_columns.size(),
        state.layout.layers.empty()
            ? 0
            : state.layout.layers[0].size - scale.action_columns.size(),
        scale.winner_frame >= 0 &&
                static_cast<std::size_t>(scale.winner_frame) < explanation.frames.size()
            ? explanation.frames[static_cast<std::size_t>(scale.winner_frame)].candidate
            : -1,
        scale.score_min, scale.score_max, scale.layer[0], scale.layer[1], scale.layer[2],
        scale.layer[3], scale.contribution);
    std::fflush(stdout);
    return true;
#endif
}

} // namespace polymesh::gui
