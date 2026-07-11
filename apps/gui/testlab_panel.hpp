// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Test Lab + Results panels. Drive polymesh_testlab only via file schemas
// (docs/dag/interfaces.md) and ProcessRunner fork/exec — no apps/testlab link.

#include "gui_settings.hpp"
#include "process_runner.hpp"
#include "testlab_data.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace polymesh::gui {

/// Runtime state for the Test Lab column and the Results column.
struct TestLabState {
    GuiSettings settings;
    ProcessRunner runner;

    std::vector<testlab::CampaignSummary> campaigns;
    int selected = -1;

    std::optional<testlab::CampaignSpec> active_spec;
    std::optional<testlab::Checkpoint> checkpoint;
    std::optional<testlab::LiveProgress> progress;
    std::vector<testlab::ResultRow> results;

    /// Campaign mesh_preview.pmp — reload when file mtime changes.
    std::optional<testlab::MeshPreview> campaign_mesh;
    std::filesystem::file_time_type campaign_mesh_mtime{};
    bool campaign_mesh_dirty = false;

    char campaigns_root_buf[512] = "bench/campaigns";
    char testlab_bin_buf[512] = "";
    std::string status = "select a campaign";
    std::string last_action;

    // Dirty flags / timers for file refresh.
    std::chrono::steady_clock::time_point last_refresh{};
    bool force_refresh = true;

    void sync_buffers_from_settings();
    void apply_buffers_to_settings();
    void refresh_campaign_list();
    void refresh_selected();
    void tick(float dt_s);

    bool start_run(/*resume=*/bool resume);
    bool pause_run();
    /// Last-resort SIGKILL of a spawned harness (prefer pause_run / SIGINT).
    bool force_stop();

    const testlab::CampaignSummary* selected_summary() const;
    std::filesystem::path selected_dir() const;
};

/// Left column: campaign browser + run controls + live progress.
void draw_testlab_panel(TestLabState& tl);

/// Right column: results.jsonl table for the selected campaign.
void draw_results_panel(TestLabState& tl);

} // namespace polymesh::gui
