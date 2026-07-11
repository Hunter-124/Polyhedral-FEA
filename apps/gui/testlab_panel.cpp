// SPDX-License-Identifier: BSD-3-Clause
#include "testlab_panel.hpp"

#include "fea/backend.hpp"
#include "theme.hpp"
#include "widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace polymesh::gui {
namespace {

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
    if (status == "over_budget") {
        return palette.status_warn;
    }
    return palette.status_err;
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

    // Mirror process state into a friendly status line.
    if (runner.is_running()) {
        status = "harness running…";
    } else if (runner.state() == ProcessRunner::State::kExited) {
        status = std::format("harness exited ({})", runner.exit_code());
    } else if (checkpoint) {
        status = std::format("{} — tier {} — {} runs",
                             testlab::checkpoint_state_cstr(checkpoint->state), checkpoint->tier,
                             checkpoint->completed_runs);
    }
}

void TestLabState::tick(float /*dt_s*/) {
    runner.poll();

    const auto now = std::chrono::steady_clock::now();
    const float interval = std::max(0.15f, settings.refresh_interval_s);
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
                ImGui::PushID(i);
                if (ImGui::Selectable(c.name.c_str(), sel)) {
                    tl.selected = i;
                    tl.force_refresh = true;
                    tl.refresh_selected();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%d results · %s", c.dir.string().c_str(), c.result_count,
                                      testlab::checkpoint_state_cstr(c.state));
                }
                ImGui::SameLine();
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
            "Harness rewrites progress.json (~500 ms) during solves (interfaces.md §6).");
    } else {
        const auto& p = *tl.progress;
        const bool done = (p.phase == "done");
        ImGui::TextColored(done ? palette.status_ok : palette.status_warn, "phase: %s",
                           p.phase.empty() ? "?" : p.phase.c_str());
        const float frac = static_cast<float>(std::clamp(p.phase_frac, 0.0, 1.0));
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0),
                           std::format("{:.0f}%", 100.0 * p.phase_frac).c_str());
        ImGui::Text("elapsed: %.1f s", p.elapsed_ms / 1000.0);
        if (p.cg_iter > 0 || p.cg_resid > 0.0) {
            ImGui::Text("CG: iter %d  resid %.3g", p.cg_iter, p.cg_resid);
        }
        if (!p.cfg_id.empty() || !p.part.empty()) {
            ImGui::TextColored(palette.text_dim, "%s  %s  tier %d", p.cfg_id.c_str(),
                               p.part.c_str(), p.tier);
        }
        if (tl.runner.is_running()) {
            ImGui::TextColored(palette.accent, "live · refresh %.1fs",
                               tl.settings.refresh_interval_s);
        }
    }
    iw::end_group_box();

    if (tl.active_spec) {
        iw::begin_group_box("spec");
        const auto& sp = *tl.active_spec;
        ImGui::TextWrapped("%s", sp.name.c_str());
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
        // Aggregate quick stats.
        int n_ok = 0, n_fail = 0;
        double best_err = 1e300;
        for (const auto& r : tl.results) {
            if (r.status == "ok") {
                ++n_ok;
                if (r.accuracy.rel_err < best_err) {
                    best_err = r.accuracy.rel_err;
                }
            } else {
                ++n_fail;
            }
        }
        ImGui::TextColored(palette.status_ok, "ok %d", n_ok);
        ImGui::SameLine(0, 12);
        ImGui::TextColored(n_fail > 0 ? palette.status_err : palette.text_dim, "fail %d", n_fail);
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
        } else if (ImGui::BeginTable("##res", 7,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("cfg");
            ImGui::TableSetupColumn("part");
            ImGui::TableSetupColumn("tier");
            ImGui::TableSetupColumn("mesh ms");
            ImGui::TableSetupColumn("solve ms");
            ImGui::TableSetupColumn("rel_err");
            ImGui::TableSetupColumn("status");
            ImGui::TableHeadersRow();

            // Show newest first (jsonl is append-only).
            for (int i = static_cast<int>(tl.results.size()) - 1; i >= 0; --i) {
                const auto& r = tl.results[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.cfg_id.c_str());
                if (ImGui::IsItemHovered() && !r.config_summary.empty()) {
                    ImGui::SetTooltip("%s", r.config_summary.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.part.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", r.tier);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.0f", r.mesh_ms);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.0f", r.solve_ms);
                ImGui::TableSetColumnIndex(5);
                if (r.accuracy.metric.empty()) {
                    ImGui::TextColored(palette.text_dim, "—");
                } else {
                    ImGui::Text("%.3g", r.accuracy.rel_err);
                }
                ImGui::TableSetColumnIndex(6);
                ImGui::TextColored(result_status_color(r.status), "%s", r.status.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    iw::end_group_box();
}

} // namespace polymesh::gui
