// SPDX-License-Identifier: BSD-3-Clause

// One (config, part, tier) run: import, mesh, solve, probe, and assemble the
// results.jsonl row (schema advisor-row-v4, docs/dag/interfaces.md).

#include "advisor/calibration.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/solve.hpp"
#include "fea/solve_cost.hpp"
#include "fea/vtu.hpp"
#include "load_area.hpp"
#include "probe_util.hpp"
#include "run_artifacts.hpp"
#include "testlab_internal.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace polymesh::testlab::detail {
namespace {

/// Thrown from solve progress (or after mesh) when max_run_wall_s is exceeded.
struct WallClockBudgetExceeded : std::runtime_error {
    explicit WallClockBudgetExceeded(double elapsed_s)
        : std::runtime_error("wall-clock budget exceeded (" + std::to_string(elapsed_s) +
                             " s)"),
          elapsed_s(elapsed_s) {}
    double elapsed_s = 0.0;
};

json case_features_json(const pipeline::CaseFeatures& f) {
    return {{"bbox_dx", f.bbox_dx},
            {"bbox_dy", f.bbox_dy},
            {"bbox_dz", f.bbox_dz},
            {"diag", f.diag},
            {"volume", f.volume},
            {"surface_area", f.surface_area},
            {"sa_over_v23", f.sa_over_v23},
            {"n_faces", f.n_faces},
            {"n_sharp_edges", f.n_sharp_edges},
            {"sharp_edge_len_total", f.sharp_edge_len_total},
            {"curved_frac", f.curved_frac},
            {"kappa_max_h", f.kappa_max_h},
            {"kappa_mean_h", f.kappa_mean_h},
            {"thin_min_over_diag", f.thin_min_over_diag},
            {"thin_p10_over_diag", f.thin_p10_over_diag},
            {"min_feature_h", f.min_feature_h},
            {"n_fix_faces", f.n_fix_faces},
            {"n_load_faces", f.n_load_faces},
            {"fix_area_frac", f.fix_area_frac},
            {"load_area_frac", f.load_area_frac},
            {"load_dir_x", f.load_dir_x},
            {"load_dir_y", f.load_dir_y},
            {"load_dir_z", f.load_dir_z},
            {"fix_load_dist_over_diag", f.fix_load_dist_over_diag},
            {"load_axis_alignment", f.load_axis_alignment},
            {"poisson", f.poisson},
            // Proximity / load / singularity columns, in the order
            // `scripts/advisor/dataset.py:FEATURE_COLUMNS` appends them. The
            // values ARE the sentinels documented on `pipeline::CaseFeatures`
            // when there is nothing to measure, so a row never has to be
            // distinguished from a row of zeros.
            {"geo_n_inner_loops", f.geo_n_inner_loops},
            {"geo_hole_spacing_min_rel", f.geo_hole_spacing_min_rel},
            {"geo_hole_spacing_p10_rel", f.geo_hole_spacing_p10_rel},
            {"geo_feat_pair_dist_min_rel", f.geo_feat_pair_dist_min_rel},
            {"geo_feat_pair_dist_p10_rel", f.geo_feat_pair_dist_p10_rel},
            {"geo_feat_pair_dist_mean_rel", f.geo_feat_pair_dist_mean_rel},
            {"geo_dihedral_p10", f.geo_dihedral_p10},
            {"geo_dihedral_p50", f.geo_dihedral_p50},
            {"geo_dihedral_p90", f.geo_dihedral_p90},
            {"geo_singular_lambda_min", f.geo_singular_lambda_min},
            {"load_to_feature_dist_min_rel", f.load_to_feature_dist_min_rel},
            {"fix_to_feature_dist_min_rel", f.fix_to_feature_dist_min_rel},
            {"case_load_multiaxiality", f.case_load_multiaxiality}};
}

json action_json(const Config& cfg, double h, double h_rel) {
    return {{"h", h},
            {"h_rel", h_rel},
            {"mesher", pipeline::mesher_name(cfg.mesher)},
            {"element_tendency", cfg.element_tendency},
            {"skin_layers", cfg.skin_layers},
            {"feature_refine", cfg.feature_refine},
            {"bc_grading", cfg.bc_grading},
            {"spectral_smooth", cfg.spectral_smooth},
            {"adapt_passes", cfg.adapt_passes},
            {"eta_target", cfg.eta_target},
            {"p_elevate", cfg.p_elevate},
            {"adapt_leb_waves", cfg.adapt_leb_waves},
            {"cost_only", cfg.cost_only},
            {"order", cfg.order}};
}

pipeline::SimSetup adaptive_setup(const pipeline::Model& model, const PartCase& part,
                                  const Config& cfg, double h) {
    pipeline::SimSetup setup;
    setup.youngs_modulus = part.E;
    setup.poissons_ratio = part.nu;
    setup.mesh_size = h;
    setup.use_feature_grading = cfg.feature_refine;
    setup.bc_grading = cfg.bc_grading;
    setup.spectral_smooth = cfg.spectral_smooth;
    setup.adapt_passes = cfg.adapt_passes;
    setup.eta_target = cfg.eta_target;
    setup.p_elevate = cfg.p_elevate || cfg.order >= 2;
    setup.adapt_leb_waves = cfg.adapt_leb_waves;
    setup.skin_layers = cfg.skin_layers;
    setup.mesher = cfg.mesher;
    setup.element_tendency = cfg.element_tendency;
    const auto& surface = model.surface;
    for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] < 0) {
            continue;
        }
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d centroid =
            (surface.vertices[tri[0]] + surface.vertices[tri[1]] + surface.vertices[tri[2]]) /
            3.0;
        for (const auto& bc : part.bcs) {
            if (bc.box.contains(centroid)) {
                setup.fixtures.insert(model.triangle_region[ti]);
                break;
            }
        }
    }
    for (const auto& load : part.loads) {
        std::map<int, double> box_area;
        std::map<int, double> aligned_area;
        const double traction_norm = load.traction.norm();
        Eigen::Vector3d direction = Eigen::Vector3d::Zero();
        if (traction_norm > 0.0) {
            direction = load.traction / traction_norm;
        }
        for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
            if (ti >= model.triangle_region.size() || model.triangle_region[ti] < 0) {
                continue;
            }
            const auto& tri = surface.triangles[ti];
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!load.box.contains(centroid)) {
                continue;
            }
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            const double twice_area = cross.norm();
            if (!(twice_area > 0.0)) {
                continue;
            }
            const int region = model.triangle_region[ti];
            const double area = 0.5 * twice_area;
            box_area[region] += area;
            if (traction_norm <= 0.0 ||
                std::abs((cross / twice_area).dot(direction)) > load.normal_min_dot) {
                aligned_area[region] += area;
            }
        }
        const auto& selected = aligned_area.empty() ? box_area : aligned_area;
        for (const auto& [region, area] : selected) {
            setup.loads[region].force += load.traction * area;
        }
    }
    return setup;
}

void accumulate_solve_cost(RunOutcome& out, double flops, double bytes, int cg_iters,
                           std::uint64_t factor_nnz, std::string_view method) {
    out.solve_flops += flops;
    out.solve_bytes += bytes;
    out.cg_iters += cg_iters;
    out.factor_nnz = factor_nnz > std::numeric_limits<std::uint64_t>::max() - out.factor_nnz
                         ? std::numeric_limits<std::uint64_t>::max()
                         : out.factor_nnz + factor_nnz;
    if (!method.empty()) {
        if (out.solve_method.empty()) {
            out.solve_method = method;
        } else if (out.solve_method != method) {
            out.solve_method = "mixed";
        }
    }
    out.line["solve_flops"] = out.solve_flops;
    out.line["solve_bytes"] = out.solve_bytes;
    out.line["cg_iters"] = out.cg_iters;
    out.line["factor_nnz"] = out.factor_nnz;
    out.line["solve_method"] =
        out.solve_method.empty() ? json(nullptr) : json(out.solve_method);
    out.line["cost_label_source"] =
        out.solve_method.empty() ? json(nullptr) : json("measured-" + out.solve_method);
}

void accumulate_solve_cost(RunOutcome& out, const fea::SolveCostMeasured& cost) {
    accumulate_solve_cost(out, cost.flops, cost.bytes, cost.cg_iterations, cost.factor_nnz,
                          cost.method);
}
// The row-first ordering depends on moving the mesh out of run_one, never copying
// it. Enforce the precondition in the compiler rather than in a review comment.
static_assert(std::is_move_constructible_v<pipeline::VolumeMeshOutput>,
              "RunOutcome moves the mesh out of run_one; a non-movable "
              "VolumeMeshOutput would silently copy a whole mesh per run");
static_assert(std::is_move_assignable_v<pipeline::VolumeMeshOutput>,
              "out.mesh = std::move(vol) must move, not copy");

// Never throws: this runs on the SolveJob worker thread via job.on_pass, where
// a throw is caught as a solve failure and would silently downgrade a healthy
// run to a mesh_fail row -- corrupting the training set rather than crashing.
void write_adapt_trace(const fs::path& run_dir,
                       const std::vector<pipeline::PassTrace>& traces) noexcept {
    if (run_dir.empty() || traces.empty()) {
        return;
    }
    static constexpr std::array<const char*, 4> kShapeNames{"keep", "hex", "tet", "poly"};
    std::ostringstream text;
    for (const auto& trace : traces) {
        const std::size_t shape =
            static_cast<std::size_t>(std::clamp(trace.global_shape, 0, 3));
        const json row{{"pass", trace.pass},
                       {"n_elems", trace.n_elems},
                       {"n_nodes", trace.n_nodes},
                       {"n_dof", trace.n_dof},
                       {"global_eta", trace.global_eta},
                       {"eta_p50", trace.eta_p50},
                       {"eta_p90", trace.eta_p90},
                       {"eta_max", trace.eta_max},
                       {"n_h_mark", trace.n_h_mark},
                       {"n_p_mark", trace.n_p_mark},
                       {"n_shape_mark", trace.n_shape_mark},
                       {"global_shape", kShapeNames[shape]},
                       {"predicted_dof_factor", trace.predicted_dof_factor},
                       {"mesh_ms", trace.mesh_ms},
                       {"solve_ms", trace.solve_ms},
                       {"solve_flops", trace.solve_flops},
                       {"solve_bytes", trace.solve_bytes},
                       {"cg_iters", trace.cg_iters},
                       {"factor_nnz", trace.factor_nnz},
                       {"solve_method", trace.solve_method}};
        text << row.dump() << '\n';
    }
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec && !fs::is_directory(run_dir)) {
        std::fprintf(stderr, "warehouse: cannot create %s: %s\n", run_dir.string().c_str(),
                     ec.message().c_str());
        return;
    }
    try {
        atomic_write(run_dir / "adapt_trace.jsonl", text.str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warehouse: adapt_trace write failed: %s\n", e.what());
    }
}

} // namespace

// Never throws: every caller is either run_one's success path or one of its
// exception handlers, and a throw from inside a handler escapes run_one past
// its own catch-all and aborts the campaign, losing the status row.
void write_warehouse_run(const fs::path& run_dir, const json& line,
                         const pipeline::VolumeMeshOutput* vol) noexcept {
    if (!polymesh::testlab::write_run_json(run_dir, line)) {
        return;
    }
    if (vol != nullptr) {
        try {
            fea::write_vtu(run_dir / "mesh.vtu", vol->mesh);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warehouse: write_vtu failed: %s\n", e.what());
        }
    }
}

RunOutcome run_one(const Config& cfg, const PartCase& part, int tier, double h_scale,
                   const fs::path& progress_path, const fs::path& mesh_preview_path,
                   const fs::path& warehouse_run_dir, double max_run_wall_s,
                   const AdvisorScorer* advisor, std::string_view host, long long budget_dof,
                   long long budget_elems) {
    using clock = std::chrono::steady_clock;
    const auto t_all0 = clock::now();
    const auto wall_elapsed_s = [&]() -> double {
        return std::chrono::duration<double>(clock::now() - t_all0).count();
    };
    const auto stamp_wall = [&](json& line) {
        line["wall_time_s"] = wall_elapsed_s();
        line["max_run_wall_s"] = max_run_wall_s;
    };

    RunOutcome out;
    out.line["cfg_id"] = cfg.id;
    out.line["config"] = cfg.values;
    out.line["part"] = part.part;
    out.line["schema"] = "advisor-row-v4";
    out.line["action"] = action_json(cfg, 0.0, cfg.h_rel.value_or(0.0));
    out.line["features"] = case_features_json(pipeline::CaseFeatures{});
    out.line["tier"] = tier;
    out.line["host"] = host.empty() ? polymesh::advisor::local_host_name() : std::string(host);
    out.line["solve_flops"] = nullptr;
    out.line["solve_bytes"] = nullptr;
    out.line["cg_iters"] = 0;
    out.line["factor_nnz"] = 0;
    out.line["solve_method"] = nullptr;
    out.line["cost_label_source"] = nullptr;
    out.line["est_solve_flops"] = nullptr;

    ProgressHeartbeat beat(progress_path, "mesh", cfg.id, part.part, tier, t_all0);

    try {
        const auto model = pipeline::Model::load(part.geometry);
        const PartCase selection_part = with_exact_cad_selections(model, part);
        const auto auto_resolved = pipeline::resolve_mesh_size(model, 0.0);
        const double bbox_diag = (model.bbox_max - model.bbox_min).norm();
        const double h = cfg.h_rel ? std::max(*cfg.h_rel * bbox_diag, 1e-9)
                                   : std::max(auto_resolved.h * h_scale, 1e-9);
        const auto resolved =
            cfg.h_rel ? pipeline::resolve_mesh_size(model, h) : auto_resolved;
        json gc = geom_class_of(model, auto_resolved.h);
        std::vector<pipeline::RefineRegion> fix_regions;
        std::vector<pipeline::RefineRegion> load_regions;
        fix_regions.reserve(part.bcs.size());
        load_regions.reserve(part.loads.size());
        for (const auto& bc : part.bcs) {
            fix_regions.push_back({bc.box.lo, bc.box.hi, 0.5});
        }
        Eigen::Vector3d load_dir = Eigen::Vector3d::Zero();
        // Per-region traction vectors, index-aligned with `load_regions`, so
        // case_load_multiaxiality sees patches pulling along different axes
        // (the summed `load_dir` cannot).
        std::vector<Eigen::Vector3d> load_tractions;
        load_tractions.reserve(part.loads.size());
        for (const auto& load : part.loads) {
            load_regions.push_back({load.box.lo, load.box.hi, 0.25});
            load_tractions.push_back(load.traction);
            load_dir += load.traction;
        }
        const pipeline::CaseFeatures features = pipeline::extract_case_features(
            model, fix_regions, load_regions, load_dir, part.nu, load_tractions);
        out.line["features"] = case_features_json(features);
        if (advisor != nullptr) {
            // The advisor is an observation: its failure must not become a
            // solve_fail row that the training set would learn from.
            try {
                out.line["advisor_decision"] = advisor->decision_json(features);
            } catch (const std::exception& e) {
                out.line["advisor_error"] = e.what();
            }
        }
        const double actual_h_rel = bbox_diag > 0.0 ? h / bbox_diag : 0.0;
        out.line["action"] = action_json(cfg, h, actual_h_rel);

        // M11: h_min feature flag (virtual-topology detector; no OCC suppress).
        const HminFeatureReport hmin = detect_hmin_features(model, h);
        gc["n_features_below_h_min"] = hmin.n_features_below_h_min;
        out.line["geom_class"] = std::move(gc);
        out.line["feature_flags"] = hmin.feature_flags;
        out.line["n_features_below_h_min"] = hmin.n_features_below_h_min;
        if (!resolved.note.empty()) {
            out.line["mesher_note"] = resolved.note;
        }

        // M4: predicted element count from bbox volume / h³ before meshing.
        // Defaults keep one pathological config from pinning an overnight
        // throughput runner; a campaign may raise them (see Campaign::max_dof).
        constexpr long long kDefaultMaxCampaignDof = 80000;
        constexpr long long kDefaultMaxCampaignElems = 60000;
        const long long kMaxCampaignDof = budget_dof > 0 ? budget_dof : kDefaultMaxCampaignDof;
        const long long kMaxCampaignElems =
            budget_elems > 0 ? budget_elems : kDefaultMaxCampaignElems;
        const double n_pred = predict_elem_count(model, h);
        out.line["n_pred_elems"] = n_pred;
        out.line["h"] = h;
        if (n_pred > 2.0 * static_cast<double>(kMaxCampaignElems)) {
            // Sizing field alone already busts budget — do not mesh.
            out.line["status"] = "over_budget";
            out.line["over_budget_cause"] = "sizing"; // N_pred ≫ tier → fix auto-h
            out.line["error"] = "N_pred=" + std::to_string(static_cast<long long>(n_pred)) +
                                " already exceeds 2× elem budget (sizing, not mesher)";
            out.line["mesh_ms"] = 0.0;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            beat.set_phase("done", 1.0);
            return out;
        }

        // M14: if already over wall before meshing (unlikely), skip mesh+solve.
        if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
            throw WallClockBudgetExceeded(wall_elapsed_s());
        }

        const auto t_mesh0 = clock::now();
        // Optional a-priori geometry+BC grading (ADR-0021): refine toward the
        // load/fixture selection boxes (loads finest) fused with geometry
        // features, so the mesh reflects the simulation setup. Off by default.
        std::vector<Eigen::Vector3d> refine_seeds;
        double refine_band = 0.0;
        mesh::SizeFieldFn size_field;
        if (cfg.bc_grading) {
            std::vector<pipeline::RefineRegion> regions = load_regions;
            regions.insert(regions.end(), fix_regions.begin(), fix_regions.end());
            const auto plan =
                pipeline::build_refinement_plan(model, h, regions, cfg.feature_refine);
            refine_seeds = plan.refine_seeds;
            refine_band = plan.seed_band;
            size_field = plan.size_field;
        }
        pipeline::VolumeMeshOutput vol;
        std::vector<pipeline::PassTrace> adapt_traces;
        if (cfg.adapt_passes > 0) {
            pipeline::SimSetup setup = adaptive_setup(model, selection_part, cfg, h);
            setup.max_elems = static_cast<std::size_t>(kMaxCampaignElems);
            setup.max_dof = static_cast<std::size_t>(kMaxCampaignDof);
            pipeline::SolveJob job;
            job.on_pass = [&](const pipeline::PassTrace& trace) {
                adapt_traces.push_back(trace);
                write_adapt_trace(warehouse_run_dir, adapt_traces);
            };
            job.start(model, setup);
            bool wall_exceeded = false;
            while (job.state() == pipeline::SolveJob::State::kMeshing ||
                   job.state() == pipeline::SolveJob::State::kSolving) {
                if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
                    wall_exceeded = true;
                    job.request_cancel();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (wall_exceeded) {
                throw WallClockBudgetExceeded(wall_elapsed_s());
            }
            auto result = job.take_result();
            if (!result) {
                throw std::runtime_error("adaptive SolveJob failed: " + job.status_text());
            }
            vol.mesh = std::move(result->volume_mesh);
            vol.boundary_quads = std::move(result->boundary_quads);
            vol.mesher_note = std::move(result->mesh_note);
            vol.fill_geometry_volume = result->fill_geometry_volume;
            vol.solved_geometry_volume = result->solved_geometry_volume;

            for (const auto& trace : adapt_traces) {
                out.mesh_ms += trace.mesh_ms;
                out.solve_ms += trace.solve_ms;
                accumulate_solve_cost(out, trace.solve_flops, trace.solve_bytes,
                                      trace.cg_iters, trace.factor_nnz, trace.solve_method);
            }
            write_adapt_trace(warehouse_run_dir, adapt_traces);
        } else {
            vol = pipeline::volume_mesh(model, h, cfg.mesher, cfg.skin_layers,
                                        cfg.feature_refine, refine_seeds, refine_band,
                                        cfg.element_tendency, 0, 0, 0, {}, size_field);
            const std::string combined_mesher_note =
                tlab::combine_mesher_notes(resolved.note, vol.mesher_note);
            if (!combined_mesher_note.empty()) {
                out.line["mesher_note"] = combined_mesher_note;
            }
            vol.mesh.check_validity();
            if (cfg.order >= 2 || cfg.p_elevate) {
                // Use the product's own curved-geometry construction rather than
                // a local promote+project copy: pyramid split, exact face and
                // sharp-edge mids, shape-floor rollback and h-refinement
                // fallback all have to be identical, or the advisor learns from
                // meshes the shipped pipeline never builds.
                auto shaped = pipeline::curve_volume_geometry(model, vol.mesh, h);
                vol.mesh = std::move(shaped.mesh);
                vol.boundary_quads = fea::extract_boundary_faces(vol.mesh);
                vol.mesher_note +=
                    std::format(" | curved_volume promoted={} pyramid_split={} projected={}"
                                " partial={} reverted={} h_refined={}",
                                shaped.n_promoted, shaped.n_pyramids_split, shaped.n_projected,
                                shaped.n_partial, shaped.n_reverted, shaped.n_h_refined);
                vol.mesh.check_validity();
            }
            pipeline::update_solved_geometry_volume(model, vol);

            const auto t_mesh1 = clock::now();
            out.mesh_ms = std::chrono::duration<double, std::milli>(t_mesh1 - t_mesh0).count();
        }
        if (!vol.mesher_note.empty()) {
            out.line["mesher_note"] = vol.mesher_note;
        }
        // Validity is mandatory before any solve (anti-cheat / engineering rule).
        vol.mesh.check_validity();

        out.line["n_elems"] = vol.mesh.elements.size();
        out.line["n_nodes"] = vol.mesh.nodes.size();
        const long long n_dof = 3 * static_cast<long long>(vol.mesh.nodes.size());
        out.line["n_dof"] = n_dof;
        out.line["quality"] = quality_of(model, vol.mesh, h);
        out.line["geo_fidelity"] = geo_fidelity_of(model, vol.mesh, h);
        if (vol.fill_geometry_volume.available) {
            out.line["geometry_fill_volume_err"] = vol.fill_geometry_volume.relative_error;
        }
        if (vol.solved_geometry_volume.available) {
            out.line["geometry_volume_err"] = vol.solved_geometry_volume.relative_error;
        }
        if (n_pred > 0.0) {
            out.line["n_elems_over_pred"] =
                static_cast<double>(vol.mesh.elements.size()) / n_pred;
        }

        beat.set_mesh_stats(vol.mesh.elements.size(), vol.mesh.nodes.size());
        write_mesh_preview(mesh_preview_path, vol);

        // M14: after mesh — if wall budget blown, skip remaining solve.
        if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
            out.line["status"] = "over_budget";
            out.line["over_budget_cause"] = "wall_clock";
            out.line["error"] = "wall-clock exceeded after mesh (" +
                                std::to_string(wall_elapsed_s()) + " s > " +
                                std::to_string(max_run_wall_s) + " s); solve skipped";
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            out.mesh = std::move(vol);
            beat.set_phase("done", 1.0);
            return out;
        }

        // Campaign budget: skip pathological meshes so one config cannot pin the
        // runner (campaign.json resources.max_dof / max_elems).
        if (n_dof > kMaxCampaignDof ||
            static_cast<long long>(vol.mesh.elements.size()) > kMaxCampaignElems) {
            out.line["status"] = "over_budget";
            // N_pred OK but N_actual high → mesher (recovery/protect cascades).
            out.line["over_budget_cause"] =
                (n_pred > 0.0 && static_cast<double>(vol.mesh.elements.size()) > 3.0 * n_pred)
                    ? "mesher"
                    : "budget";
            out.line["error"] =
                "mesh exceeds campaign DOF/elem budget (" + std::to_string(n_dof) + " dof, " +
                std::to_string(vol.mesh.elements.size()) +
                " elems, N_pred=" + std::to_string(static_cast<long long>(n_pred)) + ")";
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            out.mesh = std::move(vol);
            beat.set_phase("done", 1.0);
            return out;
        }

        beat.set_phase("assemble", 0.0);

        const fea::Material mat{.youngs_modulus = part.E, .poissons_ratio = part.nu};
        const auto bc =
            make_dirichlet(vol.mesh, selection_part.bcs, model.cad ? &*model.cad : nullptr, h);
        if (bc.dof_values.empty()) {
            throw std::runtime_error("no Dirichlet DOFs matched BC boxes for part " +
                                     part.part);
        }
        const fea::SolveCostEstimate estimated_cost = fea::analyze_solve_cost(vol.mesh, bc);
        const double direct_flops =
            estimated_cost.factor_flops + 4.0 * static_cast<double>(estimated_cost.factor_nnz);
        out.line["nfree"] = estimated_cost.nfree;
        out.line["est_solve_flops"] = direct_flops;
        out.line["cg_flops_per_iter"] = estimated_cost.cg_flops_per_iter;
        out.line["cg_bytes_per_iter"] = estimated_cost.cg_bytes_per_iter;
        if (cfg.cost_only) {
            const fea::SolveOptions defaults;
            if (estimated_cost.nfree <= defaults.cg_threshold) {
                fea::SolveCostMeasured symbolic;
                symbolic.method = "direct";
                symbolic.factor_nnz = estimated_cost.factor_nnz;
                symbolic.flops = direct_flops;
                symbolic.bytes = fea::estimate_direct_solve_bytes(estimated_cost);
                accumulate_solve_cost(out, symbolic);
            } else {
                out.factor_nnz = estimated_cost.factor_nnz;
                out.solve_method = "cg-symbolic";
                out.line["factor_nnz"] = out.factor_nnz;
                out.line["solve_method"] = out.solve_method;
            }
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.line["status"] = "cost_only";
            out.line["cost_label_source"] = estimated_cost.nfree <= defaults.cg_threshold
                                                ? "symbolic-direct"
                                                : "symbolic-cg-per-iteration";
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            out.mesh = std::move(vol);
            beat.set_phase("done", 1.0);
            return out;
        }
        const auto resolved_loads = resolve_load_faces(
            vol.mesh, model.cad ? &*model.cad : nullptr, h, selection_part.loads);
        const auto loads = make_loads(vol.mesh, selection_part.loads, resolved_loads);
        if (loads.norm() == 0.0) {
            throw std::runtime_error("zero load vector for part " + part.part);
        }
        if (std::getenv("POLYMESH_SELECTION_AUDIT") != nullptr) {
            const auto legacy_bc = make_dirichlet(vol.mesh, part.bcs, nullptr, h);
            const auto legacy_selections =
                resolve_load_faces(vol.mesh, nullptr, h, part.loads);
            const auto legacy_loads = make_loads(vol.mesh, part.loads, legacy_selections);
            const bool used_fixture_fallback =
                legacy_bc.dof_values.empty() && !bc.dof_values.empty();
            const bool used_load_fallback =
                std::any_of(resolved_loads.begin(), resolved_loads.end(),
                            [](const ResolvedLoadFaces& selection) {
                                return selection.used_exact_fallback;
                            });
            const std::uint64_t legacy_fixture_hash = dirichlet_node_set_hash(legacy_bc);
            const std::uint64_t selected_fixture_hash = dirichlet_node_set_hash(bc);
            const std::uint64_t legacy_face_hash = selected_face_set_hash(legacy_selections);
            const std::uint64_t selected_face_hash = selected_face_set_hash(resolved_loads);
            const std::uint64_t legacy_node_hash = selected_node_set_hash(legacy_selections);
            const std::uint64_t selected_node_hash = selected_node_set_hash(resolved_loads);
            const std::uint64_t legacy_vector_hash = load_vector_hash(legacy_loads);
            const std::uint64_t selected_vector_hash = load_vector_hash(loads);
            const bool unchanged = legacy_fixture_hash == selected_fixture_hash &&
                                   legacy_face_hash == selected_face_hash &&
                                   legacy_node_hash == selected_node_hash &&
                                   legacy_vector_hash == selected_vector_hash;
            if (!used_fixture_fallback && !used_load_fallback && !unchanged) {
                throw std::logic_error(
                    "selection audit: a non-fallback row changed its BC/load selection");
            }
            out.line["selection_audit"] = {
                {"used_fixture_fallback", used_fixture_fallback},
                {"used_load_fallback", used_load_fallback},
                {"legacy_fixture_node_hash", legacy_fixture_hash},
                {"selected_fixture_node_hash", selected_fixture_hash},
                {"legacy_face_hash", legacy_face_hash},
                {"selected_face_hash", selected_face_hash},
                {"legacy_node_hash", legacy_node_hash},
                {"selected_node_hash", selected_node_hash},
                {"legacy_load_vector_hash", legacy_vector_hash},
                {"selected_load_vector_hash", selected_vector_hash},
                {"unchanged", unchanged}};
        }

        beat.set_phase("solve", 0.0);

        const auto t_solve0 = clock::now();
        fea::SolveOptions sopt;
        // kAuto: LDLT up to 50000 free DOF, bounded CG above that.
        sopt.on_progress = [&](int iter, int max_iters, double resid) {
            // M14 mid-solve wall-clock kill (when progress callbacks fire).
            if (max_run_wall_s > 0.0) {
                const double elapsed = wall_elapsed_s();
                if (elapsed > max_run_wall_s) {
                    throw WallClockBudgetExceeded(elapsed);
                }
            }
            const double frac =
                max_iters > 0
                    ? std::clamp(static_cast<double>(iter) / static_cast<double>(max_iters),
                                 0.0, 1.0)
                    : 0.0;
            beat.set_cg(iter, resid);
            beat.set_frac(frac);
        };
        fea::LinearSolveResult solved =
            fea::solve_elastostatics(vol.mesh, mat, bc, loads, sopt);
        accumulate_solve_cost(out, solved.cost);
        const Eigen::VectorXd& u = solved.u;
        const auto t_solve1 = clock::now();
        out.solve_ms += std::chrono::duration<double, std::milli>(t_solve1 - t_solve0).count();

        beat.set_phase("recover", 0.5);

        const ProbeAnswers ans = compute_probes(vol.mesh, mat, u, selection_part.loads,
                                                resolved_loads, part.metrics, bc, loads);
        // INVARIANT: record every ProbeAnswers field evaluate_probe() can read.
        // scripts/build_advisor_dataset.py re-derives accuracy from `answers`, so
        // a missing input makes the row unscoreable without a campaign re-run.
        out.line["answers"] = {
            {"sigma_max", ans.sigma_max},
            {"sigma_face_mean", ans.sigma_face_mean},
            {"sigma_box_max", ans.sigma_box_max},
            {"sigma_p99", ans.sigma_p99},
            {"strain_energy", ans.strain_energy},
            {"tip_deflection", ans.tip_deflection},
            {"tip_deflection_max", ans.tip_deflection_max},
            {"mean_u_component", ans.mean_u_component},
            {"mean_ux", ans.mean_ux},
            {"mean_uz", ans.mean_uz},
            {"dominant_load_axis", ans.dominant_load_axis},
            {"n_probe_nodes", ans.n_probe_nodes},
            {"n_load_faces", ans.n_load_faces},
            {"n_quality_excluded", ans.n_quality_excluded},
            {"load_face_area", ans.load_face_area},
            {"mesh_selected_area", ans.mesh_selected_area},
            {"load_area_status",
             std::string(tlab::load_area_status_name(ans.load_area_status))},
            // Explicit null, never 0.0, when nothing could be
            // verified. A number here reads as a pass.
            {"load_area_rel_err",
             ans.load_area_rel_err ? json(*ans.load_area_rel_err) : json(nullptr)},
            // Case-definition cross-check, deliberately a
            // separate field from the mesh deficit above.
            {"authored_area_checked", ans.authored_area_checked},
            {"authored_area_consistent", ans.authored_area_consistent},
            {"authored_area_rel_diff",
             ans.authored_area_rel_diff ? json(*ans.authored_area_rel_diff) : json(nullptr)}};

        // Health gates: residual / reaction / orphans / load-area guard.
        // load_area_ok is false only when the area was genuinely verified and is
        // out of tolerance, so an UNVERIFIABLE area no longer silently satisfies
        // the gate and no longer sinks a healthy row either -- it is visible as
        // load_area_status instead.
        constexpr double kFreeResidTolDirect = 1e-6;
        constexpr double kReactionSumTol = 0.05;
        const bool health_ok = (ans.n_orphan_nodes == 0) &&
                               (ans.free_residual_rel <= kFreeResidTolDirect) &&
                               (ans.reaction_sum_err <= kReactionSumTol) && ans.load_area_ok;
        out.line["health"] = {
            {"free_residual_rel", ans.free_residual_rel},
            {"reaction_sum_err", ans.reaction_sum_err},
            {"n_orphans", ans.n_orphan_nodes},
            {"n_bc_dofs", ans.n_bc_dofs},
            {"load_area_ok", ans.load_area_ok},
            {"load_area_status",
             std::string(tlab::load_area_status_name(ans.load_area_status))},
            {"load_area_verified", ans.load_area_status == tlab::LoadAreaStatus::kVerified},
            {"authored_area_consistent", ans.authored_area_consistent},
            {"load_area_rel_err",
             ans.load_area_rel_err ? json(*ans.load_area_rel_err) : json(nullptr)},
            {"ok", health_ok}};

        // Accuracy vs hand-calc truths (loaded from bench/reference via the case).
        // When health fails, still record measured answers/rel_err but zero scores
        // so ranking never trusts a singular / residual-broken solve.
        double acc_sum = 0.0;
        int acc_n = 0;
        json acc_detail = json::array();
        for (const auto& m : part.metrics) {
            const double measured = evaluate_probe(m.probe, ans);
            const double truth = m.value;
            const double rel = (std::abs(truth) > 0.0)
                                   ? std::abs(measured - truth) / std::abs(truth)
                                   : std::abs(measured);
            const double tol = (m.tol > 0.0) ? m.tol : 1e-12;
            const double s = health_ok ? (1.0 / (1.0 + rel / tol)) : 0.0;
            acc_sum += s;
            ++acc_n;
            acc_detail.push_back({{"metric", m.name},
                                  {"value", measured},
                                  {"truth", truth},
                                  {"rel_err", rel},
                                  {"score", s},
                                  {"trusted", health_ok}});
        }
        out.accuracy_score = (acc_n > 0) ? (acc_sum / static_cast<double>(acc_n)) : 0.0;
        // Primary metric for results.jsonl (first metric or aggregate).
        if (!acc_detail.empty()) {
            out.line["accuracy"] = acc_detail.front();
            out.line["accuracy"]["all"] = acc_detail;
        } else {
            out.line["accuracy"] = {{"metric", "none"},
                                    {"value", nullptr},
                                    {"truth", nullptr},
                                    {"rel_err", nullptr}};
        }

        // Five-metric campaign scorecard (Q4).
        json scorecard = compute_scorecard_geom(model, vol.mesh, h);
        scorecard["n_dof"] = n_dof;
        if (!acc_detail.empty() && acc_detail.front().contains("rel_err")) {
            scorecard["accuracy_rel_err"] = acc_detail.front()["rel_err"];
        } else {
            scorecard["accuracy_rel_err"] = nullptr;
        }
        if (out.line.contains("quality") && out.line["quality"].contains("M6")) {
            scorecard["min_element_quality"] = out.line["quality"]["M6"];
        } else if (out.line.contains("quality") && out.line["quality"].contains("score")) {
            scorecard["min_element_quality"] = out.line["quality"]["score"];
        } else {
            scorecard["min_element_quality"] = nullptr;
        }
        scorecard["solve_residual_rel"] = ans.free_residual_rel;
        scorecard["health_ok"] = health_ok;
        out.line["scorecard"] = std::move(scorecard);

        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.line["solve_flops"] = out.solve_flops;
        out.line["solve_bytes"] = out.solve_bytes;
        out.line["cg_iters"] = out.cg_iters;
        out.line["factor_nnz"] = out.factor_nnz;
        out.line["solve_method"] =
            out.solve_method.empty() ? json(nullptr) : json(out.solve_method);
        // solve_suspect: residual/reaction/orphan gate failed — answers recorded
        // but accuracy scores zeroed so analyze can filter untrusted runs.
        out.line["status"] = health_ok ? "ok" : "solve_suspect";
        stamp_wall(out.line);

        out.mesh = std::move(vol);

        beat.set_phase("done", 1.0);
    } catch (const WallClockBudgetExceeded& e) {
        // M14: mid-solve (or pre-mesh) wall-clock kill.
        out.line["status"] = "over_budget";
        out.line["over_budget_cause"] = "wall_clock";
        out.line["error"] = e.what();
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
        beat.set_phase("done", 1.0);
    } catch (const pipeline::GeometryVolumeLimitError& e) {
        out.line["status"] = "mesh_fail";
        out.line["error"] = e.what();
        // A resolution refusal fires before any mesh exists: when nothing was
        // measured emit null, never relative_error's 0.0 (reads as a perfect match).
        const char* const volume_field =
            e.solved_stage ? "geometry_volume_err" : "geometry_fill_volume_err";
        if (e.assessment.available) {
            out.line[volume_field] = e.assessment.relative_error;
        } else {
            out.line[volume_field] = nullptr;
        }
        out.line["geometry_volume_measured"] = e.assessment.available;
        out.line["advisor_training_eligible"] = false;
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
        beat.set_phase("done", 1.0);
    } catch (const fea::FeaError& e) {
        out.line["status"] = "solve_fail";
        out.line["error"] = e.what();
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
    } catch (const std::exception& e) {
        // Mesh / I/O / validity failures.
        const std::string msg = e.what();
        out.line["status"] = (msg.find("mesh") != std::string::npos ||
                              msg.find("validity") != std::string::npos)
                                 ? "mesh_fail"
                                 : "solve_fail";
        out.line["error"] = msg;
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
    }
    return out;
}

} // namespace polymesh::testlab::detail
