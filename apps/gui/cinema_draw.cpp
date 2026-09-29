// SPDX-License-Identifier: BSD-3-Clause
// Cinema analysis pane: spectral features, activation network, cell
// microscope and solver equations, cross-faded inside one rectangle.
#include "cinema.hpp"
#include "cinema_internal.hpp"

#include "colormap.hpp"
#include "theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace polymesh::gui {

using detail::faded;
using detail::fmt;
using detail::grouped;
using detail::initial_fill_stage_count;
using detail::mesh_stage_plain;
using detail::mesher_plain;
using detail::smoothstep;

namespace detail {

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

std::string grouped(std::size_t n) {
    std::string digits = std::format("{}", n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (i - lead) % 3 == 0 && i >= lead) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

std::string_view mesher_plain(std::string_view mesher) {
    if (mesher == "graded_tet") {
        return "graded tets";
    }
    if (mesher == "hybrid_zoo") {
        return "hybrid: hex bulk, pyramid skin";
    }
    if (mesher == "hybrid_vem") {
        return "hybrid with polyhedral transitions";
    }
    if (mesher == "hex" || mesher == "hex_fill") {
        return "hexes";
    }
    if (mesher == "hex_vem") {
        return "hexes with polyhedral transitions";
    }
    if (mesher == "tet") {
        return "tets";
    }
    return mesher;
}

std::string_view mesh_stage_plain(std::string_view stage) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kNames{{
        {"lattice", "seed lattice"},
        {"expand", "cell expansion"},
        {"snap", "CAD projection"},
        {"peel", "quality repair"},
        {"reproject", "boundary recovery"},
        {"smooth", "surface smoothing"},
        {"resnap", "CAD resnap"},
        {"pin", "feature pinning"},
        {"fill", "solver cells"},
        {"ship", "validated mesh"},
    }};
    for (const auto& [id, plain] : kNames) {
        if (stage == id) {
            return plain;
        }
    }
    return stage;
}

ImU32 faded(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

} // namespace detail

namespace {

#ifdef POLYMESH_WITH_ADVISOR
/// Connections drawn per frame: only the strongest subset, because drawing
/// every connection of the deployed graph fills the lanes with a grey haze.
/// The on-screen legend discloses the count from this same constant.
constexpr std::size_t kDrawnConnections = 180;

/// Widest an activation node is allowed to get.
constexpr float kNodeRadiusMax = 8.0f;

/// Plain-English names for the deployed model's own head tensor labels, read
/// from `activation_layout.json` (the artifact is never rewritten). The mapping
/// is disclosed on screen and in docs/assets/cinema/NOTES.md. An unmapped label
/// falls through verbatim rather than being prettified by a rule.
struct HeadName {
    std::string_view tensor;
    const char* plain;
};
constexpr std::array<HeadName, 20> kHeadNames{{
    {"rel_err", "predicted error"},
    {"rel_err_rel", "error vs this part's median"},
    {"geo_chamfer", "mesh-to-CAD distance"},
    {"geo_p99", "mesh-to-CAD worst 1%"},
    {"dof", "unknowns"},
    {"mesh_ms", "meshing time"},
    {"solve_ms", "solve time"},
    {"solve_flops", "portable solve work"},
    {"solve_bytes", "portable data traffic"},
    {"mesh_work", "host-normalized meshing work"},
    {"failure_logit", "failure risk"},
    {"policy_h_rel", "cell size"},
    {"policy_adapt_passes", "refinement passes"},
    {"policy_eta_target", "error target"},
    {"policy_order_logit_1", "order 1 (linear)"},
    {"policy_order_logit_2", "order 2 (quadratic)"},
    {"policy_mesher_logit_graded_tet", "mesher: graded tets"},
    {"policy_mesher_logit_hex", "mesher: hex"},
    {"policy_mesher_logit_hybrid_vem", "mesher: hybrid VEM"},
    {"policy_mesher_logit_hybrid_zoo", "mesher: hybrid, hex + pyramids"},
}};

[[maybe_unused]] std::string_view head_name(std::string_view tensor) {
    for (const auto& entry : kHeadNames) {
        if (entry.tensor == tensor) {
            return entry.plain;
        }
    }
    return tensor;
}
#endif

ImU32 rgba(const std::array<float, 3>& rgb, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
}

/// One run of text inside a drawn line: the size multiplier and the baseline
/// offset are what give this surface real subscripts and superscripts without
/// requiring the font atlas to carry the Unicode ones. Liberation Sans, the
/// first fallback face on Linux, does not have U+2081 or U+207B, and an
/// equation that renders "sigma-box-box" is worse than no equation.
struct Run {
    std::string text;
    float scale = 1.0f; // of the line's size
    float rise = 0.0f;  // of the line's size; negative is up
    ImVec4 color{1, 1, 1, 1};
};

using Runs = std::vector<Run>;

Run plain(std::string text, ImVec4 color) { return {std::move(text), 1.0f, 0.0f, color}; }
Run sub(std::string text, ImVec4 color) { return {std::move(text), 0.62f, 0.30f, color}; }
Run sup(std::string text, ImVec4 color) { return {std::move(text), 0.62f, -0.34f, color}; }

float runs_width(ImFont* font, float size, const Runs& runs) {
    float w = 0.0f;
    for (const auto& run : runs) {
        w += font->CalcTextSizeA(size * run.scale, FLT_MAX, 0.0f, run.text.c_str()).x;
    }
    return w;
}

void draw_runs(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const Runs& runs,
               float alpha) {
    float x = at.x;
    for (const auto& run : runs) {
        const float s = size * run.scale;
        dl->AddText(font, s, ImVec2(x, at.y + run.rise * size + (size - s) * 0.5f),
                    faded(run.color, alpha), run.text.c_str());
        x += font->CalcTextSizeA(s, FLT_MAX, 0.0f, run.text.c_str()).x;
    }
}

} // namespace

void draw_cinema_equations(const CinemaState& state, const CinemaCue& cue,
                           const CinemaType& type, const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();

    const pipeline::SolveStage* stage = nullptr;
    std::size_t stage_index = 0;
    if (cue.solve_stage_index >= 0 &&
        static_cast<std::size_t>(cue.solve_stage_index) < state.solve_stages.size()) {
        stage_index = static_cast<std::size_t>(cue.solve_stage_index);
        stage = &state.solve_stages[stage_index];
    }

    const SolvePhase phase = cue.solve_phase;
    const bool on_stress =
        phase == SolvePhase::kStressSweep || phase == SolvePhase::kStressHold;
    const bool on_gradient =
        phase == SolvePhase::kGradientSweep || phase == SolvePhase::kGradientHold;
    const bool on_error = phase == SolvePhase::kError || phase == SolvePhase::kErrorHold;
    const bool on_refine = phase == SolvePhase::kRefine || phase == SolvePhase::kRefineHold;
    const bool on_ramp = phase == SolvePhase::kLoadRamp || phase == SolvePhase::kHold;

    Runs equation{plain("K u = f", palette.accent)};
    const CinemaHistogram* histogram = nullptr;
    const char* histogram_unit = "";
    double histogram_scale = 1.0;
    if (stage != nullptr && on_stress) {
        equation = {plain("K u = f   ·   ε = B u   ·   σ = D ε   ·   σ", palette.text),
                    sub("vm", palette.text)};
        if (stage_index < state.stress_histograms.size()) {
            histogram = &state.stress_histograms[stage_index];
            histogram_unit = "MPa";
            histogram_scale = 1e-6;
        }
    } else if (stage != nullptr && on_gradient) {
        equation = {plain("|∇σ", palette.text), sub("vm", palette.text),
                    plain("|", palette.text)};
        histogram = &const_cast<CinemaState&>(state).gradient_histogram(stage_index);
        histogram_unit = "MPa/mm";
        histogram_scale = 1e-9;
    } else if (stage != nullptr && on_error) {
        equation = {plain("η", palette.text),          sub("e", palette.text),
                    plain(" = ‖σ* − σ", palette.text), sub("h", palette.text),
                    plain("‖", palette.text),          sub("E,e", palette.text),
                    plain(" / ‖σ", palette.text),      sub("h", palette.text),
                    plain("‖", palette.text),          sub("E,Ω", palette.text)};
        if (stage_index < state.error_histograms.size()) {
            histogram = &state.error_histograms[stage_index];
            histogram_unit = "% local η";
            histogram_scale = 100.0;
        }
    } else if (stage != nullptr && on_refine) {
        equation = {plain("Σ", palette.text),  sub("marked", palette.text),
                    plain(" η", palette.text), sub("e", palette.text),
                    sup("2", palette.text),    plain(" ≥ θ Σ", palette.text),
                    sub("all", palette.text),  plain(" η", palette.text),
                    sub("e", palette.text),    sup("2", palette.text)};
    } else if (stage != nullptr && on_ramp) {
        equation = {plain("u(λ) = λu(1)   ·   σ(λ) = λσ(1)   ·   f(λ) = λf(1)", palette.text)};
    }

    const float equation_y = origin.y + 10.0f;
    const float equation_w = runs_width(font, type.caption, equation);
    const float equation_size = equation_w > region.x && equation_w > 0.0f
                                    ? std::max(12.0f, type.caption * region.x / equation_w)
                                    : type.caption;
    draw_runs(dl, font, equation_size, ImVec2(origin.x, equation_y), equation, alpha);

    const float pipeline_h = type.legend * 3.4f;
    const float chart_top = equation_y + type.caption * 2.0f;
    const float chart_bottom = origin.y + region.y - pipeline_h;
    const float chart_h = std::max(180.0f, chart_bottom - chart_top);
    const ImVec2 chart_min(origin.x, chart_top);
    const ImVec2 chart_max(origin.x + region.x, chart_top + chart_h);
    dl->AddRectFilled(chart_min, chart_max, faded(palette.panel_bg, 0.62f * alpha), 8.0f);
    dl->AddRect(chart_min, chart_max, faded(palette.border, alpha), 8.0f);

    const float left = chart_min.x + 28.0f;
    const float right = chart_max.x - 18.0f;
    const float top = chart_min.y + 30.0f;
    const float bottom = chart_max.y - 38.0f;
    for (int i = 1; i < 4; ++i) {
        const float y = top + (bottom - top) * static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(left, y), ImVec2(right, y), faded(palette.border, 0.42f * alpha),
                    1.0f);
    }
    dl->AddLine(ImVec2(left, bottom), ImVec2(right, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);
    dl->AddLine(ImVec2(left, top), ImVec2(left, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);

    if (histogram != nullptr && histogram->samples > 0) {
        const float plot_w = right - left;
        const float plot_h = bottom - top;
        const double span = histogram->p99 - histogram->min;
        const auto point = [&](std::size_t i) {
            const float x = left + plot_w * static_cast<float>(i) /
                                       static_cast<float>(histogram->quantiles.size() - 1);
            const double value = histogram->quantiles[i];
            const float y =
                span > 0.0 ? bottom - plot_h * std::clamp(static_cast<float>(
                                                              (value - histogram->min) / span),
                                                          0.0f, 1.0f)
                           : bottom;
            return ImVec2(x, y);
        };
        for (std::size_t i = 1; i < histogram->quantiles.size(); ++i) {
            const double value = histogram->quantiles[i];
            const float t =
                span > 0.0 ? std::clamp(static_cast<float>((value - histogram->min) / span),
                                        0.0f, 1.0f)
                           : 0.0f;
            dl->AddLine(point(i - 1), point(i), rgba(fea_colormap(t), 0.94f * alpha), 3.2f);
        }
        const float mean_y =
            span > 0.0
                ? bottom - plot_h * std::clamp(static_cast<float>(
                                                   (histogram->mean - histogram->min) / span),
                                               0.0f, 1.0f)
                : bottom;
        dl->AddLine(ImVec2(left, mean_y), ImVec2(right, mean_y),
                    faded(palette.accent, 0.70f * alpha), 1.5f);
        const std::string minimum =
            fmt("min %.3g %s", histogram->min * histogram_scale, histogram_unit);
        const std::string maximum =
            fmt("p99 %.3g %s", histogram->p99 * histogram_scale, histogram_unit);
        const std::string mean =
            fmt("mean %.3g %s", histogram->mean * histogram_scale, histogram_unit);
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, bottom - type.legend * 1.35f),
                    faded(palette.text_dim, alpha), minimum.c_str());
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, top + 4.0f),
                    faded(palette.text_dim, alpha), maximum.c_str());
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, mean_y - type.legend * 1.2f),
                    faded(palette.accent, alpha), mean.c_str());
        const char* percentile = "node percentile  0 → 99";
        const float label_w = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, percentile).x;
        dl->AddText(font, type.legend, ImVec2(0.5f * (left + right - label_w), bottom + 8.0f),
                    faded(palette.text_dim, alpha), percentile);
    } else if (stage != nullptr && on_error) {
        const double measured = stage->trace.global_eta * 100.0;
        const double target = hud.eta_target * 100.0;
        const double scale = std::max({measured, target, 1.0e-12});
        const std::array<double, 2> values{{target, measured}};
        const std::array<const char*, 2> labels{{"η*", "ηZZ"}};
        const std::array<ImVec4, 2> colors{{palette.text_dim, palette.accent}};
        const float bar_w = (right - left) * 0.22f;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float x = left + (right - left) * (0.23f + 0.54f * static_cast<float>(i));
            const float h = (bottom - top) * static_cast<float>(values[i] / scale);
            dl->AddRectFilled(ImVec2(x - 0.5f * bar_w, bottom - h),
                              ImVec2(x + 0.5f * bar_w, bottom),
                              faded(colors[i], 0.82f * alpha), 5.0f);
            const std::string value = fmt("%.3g%%", values[i]);
            dl->AddText(font, type.label,
                        ImVec2(x - 0.5f * bar_w, bottom - h - type.label * 1.35f),
                        faded(colors[i], alpha), value.c_str());
            dl->AddText(font, type.legend, ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
                        faded(palette.text_dim, alpha), labels[i]);
        }
    } else if (stage != nullptr && on_refine) {
        const std::size_t before = stage->trace.n_elems;
        const std::size_t after = stage_index + 1 < state.solve_stages.size()
                                      ? state.solve_stages[stage_index + 1].trace.n_elems
                                      : before;
        const std::array<std::size_t, 2> counts{{before, after}};
        const std::array<const char*, 2> labels{{"n", "n+1"}};
        const std::size_t max_count = std::max<std::size_t>({before, after, 1});
        const float bar_w = (right - left) * 0.24f;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const float x = left + (right - left) * (0.24f + 0.52f * static_cast<float>(i));
            const float h =
                (bottom - top) * static_cast<float>(counts[i]) / static_cast<float>(max_count);
            dl->AddRectFilled(
                ImVec2(x - 0.5f * bar_w, bottom - h), ImVec2(x + 0.5f * bar_w, bottom),
                faded(i == 0 ? palette.text_dim : palette.accent, 0.82f * alpha), 5.0f);
            const std::string value = grouped(counts[i]) + " cells";
            dl->AddText(font, type.label,
                        ImVec2(x - 0.5f * bar_w, bottom - h - type.label * 1.35f),
                        faded(i == 0 ? palette.text : palette.accent, alpha), value.c_str());
            dl->AddText(font, type.legend, ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
                        faded(palette.text_dim, alpha), labels[i]);
        }
    } else if (stage != nullptr && on_ramp) {
        dl->AddLine(ImVec2(left, bottom), ImVec2(right, top),
                    faded(palette.accent_soft_top, 0.88f * alpha), 6.0f);
        dl->AddLine(ImVec2(left, bottom), ImVec2(right, top),
                    faded(palette.status_warn, 0.92f * alpha), 2.2f);
        const float x = left + (right - left) * static_cast<float>(cue.load_factor);
        const float y = bottom - (bottom - top) * static_cast<float>(cue.load_factor);
        dl->AddCircleFilled(ImVec2(x, y), 9.0f, faded(palette.accent, alpha));
        dl->AddCircle(ImVec2(x, y), 15.0f, faded(palette.accent_soft_top, 0.55f * alpha), 0,
                      2.0f);
    }

    int active = 0;
    if (on_stress) {
        active = 1;
    } else if (on_gradient) {
        active = 2;
    } else if (on_error) {
        active = 3;
    } else if (on_refine) {
        active = 4;
    } else if (on_ramp) {
        active = 5;
    }
    const float rail_y = chart_max.y + type.legend * 1.15f;
    const float rail_left = origin.x + 20.0f;
    const float rail_right = origin.x + region.x - 20.0f;
    dl->AddLine(ImVec2(rail_left, rail_y), ImVec2(rail_right, rail_y),
                faded(palette.text_dim, 0.45f * alpha), 2.0f);
    for (int i = 0; i < 6; ++i) {
        const float x = rail_left + (rail_right - rail_left) * static_cast<float>(i) / 5.0f;
        const bool lit = i <= active;
        dl->AddCircleFilled(ImVec2(x, rail_y), lit ? 7.0f : 4.5f,
                            faded(lit ? palette.accent : palette.text_dim, alpha));
        if (i == active) {
            dl->AddCircle(ImVec2(x, rail_y), 13.0f,
                          faded(palette.accent_soft_top, 0.65f * alpha), 0, 2.0f);
        }
    }
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_network(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                         float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
    (void)type;
    ImGui::TextColored(palette.status_warn, "advisor support is not compiled into this build");
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
        ImGui::TextColored(
            palette.status_err,
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
                ImGui::TextColored(
                    palette.status_err,
                    "layer '%s' is %zu units in activation_layout.json but the "
                    "graph tap returned %zu — the artifacts disagree, so nothing "
                    "is drawn",
                    layout.layers[l].name.c_str(), layout.layers[l].size, values[l]->size());
                return;
            }
        }
    }

    // Per-layer normalisation, POOLED OVER THE WHOLE TAKE rather than over the
    // pass being drawn. Trunk and head magnitudes differ by roughly a factor of
    // ten, so one shared scale across layers would flatten the trunk into a
    // grey rule — the same reason scripts/advisor/figures.py scales the
    // activation heatmap per row. But scaling each PASS by its own maximum is
    // worse than that: it divides out the pass-to-pass difference this surface
    // exists to show, and renders 109 different forward passes as one identical
    // picture. `CinemaState::AdvisorScale` is the 98th percentile over every
    // pass, so a radius means the same activation in pass 1 and pass 109.
    std::array<float, 4> layer_scale = state.advisor_scale.layer;
    if (!state.advisor_scale.ready) {
        for (std::size_t l = 0; l < values.size(); ++l) {
            if (values[l] == nullptr) {
                continue;
            }
            float m = 0.0f;
            for (const float v : *values[l]) {
                m = std::max(m, std::fabs(v));
            }
            layer_scale[l] = m > 0.0f ? m : 1.0f;
        }
    }

    // Connections ranked by |w_ji * a_i| for THIS frame: a large weight on a
    // silent unit carries nothing, so weight alone would be the wrong ranking.
    // Brightness, though, is scaled by the same pooled percentile the nodes
    // use, for the same reason: renormalising inside each pass makes every pass
    // look equally bright and therefore identical.
    std::size_t total_connections = 0;
    for (const auto& block : layout.edges) {
        total_connections += block.rows * block.cols;
    }
    auto& picks = state.edge_scratch_;
    picks.clear();
    std::size_t drawn = 0;
    float value_scale = state.advisor_scale.ready ? state.advisor_scale.contribution : 0.0f;
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
                    if (!state.advisor_scale.ready) {
                        value_scale = std::max(value_scale, std::fabs(v));
                    }
                    picks.push_back({std::fabs(v), v, static_cast<int>(b), static_cast<int>(i),
                                     static_cast<int>(j)});
                }
            }
        }
        drawn = std::min(kDrawnConnections, picks.size());
        if (drawn > 0) {
            std::nth_element(picks.begin(),
                             picks.begin() + static_cast<std::ptrdiff_t>(drawn) - 1,
                             picks.end(),
                             [](const CinemaState::EdgePick& a,
                                const CinemaState::EdgePick& b) { return a.rank > b.rank; });
            // Weakest of the kept set first, so the strongest connections end
            // up on top instead of buried under near-silent ones.
            std::sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(drawn),
                      [](const CinemaState::EdgePick& a, const CinemaState::EdgePick& b) {
                          return a.rank < b.rank;
                      });
        }
    }

    // The network is the explanation: measured nodes, measured signed edges,
    // and a timed feed-forward pulse. Prose belongs in NOTES.md, not over the
    // graph.

    // ---- geometry, derived from the measured text ------------------------
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float graph_top = origin.y + 4.0f;
    const float graph_h = std::max(140.0f, region.y - 4.0f);

    const auto& heads = layout.layers[3];
    int winner = -1;
    std::string winner_text;
    if (frame != nullptr) {
        const std::string want = std::string("policy_mesher_logit_") + frame->action.mesher;
        for (std::size_t i = 0; i < heads.labels.size(); ++i) {
            if (heads.labels[i] == want) {
                winner = static_cast<int>(i);
                const std::string_view name = head_name(heads.labels[i]);
                winner_text = values[3] != nullptr
                                  ? std::format("{} {:+.4g}", name, (*values[3])[i])
                                  : std::format("{} —", name);
                break;
            }
        }
    }

    // Four wide activation lanes, one per layer, each spanning the pane; circles
    // remain circles because positions, never node geometry, are transformed.
    constexpr float kSidePad = 14.0f;
    const float header_h = std::floor(type.legend * 1.25f);
    const float chip_h = frame != nullptr ? std::floor(type.label * 2.5f) : 0.0f;
    // The candidate strip below the lanes. Every scored pass leaves one mark on
    // it, so a viewer can tell pass 27 from pass 82 — which the graph alone
    // cannot show: two neighbouring grid points differ by one step in one
    // action column, and their drawn connection sets overlap by 85%.
    const bool strip_live = frame != nullptr && state.advisor_scale.ready &&
                            state.advisor_scale.score_max > state.advisor_scale.score_min;
    const float strip_h = strip_live ? std::clamp(0.20f * graph_h, 96.0f, 156.0f) : 0.0f;
    const float lanes_top = graph_top + header_h;
    const float lanes_h =
        std::max(120.0f, graph_h - header_h - chip_h - strip_h -
                             std::floor(type.legend * (strip_live ? 1.1f : 0.6f)));
    const float lane_h = lanes_h / static_cast<float>(values.size());
    const float band_w = std::max(120.0f, region.x - 2.0f * kSidePad);
    const auto row_y = [&](std::size_t layer) {
        return lanes_top + lane_h * (static_cast<float>(layer) + 0.58f);
    };
    const auto node_x = [&](std::size_t i, std::size_t n) {
        return origin.x + kSidePad +
               band_w * (static_cast<float>(i) + 0.5f) /
                   static_cast<float>(std::max<std::size_t>(n, 1));
    };
    const auto node_point = [&](std::size_t layer, std::size_t i) {
        return ImVec2(node_x(i, layout.layers[layer].size), row_y(layer));
    };
    const bool replaying = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kMeshHold;
    const auto wave_strength = [&](float lane) {
        float at = lane;
        if (cue.pass_lane_live) {
            const double beat = std::max(cue.pass_beat_seconds, 1.0e-6);
            at = static_cast<float>(std::fmod(std::max(cue.act_t, 0.0) / beat, 1.0) *
                                    static_cast<double>(values.size() - 1));
        } else if (replaying) {
            at = static_cast<float>(cue.activation_wave) *
                 static_cast<float>(values.size() - 1);
        } else {
            return 1.0f;
        }
        const float d = at - lane;
        return 0.42f + 0.58f * std::exp(-2.2f * d * d);
    };

    // Quiet lane bands keep the layer topology legible while the measured
    // activations change. During candidate scoring their highlight advances one
    // layer per feed-forward beat; it is a timing cue only and never alters a
    // tensor value.
    for (std::size_t l = 0; l < values.size(); ++l) {
        const float top = lanes_top + lane_h * static_cast<float>(l) + type.legend * 1.15f;
        const float bottom = lanes_top + lane_h * static_cast<float>(l + 1) - 4.0f;
        const float pulse = wave_strength(static_cast<float>(l));
        dl->AddRectFilled(ImVec2(origin.x + kSidePad, top),
                          ImVec2(origin.x + region.x - kSidePad, bottom),
                          faded(l == 3 ? palette.accent : palette.panel_bg,
                                (l == 3 ? 0.028f : 0.055f) * pulse * alpha),
                          5.0f);
        dl->AddLine(
            ImVec2(origin.x + kSidePad, row_y(l)),
            ImVec2(origin.x + region.x - kSidePad, row_y(l)),
            faded(l == 3 ? palette.accent : palette.text_dim, (0.08f + 0.10f * pulse) * alpha),
            1.0f);
    }

    // ---- connections ----------------------------------------------------
    if (drawn > 0) {
        const float inv_scale = value_scale > 0.0f ? 1.0f / value_scale : 0.0f;
        for (std::size_t k = 0; k < drawn; ++k) {
            const auto& pick = picks[k];
            const auto b = static_cast<std::size_t>(pick.block);
            const float weight = std::clamp(pick.rank * inv_scale, 0.0f, 1.0f);
            // RdBu is white in the middle, so mapping a contribution straight
            // onto it painted the strongest connections pale. Only the top few
            // hundred of ~19,000 are drawn at all: every one of them is a
            // significant contribution, and its SIGN is the thing worth reading,
            // so the ramp starts away from the neutral midpoint.
            const float t = (pick.value < 0.0f ? -1.0f : 1.0f) * (0.34f + 0.66f * weight);
            const float pulse = wave_strength(static_cast<float>(b) + 0.5f);
            dl->AddLine(node_point(b, static_cast<std::size_t>(pick.src)),
                        node_point(b + 1, static_cast<std::size_t>(pick.dst)),
                        rgba(signed_colormap(t), (0.06f + 0.84f * weight) * pulse * alpha),
                        0.55f + 1.55f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.8f;
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float spacing = band_w / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 2.6f, kNodeRadiusMax);
        const std::string count = grouped(layer.size);
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + kSidePad, lanes_top + lane_h * static_cast<float>(l)),
                    faded(l == 3 ? palette.accent : palette.text_dim,
                          alpha * wave_strength(static_cast<float>(l))),
                    count.c_str());
        const float pulse = wave_strength(static_cast<float>(l));
        for (std::size_t i = 0; i < layer.size; ++i) {
            const float a = values[l] != nullptr ? (*values[l])[i] : 0.0f;
            const float mag = std::clamp(std::fabs(a) / layer_scale[l], 0.0f, 1.0f);
            const float r = kNodeMin + (r_max - kNodeMin) * mag;
            const ImVec2 point = node_point(l, i);
            const auto rgb = signed_colormap(std::clamp(a / layer_scale[l], -1.0f, 1.0f));
            if (mag > 0.30f) {
                dl->AddCircleFilled(point, r * 3.0f, rgba(rgb, 0.065f * mag * pulse * alpha));
                dl->AddCircleFilled(point, r * 1.8f, rgba(rgb, 0.125f * mag * pulse * alpha));
            }
            dl->AddCircleFilled(point, r, rgba(rgb, (0.42f + 0.58f * mag) * alpha));
            const bool chosen_head = l == 3 && static_cast<int>(i) == winner;
            if (mag > 0.55f || chosen_head) {
                dl->AddCircle(point, r + (chosen_head ? 3.0f : 1.4f),
                              faded(chosen_head ? palette.accent : palette.text,
                                    (chosen_head ? 0.95f : 0.45f * mag) * pulse * alpha),
                              0, chosen_head ? 2.4f : 1.2f);
            }
        }
    }

    // The input row is 81 columns wide and only 13 of them move between passes:
    // the candidate's own action columns, measured across the ensemble in
    // `advisor_display_scale`. The other 68 are this part's case features and
    // are identical in every pass by construction. Drawing all 81 alike is a
    // large part of why pass 27 and pass 82 looked like the same picture, so the
    // columns the candidate actually moves are bracketed and named.
    if (frame != nullptr && !state.advisor_scale.action_columns.empty() &&
        layout.layers[0].size > 0) {
        const auto& cols = state.advisor_scale.action_columns;
        const std::size_t n_in = layout.layers[0].size;
        const float y = row_y(0) + 10.0f;
        const float half = 0.5f * band_w / static_cast<float>(n_in);
        const ImU32 mark = faded(palette.accent, 0.78f * alpha);
        std::size_t k = 0;
        float last_x1 = origin.x + kSidePad;
        while (k < cols.size()) {
            std::size_t j = k;
            while (j + 1 < cols.size() && cols[j + 1] == cols[j] + 1) {
                ++j;
            }
            const float x0 = node_x(static_cast<std::size_t>(cols[k]), n_in) - half;
            const float x1 = node_x(static_cast<std::size_t>(cols[j]), n_in) + half;
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), mark, 2.0f);
            dl->AddLine(ImVec2(x0, y - 4.0f), ImVec2(x0, y), mark, 1.4f);
            dl->AddLine(ImVec2(x1, y - 4.0f), ImVec2(x1, y), mark, 1.4f);
            last_x1 = std::max(last_x1, x1);
            k = j + 1;
        }
        // The label ends under the last bracketed run rather than at the left
        // margin, so the count and the columns it counts are read together.
        const std::string moved =
            fmt("%s action columns: the candidate", grouped(cols.size()).c_str());
        const std::string fixed = fmt(" · %s case columns: this part, every pass",
                                      grouped(n_in - cols.size()).c_str());
        const float w_moved = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, moved.c_str()).x;
        const float w_fixed = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, fixed.c_str()).x;
        const ImVec2 at(std::max(origin.x + kSidePad, last_x1 - w_moved - w_fixed), y + 4.0f);
        dl->AddText(font, type.legend, at, faded(palette.accent, 0.9f * alpha), moved.c_str());
        dl->AddText(font, type.legend, ImVec2(at.x + w_moved, at.y),
                    faded(palette.text_dim, 0.85f * alpha), fixed.c_str());
    }

    // ---- every candidate, on one strip ----------------------------------
    //
    // The graph above can only ever show ONE pass, and two neighbouring grid
    // points differ by a single step in a single action column: their drawn
    // connection sets overlap by 85%, so the sweep alone reads as a still
    // picture with a counter running over it. This strip is the part that
    // genuinely differs per candidate — the ranking key the chooser sorts on,
    // one mark per scored pass, accumulating in enumeration order — so the act
    // has a visible record of 108 distinct candidates instead of one blur.
    if (strip_live) {
        const auto& scale = state.advisor_scale;
        std::size_t n_cand = 0;
        for (const auto& f : frames) {
            if (f.candidate >= 0) {
                ++n_cand;
            }
        }
        const float strip_top = lanes_top + lanes_h + std::floor(type.legend * 0.35f);
        const float head_y = strip_top;
        const float plot_top = strip_top + std::floor(type.legend * 1.35f);
        const float plot_bot = strip_top + strip_h - std::floor(type.legend * 1.35f);
        constexpr float kGutter = 62.0f;
        const float plot_x0 = origin.x + kSidePad + kGutter;
        const float plot_x1 = origin.x + region.x - kSidePad;
        const float span = std::max(1.0e-6f, scale.score_max - scale.score_min);
        const auto mark_x = [&](int candidate) {
            return plot_x0 + (plot_x1 - plot_x0) * (static_cast<float>(candidate) + 0.5f) /
                                 static_cast<float>(std::max<std::size_t>(n_cand, 1));
        };
        const auto mark_y = [&](double score) {
            const float u = (static_cast<float>(score) - scale.score_min) / span;
            return plot_bot - std::clamp(u, 0.0f, 1.0f) * (plot_bot - plot_top);
        };
        dl->AddRectFilled(ImVec2(plot_x0 - 6.0f, plot_top - 4.0f),
                          ImVec2(plot_x1, plot_bot + 4.0f),
                          faded(palette.panel_bg, 0.5f * alpha), 4.0f);
        dl->AddLine(ImVec2(plot_x0 - 6.0f, plot_bot + 4.0f), ImVec2(plot_x1, plot_bot + 4.0f),
                    faded(palette.text_dim, 0.22f * alpha), 1.0f);

        // Axis ends, so the two directions are named rather than assumed.
        const auto gutter_text = [&](float y, const std::string& text, ImVec4 color, float a) {
            const float w = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, text.c_str()).x;
            dl->AddText(font, type.legend, ImVec2(plot_x0 - 12.0f - w, y - type.legend * 0.5f),
                        faded(color, a * alpha), text.c_str());
        };
        gutter_text(plot_top, fmt("%+.2f", scale.score_max), palette.text_dim, 0.7f);
        gutter_text(plot_bot, fmt("%+.2f", scale.score_min), palette.text_dim, 0.7f);
        // `score` is `rel_err_rel`: the accuracy head's log10 prediction centred
        // on this case's median over the actions, which is the key the chooser
        // ranks candidates on. Calling it a predicted error would overstate it —
        // it is comparable between actions on this part and meaningless as an
        // absolute number.
        dl->AddText(font, type.legend, ImVec2(origin.x + kSidePad, head_y),
                    faded(palette.text_dim, 0.9f * alpha),
                    "ranking score of every candidate · lower is better");

        // Legend swatches, drawn rather than written, so the two marker states
        // are read off the same shapes the strip uses.
        const float legend_x = plot_x1 - 236.0f;
        const float legend_y = head_y + type.legend * 0.55f;
        dl->AddCircleFilled(ImVec2(legend_x, legend_y), 3.0f,
                            faded(palette.status_ok, 0.9f * alpha));
        dl->AddText(font, type.legend, ImVec2(legend_x + 8.0f, head_y),
                    faded(palette.text_dim, 0.8f * alpha), "gate passed");
        dl->AddCircle(ImVec2(legend_x + 126.0f, legend_y), 3.0f,
                      faded(palette.status_warn, 0.9f * alpha), 0, 1.3f);
        dl->AddText(font, type.legend, ImVec2(legend_x + 134.0f, head_y),
                    faded(palette.text_dim, 0.8f * alpha), "gate declined");

        // Only passes the sweep has actually reached are on the strip: it fills
        // as the chooser works, and is complete once the chosen pass is held.
        const int revealed = cue.frame_index;
        for (std::size_t n = 0; n < frames.size() && static_cast<int>(n) <= revealed; ++n) {
            const auto& f = frames[n];
            if (f.candidate < 0) {
                continue;
            }
            const float x = mark_x(f.candidate);
            if (!f.ranked || !std::isfinite(f.score)) {
                // A pass whose score could not be ranked is drawn as one, at the
                // bottom rule, rather than being left off the record.
                dl->AddLine(ImVec2(x - 3.0f, plot_bot + 1.0f),
                            ImVec2(x + 3.0f, plot_bot + 7.0f),
                            faded(palette.status_err, 0.8f * alpha), 1.4f);
                dl->AddLine(ImVec2(x - 3.0f, plot_bot + 7.0f),
                            ImVec2(x + 3.0f, plot_bot + 1.0f),
                            faded(palette.status_err, 0.8f * alpha), 1.4f);
                continue;
            }
            const float y = mark_y(f.score);
            const bool current = static_cast<int>(n) == revealed;
            if (f.over_budget) {
                dl->AddCircle(ImVec2(x, y), 3.0f, faded(palette.status_err, 0.75f * alpha), 0,
                              1.3f);
            } else if (f.gate_pass) {
                dl->AddCircleFilled(ImVec2(x, y), current ? 4.0f : 2.7f,
                                    faded(palette.status_ok, (current ? 1.0f : 0.8f) * alpha));
            } else {
                dl->AddCircle(ImVec2(x, y), current ? 4.0f : 2.7f,
                              faded(palette.status_warn, (current ? 1.0f : 0.72f) * alpha), 0,
                              1.4f);
            }
            if (current) {
                dl->AddLine(ImVec2(x, plot_top - 4.0f), ImVec2(x, plot_bot + 4.0f),
                            faded(palette.accent, 0.45f * alpha), 1.2f);
            }
            if (static_cast<int>(n) == scale.winner_frame) {
                dl->AddCircle(ImVec2(x, y), 8.0f, faded(palette.accent, 0.95f * alpha), 0,
                              2.0f);
            }
        }
        if (scale.winner_frame >= 0 && revealed >= scale.winner_frame) {
            dl->AddText(font, type.legend,
                        ImVec2(plot_x0, plot_bot + std::floor(type.legend * 0.35f)),
                        faded(palette.accent, 0.85f * alpha),
                        "ring · the candidate the ranking picked");
        }
    }

    // Outcome glyph: decision state → mesh. No prose is needed here; the
    // measured OOD distance or selected action remains the only text.
    if (frame != nullptr) {
        const float chip_top = lanes_top + lanes_h + strip_h +
                               std::floor(type.legend * (strip_live ? 0.7f : 0.25f));
        dl->AddRectFilled(ImVec2(origin.x, chip_top),
                          ImVec2(origin.x + region.x, chip_top + chip_h),
                          faded(palette.panel_bg, 0.72f * alpha), 7.0f);
        dl->AddRect(ImVec2(origin.x, chip_top), ImVec2(origin.x + region.x, chip_top + chip_h),
                    faded(palette.accent, 0.62f * alpha), 7.0f, 0, 1.2f);
        const ImVec2 state_at(origin.x + 28.0f, chip_top + 0.5f * chip_h);
        const ImVec4 state_color =
            cue.pass_lane_live
                ? palette.accent
                : (state.decision_applied ? palette.status_ok : palette.status_warn);
        dl->AddCircle(state_at, 11.0f, faded(state_color, alpha), 0, 2.2f);
        if (cue.pass_lane_live) {
            dl->AddCircleFilled(state_at, 4.0f, faded(state_color, alpha));
        } else if (state.decision_applied) {
            dl->AddLine(ImVec2(state_at.x - 5.0f, state_at.y),
                        ImVec2(state_at.x - 1.0f, state_at.y + 5.0f),
                        faded(state_color, alpha), 2.2f);
            dl->AddLine(ImVec2(state_at.x - 1.0f, state_at.y + 5.0f),
                        ImVec2(state_at.x + 7.0f, state_at.y - 6.0f),
                        faded(state_color, alpha), 2.2f);
        } else {
            dl->AddLine(ImVec2(state_at.x - 7.0f, state_at.y + 7.0f),
                        ImVec2(state_at.x + 7.0f, state_at.y - 7.0f),
                        faded(state_color, alpha), 2.2f);
        }
        const ImVec2 arrow_a(state_at.x + 18.0f, state_at.y);
        const ImVec2 grid_at(state_at.x + 74.0f, state_at.y);
        dl->AddLine(arrow_a, ImVec2(grid_at.x - 19.0f, grid_at.y),
                    faded(state_color, 0.72f * alpha), 2.0f);
        dl->AddTriangleFilled(
            ImVec2(grid_at.x - 14.0f, grid_at.y), ImVec2(grid_at.x - 22.0f, grid_at.y - 5.0f),
            ImVec2(grid_at.x - 22.0f, grid_at.y + 5.0f), faded(state_color, 0.72f * alpha));
        for (int i = -1; i <= 1; ++i) {
            const float d = static_cast<float>(i) * 7.0f;
            dl->AddLine(ImVec2(grid_at.x - 10.0f, grid_at.y + d),
                        ImVec2(grid_at.x + 10.0f, grid_at.y + d), faded(palette.accent, alpha),
                        1.2f);
            dl->AddLine(ImVec2(grid_at.x + d, grid_at.y - 10.0f),
                        ImVec2(grid_at.x + d, grid_at.y + 10.0f), faded(palette.accent, alpha),
                        1.2f);
        }
        const auto& decision = state.explanation->decision;
        std::string value;
        if (cue.pass_lane_live) {
            value = fmt("candidate %s / %s",
                        grouped(static_cast<std::size_t>(cue.frame_index) + 1).c_str(),
                        grouped(state.explanation->frames.size()).c_str());
        } else if (cue.stage_index >= 0 &&
                   static_cast<std::size_t>(cue.stage_index) < state.stages.size()) {
            const std::size_t total_cells =
                state.stages[static_cast<std::size_t>(cue.stage_index)].mesh.elements.size();
            const std::size_t visible_cells = std::min(
                total_cells, static_cast<std::size_t>(std::floor(
                                 cue.mesh_action_reveal * static_cast<double>(total_cells))));
            const std::string source = state.decision_applied
                                           ? std::string(mesher_plain(frame->action.mesher))
                                           : std::string("configured baseline");
            value = fmt("%s  →  %s / %s cells", source.c_str(), grouped(visible_cells).c_str(),
                        grouped(total_cells).c_str());
        } else if (state.decision_applied) {
            value = fmt("%s · h/L %.3g · p%d",
                        std::string(mesher_plain(frame->action.mesher)).c_str(),
                        frame->action.h_rel, frame->action.order);
        } else {
            value = fmt("d = %.3g · configured baseline", decision.ood_distance);
        }
        dl->AddText(font, type.label,
                    ImVec2(grid_at.x + 26.0f, state_at.y - 0.5f * type.label),
                    faded(state_color, alpha), value.c_str());
    }

    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
#endif
}

namespace {

void draw_cinema_features(const CinemaState& state, const CinemaCue& cue,
                          const CinemaType& type, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                "CAD edge network  ↔  κ(s)  ↔  FFT");

    const float chart_top = origin.y + type.caption * 1.7f;
    const float chart_h =
        std::max(260.0f, region.y - (chart_top - origin.y) - type.label * 2.2f);
    const float chart_w = std::max(120.0f, region.x);
    dl->AddRectFilled(ImVec2(origin.x, chart_top),
                      ImVec2(origin.x + chart_w, chart_top + chart_h),
                      faded(palette.panel_bg, 0.52f * alpha), 6.0f);
    dl->AddRect(ImVec2(origin.x, chart_top), ImVec2(origin.x + chart_w, chart_top + chart_h),
                faded(palette.border, 0.8f * alpha), 6.0f);

    const float pad = 13.0f;
    const float split_y = chart_top + chart_h * 0.58f;
    dl->AddLine(ImVec2(origin.x + pad, split_y), ImVec2(origin.x + chart_w - pad, split_y),
                faded(palette.border, 0.72f * alpha), 1.0f);
    const float edge_right = origin.x + chart_w * 0.35f;
    const float curve_left = edge_right + pad * 1.5f;

    // The left inset is the selected CAD edge itself, projected through its two
    // widest world axes. Three adjacent samples and their circumcircle expose
    // the actual discrete-curvature construction; the right trace is the
    // measured κ(s) those samples produced.
    const auto& edge_points = state.sizing.curve_points;
    if (edge_points.size() >= 3) {
        Eigen::Vector3d lo = edge_points.front();
        Eigen::Vector3d hi = edge_points.front();
        for (const auto& p : edge_points) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        std::array<int, 3> axes{0, 1, 2};
        const Eigen::Vector3d range = hi - lo;
        std::sort(axes.begin(), axes.end(), [&](int a, int b) { return range[a] > range[b]; });
        const int ax = axes[0];
        const int ay = axes[1];
        const double sx = std::max(range[ax], 1.0e-12);
        const double sy = std::max(range[ay], 1.0e-12);
        const float edge_top = chart_top + pad;
        const float edge_bottom = split_y - pad;
        const auto edge_point = [&](const Eigen::Vector3d& p) {
            return ImVec2(origin.x + pad +
                              (edge_right - origin.x - 2.0f * pad) *
                                  static_cast<float>((p[ax] - lo[ax]) / sx),
                          edge_bottom - (edge_bottom - edge_top) *
                                            static_cast<float>((p[ay] - lo[ay]) / sy));
        };
        const double reveal =
            cue.spectral_edge_reveal * static_cast<double>(edge_points.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(reveal)), edge_points.size() - 1);
        for (std::size_t i = 1; i < edge_points.size(); ++i) {
            const bool lit = i <= whole;
            dl->AddLine(
                edge_point(edge_points[i - 1]), edge_point(edge_points[i]),
                faded(lit ? palette.accent : palette.text_dim, alpha * (lit ? 0.92f : 0.22f)),
                lit ? 2.4f : 1.0f);
        }
        const std::size_t sample = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(
                cue.spectral_curve_cursor * static_cast<double>(edge_points.size() - 1))),
            1, edge_points.size() - 2);
        const float construction_alpha =
            alpha * static_cast<float>(cue.spectral_curve_cursor_alpha);
        const ImVec2 a = edge_point(edge_points[sample - 1]);
        const ImVec2 b = edge_point(edge_points[sample]);
        const ImVec2 c = edge_point(edge_points[sample + 1]);
        for (const ImVec2 p : {a, b, c}) {
            dl->AddCircleFilled(p, 4.0f, faded(palette.accent_soft_top, construction_alpha));
        }
        const float tangent_norm = std::hypot(c.x - a.x, c.y - a.y);
        if (tangent_norm > 1.0e-4f) {
            const ImVec2 tangent((c.x - a.x) / tangent_norm, (c.y - a.y) / tangent_norm);
            ImVec2 normal(-tangent.y, tangent.x);
            const ImVec2 inset_center(0.5f * (origin.x + edge_right),
                                      0.5f * (edge_top + edge_bottom));
            if (normal.x * (b.x - inset_center.x) + normal.y * (b.y - inset_center.y) < 0.0f) {
                normal.x = -normal.x;
                normal.y = -normal.y;
            }
            dl->AddLine(ImVec2(b.x - 24.0f * tangent.x, b.y - 24.0f * tangent.y),
                        ImVec2(b.x + 24.0f * tangent.x, b.y + 24.0f * tangent.y),
                        faded(palette.text, 0.76f * construction_alpha), 1.5f);
            const ImVec2 normal_tip(b.x + 34.0f * normal.x, b.y + 34.0f * normal.y);
            dl->AddLine(b, normal_tip, faded(palette.status_warn, 0.86f * construction_alpha),
                        2.0f);
            dl->AddCircleFilled(normal_tip, 3.5f,
                                faded(palette.status_warn, construction_alpha));
        }
        const float d = 2.0f * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::fabs(d) > 1.0e-4f) {
            const float aa = a.x * a.x + a.y * a.y;
            const float bb = b.x * b.x + b.y * b.y;
            const float cc = c.x * c.x + c.y * c.y;
            const ImVec2 center((aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d,
                                (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d);
            const float radius = std::hypot(center.x - b.x, center.y - b.y);
            const bool inside =
                center.x - radius >= origin.x + pad && center.x + radius <= edge_right - pad &&
                center.y - radius >= edge_top && center.y + radius <= edge_bottom;
            if (std::isfinite(radius) && inside) {
                dl->AddCircle(center, radius,
                              faded(palette.status_warn, 0.62f * construction_alpha), 0, 1.4f);
                dl->AddLine(center, b, faded(palette.status_warn, 0.78f * construction_alpha),
                            1.4f);
            }
        }
        const ImVec2 arrow_a(edge_right + 2.0f, 0.5f * (edge_top + edge_bottom));
        const ImVec2 arrow_b(curve_left - 6.0f, arrow_a.y);
        dl->AddLine(arrow_a, arrow_b, faded(palette.accent, 0.72f * alpha), 2.0f);
        dl->AddTriangleFilled(arrow_b, ImVec2(arrow_b.x - 8.0f, arrow_b.y - 5.0f),
                              ImVec2(arrow_b.x - 8.0f, arrow_b.y + 5.0f),
                              faded(palette.accent, 0.72f * alpha));
        dl->AddTriangleFilled(arrow_a, ImVec2(arrow_a.x + 8.0f, arrow_a.y - 5.0f),
                              ImVec2(arrow_a.x + 8.0f, arrow_a.y + 5.0f),
                              faded(palette.accent, 0.72f * alpha));
    }

    const auto& raw = state.sizing.curvature_raw;
    const auto& filtered = state.sizing.curvature_filtered;
    const auto& stations = state.sizing.stations;
    if (raw.size() >= 2 && filtered.size() == raw.size() && stations.size() == raw.size()) {
        double lo = raw.front();
        double hi = raw.front();
        for (std::size_t i = 0; i < raw.size(); ++i) {
            lo = std::min({lo, raw[i], filtered[i]});
            hi = std::max({hi, raw[i], filtered[i]});
        }
        const double span = std::max(hi - lo, 1.0e-12);
        const float plot_top = chart_top + type.legend * 1.65f;
        const float plot_bottom = split_y - 10.0f;
        const auto point = [&](double station, double value) {
            const float x = curve_left + (origin.x + chart_w - pad - curve_left) *
                                             static_cast<float>(station);
            const float y = plot_bottom -
                            (plot_bottom - plot_top) * static_cast<float>((value - lo) / span);
            return ImVec2(x, y);
        };

        const double revealed = cue.spectral_edge_reveal * static_cast<double>(raw.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(revealed)), raw.size() - 1);
        for (std::size_t i = 1; i <= whole; ++i) {
            dl->AddLine(point(stations[i - 1], raw[i - 1]), point(stations[i], raw[i]),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double y0 =
                raw[i - 1] + cue.spectral_filter_mix * (filtered[i - 1] - raw[i - 1]);
            const double y1 = raw[i] + cue.spectral_filter_mix * (filtered[i] - raw[i]);
            dl->AddLine(point(stations[i - 1], y0), point(stations[i], y1),
                        faded(palette.accent, alpha), 2.6f);
            dl->AddCircleFilled(point(stations[i], y1), 2.6f,
                                faded(palette.accent_soft_top, alpha));
        }
        if (whole + 1 < raw.size()) {
            const double part = revealed - static_cast<double>(whole);
            const double station =
                stations[whole] + part * (stations[whole + 1] - stations[whole]);
            const double raw_value = raw[whole] + part * (raw[whole + 1] - raw[whole]);
            const double filtered_value =
                filtered[whole] + part * (filtered[whole + 1] - filtered[whole]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            dl->AddLine(point(stations[whole], raw[whole]), point(station, raw_value),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double start =
                raw[whole] + cue.spectral_filter_mix * (filtered[whole] - raw[whole]);
            dl->AddLine(point(stations[whole], start), point(station, value),
                        faded(palette.accent, alpha), 2.6f);
        }
        if (cue.spectral_edge_reveal > 0.0 && cue.spectral_curve_cursor_alpha > 0.0) {
            const double cursor = std::clamp(cue.spectral_curve_cursor, 0.0, 1.0) *
                                  static_cast<double>(raw.size() - 1);
            const std::size_t lo_index =
                std::min(static_cast<std::size_t>(std::floor(cursor)), raw.size() - 1);
            const std::size_t hi_index = std::min(lo_index + 1, raw.size() - 1);
            const double part = cursor - static_cast<double>(lo_index);
            const double station =
                stations[lo_index] + part * (stations[hi_index] - stations[lo_index]);
            const double raw_value = raw[lo_index] + part * (raw[hi_index] - raw[lo_index]);
            const double filtered_value =
                filtered[lo_index] + part * (filtered[hi_index] - filtered[lo_index]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            const ImVec2 scan = point(station, value);
            const float cursor_alpha =
                alpha * static_cast<float>(cue.spectral_curve_cursor_alpha);
            dl->AddLine(ImVec2(scan.x, plot_top), ImVec2(scan.x, plot_bottom),
                        faded(palette.accent_soft_top, 0.52f * cursor_alpha), 1.2f);
            dl->AddCircleFilled(scan, 4.6f, faded(palette.accent_soft_top, cursor_alpha));
        }
    }

    const auto& spectrum = state.sizing.curve_spectrum;
    const auto& kept = state.sizing.curve_mode_kept;
    if (spectrum.size() >= 2 && kept.size() == spectrum.size()) {
        // DC is the mean curvature, not spacing variation. `truncate_modes`
        // always preserves it and excludes it from modes_total, so omitting it
        // here both matches the report's denominator and stops one huge bar
        // from flattening every explanatory non-DC mode.
        const double max_magnitude =
            std::max(*std::max_element(spectrum.begin() + 1, spectrum.end()), 1.0e-12);
        const float bars_top = split_y + pad;
        const float bars_bottom = chart_top + chart_h - 11.0f;
        const float bars_h = std::max(1.0f, bars_bottom - bars_top);
        const float bars_w = chart_w - 2.0f * pad;
        const std::size_t mode_count = spectrum.size() - 1;
        const float slot = bars_w / static_cast<float>(mode_count);
        const std::size_t visible = std::min(
            mode_count, static_cast<std::size_t>(std::ceil(cue.spectral_spectrum_reveal *
                                                           static_cast<double>(mode_count))));
        const double log_max = std::log1p(max_magnitude);
        for (std::size_t mode = 0; mode < visible; ++mode) {
            const std::size_t i = mode + 1;
            const float magnitude =
                static_cast<float>(std::log1p(spectrum[i]) / std::max(log_max, 1.0e-12));
            const float x0 = origin.x + pad + static_cast<float>(mode) * slot;
            const float x1 = x0 + std::max(1.0f, slot - 1.0f);
            const float y0 = bars_bottom - bars_h * magnitude;
            const bool survives = kept[i] != 0;
            const float discarded_alpha =
                static_cast<float>(1.0 - 0.82 * cue.spectral_filter_mix);
            const ImVec4 color =
                survives ? palette.accent
                         : ImVec4(palette.text_dim.x, palette.text_dim.y, palette.text_dim.z,
                                  palette.text_dim.w * discarded_alpha);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, bars_bottom), faded(color, alpha),
                              1.0f);
        }
    }
    const std::string status =
        state.sizing.field_points.empty()
            ? fmt("%s / %s modes · all CAD curves filtered · uniform h unchanged",
                  grouped(state.sizing.curve_modes_kept).c_str(),
                  grouped(state.sizing.curve_modes_total).c_str())
            : fmt("%s / %s modes · reconstructed across CAD · measured h(x)",
                  grouped(state.sizing.curve_modes_kept).c_str(),
                  grouped(state.sizing.curve_modes_total).c_str());
    dl->AddText(
        font, type.label, ImVec2(origin.x, chart_top + chart_h + type.label * 0.48f),
        faded(state.sizing.field_points.empty() ? palette.text_dim : palette.accent, alpha),
        status.c_str());
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_cells(const CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                       const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    (void)hud;
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    std::size_t index = cue.stage_index >= 0 ? static_cast<std::size_t>(cue.stage_index)
                                             : (n_fill > 0 ? n_fill - 1 : 0);
    if (n_fill > 0) {
        index = std::min(index, n_fill - 1);
    }
    const CinemaMeshInsight* emitted =
        index < state.stage_insights.size() ? &state.stage_insights[index] : nullptr;
    const CinemaMeshInsight* solved =
        !state.solve_insights.empty() ? &state.solve_insights.front() : emitted;

    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float bar_y = origin.y + 10.0f;
    const float bar_h = std::max(18.0f, type.legend + 2.0f);
    if (solved != nullptr) {
        const std::size_t total = std::max<std::size_t>(
            1, std::accumulate(solved->type_counts.begin(), solved->type_counts.end(),
                               std::size_t{0}));
        float x = origin.x;
        std::size_t dominant = 0;
        for (std::size_t i = 0; i < solved->type_counts.size(); ++i) {
            if (solved->type_counts[i] == 0) {
                continue;
            }
            if (solved->type_counts[i] > solved->type_counts[dominant]) {
                dominant = i;
            }
            const float w = region.x * static_cast<float>(solved->type_counts[i]) /
                            static_cast<float>(total);
            dl->AddRectFilled(
                ImVec2(x, bar_y), ImVec2(x + w, bar_y + bar_h),
                rgba(element_type_color(static_cast<fea::ElementType>(i)), alpha), 2.0f);
            x += w;
        }
        // A full-width unlabelled bar says nothing. Name what it is a bar of.
        const auto dominant_type = static_cast<fea::ElementType>(dominant);
        const std::string mix =
            fmt("element mix · %s %.0f%%", fea::element_type_name(dominant_type),
                100.0 * static_cast<double>(solved->type_counts[dominant]) /
                    static_cast<double>(total));
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + 8.0f, bar_y + 0.5f * (bar_h - type.legend)),
                    faded(palette.window_bg, 0.92f * alpha), mix.c_str());
    }

    const float card_top = bar_y + bar_h + 16.0f;
    const float card_h = std::max(260.0f, region.y - (card_top - origin.y) - 14.0f);
    const ImVec2 card_min(origin.x, card_top);
    const ImVec2 card_max(origin.x + region.x, card_top + card_h);
    dl->AddRectFilled(card_min, card_max, faded(palette.panel_bg, 0.50f * alpha), 8.0f);
    dl->AddRect(card_min, card_max, faded(palette.border, 0.72f * alpha), 8.0f);

    // The subject of this card is the difference between the two element
    // orders, so both orders are on it. Left and right hold the same cell in
    // the same pose against the same CAD arc: a tet4 owns corner nodes only, so
    // its edge cannot leave the straight chord and misses the surface, while a
    // tet10's midside nodes slide onto the arc and close that gap. Every
    // percentage drawn here is measured off the arc beside it at draw time.
    const char* title = "p1 vs p2 · what the extra nodes buy";
    dl->AddText(font, type.caption, ImVec2(card_min.x + 18.0f, card_min.y + 15.0f),
                faded(palette.status_ok, alpha), title);
    const char* subtitle = "same cell · same CAD arc · only the element order differs";
    dl->AddText(font, type.legend,
                ImVec2(card_min.x + 18.0f, card_min.y + 15.0f + type.caption * 1.35f),
                faded(palette.text_dim, alpha), subtitle);

    const float t = static_cast<float>(cue.act_t);
    const float fade_seconds = 0.17f * static_cast<float>(cue.act_span);
    const float visible_t = std::max(0.0f, t - fade_seconds);
    const float progress = std::clamp(
        visible_t / std::max(0.62f * static_cast<float>(cue.act_span), 1.0e-6f), 0.0f, 1.0f);
    const float corner_alpha = static_cast<float>(smoothstep(progress / 0.22));
    const float arc_alpha = static_cast<float>(smoothstep((progress - 0.10) / 0.22));
    const float midside_alpha = static_cast<float>(smoothstep((progress - 0.26) / 0.32));
    const float field_alpha = static_cast<float>(smoothstep((progress - 0.44) / 0.26));
    const float plot_alpha = static_cast<float>(smoothstep((progress - 0.56) / 0.28)) * alpha;
    const float xi = 0.5f - 0.5f * std::cos(t * 0.85f);

    // A circular CAD arc through one edge's two corners, sagitta 18% of the
    // chord, written as deviation from that chord at chord parameter s. A p1
    // edge can only ever answer zero here; a p2 edge answers the quadratic
    // through the arc's own midpoint. The distance between those two answers is
    // the element order, drawn.
    constexpr float kSag = 0.18f;
    constexpr float kArcR = (0.25f + kSag * kSag) / (2.0f * kSag);
    const auto exact_dev = [](float s) {
        const float x = s - 0.5f;
        return std::sqrt(std::max(0.0f, kArcR * kArcR - x * x)) - (kArcR - kSag);
    };
    const auto p2_dev = [](float s) { return 4.0f * s * (1.0f - s) * kSag; };

    // Quadratic Lagrange basis on an edge: corners at ξ = 0 and 1, midside node
    // at ξ = 0.5. The same three functions map the edge's geometry and
    // interpolate its field, which is the whole point of the element.
    const auto basis = [](float s) {
        return std::array<float, 3>{(1.0f - s) * (1.0f - 2.0f * s), s * (2.0f * s - 1.0f),
                                    4.0f * s * (1.0f - s)};
    };
    const auto mix_color = [](const ImVec4& a, const ImVec4& b, float u) {
        return ImVec4(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u,
                      a.w + (b.w - a.w) * u);
    };

    // Posed so that edge 0 — the one held against the CAD arc — has its outward
    // direction exactly along -y: the centroid of the other two corners sits at
    // y = +0.025, so chord - centroid is pure -y and the whole sagitta projects
    // into the screen plane instead of hiding along the view direction.
    static const std::array<Eigen::Vector3f, 4> kCorners{{
        {-1.0f, -0.70f, 0.0f},
        {1.0f, -0.70f, 0.0f},
        {-0.25f, 0.95f, -0.55f},
        {0.25f, 0.55f, 0.55f},
    }};
    static constexpr std::array<std::array<std::size_t, 2>, 6> kEdges{{
        {{0, 1}},
        {{0, 2}},
        {{0, 3}},
        {{1, 2}},
        {{1, 3}},
        {{2, 3}},
    }};
    static constexpr std::array<std::array<std::size_t, 3>, 4> kFaces{{
        {{0, 1, 2}},
        {{0, 1, 3}},
        {{0, 2, 3}},
        {{1, 2, 3}},
    }};
    constexpr std::size_t kLitEdge = 0;
    const auto edge_of = [](std::size_t a, std::size_t b) {
        for (std::size_t e = 0; e < kEdges.size(); ++e) {
            if ((kEdges[e][0] == a && kEdges[e][1] == b) ||
                (kEdges[e][0] == b && kEdges[e][1] == a)) {
                return e;
            }
        }
        return std::size_t{0};
    };

    // Everything is anchored off the card's bottom so the plot, its legend and
    // the honest solve note cannot collide with the measured-quality line on a
    // short pane.
    const float quality_y = card_top + card_h - type.label * 1.8f;
    const float note_y = quality_y - type.legend * 1.75f;
    const float legend_y = note_y - type.legend * 1.55f;
    const float plot_bottom = legend_y - 12.0f;
    const float plot_top = std::max(card_min.y + 0.56f * card_h, plot_bottom - 0.22f * card_h);
    const float plot_left = card_min.x + 26.0f;
    const float plot_right = card_max.x - 26.0f;

    const float col_top = card_min.y + 15.0f + type.caption * 1.35f + type.legend * 1.85f;
    const float col_bottom = plot_top - 24.0f;
    const float head_y = col_top;
    const float count_y = head_y + type.label * 1.25f;
    const float miss_y = count_y + type.legend * 1.40f;
    const float caption_y = col_bottom - type.legend * 1.15f;
    const float swatch_y = caption_y - 17.0f;
    const float body_top = miss_y + type.legend * 1.55f;
    const float body_bottom = swatch_y - 8.0f;
    const float col_w = 0.46f * region.x;
    dl->AddLine(ImVec2(card_min.x + 0.5f * region.x, col_top),
                ImVec2(card_min.x + 0.5f * region.x, col_bottom),
                faded(palette.border, 0.55f * alpha), 1.0f);

    const float yaw = -0.42f + 0.10f * std::sin(t * 0.42f);
    const float pitch = 0.46f;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float scale =
        std::min(0.34f * col_w, std::max(22.0f, 0.44f * (body_bottom - body_top)));
    // Orthographic and affine, so projecting a quadratic edge map is the same
    // curve as the quadratic map of the projected nodes.
    const auto project = [&](const Eigen::Vector3f& q, ImVec2 c) {
        const float rx = cy * q.x() + sy * q.z();
        const float rz = -sy * q.x() + cy * q.z();
        const float ry = cp * q.y() - sp * rz;
        return ImVec2(c.x + scale * rx, c.y - scale * ry);
    };
    Eigen::Vector3f body = Eigen::Vector3f::Zero();
    for (const auto& corner : kCorners) {
        body += corner;
    }
    body *= 0.25f;

    const auto draw_order = [&](float cx, bool quadratic) {
        const ImVec2 center(cx, 0.5f * (body_top + body_bottom));
        std::array<ImVec2, kCorners.size()> corner2{};
        for (std::size_t i = 0; i < kCorners.size(); ++i) {
            corner2[i] = project(kCorners[i], center);
        }
        // A tet10 lifts each midside node off its chord onto the surface it is
        // meant to represent. A tet4 has no such node, so its interior map uses
        // the chord midpoint and stays affine — the same code, order zero lift.
        const float lift = quadratic ? midside_alpha : 0.0f;
        std::array<ImVec2, kEdges.size()> mid2{};
        std::array<Eigen::Vector3f, kEdges.size()> up{};
        for (std::size_t e = 0; e < kEdges.size(); ++e) {
            const Eigen::Vector3f a = kCorners[kEdges[e][0]];
            const Eigen::Vector3f b = kCorners[kEdges[e][1]];
            const Eigen::Vector3f chord = 0.5f * (a + b);
            const Eigen::Vector3f dir = (b - a).normalized();
            Eigen::Vector3f out = chord - body;
            out -= out.dot(dir) * dir;
            const float n = out.norm();
            up[e] = n > 1.0e-6f ? Eigen::Vector3f(out / n) : Eigen::Vector3f::UnitY();
            mid2[e] = project(chord + lift * kSag * (b - a).norm() * up[e], center);
        }
        const auto edge_point = [&](std::size_t e, float s) {
            const std::array<float, 3> n = basis(s);
            const ImVec2 a = corner2[kEdges[e][0]];
            const ImVec2 b = corner2[kEdges[e][1]];
            const ImVec2 m = mid2[e];
            return ImVec2(n[0] * a.x + n[1] * b.x + n[2] * m.x,
                          n[0] * a.y + n[1] * b.y + n[2] * m.y);
        };
        const auto face_point = [&](const std::array<std::size_t, 3>& face,
                                    const std::array<float, 3>& l) {
            ImVec2 p(0.0f, 0.0f);
            for (std::size_t k = 0; k < 3; ++k) {
                const float w = l[k] * (2.0f * l[k] - 1.0f);
                p.x += w * corner2[face[k]].x;
                p.y += w * corner2[face[k]].y;
            }
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t k2 = (k + 1) % 3;
                const float w = 4.0f * l[k] * l[k2];
                const ImVec2 m = mid2[edge_of(face[k], face[k2])];
                p.x += w * m.x;
                p.y += w * m.y;
            }
            return p;
        };

        const Eigen::Vector3f lit_a = kCorners[kEdges[kLitEdge][0]];
        const Eigen::Vector3f lit_b = kCorners[kEdges[kLitEdge][1]];
        const float lit_len = (lit_b - lit_a).norm();
        const auto surface_point = [&](float s, float dev) {
            return project(lit_a + (lit_b - lit_a) * s + up[kLitEdge] * (lit_len * dev),
                           center);
        };
        const auto element_dev = [&](float s) { return quadratic ? lift * p2_dev(s) : 0.0f; };

        // One flat tint for the body: the strain difference is stated in its own
        // swatch below, where a ramp reads at video scale instead of fighting
        // the element's own edges and nodes for the same pixels. Filled on a
        // barycentric grid through the quadratic face map so the tint reaches
        // the bulged rim instead of leaving a dark crescent inside each curved
        // edge, with the AA fringe off so the grid leaves no seams.
        const ImU32 tint = faded(palette.accent, 0.085f * corner_alpha * alpha);
        {
            constexpr int kSub = 5;
            const auto lattice = [](int p, int q) {
                const float l2 = static_cast<float>(p) / static_cast<float>(kSub);
                const float l3 = static_cast<float>(q) / static_cast<float>(kSub);
                return std::array<float, 3>{1.0f - l2 - l3, l2, l3};
            };
            const ImDrawListFlags saved = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (const auto& face : kFaces) {
                for (int i = 0; i < kSub; ++i) {
                    for (int j = 0; i + j < kSub; ++j) {
                        dl->AddTriangleFilled(face_point(face, lattice(i, j)),
                                              face_point(face, lattice(i + 1, j)),
                                              face_point(face, lattice(i, j + 1)), tint);
                        if (i + j + 2 <= kSub) {
                            dl->AddTriangleFilled(face_point(face, lattice(i + 1, j)),
                                                  face_point(face, lattice(i + 1, j + 1)),
                                                  face_point(face, lattice(i, j + 1)), tint);
                        }
                    }
                }
            }
            dl->Flags = saved;
        }

        // The lune between the element's edge and the CAD arc is the geometry
        // this order gets wrong: permanent for p1, collapsing as the p2 midside
        // node reaches the arc. Filled as an explicit triangle strip between
        // the two curves — a concave-polygon fill of a crescent this thin drops
        // slivers, and a gap that renders as dashes is worse than no gap.
        constexpr int kArcSamples = 26;
        if (arc_alpha > 0.0f) {
            // Anti-aliased fills fringe every triangle, and a strip of them
            // seams into visible hatching. The fringe is off for the strip only.
            const ImU32 gap_fill = faded(palette.status_err, 0.46f * arc_alpha * alpha);
            const ImDrawListFlags saved = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (int k = 0; k < kArcSamples; ++k) {
                const float s0 = static_cast<float>(k) / kArcSamples;
                const float s1 = static_cast<float>(k + 1) / kArcSamples;
                const ImVec2 a0 = surface_point(s0, exact_dev(s0));
                const ImVec2 a1 = surface_point(s1, exact_dev(s1));
                const ImVec2 e0 = surface_point(s0, element_dev(s0));
                const ImVec2 e1 = surface_point(s1, element_dev(s1));
                dl->AddTriangleFilled(a0, a1, e1, gap_fill);
                dl->AddTriangleFilled(a0, e1, e0, gap_fill);
            }
            dl->Flags = saved;
        }

        for (std::size_t e = 0; e < kEdges.size(); ++e) {
            if (quadratic) {
                // The chord the midside node left behind, kept as the dim tet4
                // reference inside the p2 column too.
                dl->AddLine(corner2[kEdges[e][0]], corner2[kEdges[e][1]],
                            faded(palette.text_dim, 0.30f * midside_alpha * alpha), 1.2f);
            }
            constexpr int kEdgeSamples = 18;
            for (int k = 0; k <= kEdgeSamples; ++k) {
                dl->PathLineTo(edge_point(e, static_cast<float>(k) / kEdgeSamples));
            }
            const bool lit = e == kLitEdge;
            dl->PathStroke(faded(lit ? palette.status_warn : palette.status_ok,
                                 (lit ? 0.95f : 0.60f) * corner_alpha * alpha),
                           0, lit ? 2.8f : 2.4f);
        }
        if (arc_alpha > 0.0f) {
            // The arc is stroked after the element's own edges, so in the p2
            // column the surface rides visibly along the edge that landed on
            // it — "lands on it" is a thing to see, not a claim to read.
            for (int k = 0; k <= kArcSamples; ++k) {
                const float s = static_cast<float>(k) / kArcSamples;
                dl->PathLineTo(surface_point(s, exact_dev(s)));
            }
            dl->PathStroke(faded(palette.text, 0.85f * arc_alpha * alpha), 0, 2.2f);
        }
        if (quadratic) {
            // Midside nodes are diamonds and corners are circles: at video
            // scale a shape difference survives where a radius difference does
            // not, and the count difference is the point of the card.
            for (const ImVec2 mid : mid2) {
                dl->AddNgonFilled(mid, 6.2f, faded(palette.status_ok, midside_alpha * alpha),
                                  4);
                dl->AddNgon(mid, 9.6f, faded(palette.status_ok, 0.40f * midside_alpha * alpha),
                            4, 1.4f);
            }
        }
        for (const ImVec2 point : corner2) {
            dl->AddCircleFilled(point, 6.6f, faded(palette.text, corner_alpha * alpha));
            dl->AddCircle(point, 9.6f,
                          faded(palette.accent_soft_top, 0.45f * corner_alpha * alpha), 0,
                          1.6f);
        }

        // The gap tick, then the same number in text: measured off the drawn
        // arc, so it falls as the midside node climbs onto it.
        float miss = 0.0f;
        for (int k = 0; k <= 64; ++k) {
            const float s = static_cast<float>(k) / 64.0f;
            miss = std::max(miss, std::abs(exact_dev(s) - element_dev(s)));
        }
        const ImVec4 miss_color = miss > 0.02f ? palette.status_err : palette.status_ok;
        dl->AddLine(surface_point(0.5f, element_dev(0.5f)),
                    surface_point(0.5f, exact_dev(0.5f)),
                    faded(miss_color, 0.90f * arc_alpha * alpha), 1.8f);

        const auto centered = [&](float y, float px, const char* text, const ImVec4& color,
                                  float a) {
            const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
            dl->AddText(font, px, ImVec2(cx - 0.5f * w, y), faded(color, a), text);
        };
        centered(head_y, type.label, quadratic ? "tet10 · p2" : "tet4 · p1",
                 quadratic ? palette.status_ok : palette.text, alpha);
        const int nodes =
            quadratic ? 4 + static_cast<int>(std::lround(6.0f * midside_alpha)) : 4;
        const int dof =
            quadratic ? 12 + static_cast<int>(std::lround(18.0f * midside_alpha)) : 12;
        centered(count_y, type.legend, fmt("%d nodes · %d DOF", nodes, dof).c_str(),
                 palette.text_dim, alpha);
        centered(miss_y, type.legend, fmt("%.1f%% off the CAD arc", 100.0f * miss).c_str(),
                 miss_color, arc_alpha * alpha);

        // Strain order as a swatch across the cell: a p1 tet can only hold one
        // constant gradient, so its bar is one tone, while a p2 tet's varies
        // linearly inside the cell, so its bar ramps. Same two end colours in
        // both, so the flat bar is read as the degenerate case of the ramp.
        if (field_alpha > 0.0f) {
            const float swatch_w = std::min(0.62f * col_w, 220.0f);
            const ImVec2 swatch_min(cx - 0.5f * swatch_w, swatch_y);
            const ImVec2 swatch_max(cx + 0.5f * swatch_w, swatch_y + 11.0f);
            const ImVec4 lo = palette.status_ok;
            const ImVec4 hi = palette.status_err;
            const float a = 0.80f * field_alpha * alpha;
            if (quadratic) {
                dl->AddRectFilledMultiColor(swatch_min, swatch_max, faded(lo, a), faded(hi, a),
                                            faded(hi, a), faded(lo, a));
            } else {
                dl->AddRectFilled(swatch_min, swatch_max, faded(mix_color(lo, hi, 0.5f), a));
            }
            dl->AddRect(swatch_min, swatch_max, faded(palette.border, a), 2.0f);
        }
        centered(caption_y, type.legend,
                 quadratic ? "strain varies linearly in the cell"
                           : "strain is constant in the cell",
                 palette.text_dim, field_alpha * alpha);
    };

    draw_order(card_min.x + 0.27f * region.x, false);
    draw_order(card_min.x + 0.73f * region.x, true);

    if (plot_alpha > 0.0f) {
        const ImVec2 frame_min(plot_left - 12.0f, plot_top - 10.0f);
        const ImVec2 frame_max(plot_right + 12.0f, plot_bottom + 10.0f);
        dl->AddRectFilled(frame_min, frame_max, faded(palette.panel_bg, 0.72f * plot_alpha),
                          6.0f);
        dl->AddRect(frame_min, frame_max, faded(palette.border, 0.85f * plot_alpha), 6.0f);
        constexpr float kPlotLo = -0.015f;
        constexpr float kPlotHi = kSag * 1.18f;
        const auto at = [&](float s, float v) {
            return ImVec2(plot_left + (plot_right - plot_left) * s,
                          plot_bottom -
                              (plot_bottom - plot_top) * (v - kPlotLo) / (kPlotHi - kPlotLo));
        };
        // The shaded area is the deviation a straight edge cannot represent: the
        // same lune as the left column, unrolled along the edge. Triangle strip
        // for the same reason as the lune — a thin concave fill drops slivers.
        constexpr int kSamples = 48;
        const ImU32 band_fill = faded(palette.status_err, 0.13f * plot_alpha);
        const ImDrawListFlags plot_saved = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        for (int k = 0; k < kSamples; ++k) {
            const float s0 = static_cast<float>(k) / kSamples;
            const float s1 = static_cast<float>(k + 1) / kSamples;
            dl->AddTriangleFilled(at(s0, exact_dev(s0)), at(s1, exact_dev(s1)), at(s1, 0.0f),
                                  band_fill);
            dl->AddTriangleFilled(at(s0, exact_dev(s0)), at(s1, 0.0f), at(s0, 0.0f),
                                  band_fill);
        }
        dl->Flags = plot_saved;
        for (int k = 0; k <= kSamples; ++k) {
            const float s = static_cast<float>(k) / kSamples;
            dl->PathLineTo(at(s, exact_dev(s)));
        }
        dl->PathStroke(faded(palette.text, 0.85f * plot_alpha), 0, 3.4f);
        dl->AddLine(at(0.0f, 0.0f), at(1.0f, 0.0f),
                    faded(palette.status_err, 0.95f * plot_alpha), 2.8f);
        for (int k = 0; k <= kSamples; ++k) {
            const float s = static_cast<float>(k) / kSamples;
            dl->PathLineTo(at(s, p2_dev(s)));
        }
        dl->PathStroke(faded(palette.status_ok, 0.95f * plot_alpha), 0, 2.2f);
        dl->AddText(font, type.legend,
                    ImVec2(at(0.16f, exact_dev(0.16f)).x,
                           at(0.16f, exact_dev(0.16f)).y - type.legend * 1.45f),
                    faded(palette.text, 0.90f * plot_alpha), "CAD arc");
        dl->AddText(font, type.legend, at(0.50f, 0.62f * kSag),
                    faded(palette.status_ok, 0.95f * plot_alpha), "p2 edge lands on it");
        dl->AddText(font, type.legend, ImVec2(at(0.05f, 0.0f).x, at(0.05f, 0.0f).y + 4.0f),
                    faded(palette.status_err, 0.95f * plot_alpha), "p1 edge");
        dl->AddLine(at(xi, kPlotLo), at(xi, kPlotHi),
                    faded(palette.status_warn, 0.45f * plot_alpha), 1.4f);
        dl->AddCircleFilled(at(xi, exact_dev(xi)), 4.2f, faded(palette.text, plot_alpha));
        dl->AddCircleFilled(at(xi, p2_dev(xi)), 4.2f, faded(palette.status_ok, plot_alpha));
        dl->AddCircleFilled(at(xi, 0.0f), 4.2f, faded(palette.status_err, plot_alpha));
        const std::string cursor =
            fmt("ξ %.2f · p1 off %.1f%% · p2 off %.2f%%", xi, 100.0f * exact_dev(xi),
                100.0f * std::abs(exact_dev(xi) - p2_dev(xi)));
        const float cursor_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, cursor.c_str()).x;
        dl->AddText(font, type.legend, ImVec2(plot_right - cursor_w - 4.0f, plot_top + 2.0f),
                    faded(palette.status_warn, plot_alpha), cursor.c_str());
        const char* legend = "deviation from the straight chord · % of edge length · the "
                             "shaded area is what p1 discards";
        dl->AddText(font, type.legend, ImVec2(plot_left, legend_y),
                    faded(palette.text_dim, plot_alpha), legend);
    }
    const char* honest = "supported element path · this take solved tet4 · p1";
    dl->AddText(font, type.legend, ImVec2(plot_left, note_y),
                faded(palette.text_dim, 0.85f * alpha), honest);

    const double q_mean =
        solved != nullptr && solved->quality_measured > 0 ? solved->quality_mean : 0.0;
    const double q_min =
        solved != nullptr && solved->quality_measured > 0 ? solved->quality_min : 0.0;
    // "qmean", not a macron: ImGui composes no combining marks, so q + U+0304
    // rasterised as a missing-glyph box in the published take.
    const std::string quality = fmt("measured mesh   qmin %.3f   qmean %.3f", q_min, q_mean);
    const ImVec2 quality_size =
        font->CalcTextSizeA(type.label, FLT_MAX, 0.0f, quality.c_str());
    dl->AddText(font, type.label,
                ImVec2(origin.x + 0.5f * region.x - 0.5f * quality_size.x, quality_y),
                faded(palette.status_ok, alpha), quality.c_str());
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

} // namespace

void draw_cinema_panel(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                       const CinemaHud& hud) {
    // Four views share one pane. The chosen measured network remains present
    // while its selected action lands as real emitted cells; only the finished
    // mesh hold hands it to the cell audit, so "decision" and "conversion" are
    // one causal visual sentence rather than separate chapters.
    float feature_alpha = 0.0f;
    float network_alpha = 0.0f;
    float cell_alpha = 0.0f;
    float equation_alpha = 0.0f;
    if (cue.act == CinemaAct::kSkeleton) {
        feature_alpha = cue.panel_open;
    } else if (cue.act == CinemaAct::kDeliberate) {
        const double bridge = std::min(1.3, 0.18 * cue.act_span);
        const float blend =
            static_cast<float>(smoothstep(cue.act_t / std::max(bridge, 1.0e-9)));
        feature_alpha = 1.0f - blend;
        network_alpha = blend;
    } else if (cue.act == CinemaAct::kBuild) {
        network_alpha = 1.0f;
    } else if (cue.act == CinemaAct::kMeshHold) {
        const float blend =
            static_cast<float>(smoothstep(cue.act_t / std::max(0.17 * cue.act_span, 1.0e-9)));
        network_alpha = 1.0f - blend;
        cell_alpha = blend;
    } else {
        cell_alpha = 1.0f - cue.equations_alpha;
        equation_alpha = cue.equations_alpha;
    }

    const ImVec2 origin = ImGui::GetCursorPos();
    draw_cinema_features(state, cue, type, feature_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_network(state, cue, type, network_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_cells(state, cue, type, hud, cell_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_equations(state, cue, type, hud, equation_alpha);
}

} // namespace polymesh::gui
