// SPDX-License-Identifier: BSD-3-Clause

// Screenshots and headless `--auto` scripting: the same code paths the buttons
// call, driven one action per frame, plus the cinema `record` frame writer.

#include "app_state.hpp"
#include "cinema.hpp"
#include "fea/backend.hpp"
#include "fea/solve.hpp"
#include "png_writer.hpp"

#include "imgui.h"

// On Windows, glad owns GL symbols — keep GLFW from including system gl.h.
#if defined(_WIN32)
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <glad/glad.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace polymesh::gui {

namespace {

/// UTC-stamped capture name, written into the process CWD.
std::string timestamped_shot_name() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "polymesh_shot_%Y%m%dT%H%M%SZ.png", &utc) == 0) {
        return "polymesh_shot.png";
    }
    return std::string(buf);
}

/// strtod with the whole-token check it normally lacks: a typo'd number in a
/// script must fail the run, not silently mesh at h = 0.
bool parse_auto_double(const std::string& text, double& out) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

std::string auto_action_text(const AutoAction& action) {
    std::string text = action.verb;
    for (const auto& arg : action.args) {
        text += ' ';
        text += arg;
    }
    return text;
}

/// Rejects an `--auto` face id that is not a region of the loaded model, so a
/// scripted capture can never show a load case that was never applied.
/// `region_count` is the same face set the Sim Setup face list iterates.
std::optional<std::string> bad_face_id(const App& app, const char* verb, int face) {
    if (!app.model) {
        return std::format("{}: no model is loaded — `load` a part before assigning faces",
                           verb);
    }
    if (app.model->region_count <= 0) {
        return std::format("{}: {} has no faces to assign", verb, app.model->name);
    }
    if (face < 0 || face >= app.model->region_count) {
        return std::format("{}: face {} does not exist on {} — valid face ids are 0..{}", verb,
                           face, app.model->name, app.model->region_count - 1);
    }
    return std::nullopt;
}

} // namespace

/// The back buffer still holds the finished image here, and the offscreen
/// viewport FBO is already unbound (Viewport::render restores 0).
bool capture_screenshot(GLFWwindow* window, const std::string& path) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0 || path.empty()) {
        return false;
    }
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fb_w) *
                                      static_cast<std::size_t>(fb_h) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // glReadPixels hands back rows bottom-up; the writer flips them.
    return png::write_png_rgba(path.c_str(), fb_w, fb_h, pixels.data());
}

void service_screenshot(App& app, GLFWwindow* window) {
    if (app.shot_countdown > 0) {
        --app.shot_countdown;
    } else if (app.shot_countdown == 0) {
        app.shot_countdown = -1;
        const std::string name = timestamped_shot_name();
        app.shot_msg_ok = capture_screenshot(window, name);
        app.shot_msg = app.shot_msg_ok ? std::format("saved {}", name)
                                       : std::format("screenshot failed: {}", name);
        app.shot_msg_ttl = 4.0f;
    }
    if (!app.shot_env_path.empty()) {
        const double now = glfwGetTime();
        if (now - app.shot_env_last >= 1.0) {
            app.shot_env_last = now;
            capture_screenshot(window, app.shot_env_path);
        }
    }
}

// ---- headless automation (--auto) -----------------------------------------
// Exactly one action is executed per frame. The render loop has to keep
// turning between steps: `shot` reads the default framebuffer, so the new
// state must have been drawn (and the viewport FBO resolved) at least one
// full frame before the capture, and ImGui itself needs a frame to lay the
// status strip out again.

std::vector<AutoAction> parse_auto_spec(const std::string& spec) {
    std::vector<AutoAction> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t sep = spec.find(';', pos);
        const std::string seg =
            spec.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        std::vector<std::string> tok;
        for (std::size_t i = 0; i < seg.size();) {
            while (i < seg.size() && std::isspace(static_cast<unsigned char>(seg[i]))) {
                ++i;
            }
            const std::size_t start = i;
            while (i < seg.size() && !std::isspace(static_cast<unsigned char>(seg[i]))) {
                ++i;
            }
            if (i > start) {
                tok.emplace_back(seg.substr(start, i - start));
            }
        }
        if (!tok.empty()) {
            AutoAction action;
            action.verb = tok.front();
            action.args.assign(tok.begin() + 1, tok.end());
            out.push_back(std::move(action));
        }
        if (sep == std::string::npos) {
            break;
        }
        pos = sep + 1;
    }
    return out;
}

bool parse_auto_int(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

void tick_auto(AutoRunner& run, App& app, GLFWwindow* window) {
    if (!run.enabled()) {
        return;
    }
    auto fail = [&](const std::string& why) {
        std::fprintf(stderr, "auto: %s\n", why.c_str());
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
    };

    // A recording owns the queue until its last frame is written, exactly the
    // way `solve` owns it until the job settles. Nothing else may run in
    // between: a verb that changed the setup mid-take would put two different
    // states in one video.
    if (app.cinema.recording()) {
        return;
    }

    if (run.awaiting_solve) {
        const auto st = app.job.state();
        if (st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving) {
            return;
        }
        if (run.settle_frames > 0) {
            --run.settle_frames;
            return;
        }
        run.awaiting_solve = false;
        if (st == SolveJob::State::kFailed) {
            fail(std::format("solve failed: {}", app.job.status_text()));
            return;
        }
        if (st == SolveJob::State::kCancelled) {
            fail(std::format("solve cancelled: {}", app.job.status_text()));
            return;
        }
        std::fprintf(stderr, "auto: %s\n", app.status.c_str());
    }
    if (run.next >= run.actions.size()) {
        return;
    }

    const AutoAction action = run.actions[run.next++];
    const std::string& verb = action.verb;
    const auto& args = action.args;
    std::fprintf(stderr, "auto: %s\n", auto_action_text(action).c_str());

    if (verb == "load") {
        if (args.size() != 1) {
            return fail("load wants one path");
        }
        // load_model swallows import errors into app.status, and a failed load
        // leaves any previously opened model in place. Pre-checking existence
        // catches the dominant scripting mistake (wrong path) exactly.
        if (!std::filesystem::exists(args[0])) {
            return fail(std::format("load: no such file: {}", args[0]));
        }
        load_model(app, args[0]);
        if (!app.model) {
            return fail(std::format("load failed: {}", app.status));
        }
    } else if (verb == "h") {
        double mm = 0.0;
        if (args.size() != 1 || !parse_auto_double(args[0], mm) || mm <= 0.0) {
            return fail("h wants one positive element size in mm");
        }
        app.setup.mesh_size = mm / 1000.0; // SimSetup::mesh_size is metres
    } else if (verb == "material") {
        double e_gpa = 0.0;
        double nu = 0.0;
        if (args.size() != 2 || !parse_auto_double(args[0], e_gpa) ||
            !parse_auto_double(args[1], nu) || e_gpa <= 0.0 || nu <= -1.0 || nu >= 0.5) {
            return fail("material wants <E_GPa> <nu>, with E > 0 and -1 < nu < 0.5");
        }
        app.setup.youngs_modulus = e_gpa * 1e9;
        app.setup.poissons_ratio = nu;
    } else if (verb == "mesher") {
        if (args.size() != 1) {
            return fail("mesher wants one canonical mesher name");
        }
        const auto mesher = pipeline::mesher_from_name(args[0]);
        if (!mesher) {
            return fail(std::format("mesher '{}' is not recognised", args[0]));
        }
        app.setup.mesher = *mesher;
    } else if (verb == "solver") {
        if (args.size() != 1) {
            return fail("solver wants auto, direct, or cg");
        }
        if (args[0] == "auto") {
            app.setup.solve_method = fea::SolveMethod::kAuto;
        } else if (args[0] == "direct") {
            app.setup.solve_method = fea::SolveMethod::kDirect;
        } else if (args[0] == "cg") {
            app.setup.solve_method = fea::SolveMethod::kCG;
        } else {
            return fail("solver wants auto, direct, or cg");
        }
    } else if (verb == "order") {
        int order = 0;
        if (args.size() != 1 || !parse_auto_int(args[0], order) ||
            (order != 1 && order != 2)) {
            return fail("order wants 1 or 2");
        }
        app.setup.p_elevate = order == 2;
    } else if (verb == "adapt") {
        int passes = 0;
        double eta_target = 0.0;
        if (args.size() != 2 || !parse_auto_int(args[0], passes) ||
            !parse_auto_double(args[1], eta_target) || passes < 0 || passes > 8 ||
            eta_target < 0.0) {
            return fail("adapt wants <passes 0..8> <eta_target >= 0>");
        }
        app.setup.adapt_passes = passes;
        app.setup.eta_target = eta_target;
    } else if (verb == "spectral") {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("spectral wants on or off");
        }
        app.setup.spectral_smooth = args[0] == "on";
    } else if (verb == "feature") {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("feature wants on or off");
        }
        app.setup.use_feature_grading = args[0] == "on";
    } else if (verb == "fix") {
        int face = -1;
        if (args.size() != 1 || !parse_auto_int(args[0], face)) {
            return fail("fix wants one face id");
        }
        if (const auto why = bad_face_id(app, "fix", face)) {
            return fail(*why);
        }
        app.setup.loads.erase(face); // a face is fixed or loaded, never both
        app.setup.fixtures.insert(face);
        app.overlays_dirty = true;
    } else if (verb == "loadface") {
        int face = -1;
        double fx = 0.0, fy = 0.0, fz = 0.0;
        if (args.size() != 4 || !parse_auto_int(args[0], face) ||
            !parse_auto_double(args[1], fx) || !parse_auto_double(args[2], fy) ||
            !parse_auto_double(args[3], fz)) {
            return fail("loadface wants <face> <fx> <fy> <fz> (newtons)");
        }
        if (const auto why = bad_face_id(app, "loadface", face)) {
            return fail(*why);
        }
        app.setup.fixtures.erase(face);
        app.setup.loads[face].force = Eigen::Vector3d(fx, fy, fz);
        app.overlays_dirty = true;
    } else if (verb == "solve") {
        if (!args.empty()) {
            return fail("solve takes no arguments");
        }
        if (!app.model) {
            return fail("solve with no model loaded");
        }
        fea::set_openmp_threads(app.testlab.settings.max_threads);
        app.live_mesh_seen_gen = 0;
        app.status = "solving…";
        if (app.cinema.active) {
            prepare_cinema_features(app.cinema, *app.model, app.setup);
        }
        app.job.start(*app.model, app.setup);
        run.awaiting_solve = true;
        run.settle_frames = 1;
    } else if (verb == "frame") {
        if (!args.empty()) {
            return fail("frame takes no arguments");
        }
        app.viewport.frame_content(app.mode);
    } else if (verb == "wire") {
        // The near-black results wireframe is baked from the 8x-subdivided
        // curved boundary and can hide the shaded field on a fitted camera.
        // Interactively that is a checkbox; a script needs this verb.
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("wire wants on or off");
        }
        app.show_wireframe = args[0] == "on";
    } else if (verb == "savevtu") {
        if (args.size() != 1) {
            return fail("savevtu wants one output path");
        }
        std::string err;
        if (!export_result_vtu(app, args[0], err)) {
            return fail(std::format("savevtu failed: {}", err));
        }
    } else if (verb == "shot") {
        if (args.size() != 1) {
            return fail("shot wants one output path");
        }
        run.pending_shot = args[0];
    } else if (verb == "cinema") {
        const auto st = app.job.state();
        const bool worker_busy =
            st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving;
        if (args.size() == 1 && args[0] == "on") {
            // The sinks have to be installed before the worker starts, so
            // toggling them under a live job would be a data race on
            // SolveJob::on_mesh_stage / on_solve_stage. Refuse rather than race.
            if (worker_busy) {
                return fail(
                    "cinema on while a mesh/solve is running — the stage sinks must be "
                    "installed before the worker starts");
            }
            app.cinema.active = true;
            app.cinema.t = 0.0;
            app.cinema.duration = CinemaState::kDefaultDuration;
            app.cinema.clear_stages();
            app.cinema.clear_solve_stages();
            // Installed only for the take: one copies a whole NodalMesh per
            // construction stage and the other a whole SolveResult per adaptive
            // pass, which no ordinary solve should pay for.
            app.job.on_mesh_stage = [cine = &app.cinema](const pipeline::MeshStage& stage) {
                cine->push_stage(stage);
            };
            app.job.on_solve_stage = [cine = &app.cinema](const pipeline::SolveStage& stage) {
                cine->push_solve_stage(stage);
            };
            // One continuous shot. Frame once for the settled split now, then
            // again before recording after the exact result motion envelope is
            // available; neither fit occurs inside the captured take.
            app.viewport.set_camera_locked(true);
            if (app.model) {
                build_cinema_skeleton(app.cinema, *app.model, app.setup, app.viewport);
                const ImGuiViewport* main_vp = ImGui::GetMainViewport();
                const CinemaLayout layout = cinema_layout(app, *main_vp);
                app.viewport.frame_content(DisplayMode::kCinema,
                                           layout.settled_view_aspect);
            }
        } else if (args.size() == 1 && args[0] == "off") {
            if (worker_busy) {
                return fail(
                    "cinema off while a mesh/solve is running — the stage sinks cannot "
                    "be removed from under the worker");
            }
            app.cinema.active = false;
            app.viewport.set_camera_locked(false);
            app.job.on_mesh_stage = {};
            app.job.on_solve_stage = {};
            // The take's own per-pass fields were uploaded over the studio's
            // result buffers, so hand those back before the studio draws again.
            if (app.result) {
                app.viewport.set_result(*app.result);
            }
            app.cinema.invalidate_uploads();
            // Hand the viewport back to whatever the studio actually holds:
            // DisplayMode::kCinema means nothing outside the cinema layout.
            app.mode = app.result
                           ? DisplayMode::kResultsVonMises
                           : (app.viewport.has_mesh_preview() ? DisplayMode::kMeshPreview
                                                              : DisplayMode::kSetup);
        } else if (args.size() == 2 && args[0] == "advisor") {
            if (!app.model) {
                return fail("cinema advisor with no model loaded");
            }
            // A missing directory or a graph without the trunk taps is NOT a
            // scripting failure: it is a real condition the cinema is required
            // to state on screen, so the take must go on and record it.
            load_cinema_advisor(app.cinema, *app.model, app.setup, args[1]);
        } else {
            return fail("cinema wants `on`, `off`, or `advisor <model dir>`");
        }
    } else if (verb == "record") {
        if (args.size() != 2) {
            return fail("record wants an output directory and a frame count");
        }
        int frames = 0;
        if (!parse_auto_int(args[1], frames) || frames <= 0) {
            return fail("record wants a positive frame count");
        }
        if (!app.cinema.active) {
            return fail("record before `cinema on` — there is no take to record");
        }
        std::error_code ec;
        std::filesystem::create_directories(args[0], ec);
        if (!std::filesystem::is_directory(std::filesystem::path{args[0]}, ec)) {
            return fail(std::format("record: cannot create output directory {}", args[0]));
        }
        if (app.result) {
            app.viewport.set_cinema_motion_bounds(*app.result,
                                                  static_cast<float>(app.deform_scale));
            const ImGuiViewport* main_vp = ImGui::GetMainViewport();
            const CinemaLayout layout = cinema_layout(app, *main_vp);
            app.viewport.frame_content(DisplayMode::kCinema,
                                       layout.settled_view_aspect);
        }
        app.cinema.record_dir = args[0];
        app.cinema.record_frames = frames;
        app.cinema.record_next = 0;
        // The take IS the requested frames at 1/60 s, so the act schedule is
        // scaled to exactly that and to nothing else.
        app.cinema.duration = static_cast<double>(frames) * CinemaState::kRecordStep;
        app.cinema.t = 0.0;
        app.cinema.invalidate_uploads();
        // Frames are captured, not watched: waiting for the display would make
        // a 1200-frame take cost 20 s of wall clock for nothing. Restored when
        // the take ends, and on the failure path too.
        glfwSwapInterval(0);
        std::printf("cinema: take %s frames %d fps 60 duration %.4f s\n",
                    app.cinema.record_dir.c_str(), frames, app.cinema.duration);
        std::fflush(stdout);
    } else if (verb == "quit") {
        if (!args.empty()) {
            return fail("quit takes no arguments");
        }
        glfwSetWindowShouldClose(window, 1);
    } else {
        return fail(std::format("unknown action: {}", auto_action_text(action)));
    }
}

void service_auto_shot(AutoRunner& run, GLFWwindow* window) {
    if (run.pending_shot.empty()) {
        return;
    }
    const std::string path = std::move(run.pending_shot);
    run.pending_shot.clear();
    if (capture_screenshot(window, path)) {
        std::fprintf(stderr, "auto: wrote %s\n", path.c_str());
    } else {
        std::fprintf(stderr, "auto: screenshot failed: %s\n", path.c_str());
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
    }
}

/// One captured frame is one 1/60 s step of the virtual clock, set from the
/// frame INDEX (CinemaState::seek_frame) rather than accumulated DeltaTime, so
/// the recorded composition is a pure function of the frame number.
void service_cinema_record(AutoRunner& run, App& app, GLFWwindow* window) {
    CinemaState& cine = app.cinema;
    if (!cine.recording()) {
        return;
    }
    const int index = cine.record_next;
    const std::string path =
        (std::filesystem::path{cine.record_dir} / std::format("frame_{:05d}.png", index))
            .string();
    if (!capture_screenshot(window, path)) {
        // A dropped frame would silently shorten the video and desynchronise
        // every act boundary the manifest claims, so this fails the run.
        std::fprintf(stderr, "auto: cinema record failed to write %s\n", path.c_str());
        cine.record_dir.clear();
        glfwSwapInterval(1);
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
        return;
    }
    cine.record_next = index + 1;

    const CinemaCue cue = cinema_cue(cine);
    // Half-second granularity: enough for the render script to show live
    // progress without burying its log in one line per frame.
    if (index == 0 || cine.record_next >= cine.record_frames || index % 30 == 0) {
        std::printf("cinema: frame %d/%d t %.4f s act %s\n", cine.record_next,
                    cine.record_frames, cine.t, cinema_act_name(cue.act));
        std::fflush(stdout);
    }
    if (cine.record_next < cine.record_frames) {
        return;
    }

    // Take complete. The act windows and the summary are what
    // scripts/render_cinema.py records in its manifest, so they are printed
    // from the same schedule the frames were drawn with.
    for (int a = 0; a < kCinemaActCount; ++a) {
        const auto act = static_cast<CinemaAct>(a);
        double t0 = 0.0;
        double t1 = 0.0;
        cinema_act_window(cine, act, t0, t1);
        const int f0 = std::min(cine.record_frames - 1,
                                static_cast<int>(std::ceil(t0 / CinemaState::kRecordStep)));
        const int f1 =
            std::min(cine.record_frames - 1,
                     static_cast<int>(std::ceil(t1 / CinemaState::kRecordStep)) - 1);
        std::printf("cinema: act %s frames %d..%d t %.4f..%.4f s\n", cinema_act_name(act), f0,
                    std::max(f0, f1), t0, t1);
    }
    std::size_t candidates = 0;
#ifdef POLYMESH_WITH_ADVISOR
    if (cine.explanation && !cine.explanation->frames.empty()) {
        // One pass per enumerated candidate, then the final re-score.
        candidates = cine.explanation->frames.size() - 1;
    }
#endif
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    // Poster only after the analysis pane has finished opening, so no panel
    // text is clipped mid-slide.
    double opening_t0 = 0.0;
    double opening_t1 = 0.0;
    cinema_act_window(cine, CinemaAct::kSkeleton, opening_t0, opening_t1);
    const double poster_t = opening_t0 + 0.22 * (opening_t1 - opening_t0);
    const int poster = std::min(
        cine.record_frames - 1,
        static_cast<int>(std::ceil(poster_t / CinemaState::kRecordStep)));
    // The numeric tail is the manifest's compact verification record. Nodes,
    // total DOF and quality all come from the final authoritative solve stage;
    // absent data stays zero rather than being reconstructed by the script.
    std::size_t nodes = 0;
    std::size_t dof = 0;
    double quality_min = 0.0;
    double quality_mean = 0.0;
    if (!cine.solve_stages.empty()) {
        nodes = cine.solve_stages.back().trace.n_nodes;
        dof = cine.solve_stages.back().trace.n_dof;
    }
    if (!cine.solve_insights.empty()) {
        quality_min = cine.solve_insights.back().quality_min;
        quality_mean = cine.solve_insights.back().quality_mean;
    }
    for (std::size_t i = 0; i < cine.stages.size(); ++i) {
        const auto& stage = cine.stages[i];
        std::printf("cinema: mesh_stage index %zu pass %d id %s elements %zu nodes %zu\n",
                    i, stage.pass, stage.stage.c_str(), stage.mesh.elements.size(),
                    stage.mesh.nodes.size());
    }
    for (std::size_t i = 0; i < cine.solve_stages.size(); ++i) {
        const auto& stage = cine.solve_stages[i];
        std::printf("cinema: solve_stage index %zu pass %d elements %zu nodes %zu dof %zu "
                    "global_eta %.9g h_mark %zu p_mark %zu shape_mark %zu\n",
                    i, stage.pass, stage.trace.n_elems, stage.trace.n_nodes,
                    stage.trace.n_dof, stage.trace.global_eta, stage.trace.n_h_mark,
                    stage.trace.n_p_mark, stage.trace.n_shape_mark);
    }
    const double stress_p99 =
        !cine.stress_histograms.empty() ? cine.stress_histograms.back().p99 : 0.0;
    const double error_p99 =
        !cine.error_histograms.empty() ? cine.error_histograms.back().p99 : 0.0;
    const double max_displacement = app.result ? app.result->max_displacement : 0.0;
    const double max_von_mises = app.result ? app.result->max_von_mises : 0.0;
    const double global_eta = app.result ? app.result->global_eta : 0.0;
    const double model_diagonal =
        app.model ? (app.model->bbox_max - app.model->bbox_min).norm() : 0.0;
    const double visible_displacement = app.deform_scale * max_displacement;
    const double visible_fraction =
        model_diagonal > 0.0 ? visible_displacement / model_diagonal : 0.0;
    std::printf(
        "cinema: record %s frames %d fps 60 candidates %zu stages %zu elements %zu "
        "nodes %zu dof %zu quality_min %.9g quality_mean %.9g youngs_pa %.9g "
        "poisson %.9g max_von_mises_pa %.9g stress_p99_pa %.9g global_eta %.9g "
        "error_p99 %.9g max_displacement_m %.9g deform_scale %.9g "
        "visible_displacement_m %.9g visible_fraction %.9g unchanged %zu "
        "removed %zu added %zu poster %d width %d height %d skipped %zu solve_stages %zu "
        "solver %s\n",
        cine.record_dir.c_str(), cine.record_frames, candidates, cine.stages.size(),
        app.viewport.cinema_element_count(), nodes, dof, quality_min, quality_mean,
        app.setup.youngs_modulus, app.setup.poissons_ratio, max_von_mises, stress_p99,
        global_eta, error_p99, max_displacement, app.deform_scale,
        visible_displacement, visible_fraction,
        app.viewport.cinema_unchanged_element_count(),
        app.viewport.cinema_removed_element_count(), app.viewport.cinema_added_element_count(),
        poster, fb_w, fb_h, app.viewport.cinema_skipped_element_count(),
        cine.solve_stages.size(), cinema_solver_token(cine));
    std::fflush(stdout);
    cine.record_dir.clear();
    glfwSwapInterval(1);
}

} // namespace polymesh::gui
