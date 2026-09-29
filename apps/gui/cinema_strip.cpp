// SPDX-License-Identifier: BSD-3-Clause
// Cinema typography and the bottom ledger: chapter bar, captions, provenance.
#include "cinema.hpp"
#include "cinema_internal.hpp"

#include "theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
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

CinemaType cinema_type(ImFont* font, float height) {
    // One scale for every size, from the frame height: the composition is a
    // fixed fraction of the frame, so a 720p take is the same film smaller and
    // not the same pixels in a smaller frame.
    const float s = std::max(0.4f, height / kCinemaRefHeight);
    CinemaType type;
    type.font = font;
    type.headline = std::floor(40.0f * s);
    type.numbers = std::floor(27.0f * s);
    type.note = std::floor(21.0f * s);
    type.footer = std::floor(18.0f * s);
    type.chapter = std::floor(21.0f * s);
    type.caption = std::floor(22.0f * s);
    type.label = std::floor(19.0f * s);
    type.legend = std::floor(17.0f * s);
    return type;
}

namespace {

void skeleton_caption(const CinemaState& state, const CinemaCue& cue, CinemaCaption& out) {
    const double p = cue.act_t / std::max(cue.act_span, 1.0e-9);
    if (p < 0.30) {
        return;
    }
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        out.headline = "CAD  →  κ(s)  →  FFT";
        out.numbers =
            fmt("%s edges · %s exact samples", grouped(state.skeleton_polylines).c_str(),
                grouped(state.sizing.curve_points.size()).c_str());
        break;
    case SkeletonSource::kSharpEdges:
        out.headline = "surface  →  crease graph";
        out.numbers = fmt("%s creases", grouped(state.skeleton_polylines).c_str());
        break;
    case SkeletonSource::kUnavailable:
        out.headline = "CAD extraction unavailable";
        out.note = state.skeleton_note;
        out.note_color = palette.status_err;
        break;
    case SkeletonSource::kNone:
        out.headline = "No part";
        out.note_color = palette.status_err;
        break;
    }
}

void deliberate_caption(const CinemaState& state, const CinemaCue& cue, CinemaCaption& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)state;
    (void)cue;
    out.headline = "Advisor unavailable";
    out.note = "advisor support is not compiled into this build";
    out.note_color = palette.status_err;
#else
    if (!state.explanation) {
        out.headline = "Advisor unavailable";
        out.note = state.advisor_note.empty() ? std::string("no measured forward pass")
                                              : state.advisor_note;
        out.note_color = palette.status_err;
        return;
    }
    const auto& explanation = *state.explanation;
    const auto& frames = explanation.frames;
    const std::size_t candidates = frames.empty() ? 0 : frames.size() - 1;
    if (cue.chosen_pass_held) {
        const auto& decision = explanation.decision;
        out.headline = state.decision_applied
                           ? fmt("%s · h/L %.3g · p%d",
                                 std::string(mesher_plain(decision.mesher)).c_str(),
                                 decision.h_rel, decision.order)
                           : fmt("d = %.3g", decision.ood_distance);
        out.headline_color = state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("%s passes · %s candidates", grouped(frames.size()).c_str(),
                          grouped(candidates).c_str());
        return;
    }
    out.headline = "81  →  96  →  96  →  20";
    if (cue.frame_index < 0 || static_cast<std::size_t>(cue.frame_index) >= frames.size()) {
        out.numbers = fmt("%s candidates", grouped(candidates).c_str());
        return;
    }
    out.numbers = fmt("%d / %s", cue.frame_index + 1, grouped(frames.size()).c_str());
#endif
}

void build_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.stages.empty()) {
        out.headline = "No construction stages emitted";
        out.note = "nothing is substituted for missing mesher snapshots";
        out.note_color = palette.status_err;
        return;
    }
    if (cue.stage_index < 0) {
        out.headline = fmt("h %.3g mm · p%d", hud.mesh_size * 1e3, hud.order);
        out.numbers = std::string(mesher_plain(hud.mesher));
        out.headline_color = state.decision_applied ? palette.status_ok : palette.status_warn;
        out.note = "chooser complete · recorded stage follows";
        return;
    }
    const auto idx = static_cast<std::size_t>(cue.stage_index);
    if (idx >= state.stages.size()) {
        return;
    }
    const auto& stage = state.stages[idx];
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    if (idx == 0) {
        const std::size_t visible = static_cast<std::size_t>(
            smoothstep(cue.stage_reveal) * static_cast<double>(stage.mesh.elements.size()));
        out.headline = fmt("generating %s / %s cells", grouped(visible).c_str(),
                           grouped(stage.mesh.elements.size()).c_str());
        out.note = "emission-order replay · advisor held";
    } else {
        out.headline =
            fmt("stage %s / %s · %s", grouped(std::min(idx + 1, n_fill)).c_str(),
                grouped(n_fill).c_str(), std::string(mesh_stage_plain(stage.stage)).c_str());
        out.note = "recorded mesher boundary · advisor held";
    }
    out.numbers = fmt("%s nodes · p%d", grouped(stage.mesh.nodes.size()).c_str(), hud.order);
    out.headline_color = palette.accent;
}

void mesh_hold_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       CinemaCaption& out) {
    const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
    out.headline = x < 0.78 ? "tet4  ·  p1" : "K u = f";
    out.headline_color = palette.status_ok;
    if (!state.solve_stages.empty()) {
        const auto& stage = state.solve_stages.front();
        out.numbers =
            fmt("%s cells · %s nodes · %s unknowns", grouped(stage.trace.n_elems).c_str(),
                grouped(stage.trace.n_nodes).c_str(), grouped(stage.trace.n_dof).c_str());
        if (!state.solve_insights.empty()) {
            const auto& insight = state.solve_insights.front();
            out.note =
                fmt("qmin %.4g · qmean %.4g", insight.quality_min, insight.quality_mean);
        }
    }
    (void)hud;
}

void solve_caption(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.solve_stages.empty()) {
        out.headline = "No solve result";
        out.note_color = palette.status_err;
        return;
    }
    const auto i = static_cast<std::size_t>(std::max(cue.solve_stage_index, 0));
    const auto& stage = state.solve_stages[std::min(i, state.solve_stages.size() - 1)];
    const auto& r = stage.result;
    const auto& tr = stage.trace;
    const bool many = state.solve_stages.size() > 1;
    const std::string pass_tag =
        many ? fmt(" · %d/%zu", stage.pass + 1, state.solve_stages.size()) : std::string{};
    out.note_color = palette.text_dim;
    const double stress_p99 =
        i < state.stress_histograms.size() ? state.stress_histograms[i].p99 : 0.0;
    const double error_p99 =
        i < state.error_histograms.size() ? state.error_histograms[i].p99 : 0.0;
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
        out.headline = "σvm(x)";
        out.numbers = fmt("%s cells · %s unknowns%s", grouped(tr.n_elems).c_str(),
                          grouped(tr.n_dof).c_str(), pass_tag.c_str());
        out.note = stress_p99 > 0.0 ? fmt("p99 %.4g MPa", stress_p99 / 1e6) : std::string{};
        break;
    case SolvePhase::kStressHold:
        out.headline = fmt("σmax %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        out.numbers = fmt("umax %.4g mm%s", r.max_displacement * 1e3, pass_tag.c_str());
        out.note = stress_p99 > 0.0 ? fmt("p99 %.4g MPa", stress_p99 / 1e6) : std::string{};
        break;
    case SolvePhase::kGradientSweep: {
        const double gp99 = state.gradient_histogram(i).p99;
        out.headline = "|∇σvm|(x)";
        out.numbers = gp99 > 0.0 ? fmt("p99 %.4g MPa/mm", gp99 / 1e9) : std::string{};
        break;
    }
    case SolvePhase::kGradientHold: {
        const double gmax = state.gradient_max(i);
        const double gp99 = state.gradient_histogram(i).p99;
        const std::size_t unresolved = state.gradient_unresolved(i);
        if (gmax > 0.0) {
            out.headline = fmt("|∇σ|max %.4g MPa/mm", gmax / 1e9);
            out.headline_color = palette.status_ok;
            out.numbers = gp99 > 0.0 ? fmt("p99 %.4g MPa/mm", gp99 / 1e9) : std::string{};
            if (unresolved > 0) {
                out.note = fmt("%s unresolved nodes", grouped(unresolved).c_str());
                out.note_color = palette.status_warn;
            }
        } else {
            out.headline = "|∇σ| unavailable";
            out.headline_color = palette.status_warn;
        }
        break;
    }
    case SolvePhase::kError:
        out.headline = "ηZZ";
        out.numbers = hud.eta_target > 0.0 ? fmt("%.3g%%  /  %.3g%%", tr.global_eta * 100.0,
                                                 hud.eta_target * 100.0)
                                           : fmt("%.3g%%", tr.global_eta * 100.0);
        out.note = error_p99 > 0.0 ? fmt("p99 %.4g%%", error_p99 * 100.0) : std::string{};
        break;
    case SolvePhase::kErrorHold:
        out.headline = "ηe  →  h / p";
        out.numbers =
            fmt("%s h · %s p", grouped(tr.n_h_mark).c_str(), grouped(tr.n_p_mark).c_str());
        out.note = error_p99 > 0.0 ? fmt("p99 %.4g%%", error_p99 * 100.0) : std::string{};
        break;
    case SolvePhase::kRefine: {
        const std::size_t next = i + 1;
        out.headline = "ηe  →  Δmesh";
        if (next < state.solve_stages.size()) {
            out.numbers = fmt(
                "%s kept · %s removed · %s added", grouped(hud.unchanged_elements).c_str(),
                grouped(hud.removed_elements).c_str(), grouped(hud.added_elements).c_str());
        }
        break;
    }
    case SolvePhase::kRefineHold: {
        const std::size_t next = i + 1;
        out.headline = "mesh n + 1";
        out.headline_color = palette.status_ok;
        if (next < state.solve_stages.size()) {
            const auto& nx = state.solve_stages[next];
            out.numbers = fmt("%s cells · %s unknowns", grouped(nx.trace.n_elems).c_str(),
                              grouped(nx.trace.n_dof).c_str());
        }
        break;
    }
    case SolvePhase::kLoadRamp:
        out.headline = fmt("λ %.3f", cue.load_factor);
        out.numbers =
            fmt("%.4g kN · %.4g MPa · %.4g mm", cue.load_factor * hud.load_newtons / 1e3,
                cue.load_factor * r.max_von_mises / 1e6,
                cue.load_factor * r.max_displacement * 1e3);
        out.note = "u = λu(1) · σ = λσ(1)";
        break;
    case SolvePhase::kHold: {
        out.headline = fmt("σmax %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        const std::string_view token = cinema_solver_token(state);
        const char* method =
            token == "direct_ldlt" ? "LDLT" : (token == "cg" ? "CG" : "solver");
        out.numbers =
            fmt("umax %.4g mm · %s cells · %s unknowns · %s", r.max_displacement * 1e3,
                grouped(tr.n_elems).c_str(), grouped(tr.n_dof).c_str(), method);
        const double shown_mm = hud.deform_scale * r.max_displacement * 1e3;
        out.note = fmt("display %.4g mm · %.3g×", shown_mm, hud.deform_scale);
        break;
    }
    case SolvePhase::kNone:
        break;
    }
}

/// Draws `text` at `size`, or at whatever smaller size makes it fit `width`.
/// Never wraps and never clips: wrapping would change the strip's height
/// mid-take and clipping would drop the end of a sentence the film is making.
void draw_fitted(ImDrawList* dl, ImFont* font, float size, ImVec2 at, float width,
                 const std::string& text, ImU32 color) {
    if (text.empty()) {
        return;
    }
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    const float s = w > width && w > 0.0f ? std::max(8.0f, size * width / w) : size;
    dl->AddText(font, s, ImVec2(at.x, at.y + (size - s) * 0.5f), color, text.c_str());
}

} // namespace

std::vector<CinemaChapter> cinema_chapters() {
    return {
        {"exact CAD", CinemaAct::kSkeleton},
        {"advisor → mesh", CinemaAct::kDeliberate},
        {"analysis", CinemaAct::kSolve},
    };
}

CinemaCaption cinema_caption(const CinemaState& state, const CinemaCue& cue,
                             const CinemaHud& hud) {
    CinemaCaption out;
    out.headline_color = palette.text;
    out.note_color = palette.text_dim;
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        skeleton_caption(state, cue, out);
        break;
    case CinemaAct::kDeliberate:
        deliberate_caption(state, cue, out);
        break;
    case CinemaAct::kBuild:
        build_caption(state, cue, hud, out);
        break;
    case CinemaAct::kMeshHold:
        mesh_hold_caption(state, cue, hud, out);
        break;
    case CinemaAct::kSolve:
        // The gradient captions read a cached recovery, which is why this one
        // takes the state by reference. Nothing here computes a field.
        solve_caption(const_cast<CinemaState&>(state), cue, hud, out);
        break;
    }
    // The footer names the part, the run, and where the exhaustive disclosures
    // live. That pointer is load-bearing: it is what makes it honest to have
    // put one disclosure on screen instead of six.
    const std::string part = hud.part.empty() ? std::string("(no part)") : hud.part;
    out.footer =
        hud.stamp.empty()
            ? fmt("%s · provenance unavailable · docs/assets/cinema/NOTES.md", part.c_str())
            : fmt("%s · %s · docs/assets/cinema/NOTES.md", part.c_str(), hud.stamp.c_str());
    return out;
}

float cinema_strip_height(const CinemaType& type) {
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float left = std::floor(type.headline * 1.28f) + std::floor(type.numbers * 1.42f);
    const float content = std::max(left, std::floor(type.note * 1.45f));
    const float footer = std::floor(type.footer * 1.45f);
    return std::floor(chapter_h + content + footer + 2.0f * kStripPadY);
}

void draw_cinema_strip(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       const CinemaType& type) {
    const CinemaCaption caption = cinema_caption(state, cue, hud);
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);

    // Three causal chapters and one progress line. Advisor scoring, its chosen
    // action and the real cell landing remain one continuous chapter.
    const auto chapters = cinema_chapters();
    const CinemaAct here = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kMeshHold
                               ? CinemaAct::kDeliberate
                               : cue.act;
    float x = origin.x;
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float gap = std::floor(type.chapter * 1.6f);
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const bool current = chapters[i].act == here;
        const bool done = static_cast<int>(chapters[i].act) < static_cast<int>(here);
        const ImVec4 color =
            current ? palette.accent : (done ? palette.text : palette.text_dim);
        const float alpha = current ? 1.0f : (done ? 0.55f : 0.30f);
        const std::string label = fmt("%zu. %s", i + 1, chapters[i].label);
        dl->AddText(font, type.chapter, ImVec2(x, origin.y), faded(color, alpha),
                    label.c_str());
        x += font->CalcTextSizeA(type.chapter, FLT_MAX, 0.0f, label.c_str()).x + gap;
    }
    const float bar_y = origin.y + chapter_h - 5.0f;
    dl->AddRectFilled(ImVec2(origin.x, bar_y), ImVec2(origin.x + width, bar_y + 2.0f),
                      faded(palette.text_dim, 0.22f));
    dl->AddRectFilled(
        ImVec2(origin.x, bar_y),
        ImVec2(origin.x + width * static_cast<float>(cinema_progress(state)), bar_y + 2.0f),
        faded(palette.accent, 0.85f));

    // ---- horizontal information ledger ---------------------------------
    // Headline/numbers occupy the left, the one plain-language disclosure uses
    // the otherwise-empty right, and provenance retains the full width below.
    const float content_y = origin.y + chapter_h;
    const float left_w = std::floor(width * 0.61f);
    const float column_gap = 28.0f;
    const float right_x = origin.x + left_w + column_gap;
    const float right_w = std::max(80.0f, width - left_w - column_gap);
    draw_fitted(dl, font, type.headline, ImVec2(origin.x, content_y), left_w, caption.headline,
                ImGui::ColorConvertFloat4ToU32(caption.headline_color));
    draw_fitted(dl, font, type.numbers,
                ImVec2(origin.x, content_y + std::floor(type.headline * 1.28f)), left_w,
                caption.numbers, ImGui::ColorConvertFloat4ToU32(palette.text));
    draw_fitted(dl, font, type.note, ImVec2(right_x, content_y), right_w, caption.note,
                ImGui::ColorConvertFloat4ToU32(caption.note_color));
    const float content_h =
        std::max(std::floor(type.headline * 1.28f) + std::floor(type.numbers * 1.42f),
                 std::floor(type.note * 1.45f));
    draw_fitted(dl, font, type.footer, ImVec2(origin.x, content_y + content_h), width,
                caption.footer, faded(palette.text_dim, 0.90f));
}

} // namespace polymesh::gui
