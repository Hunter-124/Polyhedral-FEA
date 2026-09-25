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
using detail::mesh_mix_text;
using detail::mesher_plain;
using detail::stage_name;

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

void skeleton_caption(const CinemaState& state, CinemaCaption& out) {
    const bool curvature_sizing =
        state.sizing.spectral.applied && state.sizing.brep_curvature;
    const bool bc_sizing = state.sizing.spectral.applied && state.sizing.bc_seeds > 0;
    out.headline = curvature_sizing
                       ? "Exact CAD + spectral curvature size field"
                       : (bc_sizing ? "Exact CAD + support/load size field"
                                    : "Exact CAD + uniform size target");
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        out.numbers =
            fmt("%s exact edges · %s curve samples · %s on-part target sizes",
                grouped(state.skeleton_polylines).c_str(),
                grouped(state.sizing.edge_points.size()).c_str(),
                grouped(state.sizing.field_points.size()).c_str());
        out.note = curvature_sizing
                       ? "κ(s) → FFT energy selection → inverse transform → h(x)"
                       : (bc_sizing
                              ? "κ(s) FFT analysis · h(x) rings follow supports and load"
                              : "κ(s) FFT is analysis only · uniform h solve");
        out.note_color = curvature_sizing ? palette.text_dim : palette.status_warn;
        break;
    case SkeletonSource::kSharpEdges:
        out.numbers =
            fmt("%s tessellation creases", grouped(state.skeleton_polylines).c_str());
        out.note = "mesh input · no BRep curvature or CAD-edge spectrum is claimed";
        out.note_color = palette.status_warn;
        break;
    case SkeletonSource::kUnavailable:
        out.headline = "CAD feature extraction unavailable";
        out.note = state.skeleton_note;
        out.note_color = palette.status_err;
        break;
    case SkeletonSource::kNone:
        out.headline = "No part loaded";
        out.note = "no substitute geometry is drawn";
        out.note_color = palette.status_err;
        break;
    }
}

void deliberate_caption(const CinemaState& state, const CinemaCue& cue, CinemaCaption& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)state;
    (void)cue;
    out.headline = "Advisor unavailable";
    out.note = "POLYMESH_WITH_ADVISOR=OFF";
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
        out.headline = state.decision_applied ? "Advisor decision — final network state"
                                              : "Advisor abstained — final network state";
        out.headline_color =
            state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("final re-score held for inspection · %s candidates compared",
                          grouped(candidates).c_str());
        out.note = state.decision_vetoed
                       ? "outside calibrated descriptor envelope · configured baseline unchanged"
                       : state.decision_note;
        out.note_color =
            state.decision_applied ? palette.text_dim : palette.status_warn;
        return;
    }
    out.headline = "Mesh search — deployed advisor network";
    if (cue.frame_index < 0 || static_cast<std::size_t>(cue.frame_index) >= frames.size()) {
        out.numbers = fmt("%s candidate meshes · measured forward passes only",
                          grouped(candidates).c_str());
        return;
    }
    out.numbers = fmt("forward pass %d / %s · %s candidates compared", cue.frame_index + 1,
                      grouped(frames.size()).c_str(), grouped(candidates).c_str());
    out.note = fmt("circle = activation · line = |weight × activation| · %.3g%% failure gate",
                   explanation.gate_threshold * 100.0);
    out.note_color = palette.text_dim;
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
        out.headline = state.decision_applied ? "Advisor selection locked"
                                              : "Advisor abstained — configured baseline";
        out.headline_color = state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("%s · %.3g mm target · order %d · %d refinement pass%s",
                          std::string(mesher_plain(hud.mesher)).c_str(),
                          hud.mesh_size * 1e3, hud.order, hud.adapt_passes,
                          hud.adapt_passes == 1 ? "" : "es");
        out.note = state.decision_vetoed
                       ? "outside calibrated descriptor envelope · configured baseline unchanged"
                       : state.decision_note;
        out.note_color = state.decision_applied ? palette.text_dim : palette.status_warn;
        return;
    }
    const auto idx = static_cast<std::size_t>(cue.stage_index);
    if (idx >= state.stages.size()) {
        return;
    }
    const auto& stage = state.stages[idx];
    std::string prefix = "Studio setup landing";
    std::string handoff = "advisor unavailable → studio setup · sequential";
    ImVec4 handoff_color = palette.status_warn;
    if (state.decision_applied) {
        prefix = "Chosen action landing";
        handoff = "aligned replay: measured chosen pass → its later real mesh · sequential";
        handoff_color = palette.text_dim;
    } else if (state.decision_vetoed) {
        prefix = "Configured baseline landing";
        handoff = "measured OOD check → configured baseline · sequential";
    } else if (state.decision_unrecognized) {
        prefix = "Studio setup landing";
        handoff = "unrecognised advised mesher → unchanged studio setup · sequential";
    }
    out.headline =
        stage.stage == "fill"
            ? prefix + " — cells become the solve mesh"
            : fmt("%s — %s", prefix.c_str(),
                  std::string(stage_name(stage.stage)).c_str());
    const std::size_t total = stage.mesh.elements.size();
    const auto drawn = static_cast<std::size_t>(
        cue.mesh_action_reveal * static_cast<double>(total));
    out.numbers = fmt("%s / %s real cells · %s nodes · order-%d target",
                      grouped(drawn).c_str(), grouped(total).c_str(),
                      grouped(stage.mesh.nodes.size()).c_str(), hud.order);
    out.note = std::move(handoff);
    out.note_color = handoff_color;
    if (hud.cinema_skipped_elements > 0) {
        out.note = fmt("%s cells could not be triangulated and are excluded from this view",
                       grouped(hud.cinema_skipped_elements).c_str());
        out.note_color = palette.status_warn;
    }
}

void mesh_hold_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       CinemaCaption& out) {
    const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
    out.headline = x < 0.78 ? "Cell topology — exploded view"
                            : "Authoritative mesh — ready to solve";
    out.headline_color = palette.status_ok;
    if (!state.solve_stages.empty()) {
        const auto& stage = state.solve_stages.front();
        out.numbers = fmt("%s cells · %s nodes · %s unknowns · order %d",
                          grouped(stage.trace.n_elems).c_str(),
                          grouped(stage.trace.n_nodes).c_str(),
                          grouped(stage.trace.n_dof).c_str(), hud.order);
        if (!state.solve_insights.empty()) {
            const auto& insight = state.solve_insights.front();
            out.note = fmt("%s · shape quality min %.4g / mean %.4g",
                           mesh_mix_text(insight).c_str(), insight.quality_min,
                           insight.quality_mean);
        }
    }
    if (out.note.empty()) {
        out.note = state.decision_vetoed
                       ? "outside calibrated descriptor envelope · configured baseline unchanged"
                       : state.decision_note;
    }
    out.note_color = palette.text_dim;
}

void solve_caption(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.solve_stages.empty()) {
        out.headline = "No solve passes were delivered";
        out.note = "pipeline::SolveJob::on_solve_stage received none, so this act holds the "
                   "finished mesh instead of a field — nothing is drawn in place of an answer "
                   "that never arrived";
        out.note_color = palette.status_err;
        return;
    }
    const auto i = static_cast<std::size_t>(std::max(cue.solve_stage_index, 0));
    const auto& stage = state.solve_stages[std::min(i, state.solve_stages.size() - 1)];
    const auto& r = stage.result;
    const auto& tr = stage.trace;
    const bool many = state.solve_stages.size() > 1;
    const std::string pass_tag =
        many ? fmt(" (pass %d of %zu)", stage.pass + 1, state.solve_stages.size())
             : std::string{};
    out.note_color = palette.text_dim;
    const double stress_p99 =
        i < state.stress_histograms.size() ? state.stress_histograms[i].p99 : 0.0;
    const double error_p99 =
        i < state.error_histograms.size() ? state.error_histograms[i].p99 : 0.0;
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
        out.headline = "Stress field — spatial reveal";
        out.numbers =
            fmt("%s cells · %s unknowns · solved once%s", grouped(tr.n_elems).c_str(),
                grouped(tr.n_dof).c_str(), pass_tag.c_str());
        out.note = stress_p99 > 0.0
                       ? fmt("authoritative mesh → von Mises · colour cap p99 %.4g MPa",
                             stress_p99 / 1e6)
                       : std::string("authoritative mesh stays visible while von Mises arrives");
        break;
    case SolvePhase::kStressHold:
        out.headline = fmt("Peak stress %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        out.numbers = fmt("largest movement %.4g mm · undeformed shape%s",
                          r.max_displacement * 1e3, pass_tag.c_str());
        out.note = stress_p99 > 0.0
                       ? fmt("colour scale p99 %.4g MPa · true peak stated above",
                             stress_p99 / 1e6)
                       : std::string("von Mises stress on the exact geometry this pass solved");
        break;
    case SolvePhase::kGradientSweep: {
        const double gp99 = state.gradient_histogram(i).p99;
        out.headline = "Stress gradient — spatial reveal";
        out.numbers = "|∇σvm| recovered at every resolvable node";
        out.note = gp99 > 0.0
                       ? fmt("stress → gradient handoff · colour cap p99 %.4g MPa/mm",
                             gp99 / 1e9)
                       : std::string("completed stress stays ahead of the gradient handoff");
        break;
    }
    case SolvePhase::kGradientHold: {
        const double gmax = state.gradient_max(i);
        const double gp99 = state.gradient_histogram(i).p99;
        const std::size_t unresolved = state.gradient_unresolved(i);
        if (gmax > 0.0) {
            out.headline = fmt("Steepest change %.4g MPa per mm", gmax / 1e9);
            out.headline_color = palette.status_ok;
            out.numbers = gp99 > 0.0
                              ? fmt("red = p99 %.4g MPa/mm · steepest stated above",
                                    gp99 / 1e9)
                              : std::string("recovered change rate");
            out.note =
                unresolved > 0
                    ? fmt("%s nodes had coplanar neighborhoods and report zero gradient",
                          grouped(unresolved).c_str())
                    : std::string("stress-riser intensity, measured per unit distance");
            out.note_color = unresolved > 0 ? palette.status_warn : palette.text_dim;
        } else {
            out.headline = "No gradient could be recovered here";
            out.headline_color = palette.status_warn;
            out.numbers = "the stress field itself is still on screen";
            out.note = "nothing is drawn in place of a gradient that could not be computed";
            out.note_color = palette.status_warn;
        }
        break;
    }
    case SolvePhase::kError:
        out.headline = "Estimated solution error";
        if (hud.eta_target > 0.0) {
            out.numbers = fmt("ZZ indicator %.3g%% · target %.3g%%",
                              tr.global_eta * 100.0, hud.eta_target * 100.0);
        } else {
            out.numbers = fmt("ZZ global indicator %.3g%% · verification pass",
                              tr.global_eta * 100.0);
        }
        out.note = error_p99 > 0.0
                       ? fmt("prior field → ZZ indicator · colour cap p99 %.4g%%",
                             error_p99 * 100.0)
                       : std::string("prior field stays ahead · recovered-vs-solved indicator");
        break;
    case SolvePhase::kErrorHold:
        out.headline = "Local error map";
        out.numbers = fmt("%s h-refinement marks · %s p-refinement marks",
                          grouped(tr.n_h_mark).c_str(), grouped(tr.n_p_mark).c_str());
        out.note = error_p99 > 0.0
                       ? fmt("colour cap p99 %.4g%% · %s", error_p99 * 100.0,
                             i + 1 < state.solve_stages.size()
                                 ? "this field drives the next solved mesh"
                                 : "no unrecorded refinement is implied")
                       : std::string("reported in full");
        break;
    case SolvePhase::kRefine: {
        const std::size_t next = i + 1;
        out.headline = "Replacing only cells changed by refinement";
        if (next < state.solve_stages.size()) {
            out.numbers = fmt("%s kept · %s removed · %s replacement cells",
                              grouped(hud.unchanged_elements).c_str(),
                              grouped(hud.removed_elements).c_str(),
                              grouped(hud.added_elements).c_str());
            out.note = error_p99 > 0.0
                           ? fmt("ZZ p99 %.4g%% → corner-topology diff · kept cells stay",
                                 error_p99 * 100.0)
                           : std::string("corner-topology diff · kept cells stay rendered");
        }
        break;
    }
    case SolvePhase::kRefineHold: {
        const std::size_t next = i + 1;
        out.headline = "Incremental refinement complete";
        out.headline_color = palette.status_ok;
        if (next < state.solve_stages.size()) {
            const auto& nx = state.solve_stages[next];
            out.numbers = fmt("%s cells · %s preserved from the previous pass · %s unknowns",
                              grouped(nx.trace.n_elems).c_str(),
                              grouped(hud.unchanged_elements).c_str(),
                              grouped(nx.trace.n_dof).c_str());
            out.note = fmt("%s old cells replaced by %s solved cells; polynomial promotion "
                           "does not count as a topology change",
                           grouped(hud.removed_elements).c_str(),
                           grouped(hud.added_elements).c_str());
        }
        break;
    }
    case SolvePhase::kLoadRamp:
        out.headline = fmt("Load response — %.4g / %.4g N",
                           cue.load_factor * hud.load_newtons, hud.load_newtons);
        out.numbers =
            fmt("%.4g MPa · %.4g mm at this load", cue.load_factor * r.max_von_mises / 1e6,
                cue.load_factor * r.max_displacement * 1e3);
        out.note = stress_p99 > 0.0
                       ? fmt("exact linear response · colour p99 %.4g MPa · true %.4g mm · "
                             "shown %.4g mm at %.3g×",
                             stress_p99 / 1e6,
                             cue.load_factor * r.max_displacement * 1e3,
                             cue.load_factor * hud.deform_scale * r.max_displacement * 1e3,
                             cue.load_factor * hud.deform_scale)
                       : fmt("exact linear response · true %.4g mm · shown %.4g mm at %.3g×",
                             cue.load_factor * r.max_displacement * 1e3,
                             cue.load_factor * hud.deform_scale * r.max_displacement * 1e3,
                             cue.load_factor * hud.deform_scale);
        break;
    case SolvePhase::kHold: {
        out.headline = fmt("Full load — %.4g MPa peak", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        const std::string_view token = cinema_solver_token(state);
        const char* method = token == "direct_ldlt"
                                 ? "factorised, not iterated"
                                 : (token == "cg" ? "conjugate gradient"
                                                  : "solver method not reported");
        out.numbers = fmt("%.4g mm largest movement · %s cells · %s unknowns · %s",
                          r.max_displacement * 1e3, grouped(tr.n_elems).c_str(),
                          grouped(tr.n_dof).c_str(), method);
        const double shown_mm = hud.deform_scale * r.max_displacement * 1e3;
        const double shown_pct =
            hud.model_diagonal > 0.0
                ? 100.0 * hud.deform_scale * r.max_displacement / hud.model_diagonal
                : 0.0;
        out.note = fmt("true %.4g mm · display %.4g mm (%.2f%% of model) · %.3g×",
                       r.max_displacement * 1e3, shown_mm, shown_pct, hud.deform_scale);
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
        {"advisor", CinemaAct::kDeliberate},
        {"mesher", CinemaAct::kBuild},
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
        skeleton_caption(state, out);
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
    out.footer = hud.stamp.empty()
                     ? fmt("%s · POLYMESH_CINEMA_STAMP not set, so this run carries no "
                           "provenance line · full disclosures: docs/assets/cinema/NOTES.md",
                           part.c_str())
                     : fmt("%s · %s · full disclosures: docs/assets/cinema/NOTES.md",
                           part.c_str(), hud.stamp.c_str());
    return out;
}

float cinema_strip_height(const CinemaType& type) {
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float left = std::floor(type.headline * 1.28f) +
                       std::floor(type.numbers * 1.42f);
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

    // ---- the chapter bar -------------------------------------------------
    // Four words and a progress line. It exists so a first-time viewer knows
    // where they are in half a second without reading a clock.
    const auto chapters = cinema_chapters();
    const CinemaAct here = cue.act == CinemaAct::kMeshHold ? CinemaAct::kBuild : cue.act;
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
    draw_fitted(dl, font, type.headline, ImVec2(origin.x, content_y), left_w,
                caption.headline,
                ImGui::ColorConvertFloat4ToU32(caption.headline_color));
    draw_fitted(dl, font, type.numbers,
                ImVec2(origin.x, content_y + std::floor(type.headline * 1.28f)),
                left_w, caption.numbers,
                ImGui::ColorConvertFloat4ToU32(palette.text));
    draw_fitted(dl, font, type.note, ImVec2(right_x, content_y), right_w,
                caption.note, ImGui::ColorConvertFloat4ToU32(caption.note_color));
    const float content_h =
        std::max(std::floor(type.headline * 1.28f) +
                     std::floor(type.numbers * 1.42f),
                 std::floor(type.note * 1.45f));
    draw_fitted(dl, font, type.footer,
                ImVec2(origin.x, content_y + content_h), width, caption.footer,
                faded(palette.text_dim, 0.90f));
}

} // namespace polymesh::gui
