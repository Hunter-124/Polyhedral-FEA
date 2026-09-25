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
using detail::mesh_mix_text;
using detail::mesher_plain;
using detail::smoothstep;
using detail::stage_name;

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

std::string_view stage_name(std::string_view stage) {
    if (stage == "lattice") {
        return "laying down the cell grid";
    }
    if (stage == "expand") {
        return "splitting hexes into pyramids";
    }
    if (stage == "snap") {
        return "pulling the surface onto the CAD";
    }
    if (stage == "peel") {
        return "removing the cells the snap flattened";
    }
    if (stage == "reproject") {
        return "re-projecting the stragglers";
    }
    if (stage == "smooth") {
        return "evening out the surface spacing";
    }
    if (stage == "resnap") {
        return "snapping what moved, again";
    }
    if (stage == "pin") {
        return "pinning CAD edges and corners";
    }
    if (stage == "fill") {
        return "converting to solver elements";
    }
    if (stage == "ship") {
        return "final checks";
    }
    return stage;
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

ImU32 faded(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

std::string mesh_mix_text(const CinemaMeshInsight& insight) {
    std::string out;
    for (std::size_t i = 0; i < insight.type_counts.size(); ++i) {
        const std::size_t count = insight.type_counts[i];
        if (count == 0) {
            continue;
        }
        if (!out.empty()) {
            out += " · ";
        }
        const auto type = static_cast<fea::ElementType>(i);
        out += std::format("{} {}", grouped(count), fea::element_type_name(type));
    }
    return out.empty() ? std::string("no cells measured") : out;
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

std::string_view head_name(std::string_view tensor) {
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

    std::string title = "Structural solve — the numbers become a picture";
    std::string explain =
        "The same measured field on the part is summarised here for a fast engineering read.";
    std::string live = "waiting for an authoritative solve stage";
    std::string technical;
    Runs equation{plain("K u = f", palette.accent)};

    const CinemaHistogram* histogram = nullptr;
    const char* histogram_unit = "";
    double histogram_scale = 1.0;
    if (stage != nullptr && on_stress) {
        title = "Stress distribution — where the material works hardest";
        explain =
            "Each bar counts mesh nodes in a von Mises stress range; this is space, not time.";
        equation = {plain("K u = f   ·   ε = B u   ·   σ = D(E,ν) ε   ·   σ", palette.text),
                    sub("vm", palette.text), plain(" = equivalent stress", palette.text)};
        live = fmt("peak %.4g MPa · mean over %s solved nodes",
                   stage->result.max_von_mises / 1e6,
                   grouped(stage->result.von_mises.size()).c_str());
        if (stage_index < state.stress_histograms.size()) {
            histogram = &state.stress_histograms[stage_index];
            histogram_unit = "MPa";
            histogram_scale = 1e-6;
        }
    } else if (stage != nullptr && on_gradient) {
        title = "Stress gradient — how quickly the field changes";
        explain =
            "Tall bars mean many nodes share that change rate; the far right locates sharp hot spots.";
        equation = {plain("|∇σ", palette.text), sub("vm", palette.text),
                    plain("| from a least-squares fit over each node's cells", palette.text)};
        const double gmax = const_cast<CinemaState&>(state).gradient_max(stage_index);
        live = gmax > 0.0 ? fmt("steepest %.4g MPa/mm · spatial node distribution", gmax / 1e9)
                          : std::string("no gradient could be recovered on this pass");
        histogram = &const_cast<CinemaState&>(state).gradient_histogram(stage_index);
        histogram_unit = "MPa/mm";
        histogram_scale = 1e-9;
    } else if (stage != nullptr && on_error) {
        title = "Estimated discretisation error — is the mesh fine enough?";
        explain =
            "The measured ZZ indicator is compared with the requested stopping target.";
        equation = {plain("η", palette.text), sub("e", palette.text),
                    plain(" = ‖σ* − σ", palette.text), sub("h", palette.text),
                    plain("‖", palette.text), sub("E,e", palette.text),
                    plain(" / ‖σ", palette.text), sub("h", palette.text),
                    plain("‖", palette.text), sub("E,Ω", palette.text)};
        live = fmt("global estimate %.3g%% · target %.3g%%", stage->trace.global_eta * 100.0,
                   hud.eta_target * 100.0);
    } else if (stage != nullptr && on_refine) {
        title = "Adaptive refinement — spend cells where error is concentrated";
        explain =
            "The largest local indicators are marked first, then the next real mesh replaces them.";
        equation = {plain("Σ", palette.text), sub("marked", palette.text),
                    plain(" η", palette.text), sub("e", palette.text),
                    sup("2", palette.text), plain(" ≥ θ Σ", palette.text),
                    sub("all", palette.text), plain(" η", palette.text),
                    sub("e", palette.text), sup("2", palette.text)};
        live = fmt("%s cells marked · pass %zu → pass %zu",
                   grouped(stage->trace.n_h_mark).c_str(), stage_index, stage_index + 1);
    } else if (stage != nullptr && on_ramp) {
        title = "Load response — force, stress and deflection rise together";
        explain =
            "Linear elastostatics puts stress and displacement on the same exact straight line.";
        equation = {plain("u(λ) = λu   ·   σ(λ) = λσ   ·   f(λ) = λf", palette.text)};
        live = fmt("λ %.3f · %.4g kN · %.4g MPa · %.4g mm physical deflection",
                   cue.load_factor, cue.load_factor * hud.load_newtons / 1e3,
                   cue.load_factor * stage->result.max_von_mises / 1e6,
                   cue.load_factor * stage->result.max_displacement * 1e3);
    }

    const std::string_view token = cinema_solver_token(state);
    const char* method = token == "direct_ldlt"
                             ? "direct LDLT"
                             : (token == "cg" ? "conjugate gradient" : "method not reported");
    if (stage != nullptr) {
        technical = fmt("%s unknowns · %s · E %.6g GPa · ν %.4g",
                        grouped(stage->trace.n_dof).c_str(), method,
                        hud.youngs_modulus / 1e9, hud.poissons_ratio);
    }

    dl->AddText(font, type.caption, origin, faded(palette.text, alpha), title.c_str());
    dl->AddText(font, type.legend,
                ImVec2(origin.x, origin.y + type.caption * 1.45f),
                faded(palette.text_dim, alpha), explain.c_str(), nullptr, region.x);
    const float equation_y = origin.y + type.caption * 3.05f;
    const float equation_w = runs_width(font, type.caption, equation);
    const float equation_size =
        equation_w > region.x && equation_w > 0.0f
            ? std::max(12.0f, type.caption * region.x / equation_w)
            : type.caption;
    draw_runs(dl, font, equation_size, ImVec2(origin.x, equation_y), equation, alpha);
    dl->AddText(font, type.label,
                ImVec2(origin.x, equation_y + type.caption * 1.45f),
                faded(palette.accent, alpha), live.c_str(), nullptr, region.x);

    const float pipeline_h = type.legend * 4.8f;
    const float chart_top = equation_y + type.caption * 3.0f;
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
        dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                    faded(palette.border, 0.42f * alpha), 1.0f);
    }
    dl->AddLine(ImVec2(left, bottom), ImVec2(right, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);
    dl->AddLine(ImVec2(left, top), ImVec2(left, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);

    if (histogram != nullptr && histogram->samples > 0 && histogram->tallest_bin > 0) {
        const float plot_w = right - left;
        const float plot_h = bottom - top;
        const float slot = plot_w / static_cast<float>(histogram->bins.size());
        for (std::size_t i = 0; i < histogram->bins.size(); ++i) {
            const float height =
                plot_h * static_cast<float>(histogram->bins[i]) /
                static_cast<float>(histogram->tallest_bin);
            const float x0 = left + static_cast<float>(i) * slot + 1.0f;
            const float x1 = left + static_cast<float>(i + 1) * slot - 1.0f;
            const float t = (static_cast<float>(i) + 0.5f) /
                            static_cast<float>(histogram->bins.size());
            dl->AddRectFilled(ImVec2(x0, bottom - height), ImVec2(x1, bottom),
                              rgba(fea_colormap(t), 0.82f * alpha), 2.0f);
        }
        const double span = histogram->p99 - histogram->min;
        const float mean_x = span > 0.0
                                 ? left + (right - left) * std::clamp(
                                                                    static_cast<float>(
                                                                        (histogram->mean -
                                                                         histogram->min) /
                                                                        span),
                                                                    0.0f, 1.0f)
                                 : left;
        dl->AddLine(ImVec2(mean_x, top), ImVec2(mean_x, bottom),
                    faded(palette.accent, alpha), 2.0f);
        const std::string minimum =
            fmt("%.3g %s", histogram->min * histogram_scale, histogram_unit);
        const std::string maximum =
            fmt("≥ p99 %.3g %s", histogram->p99 * histogram_scale, histogram_unit);
        const std::string mean =
            fmt("mean %.3g %s", histogram->mean * histogram_scale, histogram_unit);
        dl->AddText(font, type.legend, ImVec2(left, bottom + 8.0f),
                    faded(palette.text_dim, alpha), minimum.c_str());
        const float max_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, maximum.c_str()).x;
        dl->AddText(font, type.legend, ImVec2(right - max_w, bottom + 8.0f),
                    faded(palette.text_dim, alpha), maximum.c_str());
        const float mean_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, mean.c_str()).x;
        dl->AddText(font, type.legend,
                    ImVec2(std::min(mean_x + 5.0f, right - mean_w), top + 4.0f),
                    faded(palette.accent, alpha), mean.c_str());
    } else if (stage != nullptr && on_error) {
        const double measured = stage->trace.global_eta * 100.0;
        const double target = hud.eta_target * 100.0;
        const double scale = std::max({measured, target, 1.0e-12});
        const std::array<double, 2> values{{target, measured}};
        const std::array<const char*, 2> labels{{"requested target", "measured estimate"}};
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
            dl->AddText(font, type.legend,
                        ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
                        faded(palette.text_dim, alpha), labels[i]);
        }
    } else if (stage != nullptr && on_refine) {
        const std::size_t before = stage->trace.n_elems;
        const std::size_t after = stage_index + 1 < state.solve_stages.size()
                                      ? state.solve_stages[stage_index + 1].trace.n_elems
                                      : before;
        const std::array<std::size_t, 2> counts{{before, after}};
        const std::array<const char*, 2> labels{{"before", "after"}};
        const std::size_t max_count = std::max<std::size_t>({before, after, 1});
        const float bar_w = (right - left) * 0.24f;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const float x = left + (right - left) * (0.24f + 0.52f * static_cast<float>(i));
            const float h = (bottom - top) * static_cast<float>(counts[i]) /
                            static_cast<float>(max_count);
            dl->AddRectFilled(ImVec2(x - 0.5f * bar_w, bottom - h),
                              ImVec2(x + 0.5f * bar_w, bottom),
                              faded(i == 0 ? palette.text_dim : palette.accent,
                                    0.82f * alpha),
                              5.0f);
            const std::string value = grouped(counts[i]) + " cells";
            dl->AddText(font, type.label,
                        ImVec2(x - 0.5f * bar_w, bottom - h - type.label * 1.35f),
                        faded(i == 0 ? palette.text : palette.accent, alpha),
                        value.c_str());
            dl->AddText(font, type.legend,
                        ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
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
        dl->AddCircle(ImVec2(x, y), 15.0f,
                      faded(palette.accent_soft_top, 0.55f * alpha), 0, 2.0f);
        dl->AddText(font, type.legend, ImVec2(left, bottom + 8.0f),
                    faded(palette.text_dim, alpha), "0 load");
        dl->AddText(font, type.legend,
                    ImVec2(right - type.legend * 4.3f, bottom + 8.0f),
                    faded(palette.text_dim, alpha), "full load");
        dl->AddText(font, type.label, ImVec2(left + 8.0f, top + 8.0f),
                    faded(palette.text, alpha),
                    "stress ratio = displacement ratio = λ");
    }

    static constexpr std::array<const char*, 6> kSteps{
        {"solve", "stress", "gradient", "estimate", "refine", "response"}};
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
    const float chips_y = chart_max.y + type.legend * 0.55f;
    const float gap = 5.0f;
    const float chip_w = (region.x - gap * 5.0f) / 6.0f;
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
        const float x = origin.x + static_cast<float>(i) * (chip_w + gap);
        const bool lit_step = static_cast<int>(i) == active;
        dl->AddRectFilled(ImVec2(x, chips_y),
                          ImVec2(x + chip_w, chips_y + type.legend * 2.0f),
                          faded(lit_step ? palette.accent_mid : palette.panel_bg,
                                alpha * (lit_step ? 0.82f : 0.55f)),
                          5.0f);
        dl->AddText(font, type.legend,
                    ImVec2(x + 7.0f, chips_y + type.legend * 0.42f),
                    faded(lit_step ? palette.text : palette.text_dim, alpha),
                    kSteps[i]);
    }
    if (!technical.empty()) {
        dl->AddText(font, type.legend,
                    ImVec2(origin.x, chips_y + type.legend * 2.45f),
                    faded(palette.text_dim, alpha), technical.c_str(), nullptr, region.x);
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

    // Per-layer normalisation. Trunk and head magnitudes differ by roughly a
    // factor of ten, so one shared scale would flatten the trunk into a flat
    // grey column.
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

    // ---- what the panel says, before anything is placed ------------------
    // Caption and legend only. The exhaustive disclosures (normalisation
    // constants, edge block shapes, the exact colormap) are in
    // docs/assets/cinema/NOTES.md, which the strip names on screen.
    std::string caption =
        "the deployed network scoring one candidate mesh per beat — its own tensors";
    if (cue.chosen_pass_held) {
        if (state.decision_applied) {
            caption =
                "the measured pass that chose this mesh — replayed while its real cells land";
        } else if (state.decision_vetoed) {
            caption =
                "the measured OOD check — the configured baseline remains active";
        } else if (state.decision_unrecognized) {
            caption =
                "the measured pass named an unavailable mesher — the studio setup remains";
        }
    }
    const std::string legend =
        frame == nullptr
            ? std::string("structure only: no forward pass is being shown on this beat")
            : fmt("%zu strongest of %zu connections · circle size is how hard a unit is "
                  "firing · blue is negative, red positive · head names in plain language",
                  drawn, total_connections);

    // ---- geometry, derived from the measured text ------------------------
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    const auto para_h = [&](float size, const std::string& s) {
        return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str()).y +
               std::floor(size * 0.35f);
    };
    const float caption_h = para_h(type.caption, caption);
    const float legend_h = para_h(type.legend, legend);
    dl->AddText(font, type.caption, origin, faded(palette.text, alpha), caption.c_str(),
                nullptr, wrap);

    const float graph_top = origin.y + caption_h;
    const float graph_h = std::max(140.0f, region.y - caption_h - legend_h);
    dl->AddText(font, type.legend, ImVec2(origin.x, graph_top + graph_h),
                faded(palette.text_dim, alpha), legend.c_str(), nullptr, wrap);

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
    const float chip_h =
        frame != nullptr
            ? std::floor(type.label * (state.decision_applied ? 3.0f : 4.5f))
            : 0.0f;
    const float lanes_top = graph_top + header_h;
    const float lanes_h = std::max(
        120.0f, graph_h - header_h - chip_h - std::floor(type.legend * 0.6f));
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
            at = static_cast<float>(
                std::fmod(std::max(cue.act_t, 0.0) / beat, 1.0) *
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
        dl->AddLine(ImVec2(origin.x + kSidePad, row_y(l)),
                    ImVec2(origin.x + region.x - kSidePad, row_y(l)),
                    faded(l == 3 ? palette.accent : palette.text_dim,
                          (0.08f + 0.10f * pulse) * alpha),
                    1.0f);
    }

    // ---- connections ----------------------------------------------------
    if (drawn > 0) {
        const float inv_max = value_max > 0.0f ? 1.0f / value_max : 0.0f;
        for (std::size_t k = 0; k < drawn; ++k) {
            const auto& pick = picks[k];
            const auto b = static_cast<std::size_t>(pick.block);
            const float t = std::clamp(pick.value * inv_max, -1.0f, 1.0f);
            const float weight = std::clamp(pick.rank * inv_max, 0.0f, 1.0f);
            const float pulse = wave_strength(static_cast<float>(b) + 0.5f);
            dl->AddLine(node_point(b, static_cast<std::size_t>(pick.src)),
                        node_point(b + 1, static_cast<std::size_t>(pick.dst)),
                        rgba(signed_colormap(t),
                             (0.04f + 0.86f * weight) * pulse * alpha),
                        0.55f + 1.55f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.8f;
    static constexpr std::array<const char*, 4> kLanePlain{
        {"1  what it measures", "2  hidden layer 1", "3  hidden layer 2",
         "4  what it predicts"}};
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float spacing = band_w / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 2.6f, kNodeRadiusMax);
        const std::string header = std::format("{} · {} units", kLanePlain[l], layer.size);
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + kSidePad,
                           lanes_top + lane_h * static_cast<float>(l)),
                    faded(l == 3 ? palette.accent : palette.text_dim,
                          alpha * wave_strength(static_cast<float>(l))),
                    header.c_str());
        const float pulse = wave_strength(static_cast<float>(l));
        for (std::size_t i = 0; i < layer.size; ++i) {
            const float a = values[l] != nullptr ? (*values[l])[i] : 0.0f;
            const float mag = std::clamp(std::fabs(a) / layer_max[l], 0.0f, 1.0f);
            const float r = kNodeMin + (r_max - kNodeMin) * mag;
            const ImVec2 point = node_point(l, i);
            const auto rgb = signed_colormap(a / layer_max[l]);
            if (mag > 0.30f) {
                dl->AddCircleFilled(point, r * 3.0f,
                                    rgba(rgb, 0.065f * mag * pulse * alpha));
                dl->AddCircleFilled(point, r * 1.8f,
                                    rgba(rgb, 0.125f * mag * pulse * alpha));
            }
            dl->AddCircleFilled(point, r,
                                rgba(rgb, (0.42f + 0.58f * mag) * alpha));
            const bool chosen_head = l == 3 && static_cast<int>(i) == winner;
            if (mag > 0.55f || chosen_head) {
                dl->AddCircle(point, r + (chosen_head ? 3.0f : 1.4f),
                              faded(chosen_head ? palette.accent : palette.text,
                                    (chosen_head ? 0.95f : 0.45f * mag) * pulse * alpha),
                              0, chosen_head ? 2.4f : 1.2f);
            }
        }
    }

    // One outcome chip instead of every head label. Every head node and value
    // remains in the measured row above; NOTES.md carries the complete name
    // table, while the film names the selected action that produced the mesh.
    if (frame != nullptr) {
        const float chip_top = lanes_top + lanes_h + type.legend * 0.25f;
        dl->AddRectFilled(ImVec2(origin.x, chip_top),
                          ImVec2(origin.x + region.x, chip_top + chip_h),
                          faded(palette.panel_bg, 0.72f * alpha), 7.0f);
        dl->AddRect(ImVec2(origin.x, chip_top),
                    ImVec2(origin.x + region.x, chip_top + chip_h),
                    faded(palette.accent, 0.75f * alpha), 7.0f, 0, 1.4f);
        std::string selected = "ADVISOR RESULT UNAVAILABLE";
        if (state.decision_applied) {
            selected = winner >= 0
                           ? std::format("SELECTED  {}", winner_text)
                           : std::string("SELECTED  action head unavailable");
        } else if (state.decision_vetoed) {
            selected = "ADVISOR ABSTAINED  ·  configured baseline remains active";
        } else if (state.decision_unrecognized) {
            selected = "UNRECOGNISED ACTION  ·  studio setup remains";
        }
        dl->AddText(font, type.label, ImVec2(origin.x + 12.0f, chip_top + 8.0f),
                    faded(palette.accent, alpha), selected.c_str());
        const std::string action =
            state.decision_applied
                ? fmt("%s · h/L %.3g · order %d · %d adapt pass%s",
                      std::string(mesher_plain(frame->action.mesher)).c_str(),
                      frame->action.h_rel, frame->action.order,
                      frame->action.adapt_passes,
                      frame->action.adapt_passes == 1 ? "" : "es")
                : state.decision_note;
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + 12.0f, chip_top + type.label * 1.65f),
                    faded(state.decision_applied ? palette.text : palette.status_warn, alpha),
                    action.c_str(), nullptr, region.x - 24.0f);
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
    const float wrap = std::max(120.0f, region.x);

    const bool curvature_sizing =
        state.sizing.spectral.applied && state.sizing.brep_curvature;
    const bool bc_sizing = state.sizing.spectral.applied && state.sizing.bc_seeds > 0;
    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                curvature_sizing
                    ? "Exact curvature → frequency modes → target spacing"
                    : "Exact curvature study · support/load target spacing");
    const std::string summary =
        state.sizing.spectral.applied
            ? fmt("%s / %s field modes · %.2f%% energy · N density %.0f → %.0f",
                  grouped(state.sizing.spectral.modes_kept).c_str(),
                  grouped(state.sizing.spectral.modes_total).c_str(),
                  100.0 * state.sizing.spectral.energy_kept,
                  state.sizing.spectral.predicted_before,
                  state.sizing.spectral.predicted_after)
            : std::string("uniform solve · FFT is geometry evidence, not a sizing input");
    dl->AddText(font, type.label, ImVec2(origin.x, origin.y + type.caption * 1.55f),
                faded(state.sizing.spectral.applied ? palette.accent : palette.text_dim, alpha),
                summary.c_str(), nullptr, wrap);
    const std::string rings =
        fmt("%s on-part samples · ring diameter = target h · orange fine → cyan coarse",
            grouped(state.sizing.field_points.size()).c_str());
    dl->AddText(font, type.legend, ImVec2(origin.x, origin.y + type.caption * 2.55f),
                faded(palette.text_dim, alpha), rings.c_str(), nullptr, wrap);

    const float chart_top = origin.y + type.caption * 3.65f;
    const float chart_h = std::max(
        240.0f, region.y - (chart_top - origin.y) - type.label * 6.0f);
    const float chart_w = std::max(120.0f, region.x);
    dl->AddRectFilled(ImVec2(origin.x, chart_top),
                      ImVec2(origin.x + chart_w, chart_top + chart_h),
                      faded(palette.panel_bg, 0.52f * alpha), 6.0f);
    dl->AddRect(ImVec2(origin.x, chart_top),
                ImVec2(origin.x + chart_w, chart_top + chart_h),
                faded(palette.border, 0.8f * alpha), 6.0f);

    const float pad = 13.0f;
    const float split_y = chart_top + chart_h * 0.58f;
    dl->AddLine(ImVec2(origin.x + pad, split_y),
                ImVec2(origin.x + chart_w - pad, split_y),
                faded(palette.border, 0.72f * alpha), 1.0f);
    dl->AddText(font, type.legend, ImVec2(origin.x + pad, chart_top + 8.0f),
                faded(palette.text_dim, alpha), "SIGNAL DOMAIN  κ(s) along selected CAD edge");
    dl->AddText(font, type.legend, ImVec2(origin.x + pad, split_y + 7.0f),
                faded(palette.text_dim, alpha),
                "FREQUENCY DOMAIN  |FFT(κ − mean κ)| · teal retained, grey discarded");

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
            const float x = origin.x + pad +
                            (chart_w - 2.0f * pad) * static_cast<float>(station);
            const float y = plot_bottom -
                            (plot_bottom - plot_top) * static_cast<float>((value - lo) / span);
            return ImVec2(x, y);
        };

        const double revealed =
            cue.spectral_edge_reveal * static_cast<double>(raw.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(revealed)), raw.size() - 1);
        for (std::size_t i = 1; i <= whole; ++i) {
            dl->AddLine(point(stations[i - 1], raw[i - 1]),
                        point(stations[i], raw[i]),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double y0 = raw[i - 1] +
                              cue.spectral_filter_mix * (filtered[i - 1] - raw[i - 1]);
            const double y1 =
                raw[i] + cue.spectral_filter_mix * (filtered[i] - raw[i]);
            dl->AddLine(point(stations[i - 1], y0), point(stations[i], y1),
                        faded(palette.accent, alpha), 2.6f);
            dl->AddCircleFilled(point(stations[i], y1), 2.6f,
                                faded(palette.accent_soft_top, alpha));
        }
        if (whole + 1 < raw.size()) {
            const double part = revealed - static_cast<double>(whole);
            const double station =
                stations[whole] + part * (stations[whole + 1] - stations[whole]);
            const double raw_value =
                raw[whole] + part * (raw[whole + 1] - raw[whole]);
            const double filtered_value =
                filtered[whole] + part * (filtered[whole + 1] - filtered[whole]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            dl->AddLine(point(stations[whole], raw[whole]), point(station, raw_value),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double start = raw[whole] +
                                 cue.spectral_filter_mix *
                                     (filtered[whole] - raw[whole]);
            dl->AddLine(point(stations[whole], start), point(station, value),
                        faded(palette.accent, alpha), 2.6f);
            const ImVec2 scan = point(station, value);
            dl->AddLine(ImVec2(scan.x, plot_top), ImVec2(scan.x, plot_bottom),
                        faded(palette.accent_soft_top, 0.5f * alpha), 1.0f);
            dl->AddCircleFilled(scan, 4.2f, faded(palette.accent_soft_top, alpha));
        }
        const std::string edge =
            fmt("edge %u · %.3g mm · %s/%s curve modes",
                state.sizing.edge_id, state.sizing.edge_length * 1e3,
                grouped(state.sizing.curve_modes_kept).c_str(),
                grouped(state.sizing.curve_modes_total).c_str());
        const float ew = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, edge.c_str()).x;
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + chart_w - ew - pad, chart_top + 8.0f),
                    faded(palette.accent, alpha), edge.c_str());
    } else {
        dl->AddText(font, type.label, ImVec2(origin.x + pad, chart_top + 36.0f),
                    faded(palette.status_warn, alpha),
                    "No curved CAD edge supplied a resolvable curvature trace.");
    }

    const auto& spectrum = state.sizing.curve_spectrum;
    const auto& kept = state.sizing.curve_mode_kept;
    if (spectrum.size() >= 2 && kept.size() == spectrum.size()) {
        // DC is the mean curvature, not spacing variation. `truncate_modes`
        // always preserves it and excludes it from modes_total, so omitting it
        // here both matches the report's denominator and stops one huge bar
        // from flattening every explanatory non-DC mode.
        const double max_magnitude =
            std::max(*std::max_element(spectrum.begin() + 1, spectrum.end()),
                     1.0e-12);
        const float bars_top = split_y + type.legend * 1.8f;
        const float bars_bottom = chart_top + chart_h - 11.0f;
        const float bars_h = std::max(1.0f, bars_bottom - bars_top);
        const float bars_w = chart_w - 2.0f * pad;
        const std::size_t mode_count = spectrum.size() - 1;
        const float slot = bars_w / static_cast<float>(mode_count);
        const std::size_t visible = std::min(
            mode_count,
            static_cast<std::size_t>(std::ceil(
                cue.spectral_spectrum_reveal * static_cast<double>(mode_count))));
        const double log_max = std::log1p(max_magnitude);
        for (std::size_t mode = 0; mode < visible; ++mode) {
            const std::size_t i = mode + 1;
            const float magnitude = static_cast<float>(
                std::log1p(spectrum[i]) / std::max(log_max, 1.0e-12));
            const float x0 = origin.x + pad + static_cast<float>(mode) * slot;
            const float x1 = x0 + std::max(1.0f, slot - 1.0f);
            const float y0 = bars_bottom - bars_h * magnitude;
            const bool survives = kept[i] != 0;
            const float discarded_alpha =
                static_cast<float>(1.0 - 0.82 * cue.spectral_filter_mix);
            const ImVec4 color =
                survives ? palette.accent
                         : ImVec4(palette.text_dim.x, palette.text_dim.y,
                                  palette.text_dim.z,
                                  palette.text_dim.w * discarded_alpha);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, bars_bottom),
                              faded(color, alpha), 1.0f);
        }
    }
    const std::array<const char*, 5> steps{
        "1 sample κ(s)", "2 FFT", "3 rank energy", "4 inverse FFT", "5 map h(x)"};
    const std::array<double, 5> progress{
        cue.spectral_edge_reveal, cue.spectral_spectrum_reveal,
        cue.spectral_filter_mix, cue.spectral_filter_mix,
        cue.spectral_field_reveal};
    const float gap = 6.0f;
    const float box_w = (chart_w - gap * 4.0f) / 5.0f;
    const float box_y = chart_top + chart_h + type.label * 0.85f;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const float x = origin.x + static_cast<float>(i) * (box_w + gap);
        const bool active = progress[i] > (i == 3 ? 0.45 : 0.04);
        dl->AddRectFilled(ImVec2(x, box_y), ImVec2(x + box_w, box_y + type.label * 2.2f),
                          faded(active ? palette.accent_mid : palette.panel_bg,
                                alpha * (active ? 0.66f : 0.44f)),
                          5.0f);
        dl->AddText(font, type.legend, ImVec2(x + 7.0f, box_y + 7.0f),
                    faded(active ? palette.text : palette.text_dim, alpha),
                    steps[i], nullptr, std::max(20.0f, box_w - 14.0f));
    }
    const std::string floor =
        state.sizing.brep_curvature
            ? "Exact BRep curvature is re-imposed after filtering."
            : (state.skeleton_source == SkeletonSource::kBrepEdges
                   ? (bc_sizing
                          ? "Exact curve measured above · h(x) is driven by supports/load."
                          : "Exact curve measured above · uniform mesh does not grade from it.")
                   : "No exact-BRep curvature report is available for this input.");
    dl->AddText(font, type.legend, ImVec2(origin.x, box_y + type.label * 2.7f),
                faded(state.sizing.brep_curvature ? palette.status_ok : palette.status_warn,
                      alpha),
                floor.c_str(), nullptr, wrap);
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_cells(const CinemaState& state, const CinemaCue& cue,
                       const CinemaType& type, const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
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
    const float wrap = std::max(120.0f, region.x);

    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                "Cell microscope — actual emitted topology");
    std::string stage = "waiting for the first emitted cell";
    if (index < state.stages.size() && emitted != nullptr) {
        stage = std::format("{} · {}", stage_name(state.stages[index].stage),
                            mesh_mix_text(*emitted));
    }
    dl->AddText(font, type.label, ImVec2(origin.x, origin.y + type.caption * 1.55f),
                faded(palette.accent, alpha), stage.c_str(), nullptr, wrap);

    const float bar_y = origin.y + type.caption * 3.2f;
    const float bar_h = 18.0f;
    if (emitted != nullptr) {
        const std::size_t total =
            std::max<std::size_t>(1, std::accumulate(emitted->type_counts.begin(),
                                                     emitted->type_counts.end(),
                                                     std::size_t{0}));
        float x = origin.x;
        for (std::size_t i = 0; i < emitted->type_counts.size(); ++i) {
            if (emitted->type_counts[i] == 0) {
                continue;
            }
            const float w = region.x * static_cast<float>(emitted->type_counts[i]) /
                            static_cast<float>(total);
            const auto rgb = element_type_color(static_cast<fea::ElementType>(i));
            dl->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + w, bar_y + bar_h),
                              rgba(rgb, alpha), 2.0f);
            x += w;
        }
    }

    const float card_top = bar_y + bar_h + type.label * 1.4f;
    const float card_h = std::max(
        260.0f, region.y - (card_top - origin.y) - type.label * 6.4f);
    const float card_gap = 12.0f;
    const float card_w = (region.x - card_gap) * 0.5f;
    const auto draw_tet = [&](float x, bool quadratic) {
        dl->AddRectFilled(ImVec2(x, card_top), ImVec2(x + card_w, card_top + card_h),
                          faded(palette.panel_bg, 0.58f * alpha), 7.0f);
        dl->AddRect(ImVec2(x, card_top), ImVec2(x + card_w, card_top + card_h),
                    faded(palette.border, alpha), 7.0f);
        const std::array<ImVec2, 4> p{{
            {x + card_w * 0.50f, card_top + card_h * 0.19f},
            {x + card_w * 0.18f, card_top + card_h * 0.78f},
            {x + card_w * 0.82f, card_top + card_h * 0.78f},
            {x + card_w * 0.63f, card_top + card_h * 0.53f},
        }};
        constexpr std::array<std::array<std::size_t, 2>, 6> edges{{
            {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}},
        }};
        for (const auto& edge : edges) {
            dl->AddLine(p[edge[0]], p[edge[1]], faded(palette.text, 0.72f * alpha), 2.0f);
            if (quadratic) {
                const ImVec2 mid{0.5f * (p[edge[0]].x + p[edge[1]].x),
                                 0.5f * (p[edge[0]].y + p[edge[1]].y)};
                dl->AddCircleFilled(mid, 4.2f, faded(palette.accent, alpha));
            }
        }
        for (const ImVec2 point : p) {
            dl->AddCircleFilled(point, 6.0f, faded(palette.text, alpha));
        }
        const char* title = quadratic ? "order 2 · tet10" : "order 1 · tet4";
        const char* subline =
            quadratic ? "six midside nodes bend with the exact CAD"
                      : "four corners define the linear fill";
        dl->AddText(font, type.label, ImVec2(x + 12.0f, card_top + 10.0f),
                    faded(quadratic ? palette.accent : palette.text, alpha), title);
        dl->AddText(font, type.legend,
                    ImVec2(x + 12.0f, card_top + card_h - type.legend * 2.2f),
                    faded(palette.text_dim, alpha), subline, nullptr, card_w - 24.0f);
    };
    draw_tet(origin.x, false);
    draw_tet(origin.x + card_w + card_gap, hud.order >= 2);

    const float facts_y = card_top + card_h + type.label * 1.1f;
    std::string quality = "quality summary arrives with the emitted mesh";
    if (solved != nullptr && solved->quality_measured > 0) {
        quality = fmt("shape quality  min %.4g · mean %.4g · %s cells measured",
                      solved->quality_min, solved->quality_mean,
                      grouped(solved->quality_measured).c_str());
    }
    dl->AddText(font, type.label, ImVec2(origin.x, facts_y),
                faded(palette.status_ok, alpha), quality.c_str(), nullptr, wrap);
    const std::string facts = fmt(
        "spectral sizing %s · exact BRep curvature %s · polynomial order %d · "
        "ZZ recovery %s",
        state.sizing.spectral.applied ? "on" : "off",
        state.sizing.brep_curvature ? "on" : "off", hud.order,
        state.solve_stages.empty() ? "waiting" : "measured");
    dl->AddText(font, type.legend, ImVec2(origin.x, facts_y + type.label * 1.7f),
                faded(palette.text_dim, alpha), facts.c_str(), nullptr, wrap);
    dl->AddText(font, type.legend, ImVec2(origin.x, facts_y + type.label * 3.1f),
                faded(palette.status_warn, 0.78f * alpha),
                "experimental alternatives: varyhedron · CVT poly-VEM · octahedral — "
                "not used in this verified solve",
                nullptr, wrap);
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
        const float blend = static_cast<float>(
            smoothstep(cue.act_t / std::max(bridge, 1.0e-9)));
        feature_alpha = 1.0f - blend;
        network_alpha = blend;
    } else if (cue.act == CinemaAct::kBuild) {
        network_alpha = 1.0f;
    } else if (cue.act == CinemaAct::kMeshHold) {
        const float blend = static_cast<float>(
            smoothstep(cue.act_t / std::max(0.9, 0.17 * cue.act_span)));
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
