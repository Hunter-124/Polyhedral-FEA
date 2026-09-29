// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"
#include "scene_internal.hpp"

#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/hp_driver.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/constraints.hpp"
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "fea/zz.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/indicators.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {
struct JobCancelled : std::runtime_error {
    JobCancelled() : std::runtime_error("cancelled") {}
};

/// Forwarder from a fill's `MeshStageSink` to `SolveJob::on_mesh_stage`, adding
/// the adapt pass that ran the fill (`volume_mesh` knows nothing about passes).
/// An unset callback yields an EMPTY sink rather than a no-op lambda, so the
/// fill's `if (on_stage)` guards stay false and it never converts or copies a
/// mesh; a set callback costs one mesh copy per stage.
MeshStageSink stage_sink(std::function<void(const MeshStage&)> callback, int pass) {
    if (!callback) {
        return {};
    }
    return [callback = std::move(callback), pass](const MeshStage& stage) {
        callback(MeshStage{stage.stage, stage.index, pass, stage.mesh});
    };
}
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

fea::SolveOptions SolveJob::solve_options_with_progress(int pass, int pass_count,
                                                        std::string& note_sink) {
    // Keep the shared default method/tolerance policy. The per-run memory cap
    // is enforced during solve preflight; four-iteration callbacks make the
    // same hook a low-latency cooperative pause/cancel point without restarting
    // the PCG recurrence.
    fea::SolveOptions opt;
    opt.max_mem_gb = active_max_mem_gb_;
    // The notes are the linear solver's own words (method choice, any CG
    // attempt and its iteration count). They are appended verbatim and joined
    // with newlines -- nothing here summarises or renames a method, because the
    // consumer displays this as the solver's account of itself. A solve that
    // emits no note leaves the sink untouched, which at this project's DOF
    // counts is the normal outcome: `fea::SolveMethod::kAuto` picks direct
    // sparse LDLT below `SolveOptions::cg_threshold` (50,000 free DOF) and the
    // CG-flavoured notes never appear.
    opt.on_note = [this, &note_sink](std::string_view note) {
        if (!note_sink.empty()) {
            note_sink.push_back('\n');
        }
        note_sink.append(note);
        set_status(std::string(note));
    };
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
    // Recompute wall-clock on every UI poll: `report()` only stamps phase
    // boundaries, and mesh/assemble/solve can run for minutes between them.
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
    // Copied before the worker starts, exactly like `on_pass`: the member may be
    // reassigned from the UI thread while the worker runs.
    const auto stage_callback = on_mesh_stage;
    worker_ = std::thread([this, model, setup, stage_callback] {
        try {
            // Global scalar h from D5 only. Do NOT min-sample geometry sizing at
            // sharp corners for uniform meshers — that forced h→h_min on every
            // CAD box and ~8× DOF, which freezes interactive "mesh only".
            // Feature grading is applied as feature_refine (graded skin / bands).
            const bool curved_geometry = setup.p_elevate && model.cad && !model.cad->empty();
            std::optional<geom::CadTopology> auto_topology;
            if (curved_geometry) {
                try {
                    auto_topology = geom::extract_topology(*model.cad);
                } catch (...) {
                    auto_topology.reset();
                }
            }
            const auto resolved =
                resolve_mesh_size(model, setup.mesh_size, 30.0, setup.max_elems, setup.max_dof,
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
                mesh_cancel_check, refinement.size_field,
                // Mesh-only preview is never an adapt pass.
                stage_sink(stage_callback, /*pass=*/0));
            if (setup.p_elevate) {
                auto curved = curve_volume_geometry(model, mesh_only_.mesh, h);
                mesh_only_.mesh = std::move(curved.mesh);
                mesh_only_.boundary_quads = fea::extract_boundary_faces(mesh_only_.mesh);
                mesh_only_.mesher_note +=
                    std::format(" | curved_volume promoted={} pyramid_split={} projected={} "
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
void fill_result_fields(SolveResult& r, const fea::ZzRecovery& zz, const Eigen::VectorXd& u,
                        const Eigen::VectorXd& reactions, bool reactions_complete) {
    r.displacement = u;
    r.reactions = reactions;
    r.reactions_complete = reactions_complete;
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
                          const fea::ZzRecovery& recovery, const adapt::HpDriverPlan& plan,
                          double mesh_ms, double solve_ms,
                          const fea::SolveCostMeasured& solve_cost) {
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
            const std::size_t index =
                static_cast<std::size_t>(q * static_cast<double>(eta.size() - 1));
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
    trace.solve_flops = solve_cost.flops;
    trace.solve_bytes = solve_cost.bytes;
    trace.cg_iters = solve_cost.cg_iterations;
    trace.factor_nnz = solve_cost.factor_nnz;
    trace.solve_method = solve_cost.method;
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
    const auto stage_callback = on_mesh_stage;
    const auto solve_stage_callback = on_solve_stage;
    worker_ = std::thread([this, model, setup, pass_count, pass_callback, stage_callback,
                           solve_stage_callback] {
        try {
            // Global h from D5 only (same as start_mesh). Feature grading is
            // feature_refine on graded fills — not global h→h_min at corners.
            const bool curved_geometry = setup.p_elevate && model.cad && !model.cad->empty();
            std::optional<geom::CadTopology> auto_topology;
            if (curved_geometry) {
                try {
                    auto_topology = geom::extract_topology(*model.cad);
                } catch (...) {
                    auto_topology.reset();
                }
            }
            const auto resolved =
                resolve_mesh_size(model, setup.mesh_size, 30.0, setup.max_elems, setup.max_dof,
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
                geo = detail::decimate_sources(std::move(geo), 0.5 * h);
                if (setup.spectral_smooth) {
                    const geom::CadTopology topo = detail::cad_sizing_topology(model);
                    auto edge =
                        detail::spectral_edge_sources(topo, 0.15 * h, h, spectral_report);
                    edge = detail::decimate_sources(std::move(edge), 0.5 * h);
                    spectral_report.n_edge_curve_seeds = edge.size();
                    geo.insert(geo.end(), edge.begin(), edge.end());
                }
                geo_only = geo;
                src.insert(src.end(), geo.begin(), geo.end());
            }
            mesh::SizeFieldFn geo_size_field; // geometry-only demand field
            if (!geo_only.empty()) {
                const auto geo_sp = adapt::seed_plan(geo_only, h, 1.5);
                geo_size_field =
                    adapt::size_field_from_sources(geo_only, geo_sp.h_fine, h, /*beta=*/1.0);
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
                    adapt_size_field = detail::apply_spectral_sizing(
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
                adapt_size_field, stage_sink(stage_callback, /*pass=*/0));
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
                    100.0 * spectral_report.energy_kept, spectral_report.predicted_before,
                    spectral_report.predicted_after,
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
            if (!std::isfinite(setup.youngs_modulus) || setup.youngs_modulus <= 0.0) {
                throw fea::FeaError("Young's modulus must be finite and positive");
            }
            if (!std::isfinite(setup.poissons_ratio) || setup.poissons_ratio <= -1.0 ||
                setup.poissons_ratio >= 0.5) {
                throw fea::FeaError("Poisson's ratio must be finite and in (-1, 0.5)");
            }
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
                        bool keep = selected_votes > 0 && selected_votes >= other_face_votes;
                        if (!keep) {
                            const auto exact =
                                geom::project_point_on_surface(*model.cad, centroid);
                            keep = exact && exact->face_id != geom::kInvalidCadSupportId &&
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

            // Exact CAD-face membership for BC application. The one-region-per-
            // node map (nearest display triangle) hands a face's perimeter ring
            // to an arbitrary adjacent face, dropping fixture nodes and breaking
            // mirror symmetry. A node belongs to a CAD face when its exact BRep
            // owner IS the face, or is an edge/vertex bordering it — fixtures
            // then cover the perimeter ring and load facet sets close exactly.
            // Owners are classified fresh on the mesh in hand (never carried
            // across a remesh, where node ids are reused for different
            // positions).
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
                        const auto face_it = std::find_if(
                            solve_topology->faces.begin(), solve_topology->faces.end(),
                            [&](const geom::CadFace& face) { return face.id == face_id; });
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
                // requested force, independent of boundary-node density.
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
                        const double tol = fea::load_conservation_tolerance(
                            requested_force.norm(), faces.size());
                        if (applied.conservation_error > tol) {
                            throw fea::FeaError(std::format(
                                "load on region {}: traction assembly lost {:.3g} N of the "
                                "requested {:.6g} N resultant, above the {:.3g} N round-off "
                                "ceiling",
                                region, applied.conservation_error, requested_force.norm(),
                                tol));
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
                    const double fallback_tol = fea::load_conservation_tolerance(
                        requested_force.norm(), it->second.size());
                    if (conservation_error > fallback_tol) {
                        throw fea::FeaError(std::format(
                            "load on region {}: nodal fallback lost {:.3g} N of the "
                            "requested {:.6g} N resultant, above the {:.3g} N round-off "
                            "ceiling",
                            region, conservation_error, requested_force.norm(), fallback_tol));
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
                    bc.dof_values.erase(static_cast<Eigen::Index>(constraint.slave_dof));
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
            // deterministic product runs. h_min is set once h_adapt_floor is
            // known (below).
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
                const bool changed = curved.n_promoted > 0 || curved.n_pyramids_split > 0;
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
                    curved.n_promoted, curved.n_pyramids_split, vol.mesh.nodes.size() - before,
                    p_constraints.size() / 3, counts.tet10, counts.hex20, curved.n_projected,
                    curved.n_partial, curved.n_reverted);
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
                                          adapt_size_field, stage_sink(stage_callback, pass));
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
                // The account of the solve whose displacement is currently in
                // `u_try`, and of no other. `solve_here` clears the sink first,
                // so the p-elevated re-solves below — which produce the
                // authoritative result and are the only solve that result's
                // displacement came from — leave their own words here rather
                // than inheriting the linear pass's. A solve that says nothing
                // leaves it empty, which is a fact and not a gap to fill.
                std::string pass_solver_note;
                fea::SolveCostMeasured pass_solve_cost;
                Eigen::VectorXd pass_reactions;
                bool pass_reactions_complete = false;
                const auto solve_here = [&] {
                    pass_solver_note.clear();
                    auto options =
                        solve_options_with_progress(pass, pass_count, pass_solver_note);
                    options.method = setup.solve_method;
                    auto solved = fea::solve_elastostatics(vol.mesh, material, bc, loads,
                                                           options, active_p_constraints());
                    pass_solve_cost = std::move(solved.cost);
                    pass_reactions = std::move(solved.reactions);
                    pass_reactions_complete = solved.reactions_complete;
                    return std::move(solved.u);
                };
                const auto solve_t0 = std::chrono::steady_clock::now();
                update_solved_geometry_volume(model, vol);

                auto u_try = solve_here();
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
                // Whether a further pass runs is decided by three predicates
                // the loop tests below. All three are pure functions of state
                // that is already final here, so evaluating them at the
                // emission point lets the observers state `final_pass`
                // truthfully; the loop then re-uses these exact values rather
                // than testing the same conditions twice.
                const bool eta_target_stop =
                    setup.eta_target > 0.0 && zz_try.global_eta <= setup.eta_target;
                const double growth = std::max(1.0, hp_plan.predicted_dof_factor);
                const double next_elems =
                    std::ceil(growth * static_cast<double>(vol.mesh.elements.size()));
                const double next_dof =
                    std::ceil(growth * 3.0 * static_cast<double>(vol.mesh.nodes.size()));
                const bool elem_cap =
                    next_elems > static_cast<double>(resolved.element_ceiling);
                const bool dof_cap = next_dof > static_cast<double>(resolved.dof_ceiling);
                const auto& sug = hp_plan.h_suggestion;
                // Early stop only when neither h nor p wants work — and no
                // coarsen pass is pending (a coarsen remesh must run, or
                // over-resolved regions would stay fine forever).
                const bool nothing_left_to_refine =
                    sug.n_marked == 0 && sug.h_next >= h_use * 0.98 &&
                    hp_plan.p_mark.empty() && hp_plan.coarsen_mark.empty();
                const bool another_pass_follows = !eta_target_stop &&
                                                  pass < setup.adapt_passes && !elem_cap &&
                                                  !dof_cap && !nothing_left_to_refine;

                // Firing order, fixed: `on_pass` first, then `on_solve_stage`,
                // from this one point, so the two observers can never be
                // handed different passes and the order never varies between
                // runs. `make_pass_trace` sorts a copy of the per-element η
                // vector, so it is built once and shared rather than twice.
                if ((setup.adapt_passes > 0 && pass_callback) || solve_stage_callback) {
                    const PassTrace trace =
                        make_pass_trace(pass, vol.mesh, zz_try, hp_plan, pass_mesh_ms,
                                        pass_solve_ms, pass_solve_cost);
                    if (setup.adapt_passes > 0 && pass_callback) {
                        pass_callback(trace);
                    }
                    if (solve_stage_callback) {
                        SolveStage stage;
                        stage.pass = pass;
                        stage.final_pass = !another_pass_follows;
                        stage.trace = trace;
                        // This pass's own fields, taken from this pass's own
                        // `vol`, `u_try` and `zz_try` — copied, not moved,
                        // because the loop still needs all three. The
                        // p-elevation re-solves further down belong to
                        // finalisation, not to the pass being reported, which
                        // is also why `on_pass` reports its trace here.
                        stage.result.volume_mesh = vol.mesh;
                        stage.result.boundary_quads = vol.boundary_quads;
                        stage.result.boundary_region_nodes = region_nodes;
                        fill_result_fields(stage.result, zz_try, u_try, pass_reactions,
                                           pass_reactions_complete);
                        stage.result.fill_geometry_volume = vol.fill_geometry_volume;
                        stage.result.solved_geometry_volume = vol.solved_geometry_volume;
                        // Only the notes that exist at this instant: the two
                        // mesh notes and this solve's own account of itself.
                        // The stop-reason suffix the final result carries is
                        // not known yet and is not invented here.
                        stage.result.mesh_note =
                            std::format("{} | {}", vol.mesher_note, hp_note);
                        stage.result.solver_note = pass_solver_note;
                        solve_stage_callback(stage);
                    }
                }

                // D2: global η target — stop when η is small enough (0 = disabled).
                if (eta_target_stop) {
                    std::string pnote;
                    if (maybe_p_elevate(hp_plan.p_mark, pnote)) {
                        publish_live_mesh(vol);
                        update_solved_geometry_volume(model, vol);

                        u_try = solve_here();
                        zz_try = fea::recover_zz(vol.mesh, material, u_try);
                    }
                    SolveResult r;
                    r.mesh_note = std::format(
                        "{} | {} | eta-target stop η={:.4g}≤{:.4g} pass={}/{} h={:.4g}{}",
                        vol.mesher_note, hp_note, zz_try.global_eta, setup.eta_target, pass,
                        setup.adapt_passes, h_use, pnote);
                    r.volume_mesh = std::move(vol.mesh);
                    r.boundary_quads = std::move(vol.boundary_quads);
                    r.boundary_region_nodes = region_nodes;
                    fill_result_fields(r, zz_try, u_try, pass_reactions,
                                       pass_reactions_complete);
                    r.solver_note = std::move(pass_solver_note);
                    r.fill_geometry_volume = vol.fill_geometry_volume;
                    r.solved_geometry_volume = vol.solved_geometry_volume;

                    result_ = std::move(r);
                    break;
                }
                if (pass < setup.adapt_passes) {
                    // growth/next_elems/next_dof/elem_cap/dof_cap: computed
                    // once above, where `final_pass` needed them.
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
                            u_try = solve_here();
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                        }
                        SolveResult r;
                        r.mesh_note = std::format("{} | {} | {}{}", vol.mesher_note, hp_note,
                                                  reason, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        r.boundary_region_nodes = region_nodes;
                        fill_result_fields(r, zz_try, u_try, pass_reactions,
                                           pass_reactions_complete);
                        r.solver_note = std::move(pass_solver_note);
                        r.fill_geometry_volume = vol.fill_geometry_volume;
                        r.solved_geometry_volume = vol.solved_geometry_volume;

                        result_ = std::move(r);
                        break;
                    }
                    if (nothing_left_to_refine) {
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

                            u_try = solve_here();
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                        }
                        SolveResult r;
                        r.mesh_note = std::format("{} | {} | adapt early-stop h={:.4g}{}",
                                                  vol.mesher_note, hp_note, h_use, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        r.boundary_region_nodes = region_nodes;
                        fill_result_fields(r, zz_try, u_try, pass_reactions,
                                           pass_reactions_complete);
                        r.solver_note = std::move(pass_solver_note);
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

                    u_try = solve_here();
                    zz_try = fea::recover_zz(vol.mesh, material, u_try);
                }
                SolveResult r;
                r.mesh_note = std::format("{} | {} | adapt_passes={} h={:.4g} seeds={}{}",
                                          vol.mesher_note, hp_note, setup.adapt_passes, h_use,
                                          adapt_seeds.size(), pnote);
                r.volume_mesh = std::move(vol.mesh);
                r.boundary_quads = std::move(vol.boundary_quads);
                r.boundary_region_nodes = region_nodes;
                fill_result_fields(r, zz_try, u_try, pass_reactions, pass_reactions_complete);
                r.solver_note = std::move(pass_solver_note);
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
