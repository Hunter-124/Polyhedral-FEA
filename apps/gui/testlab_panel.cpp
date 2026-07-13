// SPDX-License-Identifier: BSD-3-Clause
#include "testlab_panel.hpp"

#include "fea/backend.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>

#if defined(_WIN32)
#include <stdio.h> // _popen / _pclose
#endif

namespace polymesh::gui {
namespace {

/// One-shot `git rev-parse --short HEAD`. Returns "unknown" on any failure.
std::string query_git_short_head() {
#if defined(_WIN32)
    FILE* pipe = _popen("git rev-parse --short HEAD 2>nul", "r");
#else
    FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
#endif
    if (pipe == nullptr) {
        return "unknown";
    }
    std::array<char, 64> buf{};
    const char* got = std::fgets(buf.data(), static_cast<int>(buf.size()), pipe);
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    if (got == nullptr) {
        return "unknown";
    }
    std::string s(buf.data());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    // Guard against empty / non-hash junk.
    if (s.empty() || s.size() > 40) {
        return "unknown";
    }
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return "unknown";
        }
    }
    return s;
}

ImVec4 state_color(testlab::CheckpointState s) {
    switch (s) {
    case testlab::CheckpointState::kRunning:
        return palette.status_warn;
    case testlab::CheckpointState::kPaused:
        return palette.accent;
    case testlab::CheckpointState::kFinished:
        return palette.status_ok;
    case testlab::CheckpointState::kUnknown:
    default:
        return palette.text_dim;
    }
}

ImVec4 result_status_color(const std::string& status) {
    if (status == "ok") {
        return palette.status_ok;
    }
    // solve_suspect: health gate failed after a successful solve — warn, not hard fail.
    if (status == "solve_suspect" || status == "over_budget") {
        return palette.status_warn;
    }
    return palette.status_err;
}

bool is_finite_num(double v) {
    return std::isfinite(v);
}

const char* fmt_opt_num(double v, char* buf, std::size_t n, const char* fmt = "%.3g") {
    if (!is_finite_num(v)) {
        std::snprintf(buf, n, "—");
        return buf;
    }
    std::snprintf(buf, n, fmt, v);
    return buf;
}

std::string result_detail_tooltip(const testlab::ResultRow& r) {
    std::string t;
    t += std::format("status: {}\n", r.status);
    if (r.health.present) {
        t += std::format("health_ok: {}\n", r.health.ok ? "true" : "false");
        if (is_finite_num(r.health.free_residual_rel)) {
            t += std::format("free_residual_rel: {:.3g}\n", r.health.free_residual_rel);
        }
        if (is_finite_num(r.health.reaction_sum_err)) {
            t += std::format("reaction_sum_err: {:.3g}\n", r.health.reaction_sum_err);
        }
        if (r.health.has_load_area_ok) {
            t += std::format("load_area_ok: {}\n", r.health.load_area_ok ? "true" : "false");
        }
    }
    if (r.scorecard.present) {
        t += "scorecard:\n";
        if (is_finite_num(r.scorecard.edge_hausdorff_over_h)) {
            t += std::format("  edge_H/h: {:.3g}\n", r.scorecard.edge_hausdorff_over_h);
        }
        if (is_finite_num(r.scorecard.chordal_efficiency_max)) {
            t += std::format("  chordal_e_max: {:.3g}\n", r.scorecard.chordal_efficiency_max);
        }
        if (is_finite_num(r.scorecard.normal_dev_deg_max)) {
            t += std::format("  normal_dev_max: {:.3g}°\n", r.scorecard.normal_dev_deg_max);
        }
    }
    if (is_finite_num(r.answers.strain_energy)) {
        t += std::format("strain_energy: {:.4g}\n", r.answers.strain_energy);
    }
    if (is_finite_num(r.answers.tip_deflection)) {
        t += std::format("tip_deflection: {:.4g}\n", r.answers.tip_deflection);
    }
    if (is_finite_num(r.answers.sigma_face_mean)) {
        t += std::format("sigma_face_mean: {:.4g}\n", r.answers.sigma_face_mean);
    }
    if (is_finite_num(r.n_pred_elems)) {
        t += std::format("n_pred_elems: {:.0f}  n_elems: {}\n", r.n_pred_elems, r.n_elems);
    }
    if (!r.config_summary.empty()) {
        t += r.config_summary;
    }
    return t;
}

} // namespace

void TestLabState::sync_buffers_from_settings() {
    std::snprintf(campaigns_root_buf, sizeof(campaigns_root_buf), "%s",
                  settings.campaigns_root.c_str());
    std::snprintf(testlab_bin_buf, sizeof(testlab_bin_buf), "%s",
                  settings.testlab_binary.c_str());
}

void TestLabState::apply_buffers_to_settings() {
    settings.campaigns_root = campaigns_root_buf;
    settings.testlab_binary = testlab_bin_buf;
}

void TestLabState::cache_git_head() {
    git_head = query_git_short_head();
}

void TestLabState::refresh_campaign_list() {
    apply_buffers_to_settings();
    try {
        campaigns = testlab::scan_campaigns(settings.campaigns_root_path());
        if (campaigns.empty()) {
            status = std::format("no campaigns under {}", settings.campaigns_root);
        } else if (selected < 0 || selected >= static_cast<int>(campaigns.size())) {
            selected = 0;
            status = std::format("{} campaign(s)", campaigns.size());
        }
    } catch (const std::exception& e) {
        campaigns.clear();
        status = std::format("scan failed: {}", e.what());
    }
}

void TestLabState::refresh_selected() {
    const auto* sum = selected_summary();
    if (sum == nullptr) {
        active_spec.reset();
        checkpoint.reset();
        progress.reset();
        results.clear();
        handoff.reset();
        return;
    }
    try {
        active_spec = testlab::load_campaign(sum->dir);
    } catch (const std::exception& e) {
        active_spec.reset();
        status = std::format("campaign.json: {}", e.what());
    }
    try {
        checkpoint = testlab::load_checkpoint(sum->dir);
    } catch (const std::exception& e) {
        checkpoint.reset();
        status = std::format("checkpoint.json: {}", e.what());
    }
    try {
        results = testlab::load_results(sum->dir);
    } catch (const std::exception& e) {
        results.clear();
        status = std::format("results.jsonl: {}", e.what());
    }
    progress = testlab::load_progress(sum->dir);
    handoff = testlab::load_handoff(sum->dir);

    // Mirror process state into a friendly status line.
    if (runner.is_running()) {
        status = "harness running…";
    } else if (runner.state() == ProcessRunner::State::kExited) {
        status = std::format("harness exited ({})", runner.exit_code());
    } else if (checkpoint) {
        status = std::format("{} — tier {} — {} runs",
                             testlab::checkpoint_state_cstr(checkpoint->state), checkpoint->tier,
                             checkpoint->completed_runs);
    } else {
        status = std::format("{} campaign", sum->name);
    }
    // Measure-first M9 freeze baseline (ADR-0023/24).
    if (testlab::is_measure_first_baseline(sum->name) ||
        (active_spec && testlab::is_measure_first_baseline(active_spec->name))) {
        status += " · measure-first baseline (M9)";
    }
}

void TestLabState::tick(float /*dt_s*/) {
    runner.poll();

    const auto now = std::chrono::steady_clock::now();
    // Faster refresh while harness is live so progress.json heartbeats show up promptly.
    const float interval =
        runner.is_running() ? 0.25f : std::max(0.15f, settings.refresh_interval_s);
    const bool due = force_refresh ||
                     std::chrono::duration<float>(now - last_refresh).count() >= interval;
    if (!due) {
        return;
    }
    force_refresh = false;
    last_refresh = now;

    // Keep selection identity by name across rescan when possible.
    std::string keep_name;
    if (selected_summary() != nullptr) {
        keep_name = selected_summary()->name;
    }
    refresh_campaign_list();
    if (!keep_name.empty()) {
        for (int i = 0; i < static_cast<int>(campaigns.size()); ++i) {
            if (campaigns[static_cast<std::size_t>(i)].name == keep_name) {
                selected = i;
                break;
            }
        }
    }
    refresh_selected();

    // Live campaign mesh preview (mesh_preview.pmp) — only reload when mtime changes.
    const auto dir = selected_dir();
    if (!dir.empty()) {
        const auto preview_path = dir / "mesh_preview.pmp";
        std::error_code ec;
        if (std::filesystem::is_regular_file(preview_path, ec)) {
            const auto mt = std::filesystem::last_write_time(preview_path, ec);
            if (!ec && (campaign_mesh_dirty || !campaign_mesh || mt != campaign_mesh_mtime)) {
                if (auto mesh = testlab::load_mesh_preview(dir)) {
                    campaign_mesh = std::move(mesh);
                    campaign_mesh_mtime = mt;
                    campaign_mesh_dirty = true; // signal main to upload once
                }
            }
        }
    }
}

const testlab::CampaignSummary* TestLabState::selected_summary() const {
    if (selected < 0 || selected >= static_cast<int>(campaigns.size())) {
        return nullptr;
    }
    return &campaigns[static_cast<std::size_t>(selected)];
}

std::filesystem::path TestLabState::selected_dir() const {
    const auto* s = selected_summary();
    return s ? s->dir : std::filesystem::path{};
}

bool TestLabState::start_run(bool resume) {
    apply_buffers_to_settings();
    const auto dir = selected_dir();
    if (dir.empty()) {
        status = "no campaign selected";
        return false;
    }
    if (runner.is_running()) {
        status = "harness already running";
        return false;
    }

    // Resource caps for the child: campaign JSON wins when set, else GUI knobs.
    // Child inherits env; OpenMP reads OMP_NUM_THREADS at process start.
    int thr_cap = settings.max_threads;
    double mem_cap = settings.max_mem_gb;
    if (active_spec) {
        if (active_spec->resources.max_threads > 0) {
            thr_cap = active_spec->resources.max_threads;
        }
        if (active_spec->resources.max_mem_gb > 0.0) {
            mem_cap = active_spec->resources.max_mem_gb;
        }
    }
    if (thr_cap > 0) {
        // Also clamp the GUI process so any linked libs see the same default
        // if the harness loads them in-process (it does — same binary tree).
        polymesh::fea::set_openmp_threads(thr_cap);
#if defined(_WIN32)
        _putenv_s("OMP_NUM_THREADS", std::to_string(thr_cap).c_str());
#else
        ::setenv("OMP_NUM_THREADS", std::to_string(thr_cap).c_str(), /*overwrite=*/1);
#endif
    }

    const auto binary = settings.resolved_testlab_binary();
    // CLI contract (PROGRAM.yaml): polymesh_testlab run|resume <campaign_dir>
    std::vector<std::string> args;
    args.push_back(resume ? "resume" : "run");
    args.push_back(dir.string());

    if (!runner.start(binary, args, /*cwd=*/std::filesystem::current_path())) {
        status = std::format("start failed: {}", runner.last_error());
        last_action = status;
        return false;
    }
    if (thr_cap > 0 || mem_cap > 0.0) {
        last_action = std::format(
            "{} {} (threads {}, mem {})", resume ? "resume" : "run", dir.filename().string(),
            thr_cap > 0 ? std::to_string(thr_cap) : "all",
            mem_cap > 0.0 ? std::format("{:.1f} GB soft", mem_cap) : "off");
    } else {
        last_action = std::format("{} {}", resume ? "resume" : "run", dir.filename().string());
    }
    status = last_action;
    force_refresh = true;
    return true;
}

bool TestLabState::pause_run() {
    if (!runner.is_running()) {
        // No live process — still useful if checkpoint says running (orphaned).
        status = "no live harness process (send SIGINT only when we spawned it)";
        return false;
    }
    if (!runner.request_pause()) {
        status = std::format("pause failed: {}", runner.last_error());
        return false;
    }
    last_action = "SIGINT sent (pause after current run)";
    status = last_action;
    force_refresh = true;
    return true;
}

bool TestLabState::force_stop() {
    if (!runner.is_running()) {
        status = "no live harness process";
        return false;
    }
    runner.force_kill();
    last_action = "force-killed harness";
    status = last_action;
    force_refresh = true;
    return true;
}

void draw_testlab_panel(TestLabState& tl) {
    // V3c: workspace sync strip — git HEAD (startup-cached), selected campaign
    // checkpoint/progress state, and agent sync hint.
    iw::begin_group_box("workspace");
    ImGui::Text("git: %s", tl.git_head.empty() ? "unknown" : tl.git_head.c_str());
    if (tl.checkpoint) {
        const auto& cp = *tl.checkpoint;
        ImGui::TextColored(state_color(cp.state), "campaign: %s · %d runs",
                           testlab::checkpoint_state_cstr(cp.state), cp.completed_runs);
        if (cp.tier > 0 || !cp.updated_utc.empty()) {
            ImGui::TextColored(palette.text_dim, "tier %d%s%s", cp.tier,
                               cp.updated_utc.empty() ? "" : " · ",
                               cp.updated_utc.empty() ? "" : cp.updated_utc.c_str());
        }
    } else if (tl.progress) {
        // Live harness without a checkpoint yet (or mid-write).
        const auto& p = *tl.progress;
        ImGui::TextColored(palette.status_warn, "campaign: progress · %s",
                           p.phase.empty() ? "?" : p.phase.c_str());
        if (!p.cfg_id.empty() || p.tier > 0) {
            ImGui::TextColored(palette.text_dim, "%s  tier %d", p.cfg_id.c_str(), p.tier);
        }
    } else if (tl.selected_summary() != nullptr) {
        const auto* sum = tl.selected_summary();
        ImGui::TextColored(palette.text_dim, "campaign: %s · no checkpoint",
                           testlab::checkpoint_state_cstr(sum->state));
    } else {
        ImGui::TextColored(palette.text_dim, "campaign: (none selected)");
    }
    ImGui::TextColored(palette.accent, "Sync: pull --rebase before agent runs");
    iw::end_group_box();

    iw::begin_group_box("test lab");
    ImGui::TextColored(palette.text_dim, "campaigns (interfaces.md)");
    iw::input_text("campaigns root", tl.campaigns_root_buf, sizeof(tl.campaigns_root_buf),
                   "bench/campaigns");
    iw::input_text("testlab binary", tl.testlab_bin_buf, sizeof(tl.testlab_bin_buf),
                   "auto / path / PATH name");
    if (iw::button("rescan", ImVec2(-1, 0))) {
        tl.force_refresh = true;
        tl.tick(0.0f);
    }
    iw::end_group_box();

    iw::begin_group_box("campaigns");
    if (tl.campaigns.empty()) {
        ImGui::TextColored(palette.text_dim, "no campaigns found");
        ImGui::TextWrapped("Place campaign.json under %s/<name>/", tl.campaigns_root_buf);
    } else {
        const float list_h =
            std::clamp(20.0f * static_cast<float>(std::min(static_cast<int>(tl.campaigns.size()), 10)) +
                           8.0f,
                       64.0f, 200.0f);
        if (ImGui::BeginChild("##campaign_list", ImVec2(-FLT_MIN, list_h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            for (int i = 0; i < static_cast<int>(tl.campaigns.size()); ++i) {
                const auto& c = tl.campaigns[static_cast<std::size_t>(i)];
                const bool sel = (tl.selected == i);
                const bool m9 = testlab::is_measure_first_baseline(c.name);
                ImGui::PushID(i);
                if (ImGui::Selectable(c.name.c_str(), sel)) {
                    tl.selected = i;
                    tl.force_refresh = true;
                    tl.refresh_selected();
                }
                if (ImGui::IsItemHovered()) {
                    if (m9) {
                        ImGui::SetTooltip(
                            "%s\n%d results · %s\nmeasure-first M9 freeze baseline (ADR-0023/24)",
                            c.dir.string().c_str(), c.result_count,
                            testlab::checkpoint_state_cstr(c.state));
                    } else {
                        ImGui::SetTooltip("%s\n%d results · %s", c.dir.string().c_str(),
                                          c.result_count, testlab::checkpoint_state_cstr(c.state));
                    }
                }
                ImGui::SameLine();
                if (m9) {
                    ImGui::TextColored(palette.accent, "M9");
                    ImGui::SameLine();
                }
                ImGui::TextColored(state_color(c.state), "%s",
                                   testlab::checkpoint_state_cstr(c.state));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    iw::end_group_box();

    iw::begin_group_box("run control");
    const bool busy = tl.runner.is_running();
    const bool has_sel = tl.selected_summary() != nullptr;
    const bool can_resume =
        has_sel && tl.checkpoint &&
        (tl.checkpoint->state == testlab::CheckpointState::kPaused ||
         tl.checkpoint->state == testlab::CheckpointState::kRunning);

    ImGui::BeginDisabled(!has_sel || busy);
    if (iw::button("play / run", ImVec2(-1, 0), /*primary=*/true)) {
        tl.start_run(/*resume=*/false);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!has_sel || busy || !can_resume);
    if (iw::button("play / resume", ImVec2(-1, 0))) {
        tl.start_run(/*resume=*/true);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!busy);
    if (iw::button("pause (SIGINT)", ImVec2(-1, 0))) {
        tl.pause_run();
    }
    if (iw::button("force stop", ImVec2(-1, 0))) {
        tl.force_stop();
    }
    ImGui::EndDisabled();

    if (busy) {
        ImGui::TextColored(palette.status_warn, "process: running");
    } else if (tl.runner.state() == ProcessRunner::State::kExited) {
        ImGui::TextColored(palette.text_dim, "process: exited (%d)", tl.runner.exit_code());
    } else {
        ImGui::TextColored(palette.text_dim, "process: idle");
    }
    // Effective caps for the next/active run.
    {
        int thr = tl.settings.max_threads;
        double mem = tl.settings.max_mem_gb;
        if (tl.active_spec) {
            if (tl.active_spec->resources.max_threads > 0) {
                thr = tl.active_spec->resources.max_threads;
            }
            if (tl.active_spec->resources.max_mem_gb > 0.0) {
                mem = tl.active_spec->resources.max_mem_gb;
            }
        }
        if (thr > 0 || mem > 0.0) {
            const std::string thr_s = thr > 0 ? std::to_string(thr) : "all";
            const std::string mem_s =
                mem > 0.0 ? std::format("{:.1f} GB (soft)", mem) : "off";
            ImGui::TextColored(palette.text_dim, "caps: threads %s  mem %s", thr_s.c_str(),
                               mem_s.c_str());
        }
    }
    if (!tl.last_action.empty()) {
        ImGui::TextWrapped("%s", tl.last_action.c_str());
    }
    iw::end_group_box();

    iw::begin_group_box("checkpoint");
    if (!tl.checkpoint) {
        ImGui::TextColored(palette.text_dim, "no checkpoint.json");
    } else {
        const auto& cp = *tl.checkpoint;
        ImGui::TextColored(state_color(cp.state), "state: %s",
                           testlab::checkpoint_state_cstr(cp.state));
        ImGui::Text("tier: %d", cp.tier);
        ImGui::Text("completed runs: %d", cp.completed_runs);
        ImGui::Text("survivors: %zu", cp.survivors.size());
        if (!cp.survivors.empty()) {
            const float sh = std::min(80.0f, 16.0f * static_cast<float>(cp.survivors.size()) + 4.0f);
            if (ImGui::BeginChild("##survivors", ImVec2(-FLT_MIN, sh), ImGuiChildFlags_Borders)) {
                for (const auto& id : cp.survivors) {
                    ImGui::TextUnformatted(id.c_str());
                }
            }
            ImGui::EndChild();
        }
        if (!cp.updated_utc.empty()) {
            ImGui::TextColored(palette.text_dim, "updated %s", cp.updated_utc.c_str());
        }
    }
    iw::end_group_box();

    iw::begin_group_box("live progress");
    if (!tl.progress) {
        ImGui::TextColored(palette.text_dim, "no progress.json");
        ImGui::TextWrapped(
            "Harness rewrites progress.json (~500 ms) during mesh/solve (interfaces.md §6).");
    } else {
        const auto& p = *tl.progress;
        const bool done = (p.phase == "done");
        const bool live = tl.runner.is_running() && !done;
        ImGui::TextColored(done ? palette.status_ok : palette.status_warn, "phase: %s%s",
                           p.phase.empty() ? "?" : p.phase.c_str(), live ? " · live" : "");
        float frac = static_cast<float>(std::clamp(p.phase_frac, 0.0, 1.0));
        // Soft pulse while a long phase holds a fixed fraction (mesh, LDLT, etc.).
        float display = frac;
        if (live && frac < 0.995f) {
            const float pulse =
                0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.8f);
            display = std::clamp(frac + 0.03f * pulse, 0.0f, 0.99f);
        }
        ImGui::ProgressBar(display, ImVec2(-FLT_MIN, 0),
                           std::format("{:.0f}%", 100.0 * p.phase_frac).c_str());
        ImGui::Text("elapsed: %.1f s", p.elapsed_ms / 1000.0);
        if (p.cg_iter > 0) {
            ImGui::Text("CG: iter %d  resid %.3g", p.cg_iter, p.cg_resid);
        }
        if (p.n_elems > 0 || p.n_nodes > 0) {
            ImGui::Text("mesh: %zu elems  %zu nodes", p.n_elems, p.n_nodes);
        }
        if (!p.cfg_id.empty() || !p.part.empty()) {
            ImGui::TextColored(palette.text_dim, "%s  %s  tier %d", p.cfg_id.c_str(),
                               p.part.c_str(), p.tier);
        }
        if (tl.campaign_mesh) {
            ImGui::TextColored(palette.accent, "viewport: live mesh preview");
        }
        if (live) {
            ImGui::TextColored(palette.accent, "harness live · refresh 0.25s");
        }
    }
    iw::end_group_box();

    if (tl.active_spec) {
        iw::begin_group_box("spec");
        const auto& sp = *tl.active_spec;
        ImGui::TextWrapped("%s", sp.name.c_str());
        if (testlab::is_measure_first_baseline(sp.name)) {
            ImGui::TextColored(palette.accent,
                               "measure-first baseline (M9 · ADR-0023/24)");
        }
        ImGui::Text("parts: %zu  tiers: %zu", sp.parts.size(), sp.tiers.size());
        ImGui::Text("grid axes: %zu", sp.grid.size());
        for (const auto& ax : sp.grid) {
            ImGui::TextColored(palette.text_dim, "  %s ×%zu", ax.key.c_str(), ax.values.size());
        }
        ImGui::Text("score w: acc %.2f  solve %.2f  mesh %.2f", sp.score.accuracy,
                    sp.score.solve_ms, sp.score.mesh_ms);
        if (sp.resources.max_threads > 0 || sp.resources.max_mem_gb > 0.0) {
            ImGui::Text("caps: threads %d  mem %.1f GB", sp.resources.max_threads,
                        sp.resources.max_mem_gb);
        }
        iw::end_group_box();
    }

    // V10c stub: supervised open questions from handoff.json when present.
    if (tl.handoff && !tl.handoff->open_program_nodes.empty()) {
        iw::begin_group_box("open questions");
        ImGui::TextColored(palette.text_dim, "handoff.json · open program nodes");
        if (!tl.handoff->mode.empty()) {
            ImGui::Text("mode: %s", tl.handoff->mode.c_str());
        }
        if (!tl.handoff->finished_utc.empty()) {
            ImGui::TextColored(palette.text_dim, "finished %s",
                               tl.handoff->finished_utc.c_str());
        }
        const auto& nodes = tl.handoff->open_program_nodes;
        const float oh =
            std::min(100.0f, 16.0f * static_cast<float>(std::min(nodes.size(), std::size_t{8})) +
                                 4.0f);
        if (ImGui::BeginChild("##open_nodes", ImVec2(-FLT_MIN, oh), ImGuiChildFlags_Borders)) {
            for (const auto& id : nodes) {
                ImGui::TextUnformatted(id.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::TextColored(palette.accent, "V10c: queue for supervised review");
        iw::end_group_box();
    }
}

void draw_results_panel(TestLabState& tl) {
    // Measure panel height *before* auto-sized group boxes. A table height
    // taken inside AutoResizeY children is ~0, which capped the runs list at
    // ~120 px; fill the remaining column so many rows are visible at once.
    const float panel_y0 = ImGui::GetCursorScreenPos().y;
    const float panel_h = ImGui::GetContentRegionAvail().y;

    iw::begin_group_box("results");
    ImGui::TextColored(palette.text_dim, "results.jsonl (append-only)");
    if (tl.selected_summary() == nullptr) {
        ImGui::TextColored(palette.text_dim, "select a campaign");
        iw::end_group_box();
        // Still show an empty runs box so the column doesn't look sparse.
        const float used = ImGui::GetCursorScreenPos().y - panel_y0;
        const float runs_h = std::max(160.0f, panel_h - used);
        iw::begin_group_box_fill("runs", runs_h);
        ImGui::TextColored(palette.text_dim, "select a campaign to load results");
        iw::end_group_box();
        return;
    }
    ImGui::Text("%zu rows", tl.results.size());
    if (tl.results.empty()) {
        ImGui::TextWrapped("No runs recorded yet. Start a campaign from the Test Lab panel.");
    } else {
        // Summary chips: ok / solve_suspect / fail / over_budget.
        int n_ok = 0, n_suspect = 0, n_fail = 0, n_budget = 0;
        double best_err = 1e300;
        for (const auto& r : tl.results) {
            if (r.status == "ok") {
                ++n_ok;
                if (!r.accuracy.metric.empty() && r.accuracy.rel_err < best_err) {
                    best_err = r.accuracy.rel_err;
                }
            } else if (r.status == "solve_suspect") {
                ++n_suspect;
            } else if (r.status == "over_budget") {
                ++n_budget;
            } else {
                ++n_fail;
            }
        }
        ImGui::TextColored(palette.status_ok, "ok %d", n_ok);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(n_suspect > 0 ? palette.status_warn : palette.text_dim, "suspect %d",
                           n_suspect);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(n_fail > 0 ? palette.status_err : palette.text_dim, "fail %d", n_fail);
        ImGui::SameLine(0, 10);
        ImGui::TextColored(n_budget > 0 ? palette.status_warn : palette.text_dim, "budget %d",
                           n_budget);
        if (n_ok > 0 && best_err < 1e299) {
            ImGui::Text("best |rel_err|: %.4g", best_err);
        }
    }
    iw::end_group_box();

    const float used = ImGui::GetCursorScreenPos().y - panel_y0;
    const float runs_h = std::max(160.0f, panel_h - used);
    iw::begin_group_box_fill("runs", runs_h);
    // Table fills the fixed content region (scroll inside).
    const float table_h = std::max(80.0f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginChild("##results_table", ImVec2(-FLT_MIN, table_h), ImGuiChildFlags_Borders)) {
        if (tl.results.empty()) {
            ImGui::TextColored(palette.text_dim, "no runs yet");
        } else if (ImGui::BeginTable("##res", 9,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("cfg");
            ImGui::TableSetupColumn("part");
            ImGui::TableSetupColumn("status");
            ImGui::TableSetupColumn("health");
            ImGui::TableSetupColumn("rel_err");
            ImGui::TableSetupColumn("tip");
            ImGui::TableSetupColumn("energy");
            ImGui::TableSetupColumn("mesh ms");
            ImGui::TableSetupColumn("solve ms");
            ImGui::TableHeadersRow();

            char num_buf[64];
            // Show newest first (jsonl is append-only).
            for (int i = static_cast<int>(tl.results.size()) - 1; i >= 0; --i) {
                const auto& r = tl.results[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.cfg_id.c_str());
                if (ImGui::IsItemHovered()) {
                    const auto tip = result_detail_tooltip(r);
                    if (!tip.empty()) {
                        ImGui::SetTooltip("%s", tip.c_str());
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.part.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(result_status_color(r.status), "%s", r.status.c_str());
                ImGui::TableSetColumnIndex(3);
                if (r.health.present) {
                    ImGui::TextColored(r.health.ok ? palette.status_ok : palette.status_warn, "%s",
                                       r.health.ok ? "ok" : "no");
                } else if (r.scorecard.has_health_ok) {
                    ImGui::TextColored(r.scorecard.health_ok ? palette.status_ok
                                                             : palette.status_warn,
                                       "%s", r.scorecard.health_ok ? "ok" : "no");
                } else {
                    ImGui::TextColored(palette.text_dim, "—");
                }
                ImGui::TableSetColumnIndex(4);
                if (r.accuracy.metric.empty()) {
                    ImGui::TextColored(palette.text_dim, "—");
                } else {
                    ImGui::Text("%.3g", r.accuracy.rel_err);
                }
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(fmt_opt_num(r.answers.tip_deflection, num_buf, sizeof(num_buf)));
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(fmt_opt_num(r.answers.strain_energy, num_buf, sizeof(num_buf)));
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%.0f", r.mesh_ms);
                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%.0f", r.solve_ms);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    iw::end_group_box();
}

} // namespace polymesh::gui
