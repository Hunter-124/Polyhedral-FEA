// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// GUI layout + test-lab path preferences. Presentation only — no physics.
// Paths default to the normative locations under the repo root (CWD for the
// desktop app when launched from the tree).

#include "theme.hpp"

#include <filesystem>
#include <string>

namespace polymesh::gui {

struct GuiSettings {
    /// Root directory that holds campaign subfolders (interfaces.md §1–3).
    std::string campaigns_root = "bench/campaigns";

    /// Override path to the polymesh_testlab binary. Empty → auto-discover.
    std::string testlab_binary;

    /// Fixed-layout panel widths (pixels). Splitters may mutate these at runtime.
    float testlab_width = 280.0f;
    float sim_width = 340.0f;
    float results_width = 300.0f;

    ThemeId theme = ThemeId::kInterwebz;

    /// How often (seconds) to re-scan campaign files while a run is live.
    float refresh_interval_s = 0.5f;

    /// OpenMP thread cap for interactive mesh/solve (0 = process default / all).
    /// Applied via `fea::set_openmp_threads` before starting a SolveJob.
    int max_threads = 0;

    /// Soft memory budget for the UI (GB). 0 = unlimited.
    /// Displayed as a soft note; not a hard process RSS kill (no existing hook).
    double max_mem_gb = 0.0;

    /// Resolve the harness binary (override, then common build paths, then bare name).
    std::filesystem::path resolved_testlab_binary() const;

    /// Campaigns root as a path (relative to CWD unless absolute).
    std::filesystem::path campaigns_root_path() const;
};

} // namespace polymesh::gui
