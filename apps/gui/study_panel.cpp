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

void detach_live_callbacks(App& app) {
    if (!app.live_callbacks_attached) {
        return;
    }
    app.live.detach(app.job);
    app.live_callbacks_attached = false;
}

namespace {

void apply_mesh_preset(App& app, MeshPreset preset) {
    app.mesh_preset = preset;
    switch (preset) {
    case MeshPreset::kFast:
        app.setup.mesher = VolumeMesher::kTetFill;
        app.setup.adapt_passes = 0;
        app.setup.eta_target = 0.0;
        app.setup.adapt_leb_waves = 0;
        app.setup.use_feature_grading = false;
        app.setup.p_elevate = false;
        app.setup.skin_layers = 1;
        break;
    case MeshPreset::kBalanced:
        app.setup.mesher = VolumeMesher::kGradedTet;
        app.setup.adapt_passes = 2;
        app.setup.eta_target = 0.12;
        app.setup.adapt_leb_waves = 2;
        app.setup.use_feature_grading = true;
        app.setup.p_elevate = true;
        app.setup.skin_layers = 1;
        break;
    case MeshPreset::kRefined:
        app.setup.mesher = VolumeMesher::kGradedTet;
        app.setup.adapt_passes = 4;
        app.setup.eta_target = 0.05;
        app.setup.adapt_leb_waves = 3;
        app.setup.use_feature_grading = true;
        app.setup.p_elevate = true;
        app.setup.skin_layers = 2;
        break;
    case MeshPreset::kCustom:
        break;
    }
}

void start_interactive_job(App& app, bool mesh_only) {
    std::optional<advisor::AdvisorExplanation> explanation;
    std::optional<advisor::NetworkLayout> layout;
    if (!app.advisor_dir.empty()) {
        const bool explained =
            load_cinema_advisor(app.cinema, *app.model, app.setup, app.advisor_dir);
#ifdef POLYMESH_WITH_ADVISOR
        if (explained && app.cinema.explanation) {
            explanation = std::move(app.cinema.explanation);
            layout = std::move(app.cinema.layout);
            if (app.cinema.decision_applied) {
                app.mesh_preset = MeshPreset::kCustom;
            }
        }
#else
        (void)explained;
#endif
    }

    app.live.reset();
    app.live.set_setup(app.setup);
    app.live.set_explanation(std::move(explanation), std::move(layout));
    app.live.attach(app.job);
    app.live_callbacks_attached = true;

    {
        std::scoped_lock lock(app.pass_trace_mutex);
        app.pass_traces.clear();
    }
    auto live_pass_sink = std::move(app.job.on_pass);
    app.job.on_pass = [&app,
                       sink = std::move(live_pass_sink)](const pipeline::PassTrace& trace) {
        if (sink) {
            sink(trace);
        }
        std::scoped_lock lock(app.pass_trace_mutex);
        app.pass_traces.push_back(trace);
    };

    fea::set_openmp_threads(app.testlab.settings.max_threads);
    app.live_mesh_seen_gen = 0;
    app.status = mesh_only ? "meshing…" : "solving…";
    if (mesh_only) {
        app.job.start_mesh(*app.model, app.setup);
    } else {
        app.job.start(*app.model, app.setup);
    }
}

bool begin_guided_step(App& app, int index, const char* title, const char* subtitle,
                       bool done) {
    bool open = app.expanded_step == index;
    const bool visible = iw::begin_step(index + 1, title, subtitle, done, &open);
    if (open) {
        app.expanded_step = index;
    } else if (app.expanded_step == index) {
        app.expanded_step = -1;
    }
    return visible;
}

void draw_model_step(App& app) {
    const std::string subtitle =
        app.model ? std::format("{} · {} triangles · {} faces", app.model->name,
                                app.model->surface.triangles.size(), app.model->region_count)
                  : "Open or drop STEP, BRep, or STL";
    if (!begin_guided_step(app, 0, "Model", subtitle.c_str(), app.model.has_value())) {
        return;
    }
    ImGui::TextWrapped("Drop a part anywhere, or enter its path.");
    iw::input_text("Part path", app.open_path, sizeof(app.open_path), "path/to/part.step");
    if (iw::button("Open model", ImVec2(-1, 0), true,
                   "Load STEP/STP, OpenCASCADE BRep, or STL geometry. You can also drag a "
                   "part anywhere onto the window.",
                   iw::Icon::kOpen) &&
        app.open_path[0] != '\0') {
        load_model(app, app.open_path);
    }
    if (app.model) {
        iw::stat_row("Part", app.model->name.c_str(), app.mono_font);
        const auto triangles = std::format("{}", app.model->surface.triangles.size());
        const auto faces = std::format("{}", app.model->region_count);
        iw::stat_row("Triangles", triangles.c_str(), app.mono_font);
        iw::stat_row("CAD faces", faces.c_str(), app.mono_font);
    }
    iw::end_step();
}

void draw_material_step(App& app) {
    static const char* kMaterials[] = {
        "Structural steel · 200 GPa / 0.30",
        "Aluminium 6061 · 69 GPa / 0.33",
        "Titanium Ti-6Al-4V · 116 GPa / 0.34",
        "Manual",
    };
    static const char* kMaterialHelp[] = {
        "Structural steel baseline: Young's modulus 200 GPa, Poisson's ratio 0.30.",
        "Aluminium 6061 baseline: Young's modulus 69 GPa, Poisson's ratio 0.33.",
        "Ti-6Al-4V baseline: Young's modulus 116 GPa, Poisson's ratio 0.34.",
        "Enter an isotropic linear-elastic Young's modulus and Poisson's ratio manually.",
    };
    const std::string subtitle = std::format(
        "{:.3g} GPa · ν {:.3g}", app.setup.youngs_modulus / 1e9, app.setup.poissons_ratio);
    if (!begin_guided_step(app, 1, "Material", subtitle.c_str(), true)) {
        return;
    }
    int material = app.material_preset;
    if (iw::selector("Preset", &material, kMaterials, 4, kMaterialHelp, nullptr,
                     iw::Icon::kMaterial)) {
        app.material_preset = material;
        if (material == 0) {
            app.setup.youngs_modulus = 200e9;
            app.setup.poissons_ratio = 0.30;
        } else if (material == 1) {
            app.setup.youngs_modulus = 69e9;
            app.setup.poissons_ratio = 0.33;
        } else if (material == 2) {
            app.setup.youngs_modulus = 116e9;
            app.setup.poissons_ratio = 0.34;
        }
    }
    double e_gpa = app.setup.youngs_modulus / 1e9;
    if (iw::input_double("Young's modulus (GPa)", &e_gpa, "%.1f")) {
        app.setup.youngs_modulus = e_gpa * 1e9;
        app.material_preset = 3;
    }
    iw::tooltip("Material stiffness E. PolyMesh currently solves isotropic, linear "
                "elastostatics; enter the value in gigapascals.");
    if (iw::input_double("Poisson's ratio", &app.setup.poissons_ratio, "%.3f")) {
        app.material_preset = 3;
    }
    iw::tooltip("Lateral contraction ratio ν. For stable isotropic elasticity use "
                "-1 < ν < 0.5; most structural metals are near 0.30.");
    iw::end_step();
}

void draw_boundary_step(App& app) {
    const bool done = !app.setup.fixtures.empty() && !app.setup.loads.empty();
    const std::string subtitle =
        std::format("{} fixture{} · {} load{}", app.setup.fixtures.size(),
                    app.setup.fixtures.size() == 1 ? "" : "s", app.setup.loads.size(),
                    app.setup.loads.size() == 1 ? "" : "s");
    if (!begin_guided_step(app, 2, "Fixtures & loads", subtitle.c_str(), done)) {
        return;
    }
    if (!app.model) {
        ImGui::TextColored(palette.text_dim, "Open a model to assign CAD faces.");
        iw::end_step();
        return;
    }
    if (app.mode != DisplayMode::kSetup) {
        if (iw::button("Show CAD and select faces", ImVec2(-1, 0), true,
                       "Return to the original CAD boundary and enable face picking. "
                       "Fixtures and loads attach to CAD face ids, not display triangles.")) {
            app.mode = DisplayMode::kSetup;
            app.pick_faces = true;
            app.overlays_dirty = true;
        }
    } else {
        app.pick_faces = true;
        ImGui::TextWrapped("Click a face · right-drag orbit · shift+left-drag pan");
    }

    iw::field_label("CAD faces");
    const float rows = static_cast<float>(std::min(app.model->region_count, 5));
    const float list_h = std::clamp(rows * ImGui::GetTextLineHeightWithSpacing() + ui_px(8.0f),
                                    ui_px(68.0f), ui_px(136.0f));
    if (ImGui::BeginChild("##face_list", ImVec2(-FLT_MIN, list_h), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int region = 0; region < app.model->region_count; ++region) {
            const bool is_fixture = app.setup.fixtures.contains(region);
            const bool is_load = app.setup.loads.contains(region);
            const char* tag = is_fixture ? " · fixture" : (is_load ? " · load" : "");
            if (is_fixture) {
                ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_fixture);
            } else if (is_load) {
                ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_load);
            }
            if (ImGui::Selectable(std::format("Face {}{}", region, tag).c_str(),
                                  app.selected_region == region)) {
                app.selected_region = region;
                app.mode = DisplayMode::kSetup;
                app.overlays_dirty = true;
                if (is_load) {
                    const auto& force = app.setup.loads[region].force;
                    app.load_force[0] = static_cast<float>(force[0]);
                    app.load_force[1] = static_cast<float>(force[1]);
                    app.load_force[2] = static_cast<float>(force[2]);
                }
            }
            if (is_fixture || is_load) {
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    if (app.selected_region >= 0) {
        const bool fixed = app.setup.fixtures.contains(app.selected_region);
        const bool loaded = app.setup.loads.contains(app.selected_region);
        ImGui::Text("Face %d", app.selected_region);
        if (iw::button(
                fixed ? "Remove fixture" : "Fix all translations", ImVec2(-1, 0), false,
                fixed ? "Release this CAD face."
                      : "Set ux = uy = uz = 0 on every node belonging to this CAD "
                        "face. Rotations are not independent DOFs in this solid model.")) {
            if (fixed) {
                app.setup.fixtures.erase(app.selected_region);
            } else {
                app.setup.fixtures.insert(app.selected_region);
                app.setup.loads.erase(app.selected_region);
            }
            app.overlays_dirty = true;
        }
        iw::input_float3("Force (N)", app.load_force);
        iw::tooltip("Resultant force vector [Fx, Fy, Fz] in newtons. PolyMesh integrates "
                    "a consistent traction over the selected CAD face so the assembled "
                    "nodal loads conserve this resultant.");
        if (iw::button(loaded ? "Update face load" : "Apply face load", ImVec2(-1, 0), false,
                       "Apply the entered resultant to this CAD face as a consistent "
                       "surface traction. The vector is a total force, not pressure.")) {
            app.setup.loads[app.selected_region].force =
                Eigen::Vector3d(app.load_force[0], app.load_force[1], app.load_force[2]);
            app.setup.fixtures.erase(app.selected_region);
            app.overlays_dirty = true;
        }
        if (loaded && iw::button("Remove load", ImVec2(-1, 0))) {
            app.setup.loads.erase(app.selected_region);
            app.overlays_dirty = true;
        }
    } else {
        ImGui::TextColored(palette.text_dim, "Select a face in the list or viewport.");
    }
    if (!app.setup.fixtures.empty() || !app.setup.loads.empty()) {
        if (iw::button("Clear all fixtures and loads", ImVec2(-1, 0), false,
                       "Remove every boundary condition from this study. The model and "
                       "mesh settings stay unchanged.")) {
            app.setup.fixtures.clear();
            app.setup.loads.clear();
            app.overlays_dirty = true;
        }
    }
    iw::end_step();
}

void draw_run_step(App& app) {
    const auto state = app.job.state();
    const bool worker_busy =
        state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    const bool paused = worker_busy && app.job.pause_requested();
    const char* subtitle = app.live.caption();
    if (!worker_busy) {
        subtitle = app.result         ? "Study complete"
                   : app.mesh_preview ? "Mesh preview ready"
                                      : "Choose fidelity and run";
    } else if (subtitle == nullptr || subtitle[0] == '\0') {
        subtitle = "Preparing study";
    }
    if (!begin_guided_step(app, 3, "Run", subtitle,
                           app.result.has_value() || app.mesh_preview.has_value())) {
        return;
    }

    int preset = static_cast<int>(app.mesh_preset);
    static const char* kPresets[] = {"Fast", "Standard", "Fine", "Manual"};
    static const char* kPresetHelp[] = {
        "Fast setup check: straight-sided tetrahedra, no error-driven refinement. "
        "Use this to validate geometry, fixtures, and loads—not as a final accuracy claim.",
        "Recommended first study: geometry-aware graded tetrahedra, quadratic field "
        "interpolation, and two ZZ-guided refinement passes toward η ≤ 0.12.",
        "Higher-accuracy study: four ZZ-guided refinement passes toward η ≤ 0.05 with "
        "quadratic elements. Expect more memory and solve time.",
        "Keep the exact mesher, element size, polynomial order, and adaptivity choices "
        "shown under Advanced.",
    };
    static const iw::Icon kPresetIcons[] = {
        iw::Icon::kFast,
        iw::Icon::kStandard,
        iw::Icon::kFine,
        iw::Icon::kManual,
    };
    if (iw::selector("Mesh fidelity", &preset, kPresets, 4, kPresetHelp, kPresetIcons,
                     iw::Icon::kMesh)) {
        apply_mesh_preset(app, static_cast<MeshPreset>(preset));
    }
    double h_mm = app.setup.mesh_size * 1e3;
    if (iw::input_double("Element size (mm, 0 = auto)", &h_mm, "%.2f")) {
        app.setup.mesh_size = std::max(0.0, h_mm / 1e3);
        app.mesh_preset = MeshPreset::kCustom;
    }

    iw::tooltip("Characteristic element size in millimetres. Set 0 for the mesher's "
                "geometry-derived estimate; smaller values increase element count, "
                "memory use, and solve time.");
    if (worker_busy) {
        const auto progress = app.job.progress();
        float overall = static_cast<float>(std::clamp(progress.phase_frac, 0.0, 1.0));
        if (progress.pass_count > 0) {
            const float span = 1.0f / static_cast<float>(progress.pass_count + 1);
            overall = std::clamp(static_cast<float>(progress.pass) * span + overall * span,
                                 0.0f, 1.0f);
        }
        const std::string value =
            std::format("{:.0f}% · {:.1f} s", 100.0f * overall, progress.elapsed_ms / 1000.0);
        iw::progress(subtitle, overall, value.c_str());
        if (progress.cg_iter > 0) {
            const auto cg =
                std::format("{} · residual {:.3g}", progress.cg_iter, progress.cg_resid);
            iw::stat_row("CG iteration", cg.c_str(), app.mono_font);
        }
    }

    ImGui::BeginDisabled(!app.model || worker_busy || app.cinema.active ||
                         state != SolveJob::State::kIdle);
    if (iw::button("Build mesh only", ImVec2(-1, 0), false,
                   "Generate and inspect the volume mesh without assembling or solving "
                   "the elasticity system. This is the fastest way to validate topology.",
                   iw::Icon::kMeshOnly)) {
        start_interactive_job(app, true);
    }
    if (iw::button(worker_busy ? "Working…" : "Solve study", ImVec2(-1, 0), true,
                   "Run the complete study: advisor deliberation when available, volume "
                   "meshing, stiffness assembly, linear solve, ZZ error recovery, and any "
                   "requested adaptive passes.",
                   iw::Icon::kSolve)) {
        start_interactive_job(app, false);
    }
    ImGui::EndDisabled();

    if (worker_busy) {
        const float gap = ui_px(8.0f);
        const float width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (paused) {
            if (iw::button("Resume", ImVec2(width, 0), true, nullptr, iw::Icon::kResume)) {
                app.job.request_resume();
            }
        } else if (iw::button("Pause", ImVec2(width, 0), false, nullptr, iw::Icon::kPause)) {
            app.job.request_pause();
        }
        ImGui::SameLine(0.0f, gap);
        if (iw::button("Cancel", ImVec2(width, 0), false, nullptr, iw::Icon::kCancel)) {
            app.job.request_cancel();
        }
    } else if (state == SolveJob::State::kFailed || state == SolveJob::State::kCancelled) {
        ImGui::TextColored(state == SolveJob::State::kFailed ? palette.status_err
                                                             : palette.status_warn,
                           "%s", app.job.status_text().c_str());
        if (iw::button("Dismiss", ImVec2(-1, 0), false, nullptr, iw::Icon::kDismiss)) {
            app.job.clear_failure();
            detach_live_callbacks(app);
            app.status = "ready";
        }
    }
    iw::end_step();
}

void draw_advanced(App& app) {
    if (!iw::disclosure("Advanced", &app.advanced_setup) || !app.advanced_setup) {
        return;
    }
    static const char* kMesherLabels[] = {
        "Graded tet", "Tet grid",   "Hex grid", "Hex + pyramid", "Prism sweep",
        "Hybrid zoo", "Hybrid VEM", "Hex VEM",  "Varyhedron",    "CVT poly",
    };
    static const char* kMesherHelp[] = {
        "Geometry-aware graded tetrahedra. Robust general-purpose choice with local "
        "feature refinement and longest-edge bisection.",
        "Uniform tetrahedral fill. Fastest topology check; less geometry-aware than the "
        "graded pipeline.",
        "Structured hexahedral fill where the geometry permits it.",
        "Hex-dominant fill closed with pyramids at transitions.",
        "Prismatic sweep for geometry with a usable sweep direction.",
        "Hybrid element zoo: hex, prism, pyramid, tet, and arbitrary cells selected by "
        "local geometry. This is PolyMesh's full construction pipeline.",
        "Hybrid finite elements plus Virtual Element Method cells for arbitrary "
        "polyhedra, assembled into one global system.",
        "Hex-dominant mesh with VEM fallback where a conforming hex transition would "
        "otherwise create poor cells.",
        "General varyhedral cells retained instead of being shattered into sliver tets.",
        "Centroidal Voronoi polyhedra. Experimental; use when cell isotropy matters more "
        "than CAD-aligned topology.",
    };
    static constexpr VolumeMesher kMesherValues[] = {
        VolumeMesher::kGradedTet,  VolumeMesher::kTetFill,    VolumeMesher::kHexFill,
        VolumeMesher::kHexPyramid, VolumeMesher::kPrismSweep, VolumeMesher::kHybrid,
        VolumeMesher::kHybridVem,  VolumeMesher::kHexVem,     VolumeMesher::kVaryhedron,
        VolumeMesher::kCvtPoly,
    };
    int mesher = 0;
    for (int i = 0; i < static_cast<int>(std::size(kMesherValues)); ++i) {
        if (kMesherValues[static_cast<std::size_t>(i)] == app.setup.mesher) {
            mesher = i;
            break;
        }
    }
    if (iw::selector("Mesher", &mesher, kMesherLabels,
                     static_cast<int>(std::size(kMesherLabels)), kMesherHelp)) {
        app.setup.mesher = kMesherValues[static_cast<std::size_t>(mesher)];
        app.mesh_preset = MeshPreset::kCustom;
    }
    int passes = app.setup.adapt_passes;
    iw::field_label("Adaptive passes");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##adapt_passes", &passes, 0, 8)) {
        app.setup.adapt_passes = passes;
        app.mesh_preset = MeshPreset::kCustom;
    }
    iw::tooltip("Number of solve → recover error → refine cycles after the initial solve. "
                "Zero disables adaptivity.");

    if (iw::input_double("ZZ η target (0 = off)", &app.setup.eta_target, "%.4g")) {
        app.setup.eta_target = std::max(0.0, app.setup.eta_target);
        app.mesh_preset = MeshPreset::kCustom;
    }
    iw::tooltip("Target global Zienkiewicz–Zhu recovery indicator η. This is an error "
                "estimator, not a confidence score. Set 0 to disable the target.");

    bool mesh_options_changed = false;
    const bool grading_changed =
        iw::checkbox("Geometry feature grading", &app.setup.use_feature_grading);
    iw::tooltip("Reduce element size near CAD edges, corners, curvature, fixtures, and "
                "loads instead of spending the same density everywhere.");
    mesh_options_changed |= grading_changed;
    const bool order_changed = iw::checkbox("Quadratic elements (P2)", &app.setup.p_elevate);
    iw::tooltip("Elevate supported linear cells to quadratic Tet10/Hex20 interpolation "
                "and use curved geometry where the CAD boundary provides it.");
    mesh_options_changed |= order_changed;
    if (mesh_options_changed) {
        app.mesh_preset = MeshPreset::kCustom;
    }

    int skin = app.setup.skin_layers;
    iw::field_label("Boundary skin layers");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##skin_layers", &skin, 1, 4)) {
        app.setup.skin_layers = skin;
        app.mesh_preset = MeshPreset::kCustom;
    }
    iw::tooltip("Number of protected boundary layers before interior coarsening. More "
                "layers preserve near-surface resolution at higher memory cost.");

    int threads = app.testlab.settings.max_threads;
    const int hardware_threads = fea::openmp_default_threads();
    iw::field_label("Thread cap (0 = all)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##max_threads", &threads, 0, std::max(1, hardware_threads))) {
        app.testlab.settings.max_threads = threads;
    }
    iw::tooltip("Maximum OpenMP worker threads for meshing, assembly, and solving. Zero "
                "uses the machine default.");

    if (iw::input_double("Memory cap (GB, 0 = auto)", &app.testlab.settings.max_mem_gb,
                         "%.2f")) {
        app.testlab.settings.max_mem_gb = std::max(0.0, app.testlab.settings.max_mem_gb);
    }
    iw::tooltip("Hard study memory ceiling in GiB. Zero derives a safe cap from available "
                "system memory; the mesher refuses a predicted over-budget case.");
    app.setup.max_mem_gb = app.testlab.settings.max_mem_gb;
    const auto budget = fea::effective_memory_budget(app.setup.max_mem_gb);
    const auto cap = fea::format_memory_bytes(budget.effective_cap_bytes);
    iw::stat_row("Enforced memory", cap.c_str(), app.mono_font);
}

void draw_pipeline_dock(App& app) {
    // Each stage row carries a label with a mono detail line under it, so a row
    // narrower than 34 dp runs the detail into the next stage's label. Add the
    // child's own padding and its "Study pipeline" caption and that is the rail
    // height below which this dock stays closed rather than clip Results or
    // stack text on text.
    const float row_floor = ui_px(34.0f);
    const float dock_chrome = ui_px(68.0f);
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.y < dock_chrome + 6.0f * row_floor || available.x < ui_px(220.0f)) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, ui_px(8.0f)));
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface_hi);
    ImGui::BeginChild("##study_pipeline", size,
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    iw::field_label("Study pipeline", iw::Icon::kMesh);

    const auto state = app.job.state();
    const auto progress = app.job.progress();
    const bool model_done = app.model.has_value();
    const bool setup_done = !app.setup.fixtures.empty() && !app.setup.loads.empty();
    const bool mesh_done = app.mesh_preview.has_value() || app.result.has_value() ||
                           state == SolveJob::State::kSolving ||
                           state == SolveJob::State::kDone;
    const bool solve_done = app.result.has_value();
    const bool advisor_done = app.cinema.advisor_ran;

    std::string mesh_detail{pipeline::mesher_name(app.setup.mesher)};
    if (state == SolveJob::State::kMeshing) {
        mesh_detail =
            progress.phase.empty() ? std::string{"constructing cells"} : progress.phase;
    } else if (mesh_done) {
        const std::size_t elements = app.result ? app.result->volume_mesh.elements.size()
                                                : app.mesh_preview->mesh.elements.size();
        mesh_detail = std::format("{} elements", elements);
    }
    std::string solve_detail{"linear elastostatics"};
    if (state == SolveJob::State::kSolving) {
        solve_detail = progress.cg_iter > 0 ? std::format("CG iteration {}", progress.cg_iter)
                                            : std::string{"assemble / solve / recover"};
    } else if (solve_done) {
        solve_detail = "linear system complete";
    }
    std::array<std::string, 6> detail{
        app.model ? app.model->name : std::string{"STEP / BRep / STL"},
        std::format("{} fixture{} · {} load{}", app.setup.fixtures.size(),
                    app.setup.fixtures.size() == 1 ? "" : "s", app.setup.loads.size(),
                    app.setup.loads.size() == 1 ? "" : "s"),
        app.advisor_dir.empty()
            ? "not configured"
            : (app.cinema.decision_vetoed
                   ? "abstained · baseline kept"
                   : (app.cinema.decision_applied ? "decision applied" : "ready before mesh")),
        std::move(mesh_detail),
        std::move(solve_detail),
        solve_done ? std::format("fields ready · η {:.3g}", app.result->global_eta)
                   : std::string{"stress · displacement · ZZ η"},
    };
    static constexpr const char* kLabels[] = {
        "Geometry", "Study setup", "Mesh advisor", "Volume mesh", "Solve", "Results",
    };
    // One glyph per stage, so the rail is readable at a glance instead of six
    // identical dots. The dot still carries the state (done / current / pending);
    // the glyph only says which stage it is.
    static constexpr iw::Icon kStageIcons[] = {
        iw::Icon::kCad,  iw::Icon::kFixture, iw::Icon::kAdvisor,
        iw::Icon::kMesh, iw::Icon::kSolve,   iw::Icon::kStress,
    };
    const std::array<bool, 6> done{
        model_done, setup_done, advisor_done, mesh_done, solve_done, solve_done,
    };
    int current = !model_done                          ? 0
                  : !setup_done                        ? 1
                  : state == SolveJob::State::kMeshing ? 3
                  : state == SolveJob::State::kSolving ? 4
                  : solve_done                         ? 5
                                                       : 2;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Fit every phase into the rail we actually have. The previous fixed 42 dp
    // rows clipped Solve/Results at 1000px-tall windows; the 26-30 dp
    // replacement stopped clipping but ran each detail line into the next
    // stage's label. row_floor is the smallest row that holds a label above a
    // mono detail line, and the dock's entry guard above refuses to open below
    // six of them. The rows own their own rhythm — every offset below is
    // measured from `row`, so ImGui's inter-item spacing would silently add a
    // seventh row's worth of height and clip Results again.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui_px(0.0f), ui_px(0.0f)));
    const float row_h =
        std::clamp(ImGui::GetContentRegionAvail().y / 6.0f, row_floor, ui_px(40.0f));
    const float dot_x = ImGui::GetCursorScreenPos().x + ui_px(8.0f);
    // Every row shares one text budget: the child's right content edge. Both the
    // label and the mono detail are ellipsized against it, so a long detail line
    // ("stress · displacement · ZZ η") ends in "..." instead of being sliced
    // through its last glyph by the clip rect.
    const float text_limit = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImFont* detail_font = app.mono_font != nullptr ? app.mono_font : ImGui::GetFont();
    const float detail_size =
        app.mono_font != nullptr ? app.mono_font->FontSize : ImGui::GetFontSize();
    char fitted[192]{};
    for (int i = 0; i < 6; ++i) {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float cy = row.y + ui_px(13.0f);
        if (i < 5) {
            dl->AddLine(ImVec2(dot_x, cy + ui_px(7.0f)),
                        ImVec2(dot_x, cy + row_h - ui_px(7.0f)),
                        ImGui::GetColorU32(palette.border), ui_px(1.0f));
        }
        const ImVec4 tone = done[static_cast<std::size_t>(i)]
                                ? palette.accent
                                : (i == current ? palette.accent2 : palette.text_disabled);
        const float dot_r = ui_px(6.0f);
        if (done[static_cast<std::size_t>(i)]) {
            // Same check geometry the step chips use, so "done" reads identically
            // in the left rail and in this dock.
            dl->AddCircleFilled(ImVec2(dot_x, cy), dot_r, ImGui::GetColorU32(tone));
            const ImU32 mark = ImGui::GetColorU32(palette.text);
            dl->AddLine(ImVec2(dot_x - dot_r * 0.46f, cy + dot_r * 0.06f),
                        ImVec2(dot_x - dot_r * 0.12f, cy + dot_r * 0.40f), mark, ui_px(1.5f));
            dl->AddLine(ImVec2(dot_x - dot_r * 0.12f, cy + dot_r * 0.40f),
                        ImVec2(dot_x + dot_r * 0.50f, cy - dot_r * 0.36f), mark, ui_px(1.5f));
        } else {
            dl->AddCircle(ImVec2(dot_x, cy), dot_r, ImGui::GetColorU32(tone), 0,
                          ui_px(i == current ? 2.4f : 1.0f));
        }
        // Stage glyph sits between the state dot and the label and shares the
        // dot's tone, so one read gives both "which stage" and "how far".
        const float glyph = ui_px(12.0f);
        iw::draw_icon(dl, ImVec2(row.x + ui_px(23.0f), cy), glyph,
                      kStageIcons[static_cast<std::size_t>(i)], ImGui::GetColorU32(tone));
        const float text_x = row.x + ui_px(35.0f);
        const float budget = std::max(0.0f, text_limit - text_x);
        dl->AddText(ImVec2(text_x, row.y),
                    ImGui::GetColorU32(i == current ? palette.text : palette.text_dim),
                    iw::fit_text(fitted, sizeof(fitted), kLabels[i], budget, ImGui::GetFont(),
                                 ImGui::GetFontSize()));
        dl->AddText(detail_font, detail_size, ImVec2(text_x, row.y + ui_px(18.0f)),
                    ImGui::GetColorU32(palette.text_disabled),
                    iw::fit_text(fitted, sizeof(fitted),
                                 detail[static_cast<std::size_t>(i)].c_str(), budget,
                                 detail_font, detail_size));
        ImGui::Dummy(ImVec2(0.0f, row_h));
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace

void draw_study_panel(App& app) {
    const bool model_done = app.model.has_value();
    const bool boundary_done = !app.setup.fixtures.empty() && !app.setup.loads.empty();
    if (!model_done) {
        app.model_step_seen = false;
        app.expanded_step = 0;
    } else if (!app.model_step_seen) {
        app.model_step_seen = true;
        app.expanded_step = 2;
    }
    if (!boundary_done) {
        app.boundary_step_seen = false;
    } else if (!app.boundary_step_seen) {
        app.boundary_step_seen = true;
        app.expanded_step = 3;
    }

    draw_model_step(app);
    draw_material_step(app);
    draw_boundary_step(app);
    draw_run_step(app);
    draw_advanced(app);
    if (!app.advanced_setup) {
        draw_pipeline_dock(app);
    }
}

std::vector<pipeline::PassTrace> pass_trace_snapshot(App& app) {
    std::scoped_lock lock(app.pass_trace_mutex);
    return app.pass_traces;
}

namespace {

void draw_camera_actions(App& app) {
    iw::field_label("Camera", iw::Icon::kCamera);
    const float gap = ui_px(4.0f);
    const float width =
        std::max(ui_px(42.0f), (ImGui::GetContentRegionAvail().x - 3.0f * gap) / 4.0f);
    constexpr float kPi = 3.14159265358979323846f;
    if (iw::button("Iso", ImVec2(width, 0), false,
                   "Isometric engineering view, then fit the current field to the pane.",
                   iw::Icon::kIso)) {
        app.viewport.camera.set_orbit(0.70f, 0.50f);
        app.viewport.frame_content(app.mode);
    }
    ImGui::SameLine(0.0f, gap);
    if (iw::button("Front", ImVec2(width, 0), false,
                   "Look square-on from the front, then fit the current field.",
                   iw::Icon::kFront)) {
        app.viewport.camera.set_orbit(0.0f, 0.0f);
        app.viewport.frame_content(app.mode);
    }
    ImGui::SameLine(0.0f, gap);
    if (iw::button("Right", ImVec2(width, 0), false,
                   "Look square-on from the right, then fit the current field.",
                   iw::Icon::kRight)) {
        app.viewport.camera.set_orbit(0.5f * kPi, 0.0f);
        app.viewport.frame_content(app.mode);
    }
    ImGui::SameLine(0.0f, gap);
    if (iw::button("Top", ImVec2(width, 0), false,
                   "Look down from above, then fit the current field.", iw::Icon::kTop)) {
        app.viewport.camera.set_orbit(0.0f, 0.5f * kPi - 0.01f);
        app.viewport.frame_content(app.mode);
    }

    const float action_gap = ui_px(4.0f);
    const float action_width =
        std::max(ui_px(92.0f), (ImGui::GetContentRegionAvail().x - action_gap) * 0.5f);
    if (iw::button("Fit field", ImVec2(action_width, 0), false,
                   "Keep the current orientation and fit the active CAD, mesh, or result "
                   "field to the pane. Keyboard shortcut: F.",
                   iw::Icon::kFit)) {
        app.viewport.frame_content(app.mode);
    }
    ImGui::SameLine(0.0f, action_gap);
    if (iw::button("Save image", ImVec2(action_width, 0), false,
                   "Capture the complete studio window as a PNG. Keyboard shortcut: F12.",
                   iw::Icon::kSave)) {
        app.shot_countdown = 0;
    }
}

} // namespace

void draw_analysis_panel(App& app) {
    // This rail is a bare child, not a group box, so nothing had ever set an
    // item width: every iw:: control fell back to ImGui's default ~65% and
    // stopped roughly 90 px short of the child's right edge. Claim the full
    // content width once, here, and the whole rail lines up with its own border.
    ImGui::PushItemWidth(-FLT_MIN);
    const bool has_output = app.result.has_value() || app.mesh_preview.has_value();
    static const char* kModes[] = {"CAD", "Mesh", "Stress", "Deflection", "Error η"};
    static const iw::Icon kModeIcons[] = {
        iw::Icon::kCad,        iw::Icon::kMesh,  iw::Icon::kStress,
        iw::Icon::kDeflection, iw::Icon::kError,
    };
    static const char* kModeHelp[] = {
        "Original CAD boundary. Use this view to pick face ids for fixtures and loads.",
        "Volume-mesh boundary coloured by element family. Turn on edges to inspect "
        "topology and local density.",
        "Von Mises equivalent stress. This combines the deviatoric stress state into one "
        "yield-oriented scalar; it is not maximum principal stress.",
        "Displacement magnitude |u|. Deformation can be magnified for legibility or shown "
        "at the physically true 1× scale.",
        "Nodal Zienkiewicz–Zhu recovery indicator η. It drives adaptivity and estimates "
        "discretisation error; it is not a probability or confidence score.",
    };
    if (has_output) {
        int mode = std::clamp(static_cast<int>(app.mode), 0, 4);
        // The heading glyph rides the selector's own caption — a separate
        // field_label here would print FIELD twice.
        if (iw::selector("Field", &mode, kModes, 5, kModeHelp, kModeIcons, iw::Icon::kCad)) {
            app.mode = static_cast<DisplayMode>(mode);
            sanitize_display_mode(app);
        }
        iw::checkbox("Wireframe edges", &app.show_wireframe, iw::Icon::kWire);
        iw::tooltip("Overlay boundary element edges. Useful for topology inspection; dense "
                    "meshes can obscure a scalar field, so it defaults off.");
        if (app.result) {
            iw::checkbox("Undeformed reference", &app.show_undeformed, iw::Icon::kUndeformed);
            iw::tooltip("Draw the original, unloaded boundary behind the deformed result.");
            static const char* kDeformationModes[] = {"Auto", "True 1×", "Custom"};
            static const char* kDeformationHelp[] = {
                "Magnify displacement just enough to make the deformation legible. The "
                "reported displacement values remain physical.",
                "Draw the deformed body at its true physical scale (magnification = 1).",
                "Choose an explicit visual magnification. This changes geometry display "
                "only, never the computed displacement.",
            };
            static const iw::Icon kDeformationIcons[] = {
                iw::Icon::kAuto,
                iw::Icon::kTrueScale,
                iw::Icon::kCustom,
            };
            int deformation = static_cast<int>(app.deformation_view);
            if (iw::selector("Deformation", &deformation, kDeformationModes, 3,
                             kDeformationHelp, kDeformationIcons, iw::Icon::kDeflection)) {
                app.deformation_view = static_cast<DeformationView>(deformation);
                app.deform_scale = app.deformation_view == DeformationView::kAuto
                                       ? app.deform_auto
                                       : (app.deformation_view == DeformationView::kTrueScale
                                              ? 1.0
                                              : app.deform_scale);
            }
            if (app.deformation_view == DeformationView::kCustom) {
                const double scale_max =
                    std::max({100.0, app.deform_auto * 20.0, app.deform_scale * 2.0});
                iw::slider_double("Visual magnification", &app.deform_scale, 0.0, scale_max,
                                  "%.3gx");
                iw::tooltip("Purely visual displacement magnification. All legends and "
                            "reported values remain the unscaled physical solution.");
            }
        }
    } else {
        iw::field_label("Live instrumentation");
        ImGui::TextWrapped(app.live.active()
                               ? "Real ONNX activations and meshing telemetry from this run "
                                 "stay in this rail; the centre remains geometry-only."
                               : "The advisor and mesher will report real frames here when "
                                 "the study starts. Nothing synthetic is shown while idle.");
    }

    if (app.model) {
        ImGui::Separator();
        draw_camera_actions(app);
    }

    // Both instrument flags are needed before the pass table decides how much
    // rail it may spend: the table and the eta plot carry the same per-pass
    // numbers, and of the two only the plot needs room to be legible.
    const bool advisor_instrument = app.live.has_advisor_content();
    const bool convergence_instrument = app.live.has_convergence_content();
    const float instrument_reserve =
        (advisor_instrument || convergence_instrument) ? ui_px(220.0f) : 0.0f;
    if (has_output) {
        ImGui::Separator();
    }
    if (app.result) {
        // Same formatter the colorbar legend uses, so the rail and the legend
        // never report one quantity in two unit systems (0.009419 MPa vs 9.42 kPa).
        const auto stress =
            format_legend_value(static_cast<float>(app.result->max_von_mises), "Pa");
        const auto displacement =
            format_legend_value(static_cast<float>(app.result->max_displacement), "m");
        const auto error = std::format("{:.4g}", app.result->global_eta);
        const auto nodes = std::format("{}", app.result->volume_mesh.nodes.size());
        const auto elements = std::format("{}", app.result->volume_mesh.elements.size());
        const auto dof = std::format("{}", app.dof_count);
        iw::field_label("Results", iw::Icon::kStress);
        iw::stat_row("Max von Mises", stress.c_str(), app.mono_font, iw::Icon::kStress);
        iw::stat_row("Max displacement", displacement.c_str(), app.mono_font,
                     iw::Icon::kDeflection);
        iw::stat_row("Global ZZ η", error.c_str(), app.mono_font, iw::Icon::kError);
        iw::tooltip("Nodal Zienkiewicz–Zhu recovery indicator for the whole mesh. Distinct "
                    "from the per-pass eta shown on the convergence card.");
        iw::stat_row("Nodes", nodes.c_str(), app.mono_font, iw::Icon::kNodes);
        iw::stat_row("Elements", elements.c_str(), app.mono_font, iw::Icon::kElements);
        iw::stat_row("DOF", dof.c_str(), app.mono_font, iw::Icon::kDof);

        const auto traces = pass_trace_snapshot(app);
        if (!traces.empty() && !traces.back().solve_method.empty()) {
            iw::stat_row("Solver", traces.back().solve_method.c_str(), app.mono_font,
                         iw::Icon::kSolver);
        }
        // The full per-pass table is a luxury; the docked plot is the instrument.
        // When both cannot fit, the table collapses to its one honest summary row
        // rather than starving the plot below it.
        const float table_height =
            ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(traces.size() + 1);
        const float after_table = ImGui::GetContentRegionAvail().y - table_height -
                                  ImGui::GetFrameHeightWithSpacing();
        if (traces.size() > 1 && after_table < instrument_reserve) {
            const auto span = std::format("{} passes · η {:.3g} → {:.3g}", traces.size(),
                                          traces.front().global_eta, traces.back().global_eta);
            iw::stat_row("Adaptive history", span.c_str(), app.mono_font, iw::Icon::kFine);
        } else if (traces.size() > 1 &&
                   ImGui::BeginTable("##convergence", 4,
                                     ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("Elements");
            ImGui::TableSetupColumn("η");
            ImGui::TableSetupColumn("Solve");
            ImGui::TableHeadersRow();
            for (const auto& trace : traces) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", trace.pass);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", trace.n_elems);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3g", trace.global_eta);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.0f ms", trace.solve_ms);
            }
            ImGui::EndTable();
        }
        if (iw::button("Export result · VTU", ImVec2(-1, 0), true,
                       "Write the volume mesh and current result fields for ParaView: "
                       "displacement, von Mises stress, and element quality.",
                       iw::Icon::kExport)) {
            const std::string output =
                app.model ? (app.model->name + "_result.vtu") : "result.vtu";
            std::string error_text;
            app.status = export_result_vtu(app, output, error_text)
                             ? std::format("wrote {}", output)
                             : std::format("export failed: {}", error_text);
        }
    } else if (app.mesh_preview) {
        const auto nodes = std::format("{}", app.mesh_preview->mesh.nodes.size());
        const auto elements = std::format("{}", app.mesh_preview->mesh.elements.size());
        const auto dof = std::format("{}", 3 * app.mesh_preview->mesh.nodes.size());
        // Mesh-only branch: nothing here is a stress field, so the heading wears
        // the mesh glyph rather than the results-field one.
        iw::field_label("Results", iw::Icon::kMesh);
        iw::stat_row("Nodes", nodes.c_str(), app.mono_font, iw::Icon::kNodes);
        iw::stat_row("Elements", elements.c_str(), app.mono_font, iw::Icon::kElements);
        iw::stat_row("DOF", dof.c_str(), app.mono_font, iw::Icon::kDof);
        iw::stat_row("Mesher", pipeline::mesher_name(app.setup.mesher).data(), app.mono_font,
                     iw::Icon::kMeshOnly);
    }

    if (advisor_instrument || convergence_instrument) {
        // The floors come from LiveView, not from a second copy of its numbers
        // here. The old local 270 dp advisor floor was 32 dp short of what
        // draw_advisor actually measures, so a mid-solve rail reserved 298 dp for
        // the advisor, the advisor declined it, and the rail showed a hole.
        const float advisor_floor = app.live.advisor_dock_floor();
        const float convergence_floor = app.live.convergence_dock_floor();
        // A residual band this tall is legible rather than merely drawable.
        const float convergence_share = std::max(convergence_floor, ui_px(180.0f));
        const float dock_floor =
            convergence_instrument
                ? (advisor_instrument ? std::min(convergence_floor, advisor_floor)
                                      : convergence_floor)
                : advisor_floor;
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.y >= dock_floor) {
            ImGui::BeginChild("##instrument_dock", available, ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse);
            const ImVec2 minimum = ImGui::GetCursorScreenPos();
            const ImVec2 size = ImGui::GetContentRegionAvail();
            const ImVec2 maximum(minimum.x + size.x, minimum.y + size.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Split only when both instruments clear their own floor, and hand
            // each one a rect the other never sees — passing the full dock to
            // both painted the convergence card over the advisor's lanes.
            if (advisor_instrument && convergence_instrument &&
                size.y >= advisor_floor + convergence_share) {
                const float convergence_height = std::min(
                    std::max(convergence_share, size.y * 0.40f), size.y - advisor_floor);
                const float boundary = maximum.y - convergence_height;
                app.live.draw_advisor_dock(dl, minimum, ImVec2(maximum.x, boundary),
                                           app.mono_font);
                app.live.draw_convergence_dock(dl, ImVec2(minimum.x, boundary), maximum,
                                               app.mono_font);
            } else if (convergence_instrument) {
                // After a solve the residual is the instrument the user is
                // staring at, so it takes the whole leftover rather than leaving
                // a hole where the advisor would have declined to draw.
                app.live.draw_convergence_dock(dl, minimum, maximum, app.mono_font);
            } else {
                app.live.draw_advisor_dock(dl, minimum, maximum, app.mono_font);
            }
            ImGui::EndChild();
        }
    }
    ImGui::PopItemWidth();
}

} // namespace polymesh::gui
