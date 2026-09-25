// SPDX-License-Identifier: BSD-3-Clause

// Sim Setup column: model, material, mesh, fixtures/loads, resources, the
// mesh/solve controls with live progress, diagnostics and result display.

#include "app_state.hpp"
#include "fea/backend.hpp"
#include "fea/resource_budget.hpp"
#include "fea/solve.hpp"
#include "fea/vtu.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <string>
#include <vector>

namespace polymesh::gui {

bool export_result_vtu(const App& app, const std::string& path, std::string& err) {
    if (!app.result) {
        err = "no solve result";
        return false;
    }
    try {
        std::vector<fea::VtuPointData> pdata;
        pdata.push_back(
            {.name = "von_Mises", .scalars = app.result->von_mises, .vectors = {}});
        pdata.push_back(
            {.name = "displacement", .scalars = {}, .vectors = app.result->displacement});
        if (!app.result->nodal_eta.empty()) {
            pdata.push_back(
                {.name = "ZZ_eta", .scalars = app.result->nodal_eta, .vectors = {}});
        }
        std::vector<fea::VtuCellData> cdata;
        cdata.push_back(
            {.name = "quality", .scalars = fea::tet4_cell_quality(app.result->volume_mesh)});
        fea::write_vtu(path, app.result->volume_mesh, pdata, cdata);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void draw_study_panel(App& app) {
    iw::begin_group_box("model");
    ImGui::TextColored(palette.text_dim, "drop .step/.stp/.brep on window");
    iw::input_text("path", app.open_path, sizeof(app.open_path), "path/to/part.step|.brep");
    if (iw::button("open", ImVec2(-1, 0)) && app.open_path[0] != '\0') {
        load_model(app, app.open_path);
    }
    iw::end_group_box();

    iw::begin_group_box("material");
    double e_gpa = app.setup.youngs_modulus / 1e9;
    if (iw::input_double("young's modulus (GPa)", &e_gpa, "%.1f")) {
        app.setup.youngs_modulus = e_gpa * 1e9;
    }
    iw::input_double("poisson's ratio", &app.setup.poissons_ratio, "%.3f");
    iw::end_group_box();

    iw::begin_group_box("mesh");
    double h_mm = app.setup.mesh_size * 1e3;
    if (iw::input_double("element size (mm, 0=auto)", &h_mm, "%.2f")) {
        app.setup.mesh_size = h_mm / 1e3;
    }
    {
        int m = static_cast<int>(app.setup.mesher);
        // Order matches VolumeMesher enum. Graded tet is the product default.
        static const char* kMeshers[] = {
            "tet (grid)",  "hex (grid)",   "hex VEM (grid)", "graded tet (default)",
            "hex+pyramid", "prism (grid)", "hybrid zoo",     "octa (exp)",
            "hybrid VEM",  "Varyhedron",   "CVT poly (G4)",
        };
        if (iw::selector("mesher", &m, kMeshers, 11)) {
            app.setup.mesher = static_cast<VolumeMesher>(m);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "graded tet (default): multi-level LEB size field; CAD parts are\n"
                "solved on projected tet10 geometry (ADR-0035, 'curved solve geometry').\n"
                "hybrid zoo: hex bulk + pyramid skin → all-pyramid FE.\n"
                "hybrid VEM: hex FE bulk + native poly VEM transitions (ADR-0019).\n"
                "Varyhedron: variable poly packing (ADR-0021). Sharp-only edge protect;\n"
                "tet FE is the default product claim; VEM gated. Measure-first path:\n"
                "health + scorecard before packing loops (ADR-0023/24). STEP product\n"
                "CAD path needs OCC build. CAD edge profiles within element budget.\n"
                "CVT poly: restricted CVT clipped Voronoi → kPolyVem (G1–G4 / M5 gate).\n"
                "octa: experimental BCC (budget-capped; not product).");
        }
    }
    {
        // Stack label above full-width slider so ImGui's trailing label never
        // overflows the group box (PushItemWidth only sizes the frame).
        int ap = app.setup.adapt_passes;
        ImGui::TextColored(palette.text_dim, "adapt passes (0=off)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##adapt_passes", &ap, 0, 8)) {
            app.setup.adapt_passes = ap;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Max ZZ→LEB/seed-remesh loops after the first solve. Stops early if η "
                "target is met. Prefer graded tet for a posteriori seed balls.");
        }
        double eta_t = app.setup.eta_target;
        if (iw::input_double("η target (0=off)", &eta_t, "%.4g")) {
            app.setup.eta_target = eta_t < 0.0 ? 0.0 : eta_t;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Stop adapting when global ZZ η ≤ this value (energy-norm style). "
                "0 disables early stop and runs all adapt passes.");
        }
        bool fg = app.setup.use_feature_grading;
        if (iw::checkbox("feature grading", &fg)) {
            app.setup.use_feature_grading = fg;
        }
        bool pe = app.setup.p_elevate;
        if (iw::checkbox("curved solve geometry", &pe)) {
            app.setup.p_elevate = pe;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Promote low-η tet4/hex8 → tet10/hex20 (auto when adapt>0)");
        }
        int skin = app.setup.skin_layers;
        ImGui::TextColored(palette.text_dim, "skin layers");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##skin_layers", &skin, 1, 4)) {
            app.setup.skin_layers = skin;
        }
    }
    iw::end_group_box();

    iw::begin_group_box("fixtures & loads");
    if (!app.model) {
        ImGui::TextColored(palette.text_dim, "open a model first");
    } else {
        // Face picking only works on the CAD surface (setup mode). Mesh/results
        // modes hide region colors — auto-switch when the user wants BCs.
        if (app.mode != DisplayMode::kSetup) {
            ImGui::TextColored(palette.status_warn, "switch to setup (CAD) to pick faces");
            if (iw::button("show CAD + pick faces", ImVec2(-1, 0), /*primary=*/true)) {
                app.mode = DisplayMode::kSetup;
                app.pick_faces = true;
                app.overlays_dirty = true;
            }
        } else {
            ImGui::TextColored(palette.text_dim,
                               "click a face (no drag). shift+lmb pan, wheel zoom");
            if (iw::checkbox("click-to-select faces", &app.pick_faces)) {
                /* toggle only */
            }
        }

        // Face list: works even when viewport pick is awkward (small faces).
        ImGui::TextColored(palette.text_dim, "faces (%d) — click to select",
                           app.model->region_count);
        const float list_h =
            std::clamp(18.0f * static_cast<float>(std::min(app.model->region_count, 8)) + 8.0f,
                       56.0f, 160.0f);
        if (ImGui::BeginChild("##face_list", ImVec2(-FLT_MIN, list_h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            for (int r = 0; r < app.model->region_count; ++r) {
                const bool is_fix = app.setup.fixtures.contains(r);
                const bool is_load = app.setup.loads.contains(r);
                const char* tag = is_fix ? " [fixture]" : (is_load ? " [load]" : "");
                const bool selected = (app.selected_region == r);
                if (is_fix) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_fixture);
                } else if (is_load) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_load);
                }
                if (ImGui::Selectable(std::format("face {}{}", r, tag).c_str(), selected)) {
                    app.selected_region = r;
                    app.mode = DisplayMode::kSetup;
                    app.overlays_dirty = true;
                    if (is_load) {
                        const auto& f = app.setup.loads[r].force;
                        app.load_force[0] = static_cast<float>(f[0]);
                        app.load_force[1] = static_cast<float>(f[1]);
                        app.load_force[2] = static_cast<float>(f[2]);
                    }
                }
                if (is_fix || is_load) {
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndChild();

        if (app.selected_region >= 0) {
            ImGui::Text("selected face: %d", app.selected_region);
            const bool fixed = app.setup.fixtures.contains(app.selected_region);
            if (iw::button(fixed ? "remove fixture" : "fix face (all DOFs)", ImVec2(-1, 0))) {
                if (fixed) {
                    app.setup.fixtures.erase(app.selected_region);
                } else {
                    app.setup.fixtures.insert(app.selected_region);
                    app.setup.loads.erase(app.selected_region);
                }
                app.overlays_dirty = true;
            }
            iw::input_float3("force (N)", app.load_force);
            const bool loaded = app.setup.loads.contains(app.selected_region);
            if (iw::button(loaded ? "update load" : "apply load", ImVec2(-1, 0))) {
                app.setup.loads[app.selected_region].force =
                    Eigen::Vector3d(app.load_force[0], app.load_force[1], app.load_force[2]);
                app.setup.fixtures.erase(app.selected_region);
                app.overlays_dirty = true;
            }
            if (loaded && iw::button("remove load", ImVec2(-1, 0))) {
                app.setup.loads.erase(app.selected_region);
                app.overlays_dirty = true;
            }
        } else {
            ImGui::TextColored(palette.text_dim, "no face selected");
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(palette.sim_fixture, "fixtures: %zu", app.setup.fixtures.size());
    {
        const std::string loads_txt = std::format("loads: {}", app.setup.loads.size());
        if (ImGui::GetContentRegionAvail().x >
            ImGui::CalcTextSize(loads_txt.c_str()).x + 18.0f) {
            ImGui::SameLine(0, 18);
        }
    }
    ImGui::TextColored(palette.sim_load, "loads: %zu", app.setup.loads.size());
    if (!app.setup.fixtures.empty() || !app.setup.loads.empty()) {
        if (iw::button("clear all BCs", ImVec2(-1, 0))) {
            app.setup.fixtures.clear();
            app.setup.loads.clear();
            app.overlays_dirty = true;
        }
    }
    iw::end_group_box();

    iw::begin_group_box("resources");
    {
        // Cap OpenMP threads for interactive mesh/solve (0 = process default).
        int hw = fea::openmp_default_threads();
        int thr = app.testlab.settings.max_threads;
        ImGui::TextColored(palette.text_dim, "max threads (0=all, hw=%d)", hw);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##max_threads", &thr, 0, std::max(1, hw))) {
            app.testlab.settings.max_threads = thr;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("OpenMP thread cap for mesh/assemble/solve hot paths.\n"
                              "0 keeps the process default (OMP_NUM_THREADS / hardware).");
        }
        double mem = app.testlab.settings.max_mem_gb;
        if (iw::input_double("max mem (GB, 0=auto)", &mem, "%.2f")) {
            app.testlab.settings.max_mem_gb = std::max(0.0, mem);
        }
        app.setup.max_mem_gb = app.testlab.settings.max_mem_gb;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enforced before stiffness assembly/factorization.\n"
                              "0 uses 70%% of currently available system memory.");
        }

        static fea::EffectiveMemoryBudget shown_budget;
        static double budget_refresh_time = -1.0e9;
        static double shown_user_cap = -1.0;
        const double now = ImGui::GetTime();
        if (now - budget_refresh_time >= 1.0 || shown_user_cap != app.setup.max_mem_gb) {
            shown_budget = fea::effective_memory_budget(app.setup.max_mem_gb);
            shown_user_cap = app.setup.max_mem_gb;
            budget_refresh_time = now;
        }
        const auto cap_text = fea::format_memory_bytes(shown_budget.effective_cap_bytes);
        ImGui::TextColored(palette.status_ok, "ENFORCED cap: %s%s", cap_text.c_str(),
                           app.setup.max_mem_gb > 0.0 ? " (user/system minimum)"
                                                      : " (70% MemAvailable)");

        const fea::NodalMesh* projected_mesh = nullptr;
        if (app.mesh_preview) {
            projected_mesh = &app.mesh_preview->mesh;
        } else if (app.result) {
            projected_mesh = &app.result->volume_mesh;
        }
        if (projected_mesh != nullptr) {
            static const fea::NodalMesh* cached_mesh = nullptr;
            static std::size_t cached_nodes = 0;
            static std::size_t cached_elements = 0;
            static fea::SolveResourceEstimate projected;
            if (cached_mesh != projected_mesh ||
                cached_nodes != projected_mesh->nodes.size() ||
                cached_elements != projected_mesh->elements.size()) {
                const auto projected_free =
                    3 * static_cast<Eigen::Index>(projected_mesh->nodes.size());
                projected = fea::estimate_solve_resources(*projected_mesh, projected_free);
                cached_mesh = projected_mesh;
                cached_nodes = projected_mesh->nodes.size();
                cached_elements = projected_mesh->elements.size();
            }
            fea::SolveOptions projection_options;
            projection_options.max_mem_gb = app.setup.max_mem_gb;
            const auto projected_decision =
                fea::decide_solve_method(projected.nfree, projection_options, projected,
                                         shown_budget.effective_cap_bytes);
            const bool projected_over =
                projected_decision.estimated_bytes > shown_budget.effective_cap_bytes;
            const char* method =
                projected_decision.method == fea::SolveMethod::kDirect ? "LDLT" : "CG";
            const auto footprint =
                fea::format_memory_bytes(projected_decision.estimated_bytes);
            ImGui::TextColored(projected_over ? palette.status_warn : palette.text_dim,
                               "projected solve: %s (%s, conservative)", footprint.c_str(),
                               method);
        } else {
            ImGui::TextColored(palette.text_dim, "projected solve: mesh required");
        }
        ImGui::TextColored(palette.text_dim, "%s", fea::performance_description().c_str());
    }
    iw::end_group_box();

    iw::begin_group_box("mesh & solve");
    const auto state = app.job.state();
    const bool busy = state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    const bool paused = busy && app.job.pause_requested();
    // Live progress while worker runs (phase / frac / elapsed from SolveJob).
    // Elapsed is wall-clock polled every frame; phase_frac only advances at
    // report() boundaries (mesh/solve can sit on one fraction for a long time).
    if (busy) {
        const auto prog = app.job.progress();
        const char* phase = prog.phase.empty()
                                ? (state == SolveJob::State::kMeshing ? "mesh" : "solve")
                                : prog.phase.c_str();
        ImGui::TextColored(paused ? palette.accent : palette.status_warn, "phase: %s%s", phase,
                           paused ? " (paused)" : "");
        const float frac = static_cast<float>(std::clamp(prog.phase_frac, 0.0, 1.0));
        // Overall bar: blend adapt pass index when available.
        float overall = frac;
        if (prog.pass_count > 0) {
            const float span = 1.0f / static_cast<float>(prog.pass_count + 1);
            overall =
                std::clamp(static_cast<float>(prog.pass) * span + frac * span, 0.0f, 1.0f);
        }
        // Soft pulse while a long phase holds a fixed fraction so the bar still
        // reads as "alive" (mesh/CG do not emit mid-phase progress yet).
        float display = overall;
        if (!paused && overall < 0.995f) {
            const float pulse =
                0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.8f);
            display = std::clamp(overall + 0.025f * pulse, 0.0f, 0.99f);
        }
        ImGui::ProgressBar(display, ImVec2(-FLT_MIN, 0),
                           std::format("{:.0f}%", 100.0 * overall).c_str());
        ImGui::Text("elapsed: %.1f s", prog.elapsed_ms / 1000.0);
        if (prog.pass_count > 0) {
            ImGui::TextColored(palette.text_dim, "adapt pass %d / %d", prog.pass,
                               prog.pass_count);
        }
        if (prog.cg_iter > 0) {
            ImGui::Text("CG: iter %d  resid %.3g", prog.cg_iter, prog.cg_resid);
        }
        if (prog.n_elems > 0) {
            ImGui::TextColored(palette.text_dim, "mesh %zu elems · %zu nodes", prog.n_elems,
                               prog.n_nodes);
        }
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        app.status = app.job.status_text();
    }
    auto apply_resource_caps = [&]() {
        fea::set_openmp_threads(app.testlab.settings.max_threads);
    };
    ImGui::BeginDisabled(!app.model || busy);
    if (iw::button("mesh only", ImVec2(-1, 0))) {
        apply_resource_caps();
        app.live_mesh_seen_gen = 0;
        app.status = "meshing…";
        app.job.start_mesh(*app.model, app.setup);
    }
    if (iw::button(busy ? "working…" : "solve", ImVec2(-1, 0), /*primary=*/true)) {
        apply_resource_caps();
        app.live_mesh_seen_gen = 0;
        app.status = "solving…";
        if (app.cinema.active) {
            prepare_cinema_features(app.cinema, *app.model, app.setup);
        }
        app.job.start(*app.model, app.setup);
    }
    ImGui::EndDisabled();
    if (busy) {
        // Pause / play / cancel — cooperative between mesh/adapt/solve phases.
        if (paused) {
            if (iw::button("play (resume)", ImVec2(-1, 0), /*primary=*/true)) {
                app.job.request_resume();
                app.status = "resuming…";
            }
        } else if (iw::button("pause", ImVec2(-1, 0))) {
            app.job.request_pause();
            app.status = "pause requested…";
        }
        if (iw::button("cancel", ImVec2(-1, 0))) {
            app.job.request_cancel();
            app.status = "cancelling…";
        }
    }
    if (state == SolveJob::State::kFailed) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.status_err);
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        ImGui::PopStyleColor();
        if (iw::button("dismiss error", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (state == SolveJob::State::kCancelled) {
        ImGui::TextColored(palette.status_warn, "%s", app.job.status_text().c_str());
        if (iw::button("dismiss cancel", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (!busy &&
               (state != SolveJob::State::kIdle || app.result || !app.mesh_status.empty())) {
        ImGui::TextColored(palette.status_ok, "%s", app.job.status_text().c_str());
        const auto prog = app.job.progress();
        if (prog.elapsed_ms > 0.0 && prog.phase == "done") {
            ImGui::TextColored(palette.text_dim, "last run: %.1f s", prog.elapsed_ms / 1000.0);
        }
    }
    if (app.dof_count > 0) {
        ImGui::Text("DOF: %zu  (3 × nodes)", app.dof_count);
    }
    if (!app.mesh_note.empty()) {
        ImGui::TextWrapped("%s", app.mesh_note.c_str());
    } else if (!app.mesh_status.empty()) {
        ImGui::TextWrapped("%s", app.mesh_status.c_str());
    }
    iw::end_group_box();

    iw::begin_group_box("diagnostics");
    {
        const auto prog = app.job.progress();
        if (prog.n_elems > 0 && prog.elapsed_ms > 0.0) {
            const double eps = static_cast<double>(prog.n_elems) / (prog.elapsed_ms / 1000.0);
            ImGui::TextColored(palette.text_dim, "throughput: %.0f elem/s (%zu elems, %.1f s)",
                               eps, prog.n_elems, prog.elapsed_ms / 1000.0);
            if (prog.cg_iter > 0) {
                ImGui::TextColored(palette.text_dim, "CG: %d iters, resid %.2e", prog.cg_iter,
                                   prog.cg_resid);
            }
        }
    }
    iw::end_group_box();

    if (app.mesh_preview || app.result) {
        iw::begin_group_box("display");
        static const char* kModes[] = {"setup (CAD)", "mesh", "von mises", "deflection",
                                       "error η"};
        int mode = static_cast<int>(app.mode);
        if (mode < 0 || mode > 4) {
            mode = 0;
        }
        if (iw::selector("mode", &mode, kModes, 5)) {
            app.mode = static_cast<DisplayMode>(mode);
            if (app.mode == DisplayMode::kMeshPreview && !app.viewport.has_mesh_preview()) {
                app.mode = DisplayMode::kSetup;
            }
            if ((app.mode == DisplayMode::kResultsVonMises ||
                 app.mode == DisplayMode::kResultsDisplacement ||
                 app.mode == DisplayMode::kResultsError) &&
                !app.result) {
                app.mode = app.viewport.has_mesh_preview() ? DisplayMode::kMeshPreview
                                                           : DisplayMode::kSetup;
            }
        }
        iw::checkbox("wireframe edges", &app.show_wireframe);
        if (app.result) {
            iw::checkbox("undeformed outline", &app.show_undeformed);
            if (iw::checkbox("true-scale deflection", &app.deform_true_scale)) {
                app.deform_scale = app.deform_true_scale ? 1.0 : app.deform_auto;
            }
            // Range: true-scale (1) up through auto and beyond — tiny |u| needs huge ×.
            const double scale_max =
                std::max({100.0, app.deform_auto * 20.0, app.deform_scale * 2.0, 10.0});
            iw::slider_double("deformation scale", &app.deform_scale, 0.0, scale_max, "%.3gx");
            if (app.result->max_displacement > 0.0 && app.model) {
                const double diag = (app.model->bbox_max - app.model->bbox_min).norm();
                const double tip_frac =
                    (app.deform_scale * app.result->max_displacement) / std::max(diag, 1e-30);
                ImGui::TextColored(palette.text_dim, "auto %.3gx → tip ~%.1f%% of model",
                                   app.deform_auto, 100.0 * tip_frac);
            }
            ImGui::Text("max von mises: %.4g MPa", app.result->max_von_mises / 1e6);
            ImGui::Text("max deflection: %.4g mm", app.result->max_displacement * 1e3);
            ImGui::Text("ZZ η global: %.4g  max nodal: %.4g", app.result->global_eta,
                        app.result->max_nodal_eta);
            ImGui::Text("nodes %zu  DOF %zu", app.result->volume_mesh.nodes.size(),
                        3 * app.result->volume_mesh.nodes.size());
            ImGui::TextWrapped("%s", app.result->mesh_note.c_str());
            if (iw::button("export VTU", ImVec2(-1, 0))) {
                const std::string out =
                    app.model ? (app.model->name + "_result.vtu") : "result.vtu";
                std::string err;
                app.status = export_result_vtu(app, out, err)
                                 ? std::format("wrote {}", out)
                                 : std::format("export failed: {}", err);
            }
        } else if (app.mesh_preview) {
            ImGui::Text("nodes %zu  elems %zu  DOF %zu", app.mesh_preview->mesh.nodes.size(),
                        app.mesh_preview->mesh.elements.size(),
                        3 * app.mesh_preview->mesh.nodes.size());
            ImGui::TextWrapped("%s", app.mesh_preview->mesher_note.c_str());
        }
        iw::end_group_box();
    }
}

} // namespace polymesh::gui
