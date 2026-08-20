// SPDX-License-Identifier: BSD-3-Clause
#include "cinema.hpp"

#include "colormap.hpp"
#include "theme.hpp"

#include "geom/cad_topology.hpp"
#include "geom/features.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace polymesh::gui {
namespace {

/// Act spans as fractions of the take. The two data acts are equal because
/// neither is subordinate: the network's deliberation and the mesher's
/// construction are the two halves of the claim this video makes. The opening
/// outline and the closing field are cards, so they get a sixth each.
/// They sum to 1 by construction; `cinema_cue` asserts nothing and simply
/// clamps, because a recording is an exact frame count, not an exact act sum.
constexpr std::array<double, 4> kActFraction = {0.12, 0.38, 0.38, 0.12};

/// Connections drawn per frame. The deployed graph has 15,936 of them for this
/// layout; drawing thousands fills the column band with a solid grey haze that
/// buries the nodes and shows nothing, so only the strongest subset is drawn
/// and the count is disclosed on screen from THIS constant -- shrink it and the
/// on-screen sentence shrinks with it. A few hundred is where individual paths
/// stay separable at the sizes the recorder captures.
constexpr std::size_t kDrawnConnections = 280;

/// Widest a head-unit node is allowed to get, and the gap between it and its
/// label. Fixed so the label gutter can be reserved before the column height
/// is known.
constexpr float kNodeRadiusMax = 9.0f;
constexpr float kLabelGap = 8.0f;

/// printf into a std::string.
///
/// The ticker and the network disclosures have to exist as strings before
/// anything is drawn, so the composition can size itself from them. They keep
/// the printf format strings this surface already used: rewriting a hundred
/// conversions into another formatting library would be a hundred chances to
/// change a displayed number, which is the one thing this file may not do.
#if defined(__GNUC__)
[[gnu::format(printf, 1, 2)]]
#endif
std::string fmt(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list measure;
    va_copy(measure, args);
    const int n = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        std::vsnprintf(out.data(), static_cast<std::size_t>(n) + 1, format, args);
    }
    va_end(args);
    return out;
}

/// How the surface names the pass a beat is showing. ONE definition, used by
/// both the network panel and the ticker, so the two cannot label the same
/// instant differently.
///
/// Two indices are in play and both are real: the pass ordinal (1-based, over
/// every forward pass INCLUDING the final re-score) and
/// `advisor::ActivationFrame::candidate`, a 0-based index into the enumerated
/// candidate grid. Side by side without saying which is which they read as an
/// off-by-one, and naming the pass ordinal a candidate ordinal is wrong
/// outright: the grid holds `n_frames - 1` candidates -- the same count
/// `service_cinema_record` prints -- and the last pass is not one of them.
std::string pass_role(int candidate, std::size_t n_frames) {
    if (candidate < 0) {
        return "final re-score of the recommended action";
    }
    return fmt("candidate grid index %d of %zu (0-based)", candidate,
               n_frames > 0 ? n_frames - 1 : 0);
}

/// Cubic smoothstep on [0,1]. Used for opacity and the shrink collapse only --
/// never for a displayed number.
double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/// Overflow-safe logistic, the same branch-on-sign form as
/// `src/advisor/src/advisor.cpp`: exp of a large positive logit overflows to
/// inf and would turn the probability into NaN.
double sigmoid(double x) {
    if (x >= 0.0) {
        return 1.0 / (1.0 + std::exp(-x));
    }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

ImU32 rgba(const std::array<float, 3>& rgb, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
}

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

/// `h` as the HUD should state it: the setup's own number, or the fact that it
/// is resolved by the mesher. A cinema that printed a resolved h it had not
/// measured would be exactly the kind of plausible-looking invention this
/// feature exists to avoid.
std::string h_text(const CinemaHud& hud) {
    if (hud.mesh_size > 0.0) {
        return std::format("h {:.4g} mm (SimSetup::mesh_size)", hud.mesh_size * 1e3);
    }
    if (hud.geometry_h > 0.0) {
        return std::format("h auto -> {:.4g} mm (VolumeMeshOutput::geometry_h)",
                           hud.geometry_h * 1e3);
    }
    return "h auto (SimSetup::mesh_size = 0; nothing meshed yet, so no resolved h to report)";
}

} // namespace

const char* cinema_act_name(CinemaAct act) {
    switch (act) {
    case CinemaAct::kSkeleton:
        return "skeleton";
    case CinemaAct::kAdvisor:
        return "advisor";
    case CinemaAct::kMesh:
        return "mesh";
    case CinemaAct::kResult:
        return "result";
    }
    return "unknown"; // only reachable from an out-of-range int cast
}

// ---- take state -----------------------------------------------------------

void CinemaState::push_stage(const pipeline::MeshStage& stage) {
    // Worker thread. The stage's mesh is the worker's live buffer, so the copy
    // is mandatory, not a convenience.
    const std::lock_guard<std::mutex> lock(stage_mutex_);
    pending_stages_.push_back(stage);
}

void CinemaState::drain_stages() {
    std::vector<pipeline::MeshStage> pending;
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        if (pending_stages_.empty()) {
            return;
        }
        pending.swap(pending_stages_);
    }
    stages.insert(stages.end(), std::make_move_iterator(pending.begin()),
                  std::make_move_iterator(pending.end()));
}

void CinemaState::clear_stages() {
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        pending_stages_.clear();
    }
    stages.clear();
    uploaded_stage = -1;
}

void CinemaState::advance(double dt) {
    if (dt > 0.0) {
        t += dt;
    }
}

// ---- act sequencing -------------------------------------------------------

void cinema_act_window(const CinemaState& state, CinemaAct act, double& t0, double& t1) {
    const double total = std::max(state.duration, 1.0e-6);
    const auto want = static_cast<std::size_t>(act);
    double start = 0.0;
    for (std::size_t a = 0; a < kActFraction.size(); ++a) {
        const double span = kActFraction[a] * total;
        if (a == want) {
            t0 = start;
            t1 = start + span;
            return;
        }
        start += span;
    }
    t0 = total;
    t1 = total;
}

double cinema_opening_fade(const CinemaState& state) {
    const double skeleton_span = kActFraction[0] * std::max(state.duration, 1.0e-6);
    return std::min(CinemaState::kOpeningFade, 0.5 * skeleton_span);
}

CinemaCue cinema_cue(const CinemaState& state) {
    CinemaCue cue;
    const double total = std::max(state.duration, 1.0e-6);

    double start = 0.0;
    for (std::size_t a = 0; a < kActFraction.size(); ++a) {
        const double span = kActFraction[a] * total;
        const bool last = a + 1 == kActFraction.size();
        if (state.t < start + span || last) {
            cue.act = static_cast<CinemaAct>(a);
            cue.act_span = span;
            // Clamped, not wrapped: a clock that overran the schedule holds the
            // final instant of the act it is in.
            cue.act_t = std::clamp(state.t - start, 0.0, span);
            break;
        }
        start += span;
    }

    // The opening act is the part's. The network panel is held dark through the
    // first half of it and faded up across the second, so the take opens on the
    // outline rather than on a full-strength wall of connections. Pure opacity:
    // the panel's content at alpha 0.4 is the same content it has at alpha 1.
    if (cue.act == CinemaAct::kSkeleton) {
        const double half = 0.5 * cue.act_span;
        cue.network_alpha =
            static_cast<float>(smoothstep((cue.act_t - half) / std::max(half, 1.0e-9)));
    }

    // Advisor beats: one per forward pass, in the order the chooser ran them.
    // After the advisor act the index sticks at the last pass, so the network
    // stays lit with the decision while the mesh it chose is built -- that is
    // the causal link the video is about, and it is the same real data.
    std::size_t n_frames = 0;
#ifdef POLYMESH_WITH_ADVISOR
    if (state.explanation) {
        n_frames = state.explanation->frames.size();
    }
#endif
    if (n_frames > 0) {
        const double advisor_span = kActFraction[1] * total;
        const double beat = advisor_span / static_cast<double>(n_frames);
        if (cue.act == CinemaAct::kSkeleton) {
            cue.frame_index = -1;
        } else if (cue.act == CinemaAct::kAdvisor) {
            const auto i = static_cast<std::size_t>(cue.act_t / std::max(beat, 1.0e-9));
            cue.frame_index = static_cast<int>(std::min(i, n_frames - 1));
            cue.beat_seconds = beat;
        } else {
            cue.frame_index = static_cast<int>(n_frames - 1);
        }
    }

    // Mesh beats: one per collected construction stage, in emission order.
    if (!state.stages.empty()) {
        const std::size_t n = state.stages.size();
        const double mesh_span = kActFraction[2] * total;
        const double beat = mesh_span / static_cast<double>(n);
        if (cue.act == CinemaAct::kSkeleton || cue.act == CinemaAct::kAdvisor) {
            cue.stage_index = -1;
        } else if (cue.act == CinemaAct::kMesh) {
            const auto i = static_cast<std::size_t>(cue.act_t / std::max(beat, 1.0e-9));
            const std::size_t clamped = std::min(i, n - 1);
            cue.stage_index = static_cast<int>(clamped);
            // Linear in time on purpose: the on-screen spawn rate is then
            // proportional to the stage's own element count, so a stage that
            // added twice as many elements visibly takes twice the work.
            const double within = cue.act_t - static_cast<double>(clamped) * beat;
            cue.stage_reveal = std::clamp(within / std::max(beat, 1.0e-9), 0.0, 1.0);
            cue.beat_seconds = beat;
        } else {
            cue.stage_index = static_cast<int>(n - 1);
            cue.stage_reveal = 1.0;
        }
    }
    return cue;
}

Viewport::CinemaView cinema_view(const CinemaState& state, const CinemaCue& cue) {
    Viewport::CinemaView view;
    view.edges = true;
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        view.skeleton_alpha =
            static_cast<float>(smoothstep(state.t / std::max(cinema_opening_fade(state), 1.0e-9)));
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        break;
    case CinemaAct::kAdvisor:
        // Nothing has been meshed while the network deliberates, so nothing may
        // be drawn as mesh. The outline is the whole picture here.
        view.skeleton_alpha = 1.0f;
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        break;
    case CinemaAct::kMesh:
        // The outline stays as a dim reference frame so the growing fill can be
        // read against the part it is filling.
        view.skeleton_alpha = 0.45f;
        view.reveal = static_cast<float>(cue.stage_reveal);
        view.mesh_alpha = 1.0f;
        // Elements land shrunk toward their own centroid so each reads as a
        // separate cell as it appears, and close up to touching by the middle
        // of the stage. Geometry, not data: the element is the element.
        view.shrink =
            static_cast<float>(0.35 * (1.0 - smoothstep(std::min(1.0, cue.stage_reveal * 2.0))));
        break;
    case CinemaAct::kResult:
        view.skeleton_alpha = 0.25f;
        view.reveal = 1.0f;
        view.mesh_alpha = 1.0f;
        view.shrink = 0.0f;
        break;
    }
    return view;
}

DisplayMode cinema_display_mode(const CinemaState& /*state*/, const CinemaCue& cue,
                                bool has_result) {
    if (cue.act == CinemaAct::kResult && has_result) {
        return DisplayMode::kResultsVonMises;
    }
    return DisplayMode::kCinema;
}

void sync_cinema_viewport(CinemaState& state, const CinemaCue& cue, Viewport& viewport) {
    if (cue.stage_index >= 0 && cue.stage_index != state.uploaded_stage &&
        static_cast<std::size_t>(cue.stage_index) < state.stages.size()) {
        viewport.set_cinema_mesh(state.stages[static_cast<std::size_t>(cue.stage_index)].mesh);
        state.uploaded_stage = cue.stage_index;
    }
    viewport.set_cinema_view(cinema_view(state, cue));
}

// ---- act 1: the part's own skeleton --------------------------------------

void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model, Viewport& viewport) {
    std::vector<std::vector<Eigen::Vector3d>> polylines;
    state.skeleton_note.clear();
    state.skeleton_source = SkeletonSource::kNone;

    if (model.cad && !model.cad->empty()) {
        try {
            // 16 samples per edge: enough that a fillet or a bore rim reads as
            // a curve rather than a chord chain at the 1600 px the recorder
            // captures, and the same density the CLI's BRep wire export uses.
            const geom::CadTopology topology = geom::extract_topology(*model.cad, 16);
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
                    polylines.push_back({model.surface.vertices[edge.v0],
                                         model.surface.vertices[edge.v1]});
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
}

// ---- act 2: the network ---------------------------------------------------

bool load_cinema_advisor(CinemaState& state, const pipeline::Model& model,
                         pipeline::SimSetup& setup, const std::string& dir) {
    state.advisor_dir = dir;
    state.advisor_note.clear();
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
    return unavailable("this polymesh-gui was configured with POLYMESH_WITH_ADVISOR=OFF, so it "
                       "carries no inference module at all — reconfigure with "
                       "-DPOLYMESH_WITH_ADVISOR=ON");
#else
    state.explanation.reset();
    state.layout = advisor::NetworkLayout{};

    if (model.surface.triangles.empty()) {
        return unavailable("no part is loaded, so there is no feature row to run the network on");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path{dir}, ec)) {
        return unavailable(std::format("no such model directory: {}", dir));
    }

    // The feature row is built exactly the way the product path builds it
    // (apps/cli/main.cpp cmd_solve): fix/load context as region boxes, then
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
        state.explanation =
            advisor.explain(features, static_cast<double>(setup.max_dof));
    } catch (const std::exception& e) {
        return unavailable(e.what());
    }

    const auto& explanation = *state.explanation;
    const auto& decision = explanation.decision;

    // Applying the decision is what makes the causal claim true: the mesher
    // must execute the action the network chose, or the video would be showing
    // two unrelated things side by side. A refusal is NOT applied -- it is a
    // real outcome and is shown as one.
    if (decision.vetoed) {
        state.decision_note = std::format(
            "REFUSED and NOT applied ({}): ood_distance {:.4g}, failure_prob {:.4g}{} — the "
            "mesh act runs on the studio's own setup",
            decision.budget_refusal ? "no candidate fit the max_dof budget" : "vetoed",
            decision.ood_distance, decision.failure_prob,
            decision.note.empty() ? std::string{}
                                  : std::format(", note: {}", decision.note));
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
            "applied to SimSetup: mesher {} · h {:.4g} mm (h_rel {:.4g} × diag {:.4g} mm) · "
            "adapt {} · η target {:.4g} · order {}{}",
            decision.mesher, setup.mesh_size * 1e3, decision.h_rel, diag * 1e3,
            setup.adapt_passes, setup.eta_target, setup.p_elevate ? 2 : 1,
            decision.order > 2 ? std::format(" (order {} executed as quadratic)", decision.order)
                               : std::string{});
    } else {
        // Same refusal the CLI makes: meshing something other than what was
        // recommended, while reporting the recommendation, is the failure mode
        // this whole surface exists to rule out.
        state.decision_note = std::format(
            "NOT applied: advised mesher '{}' is not a name this build recognises "
            "(pipeline::mesher_from_name refused it) — the mesh act runs on the studio's own "
            "setup rather than silently meshing something else",
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
    std::fflush(stdout);
    return true;
#endif
}

void draw_cinema_network(CinemaState& state, const CinemaCue& cue) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
    ImGui::TextColored(palette.status_warn,
                       "no network to draw: this polymesh-gui was built with "
                       "POLYMESH_WITH_ADVISOR=OFF");
    ImGui::TextWrapped("%s", state.advisor_note.c_str());
#else
    if (!state.explanation || state.layout.empty()) {
        ImGui::TextColored(palette.status_warn, "no advisor forward pass to draw");
        ImGui::Spacing();
        if (state.advisor_note.empty()) {
            ImGui::TextWrapped(
                "run `cinema advisor <model dir>` — until then nothing is drawn here, because "
                "the only honest picture of a network that has not run is an empty one");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, palette.status_err);
            ImGui::TextWrapped("%s", state.advisor_note.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    const advisor::NetworkLayout& layout = state.layout;
    const auto& frames = state.explanation->frames;
    if (layout.layers.size() != 4 || layout.edges.size() != 3) {
        ImGui::TextColored(palette.status_err,
                           "activation_layout.json describes %zu layers and %zu weight blocks; "
                           "this surface draws the four-column trunk (input / trunk.fc1 / "
                           "trunk.fc2 / heads) and its three blocks, so it will not guess",
                           layout.layers.size(), layout.edges.size());
        return;
    }

    // The four activation vectors of the pass this beat is showing. Null before
    // the first beat: the structure is drawn, unlit, and said to be unlit.
    std::array<const std::vector<float>*, 4> values{nullptr, nullptr, nullptr, nullptr};
    const advisor::ActivationFrame* frame = nullptr;
    if (cue.frame_index >= 0 && static_cast<std::size_t>(cue.frame_index) < frames.size()) {
        frame = &frames[static_cast<std::size_t>(cue.frame_index)];
        values = {&frame->input, &frame->fc1, &frame->fc2, &frame->heads};
        for (std::size_t l = 0; l < 4; ++l) {
            if (values[l]->size() != layout.layers[l].size) {
                ImGui::TextColored(palette.status_err,
                                   "layer '%s' is %zu units in activation_layout.json but the "
                                   "graph tap returned %zu — the artifacts disagree, so nothing "
                                   "is drawn",
                                   layout.layers[l].name.c_str(), layout.layers[l].size,
                                   values[l]->size());
                return;
            }
        }
    }

    // ---- everything this panel will say, before anything is placed ------
    // The layout is sized FROM the text: the caption, the disclosure and the
    // head labels reserve exactly the room they need and the graph takes what
    // is left. Composing the strings first is what makes that possible, and a
    // clipped disclosure is a disclosure that was not made.

    // Per-layer normalisation. Trunk and head magnitudes differ by roughly a
    // factor of ten, so one shared scale would flatten the trunk into a flat
    // grey column — the same reason scripts/advisor/figures.py scales the
    // activation heatmap per row. The scales are printed below the graph.
    std::array<float, 4> layer_max{1.0f, 1.0f, 1.0f, 1.0f};
    for (std::size_t l = 0; l < values.size(); ++l) {
        if (values[l] == nullptr) {
            continue;
        }
        float m = 0.0f;
        for (const float v : *values[l]) {
            m = std::max(m, std::fabs(v));
        }
        layer_max[l] = m > 0.0f ? m : 1.0f;
    }

    // Connections ranked by |w_ji * a_i| for THIS frame: a large weight on a
    // silent unit carries nothing, so weight alone would be the wrong ranking.
    std::size_t total_connections = 0;
    for (const auto& block : layout.edges) {
        total_connections += block.rows * block.cols;
    }
    auto& picks = state.edge_scratch_;
    picks.clear();
    std::size_t drawn = 0;
    float value_max = 0.0f;
    if (frame != nullptr && total_connections > 0) {
        picks.reserve(total_connections);
        for (std::size_t b = 0; b < layout.edges.size(); ++b) {
            const auto& block = layout.edges[b];
            if (block.weights.size() != block.rows * block.cols ||
                block.cols != values[b]->size() || block.rows != values[b + 1]->size()) {
                continue; // a block that does not match the layers it joins is not drawn
            }
            const auto& src = *values[b];
            for (std::size_t j = 0; j < block.rows; ++j) {
                const float* row = block.weights.data() + j * block.cols;
                for (std::size_t i = 0; i < block.cols; ++i) {
                    const float v = row[i] * src[i];
                    value_max = std::max(value_max, std::fabs(v));
                    picks.push_back({std::fabs(v), v, static_cast<int>(b),
                                     static_cast<int>(i), static_cast<int>(j)});
                }
            }
        }
        drawn = std::min(kDrawnConnections, picks.size());
        if (drawn > 0) {
            std::nth_element(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(drawn) - 1,
                             picks.end(), [](const CinemaState::EdgePick& a,
                                             const CinemaState::EdgePick& b) {
                                 return a.rank > b.rank;
                             });
            // Weakest of the kept set first, so the strongest connections end
            // up on top instead of buried under near-silent ones.
            std::sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(drawn),
                      [](const CinemaState::EdgePick& a, const CinemaState::EdgePick& b) {
                          return a.rank < b.rank;
                      });
        }
    }

    const std::vector<CinemaLine> caption = {
        {palette.text_dim, fmt("deployed advisor graph — %s", state.advisor_dir.c_str())},
        {palette.text_dim,
         "node fills are model.onnx's own tensors: outputs trunk_input, trunk_fc1, "
         "trunk_fc2 (post-GELU) and the head vector — not a re-implementation of the "
         "forward pass"},
    };

    std::vector<CinemaLine> disclosure;
    if (frame == nullptr) {
        disclosure.push_back({palette.status_warn,
                              "structure only: no forward pass is being shown on this beat, so "
                              "every node is drawn at its minimum radius"});
    } else {
        // `drawn` is the real size of the subset that was just selected, so this
        // sentence tracks kDrawnConnections instead of restating it.
        disclosure.push_back(
            {palette.text_dim,
             fmt("showing the %zu strongest of %zu connections, by |weight × source "
                 "activation| for this frame (activation_layout.json edge blocks "
                 "%zu×%zu, %zu×%zu, %zu×%zu)",
                 drawn, total_connections, layout.edges[0].rows, layout.edges[0].cols,
                 layout.edges[1].rows, layout.edges[1].cols, layout.edges[2].rows,
                 layout.edges[2].cols)});
        disclosure.push_back(
            {palette.text_dim,
             fmt("node radius and fill are |a| / max|a| WITHIN each layer — input "
                 "±%.3g · trunk.fc1 ±%.3g · trunk.fc2 ±%.3g · heads ±%.3g — because "
                 "trunk and head magnitudes differ by ~10×, and one shared scale "
                 "would flatten the trunk",
                 static_cast<double>(layer_max[0]), static_cast<double>(layer_max[1]),
                 static_cast<double>(layer_max[2]), static_cast<double>(layer_max[3]))});
        disclosure.push_back(
            {palette.text_dim,
             fmt("pass %d of %zu · %s · blue → white → red is the sign of the "
                 "activation (colormap.hpp signed_colormap), and line opacity and width "
                 "are |weight × source activation| / max",
                 cue.frame_index + 1, frames.size(),
                 pass_role(frame->candidate, frames.size()).c_str())});
    }

    const auto& heads = layout.layers[3];
    std::vector<std::string> head_text(heads.size);
    for (std::size_t i = 0; i < heads.size; ++i) {
        const char* name = i < heads.labels.size() ? heads.labels[i].c_str() : "(unlabelled)";
        head_text[i] = values[3] != nullptr
                           ? std::format("{} {:+.4g}", name, (*values[3])[i])
                           : std::format("{} —", name);
    }

    // ---- geometry, derived from the measured text ------------------------
    const float fade = std::clamp(cue.network_alpha, 0.0f, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    // The prose is set smaller than the UI face so the graph gets the panel.
    // Nothing is dropped to make room: it wraps.
    const float body_size = std::floor(ImGui::GetFontSize() * 0.80f);
    const float para_gap = std::floor(body_size * 0.30f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    const auto faded = [fade](ImVec4 c) {
        c.w *= fade;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    const auto para_h = [&](const std::string& s) {
        return font->CalcTextSizeA(body_size, FLT_MAX, wrap, s.c_str()).y + para_gap;
    };
    const auto block_h = [&](const std::vector<CinemaLine>& lines) {
        float h = 0.0f;
        for (const auto& line : lines) {
            h += para_h(line.text);
        }
        return h;
    };
    const auto draw_block = [&](const std::vector<CinemaLine>& lines, float y) {
        for (const auto& line : lines) {
            dl->AddText(font, body_size, ImVec2(origin.x, y), faded(line.color),
                        line.text.c_str(), nullptr, wrap);
            y += para_h(line.text);
        }
    };

    const float caption_h = block_h(caption);
    const float disclosure_h = block_h(disclosure);
    const float graph_top = origin.y + caption_h;
    const float graph_h = std::max(140.0f, region.y - caption_h - disclosure_h);

    // Head labels are real unit names carrying real values, and they are long
    // (`policy_mesher_logit_graded_tet +5.722`). Set them at whatever size makes
    // the longest one fit the gutter the panel can afford, then reserve exactly
    // that gutter — so no label is ever cut off, at any panel width.
    const auto widest_label = [&](float size) {
        float w = 0.0f;
        for (const auto& t : head_text) {
            w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x);
        }
        return w;
    };
    float label_size = std::floor(ImGui::GetFontSize() * 0.78f);
    float label_w = widest_label(label_size);
    const float label_budget = std::max(80.0f, region.x * 0.42f);
    if (label_w > label_budget && label_w > 0.0f) {
        label_size = std::max(8.0f, std::floor(label_size * label_budget / label_w));
        label_w = widest_label(label_size);
    }

    // Enough left margin for the leftmost column's own centred header.
    constexpr float kColumnMargin = 30.0f;
    const float gutter = label_w + kNodeRadiusMax + kLabelGap + 4.0f;
    const float band_w = std::max(120.0f, region.x - kColumnMargin - gutter);
    const float header_h = std::floor(body_size * 1.35f);
    const float col_top = graph_top + header_h;
    const float col_h = std::max(60.0f, graph_h - header_h - 4.0f);

    // Columns span the whole band rather than sitting in quarter-slots: the
    // extra separation is what makes an individual connection followable.
    const auto column_x = [&](std::size_t c) {
        return origin.x + kColumnMargin +
               band_w * static_cast<float>(c) / static_cast<float>(values.size() - 1);
    };
    const auto node_y = [&](std::size_t i, std::size_t n) {
        return col_top + col_h * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    };

    draw_block(caption, origin.y);
    draw_block(disclosure, graph_top + graph_h);

    // ---- connections ----------------------------------------------------
    if (drawn > 0) {
        const float inv_max = value_max > 0.0f ? 1.0f / value_max : 0.0f;
        for (std::size_t k = 0; k < drawn; ++k) {
            const auto& pick = picks[k];
            const auto b = static_cast<std::size_t>(pick.block);
            const float t = std::clamp(pick.value * inv_max, -1.0f, 1.0f);
            const float weight = std::clamp(pick.rank * inv_max, 0.0f, 1.0f);
            // Sign is the colour (the same signed_colormap the nodes use);
            // opacity AND width are the normalised magnitude, so the paths that
            // actually carry this pass are the ones that read, and the rest are
            // hairlines instead of a uniform grey haze.
            dl->AddLine(ImVec2(column_x(b),
                               node_y(static_cast<std::size_t>(pick.src),
                                      layout.layers[b].size)),
                        ImVec2(column_x(b + 1),
                               node_y(static_cast<std::size_t>(pick.dst),
                                      layout.layers[b + 1].size)),
                        rgba(signed_colormap(t), (0.05f + 0.85f * weight) * fade),
                        0.55f + 1.35f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.3f;
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float x = column_x(l);
        const float spacing = col_h / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 1.8f, kNodeRadiusMax);
        const std::string header = std::format("{} {}", layer.name, layer.size);
        const float header_w = font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, header.c_str()).x;
        dl->AddText(font, body_size, ImVec2(x - 0.5f * header_w, graph_top),
                    faded(palette.text_dim), header.c_str());
        for (std::size_t i = 0; i < layer.size; ++i) {
            const float a = values[l] != nullptr ? (*values[l])[i] : 0.0f;
            const float mag = std::clamp(std::fabs(a) / layer_max[l], 0.0f, 1.0f);
            const float r = kNodeMin + (r_max - kNodeMin) * mag;
            const float y = node_y(i, layer.size);
            const auto rgb = signed_colormap(a / layer_max[l]);
            // Halo on the units carrying this pass, so the firing pattern is
            // legible over the connections. Opacity only: the radius is still
            // |a| / max|a| within the layer and nothing else.
            if (mag > 0.30f) {
                dl->AddCircleFilled(ImVec2(x, y), r * 3.0f, rgba(rgb, 0.055f * mag * fade));
                dl->AddCircleFilled(ImVec2(x, y), r * 1.8f, rgba(rgb, 0.110f * mag * fade));
            }
            dl->AddCircleFilled(ImVec2(x, y), r, rgba(rgb, (0.40f + 0.60f * mag) * fade));
            if (mag > 0.55f) {
                dl->AddCircle(ImVec2(x, y), r + 1.4f,
                              ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.45f * mag * fade)),
                              0, 1.2f);
            }
        }
    }

    // ---- head units, with their real names and real values --------------
    {
        const float head_r = std::clamp(
            0.46f * col_h / static_cast<float>(std::max<std::size_t>(heads.size, 1)), 1.8f,
            kNodeRadiusMax);
        const float x = column_x(3) + head_r + kLabelGap;
        const ImU32 col = faded(palette.text);
        for (std::size_t i = 0; i < heads.size; ++i) {
            dl->AddText(font, label_size,
                        ImVec2(x, node_y(i, heads.size) - 0.5f * label_size), col,
                        head_text[i].c_str());
        }
    }

    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
#endif
}

// ---- ticker ---------------------------------------------------------------

namespace {

void push_line(std::vector<CinemaLine>& out, const ImVec4& color, std::string text) {
    out.push_back({color, std::move(text)});
}

void skeleton_ticker(const CinemaState& state, std::vector<CinemaLine>& out) {
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        push_line(out, palette.status_ok,
                  fmt("outline source: BRep feature edges — "
                      "geom::extract_topology(*model.cad, 16), %zu edge polylines / %zu "
                      "sampled points",
                      state.skeleton_polylines, state.skeleton_points));
        break;
    case SkeletonSource::kSharpEdges:
        push_line(out, palette.status_warn,
                  fmt("outline source: tessellation crease network — "
                      "geom::detect_sharp_edges(model.surface, 30°), %zu segments. This "
                      "part carries no BRep (STL input), so this is NOT a CAD skeleton",
                      state.skeleton_polylines));
        break;
    case SkeletonSource::kUnavailable:
        push_line(out, palette.status_err,
                  fmt("outline source: UNAVAILABLE — %s. Nothing is drawn in its place",
                      state.skeleton_note.c_str()));
        break;
    case SkeletonSource::kNone:
        push_line(out, palette.status_err,
                  "outline source: none — no part is loaded, so there is nothing to outline");
        break;
    }
}

void advisor_ticker(const CinemaState& state, const CinemaCue& cue,
                    std::vector<CinemaLine>& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
    push_line(out, palette.status_err,
              "advisor act skipped: built with POLYMESH_WITH_ADVISOR=OFF");
    if (!state.advisor_note.empty()) {
        push_line(out, palette.text_dim, state.advisor_note);
    }
#else
    if (!state.explanation) {
        push_line(out, palette.status_err,
                  fmt("advisor act skipped: %s",
                      state.advisor_note.empty()
                          ? "no `cinema advisor <dir>` was run for this take"
                          : state.advisor_note.c_str()));
        push_line(out, palette.text_dim,
                  "no decision is shown, and none was applied — the mesh act runs on "
                  "the studio's own setup");
        return;
    }
    const auto& explanation = *state.explanation;
    const auto& frames = explanation.frames;
    if (cue.frame_index >= 0 && static_cast<std::size_t>(cue.frame_index) < frames.size()) {
        const auto& f = frames[static_cast<std::size_t>(cue.frame_index)];
        const double failure_prob = sigmoid(f.outputs.failure_logit);
        push_line(out, palette.text,
                  fmt("pass %d/%zu — %s — scoring mesher %s · h_rel %.4g · order %d · "
                      "adapt %d · η target %.4g · p_elevate %d",
                      cue.frame_index + 1, frames.size(),
                      pass_role(f.candidate, frames.size()).c_str(), f.action.mesher.c_str(),
                      f.action.h_rel, f.action.order, f.action.adapt_passes,
                      f.action.eta_target, f.action.p_elevate ? 1 : 0));
        push_line(
            out, palette.text_dim,
            fmt("rel_err_rel %+.5g (ranking key, per-case, lower is better) · dof 10^%.3f = "
                "%.0f · mesh %.0f ms · solve %.0f ms · geo chamfer 10^%.3f",
                f.score, f.outputs.dof_log10, std::pow(10.0, f.outputs.dof_log10),
                std::pow(10.0, f.outputs.mesh_ms_log10),
                std::pow(10.0, f.outputs.solve_ms_log10), f.outputs.geo_chamfer_log10));
        // `ranked` and `over_budget` are candidate-loop bookkeeping: the final
        // re-score pass is not a candidate, so both stay false there and
        // reporting them would say "not ranked, within budget" about a pass
        // that was never a candidate in the first place.
        const std::string bookkeeping =
            f.candidate < 0
                ? std::string(" · re-score of the action the ranking already chose")
                : std::format(" · {} · {}",
                              f.over_budget ? "over the max_dof budget"
                                            : "within the max_dof budget",
                              f.ranked ? "ranked" : "not ranked (score was not finite)");
        push_line(out, f.gate_pass ? palette.status_ok : palette.status_warn,
                  fmt("gate σ(failure_logit) %.4g %s threshold %.4g → %s%s%s", failure_prob,
                      f.gate_pass ? "≤" : ">", explanation.gate_threshold,
                      f.gate_pass ? "PASS" : "DROPPED", bookkeeping.c_str(),
                      f.recommended ? " · THIS IS THE RECOMMENDED ACTION" : ""));
    } else {
        push_line(out, palette.text_dim,
                  fmt("%zu forward passes recorded; the first beat has not started",
                      frames.size()));
    }
    // The decision itself, once the last pass has been reached. Before that the
    // ticker must not pre-announce an outcome the beats have not shown.
    const bool locked =
        cue.act != CinemaAct::kAdvisor ||
        (!frames.empty() && cue.frame_index == static_cast<int>(frames.size()) - 1);
    if (locked) {
        const auto& d = explanation.decision;
        push_line(out, d.vetoed ? palette.status_err : palette.status_ok,
                  fmt("DECISION mesher %s · h_rel %.4g · order %d · adapt %d · η %.4g · "
                      "p_elevate %d · failure_prob %.4g · ood_distance %.4g%s%s",
                      d.mesher.c_str(), d.h_rel, d.order, d.adapt_passes, d.eta_target,
                      d.p_elevate ? 1 : 0, d.failure_prob, d.ood_distance,
                      d.vetoed ? " · REFUSED" : "", d.clamped ? " · clamped" : ""));
        push_line(out, state.decision_applied ? palette.status_ok : palette.status_warn,
                  state.decision_note);
    }
#endif
}

void mesh_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                 std::vector<CinemaLine>& out) {
    if (state.stages.empty()) {
        push_line(out, palette.status_err,
                  "no construction stages were collected: "
                  "pipeline::SolveJob::on_mesh_stage received none");
        push_line(out, palette.text_dim,
                  "the stage sink is installed only while `cinema on`, and only the "
                  "graded tet fill emits stages — run `cinema on` before `solve`. "
                  "Nothing is drawn in place of stages that were never emitted");
        return;
    }
    const auto idx = static_cast<std::size_t>(std::max(cue.stage_index, 0));
    const auto& stage = state.stages[idx];
    push_line(out, palette.text,
              fmt("stage %zu/%zu '%s' — emission index %d, adapt pass %d, %zu elements / "
                  "%zu nodes in this stage",
                  idx + 1, state.stages.size(), stage.stage.c_str(), stage.index, stage.pass,
                  stage.mesh.elements.size(), stage.mesh.nodes.size()));
    // The viewport draws every element whose index is below reveal * count, so
    // the drawn count is that product -- exact arithmetic on two real numbers,
    // not an estimate of progress.
    const auto drawn =
        static_cast<std::size_t>(cue.stage_reveal * static_cast<double>(hud.cinema_elements));
    push_line(out, palette.text_dim,
              fmt("reveal %.0f%% — %zu of %zu drawable elements, appearing in the mesher's "
                  "own emission order (their index in mesh.elements): nothing is sorted, "
                  "and no element is drawn before the stage that built it",
                  100.0 * cue.stage_reveal, drawn, hud.cinema_elements));
    if (hud.cinema_skipped_elements > 0) {
        push_line(out, palette.status_warn,
                  fmt("%zu of this stage's %zu elements could not be triangulated for "
                      "drawing (degenerate connectivity or faceless poly cells) and are "
                      "absent from the reveal",
                      hud.cinema_skipped_elements, stage.mesh.elements.size()));
    }
}

void result_ticker(const CinemaHud& hud, std::vector<CinemaLine>& out) {
    if (!hud.has_result) {
        push_line(out, palette.status_warn,
                  "no solve result in this take — holding the final fill stage. "
                  "Nothing is drawn in place of a field that was not computed");
        return;
    }
    push_line(out, palette.status_ok,
              fmt("real von Mises field — max %.4g MPa · max |u| %.4g mm · ZZ η global "
                  "%.4g · %zu elements / %zu nodes / %zu DOF",
                  hud.max_von_mises / 1e6, hud.max_displacement * 1e3, hud.global_eta,
                  hud.elements, hud.nodes, hud.dof));
    push_line(out, palette.text_dim,
              fmt("nodal values are SolveResult::von_mises; the deformed shape is the "
                  "solved displacement drawn at %.4g× (App::deform_scale — true scale on "
                  "this part is invisible at %.4g mm peak)",
                  hud.deform_scale, hud.max_displacement * 1e3));
}

/// Flows the header chips across rows `wrap_width` wide with `gap` between
/// them, drawing when `draw` is set. Returns the row count. One function for
/// both passes, so the height the strip reserves is the height it uses.
int flow_chips(const std::vector<CinemaLine>& chips, float wrap_width, float gap, bool draw) {
    int rows = 1;
    float x = 0.0f;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const float w = ImGui::CalcTextSize(chips[i].text.c_str()).x;
        const float advance = (i == 0 || x <= 0.0f) ? w : gap + w;
        if (i > 0 && x > 0.0f && x + advance > wrap_width) {
            ++rows;
            x = w;
            if (draw) {
                ImGui::TextColored(chips[i].color, "%s", chips[i].text.c_str());
            }
            continue;
        }
        if (draw) {
            if (i > 0 && x > 0.0f) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::TextColored(chips[i].color, "%s", chips[i].text.c_str());
        }
        x += advance;
    }
    return rows;
}

constexpr float kTickerChipGap = 14.0f;

} // namespace

std::vector<CinemaLine> cinema_ticker_chips(const CinemaState& state, const CinemaCue& cue) {
    std::vector<CinemaLine> chips;
    push_line(chips, palette.accent,
              fmt("act %d/4 %s", static_cast<int>(cue.act) + 1, cinema_act_name(cue.act)));
    push_line(chips, palette.text_dim,
              fmt("t %.3f / %.3f s (virtual clock)", state.t, state.duration));
    if (cue.beat_seconds > 0.0) {
        push_line(chips, palette.text_dim,
                  fmt("beat %.3f s = %.1f frames at 60 fps", cue.beat_seconds,
                      cue.beat_seconds * 60.0));
    }
    if (state.recording()) {
        push_line(chips, palette.status_warn,
                  fmt("recording frame %d/%d", state.record_next + 1, state.record_frames));
    }
    return chips;
}

std::vector<CinemaLine> cinema_ticker_body(const CinemaState& state, const CinemaCue& cue,
                                           const CinemaHud& hud) {
    std::vector<CinemaLine> body;
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        skeleton_ticker(state, body);
        break;
    case CinemaAct::kAdvisor:
        advisor_ticker(state, cue, body);
        break;
    case CinemaAct::kMesh:
        mesh_ticker(state, cue, hud, body);
        break;
    case CinemaAct::kResult:
        result_ticker(hud, body);
        break;
    }
    // The HUD reports the setup that is actually meshing, sourced from
    // SimSetup, never from the decision struct: the two agree only when the
    // decision was applied, and when they disagree the setup is the truth.
    push_line(body, palette.text,
              fmt("part %s · mesher %s · %s · order %d · adapt %d · η target %.4g · "
                  "elements %zu · nodes %zu · DOF %zu",
                  hud.part.empty() ? "(none loaded)" : hud.part.c_str(), hud.mesher.c_str(),
                  h_text(hud).c_str(), hud.order, hud.adapt_passes, hud.eta_target, hud.elements,
                  hud.nodes, hud.dof));
    if (hud.stamp.empty()) {
        push_line(body, palette.text_dim,
                  "POLYMESH_CINEMA_STAMP not set — no provenance line was supplied to this run");
    } else {
        push_line(body, palette.text_dim, hud.stamp);
    }
    return body;
}

float cinema_ticker_height(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                           float wrap_width) {
    const float wrap = std::max(80.0f, wrap_width);
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const auto chips = cinema_ticker_chips(state, cue);
    float h = static_cast<float>(flow_chips(chips, wrap, kTickerChipGap, false)) *
              ImGui::GetTextLineHeightWithSpacing();
    for (const auto& line : cinema_ticker_body(state, cue, hud)) {
        h += ImGui::CalcTextSize(line.text.c_str(), nullptr, false, wrap).y + spacing;
    }
    // Plus the strip's own vertical window padding, both edges.
    return std::floor(h + 2.0f * kTickerPadY);
}

float cinema_ticker_reserve(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                            float wrap_width) {
    float want = 0.0f;
    for (int a = 0; a < 4; ++a) {
        CinemaCue probe = cue;
        probe.act = static_cast<CinemaAct>(a);
#ifdef POLYMESH_WITH_ADVISOR
        if (probe.act == CinemaAct::kAdvisor && state.explanation &&
            !state.explanation->frames.empty()) {
            // Tallest advisor beat: on the last pass the DECISION row and the
            // applied/refused note join the three per-pass rows.
            probe.frame_index = static_cast<int>(state.explanation->frames.size()) - 1;
        }
#endif
        want = std::max(want, cinema_ticker_height(state, probe, hud, wrap_width));
    }
    // High-water mark, never lowered inside a take: a strip that shrank would
    // grow the viewport pane and resize the part, which is the same cut as a
    // strip that grew.
    state.ticker_reserve = std::max(state.ticker_reserve, want);
    return state.ticker_reserve;
}

void draw_cinema_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud) {
    const float wrap = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    flow_chips(cinema_ticker_chips(state, cue), wrap, kTickerChipGap, true);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap);
    for (const auto& line : cinema_ticker_body(state, cue, hud)) {
        ImGui::TextColored(line.color, "%s", line.text.c_str());
    }
    ImGui::PopTextWrapPos();
}

} // namespace polymesh::gui
