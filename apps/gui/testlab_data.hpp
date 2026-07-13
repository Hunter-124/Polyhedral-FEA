// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// ImGui-free parsers for the normative test-lab file formats in
// docs/dag/interfaces.md. The GUI talks to polymesh_testlab only through
// these on-disk schemas — never by linking apps/testlab sources.

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::gui::testlab {

struct CampaignTier {
    double h_scale = 1.0;
    double keep_frac = 1.0;
};

struct CampaignScoreWeights {
    double accuracy = 0.5;
    double solve_ms = 0.25;
    double mesh_ms = 0.25;
};

struct CampaignResources {
    int max_threads = 0;   // 0 = unlimited
    double max_mem_gb = 0; // 0 = unlimited
};

/// One axis of the factorial grid; values stored as display strings.
struct GridAxis {
    std::string key;
    std::vector<std::string> values;
};

struct CampaignSpec {
    std::string name;
    std::vector<std::string> parts;
    std::vector<CampaignTier> tiers;
    std::vector<GridAxis> grid;
    CampaignScoreWeights score;
    CampaignResources resources;
    std::filesystem::path dir;  // campaign directory
    std::filesystem::path path; // campaign.json
};

enum class CheckpointState : int {
    kUnknown = 0,
    kRunning,
    kPaused,
    kFinished,
};

struct Checkpoint {
    std::string campaign;
    CheckpointState state = CheckpointState::kUnknown;
    int tier = 0;
    int completed_runs = 0;
    std::vector<std::string> survivors;
    std::string started_utc;
    std::string updated_utc;
};

struct QualityInfo {
    double M1max = 0.0;
    double M2max = 0.0;
    double M6 = 0.0;
    double score = 0.0;
};

struct AccuracyInfo {
    std::string metric;
    double value = 0.0;
    double truth = 0.0;
    double rel_err = 0.0;
};

/// Health gates from results.jsonl `health` (interfaces.md §3). Absent when
/// the object is missing; individual doubles are NaN when null/absent.
struct HealthInfo {
    bool present = false;
    bool ok = false;
    double free_residual_rel = std::numeric_limits<double>::quiet_NaN();
    double reaction_sum_err = std::numeric_limits<double>::quiet_NaN();
    bool load_area_ok = true;
    bool has_load_area_ok = false;
    int n_orphans = 0;
};

/// Measure-first scorecard (interfaces.md §3 / ADR-0023). Doubles are NaN when
/// null or absent (e.g. edge Hausdorff without CAD).
struct ScorecardInfo {
    bool present = false;
    double edge_hausdorff_over_h = std::numeric_limits<double>::quiet_NaN();
    double chordal_efficiency_max = std::numeric_limits<double>::quiet_NaN();
    double normal_dev_deg_max = std::numeric_limits<double>::quiet_NaN();
    double accuracy_rel_err = std::numeric_limits<double>::quiet_NaN();
    double min_element_quality = std::numeric_limits<double>::quiet_NaN();
    double solve_residual_rel = std::numeric_limits<double>::quiet_NaN();
    bool health_ok = false;
    bool has_health_ok = false;
    int n_dof = 0;
};

/// Subset of `answers` used by the Results panel (score / secondary health).
struct AnswersInfo {
    double strain_energy = std::numeric_limits<double>::quiet_NaN();
    double tip_deflection = std::numeric_limits<double>::quiet_NaN();
    double sigma_face_mean = std::numeric_limits<double>::quiet_NaN();
};

struct ResultRow {
    std::string cfg_id;
    std::string part;
    int tier = 0;
    double mesh_ms = 0.0;
    double solve_ms = 0.0;
    int n_elems = 0;
    int n_nodes = 0;
    int n_dof = 0;
    /// Predicted element count (M4); NaN if absent. May be fractional (C·V/h³).
    double n_pred_elems = std::numeric_limits<double>::quiet_NaN();
    QualityInfo quality;
    AccuracyInfo accuracy;
    HealthInfo health;
    ScorecardInfo scorecard;
    AnswersInfo answers;
    /// ok | solve_suspect | mesh_fail | solve_fail | over_budget
    std::string status;
    std::string config_summary; // short display of config object
};

/// Machine handoff pack (interfaces.md §8a) when present under the campaign dir.
struct HandoffInfo {
    std::string campaign;
    std::string git_head;
    std::string finished_utc;
    std::string mode; // autonomous | supervised
    std::vector<std::string> open_program_nodes;
    std::string checkpoint_state;
};

struct LiveProgress {
    std::string phase; // mesh | assemble | solve | recover | done
    double phase_frac = 0.0;
    double elapsed_ms = 0.0;
    int cg_iter = 0;
    double cg_resid = 0.0;
    std::string cfg_id;
    std::string part;
    int tier = 0;
    std::size_t n_elems = 0;
    std::size_t n_nodes = 0;
};

struct CampaignSummary {
    std::filesystem::path dir;
    std::string name;
    bool has_campaign_json = false;
    bool has_checkpoint = false;
    bool has_results = false;
    int result_count = 0;
    CheckpointState state = CheckpointState::kUnknown;
};

/// Parse helpers (throw std::runtime_error on hard failures; empty optional
/// loaders return nullopt when the file is missing).
CampaignSpec parse_campaign(const std::string& json_text);
Checkpoint parse_checkpoint(const std::string& json_text);
ResultRow parse_result_line(const std::string& line);
LiveProgress parse_progress(const std::string& json_text);

CheckpointState parse_checkpoint_state(const std::string& s);
const char* checkpoint_state_cstr(CheckpointState s);

std::optional<CampaignSpec> load_campaign(const std::filesystem::path& dir);
std::optional<Checkpoint> load_checkpoint(const std::filesystem::path& dir);
std::vector<ResultRow> load_results(const std::filesystem::path& dir);
std::optional<LiveProgress> load_progress(const std::filesystem::path& dir);
std::optional<HandoffInfo> load_handoff(const std::filesystem::path& dir);
HandoffInfo parse_handoff(const std::string& json_text);

/// True when a campaign name looks like the measure-first M9 freeze baseline.
bool is_measure_first_baseline(const std::string& campaign_name);

/// Boundary mesh from campaign `mesh_preview.pmp` (interfaces.md §6b).
struct MeshPreview {
    std::vector<std::array<double, 3>> nodes;
    std::vector<std::array<std::uint32_t, 4>> quads;
    std::size_t n_elems = 0;
    std::string note;
};

/// Load campaign mesh_preview.pmp for live viewport. nullopt if missing/bad.
std::optional<MeshPreview> load_mesh_preview(const std::filesystem::path& dir);

/// List immediate subdirs of `root` that look like campaigns (have
/// campaign.json and/or checkpoint.json / results.jsonl).
std::vector<CampaignSummary> scan_campaigns(const std::filesystem::path& root);

/// Count non-empty lines in a results.jsonl without full parse (fast scan).
int count_result_lines(const std::filesystem::path& results_path);

} // namespace polymesh::gui::testlab
