// SPDX-License-Identifier: BSD-3-Clause

// Campaign orchestration: successive-halving tiers, resume from
// checkpoint.json, SIGINT pause, post-campaign hooks, and the CLI commands.

#include "testlab_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace polymesh::testlab::detail {
namespace {

// ── SIGINT → pause after the current run finishes ───────────────────────────

std::atomic<bool> g_pause_requested{false};

void on_sigint(int /*sig*/) {
    g_pause_requested.store(true, std::memory_order_relaxed);
    // Restore default so a second SIGINT aborts hard (user impatience).
    std::signal(SIGINT, SIG_DFL);
}

/// M14: per-run wall limit (s). Explicit campaign override, else tier defaults
/// (ADR-0024 Q9): tier0=15min, tier1=15min, tier2+=45min.
double run_wall_limit_s(const Campaign& camp, int tier) {
    if (camp.max_run_wall_s > 0.0) {
        return camp.max_run_wall_s;
    }
    if (tier <= 0) {
        return 900.0;
    }
    if (tier == 1) {
        return 900.0;
    }
    return 2700.0;
}

double scalar_score(const Campaign& camp, double accuracy, double mesh_ms, double solve_ms) {
    // Soft inverse-time maps ms → (0,1]; accuracy already in [0,1].
    const double s_mesh = 1.0 / (1.0 + mesh_ms / 1000.0);
    const double s_solve = 1.0 / (1.0 + solve_ms / 1000.0);
    return camp.w_accuracy * accuracy + camp.w_mesh_ms * s_mesh + camp.w_solve_ms * s_solve;
}

// Which (cfg_id, part, tier) triples already appear in results.jsonl?
std::set<std::string> completed_keys(const fs::path& results_path) {
    std::set<std::string> keys;
    if (!fs::exists(results_path)) {
        return keys;
    }
    std::ifstream in(results_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const json j = json::parse(line);
            keys.insert(j.at("cfg_id").get<std::string>() + "|" +
                        j.at("part").get<std::string>() + "|" +
                        std::to_string(j.at("tier").get<int>()));
        } catch (...) {
            // skip corrupt lines
        }
    }
    return keys;
}

// Aggregate scores from results for a given tier → cfg_id → mean score.
std::map<std::string, double> scores_from_results(const fs::path& results_path,
                                                  const Campaign& camp, int tier) {
    std::map<std::string, std::vector<double>> acc;
    if (!fs::exists(results_path)) {
        return {};
    }
    std::ifstream in(results_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        try {
            const json j = json::parse(line);
            if (j.value("tier", -1) != tier) {
                continue;
            }
            if (j.value("status", "") != "ok") {
                acc[j.at("cfg_id").get<std::string>()].push_back(0.0);
                continue;
            }
            double accuracy = 0.0;
            if (j.contains("accuracy") && j["accuracy"].contains("score")) {
                accuracy = j["accuracy"]["score"].get<double>();
            } else if (j.contains("accuracy") && j["accuracy"].contains("rel_err") &&
                       j["accuracy"].contains("truth")) {
                // Reconstruct if score missing.
                const double rel = j["accuracy"]["rel_err"].get<double>();
                accuracy = 1.0 / (1.0 + rel / 0.05);
            }
            const double mesh_ms = j.value("mesh_ms", 0.0);
            const double solve_ms = j.value("solve_ms", 0.0);
            acc[j.at("cfg_id").get<std::string>()].push_back(
                scalar_score(camp, accuracy, mesh_ms, solve_ms));
        } catch (...) {
        }
    }
    std::map<std::string, double> mean;
    for (auto& [id, v] : acc) {
        double s = 0.0;
        for (double x : v) {
            s += x;
        }
        mean[id] = v.empty() ? 0.0 : s / static_cast<double>(v.size());
    }
    return mean;
}

std::vector<std::string> trim_survivors(std::vector<std::string> candidates,
                                        const std::map<std::string, double>& scores,
                                        double keep_frac) {
    std::sort(candidates.begin(), candidates.end(),
              [&](const std::string& a, const std::string& b) {
                  const double sa = scores.count(a) ? scores.at(a) : 0.0;
                  const double sb = scores.count(b) ? scores.at(b) : 0.0;
                  if (sa != sb) {
                      return sa > sb;
                  }
                  return a < b;
              });
    auto n_keep = static_cast<std::size_t>(
        std::ceil(keep_frac * static_cast<double>(candidates.size())));
    if (n_keep < 1 && !candidates.empty()) {
        n_keep = 1;
    }
    if (n_keep > candidates.size()) {
        n_keep = candidates.size();
    }
    candidates.resize(n_keep);
    return candidates;
}

/// Interpreter for the post-campaign hooks (ADR-0022); `python3` may lack numpy.
/// Order: $POLYMESH_PYTHON (scripts/advisor/run_batch.py sets its sys.executable),
/// then `python` if it runs (Windows; mirrors python_exe() in the tests), then `python3`.
const std::string& hook_python() {
    static const std::string exe = []() -> std::string {
        if (const char* env = std::getenv("POLYMESH_PYTHON"); env != nullptr && *env != '\0') {
            return std::string("\"") + env + "\"";
        }
#if defined(_WIN32)
        if (std::system("python -c \"import sys\" >nul 2>&1") == 0) {
            return "python";
        }
#endif
        return "python3";
    }();
    return exe;
}

} // namespace

int cmd_pause_status(const fs::path& camp_dir) {
    const fs::path cp_path = camp_dir / "checkpoint.json";
    if (!fs::exists(cp_path)) {
        std::printf("state: none (no checkpoint.json)\n");
        return 0;
    }
    const auto cp = load_checkpoint(cp_path);
    std::printf("state: %s\n"
                "campaign: %s\n"
                "tier: %d\n"
                "completed_runs: %d\n"
                "survivors: %zu\n"
                "started_utc: %s\n"
                "updated_utc: %s\n",
                cp.state.c_str(), cp.campaign.c_str(), cp.tier, cp.completed_runs,
                cp.survivors.size(), cp.started_utc.c_str(), cp.updated_utc.c_str());
    return 0;
}

int cmd_validate(const fs::path& camp_dir) {
    const Campaign camp = load_campaign(camp_dir / "campaign.json");
    const auto configs = expand_grid(camp.grid);
    for (const auto& path : camp.parts) {
        (void)load_case(path);
    }
    const std::size_t max_runs = configs.size() * camp.parts.size() * camp.tiers.size();
    std::printf("valid: %s configs=%zu parts=%zu tiers=%zu max_runs=%zu\n", camp.name.c_str(),
                configs.size(), camp.parts.size(), camp.tiers.size(), max_runs);
    return 0;
}

int run_campaign(const fs::path& camp_dir, bool resume, const AdvisorScorer* advisor) {
    const fs::path camp_path = camp_dir / "campaign.json";
    if (!fs::exists(camp_path)) {
        std::fprintf(stderr, "missing %s\n", camp_path.string().c_str());
        return 1;
    }
    const Campaign camp = load_campaign(camp_path);
    const auto all_configs = expand_grid(camp.grid);
    std::map<std::string, Config> by_id;
    for (const auto& c : all_configs) {
        by_id[c.id] = c;
    }

    // Load part cases once (truths via case → bench/reference only).
    std::vector<PartCase> parts;
    parts.reserve(camp.parts.size());
    for (const auto& p : camp.parts) {
        parts.push_back(load_case(p));
    }

    const fs::path results_path = camp_dir / "results.jsonl";
    const fs::path cp_path = camp_dir / "checkpoint.json";
    const fs::path progress_path = camp_dir / "progress.json";
    // Boundary mesh for GUI live viewport (see interfaces.md §6 / mesh_preview.pmp).
    const fs::path mesh_preview_path = camp_dir / "mesh_preview.pmp";

    Checkpoint cp;
    if (resume) {
        if (!fs::exists(cp_path)) {
            std::fprintf(stderr, "resume: no checkpoint at %s\n", cp_path.string().c_str());
            return 1;
        }
        cp = load_checkpoint(cp_path);
        if (cp.state == "finished") {
            std::printf("campaign already finished (%d runs)\n", cp.completed_runs);
            return 0;
        }
        if (cp.survivors.empty()) {
            // Recover survivors from full grid if checkpoint is incomplete.
            for (const auto& c : all_configs) {
                cp.survivors.push_back(c.id);
            }
        }
        cp.state = "running";
    } else {
        // Fresh run: truncate results, seed survivors = all configs.
        { std::ofstream trunc(results_path, std::ios::trunc); }
        cp.campaign = camp.name;
        cp.state = "running";
        cp.tier = 0;
        cp.completed_runs = 0;
        cp.survivors.clear();
        for (const auto& c : all_configs) {
            cp.survivors.push_back(c.id);
        }
        cp.started_utc = utc_now();
        cp.updated_utc = cp.started_utc;
        write_checkpoint(cp_path, cp);
    }

    std::signal(SIGINT, on_sigint);

    auto done = completed_keys(results_path);
    std::ofstream results_app(results_path, std::ios::app);
    if (!results_app) {
        std::fprintf(stderr, "cannot append %s\n", results_path.string().c_str());
        return 1;
    }

    std::printf("campaign %s: %zu configs, %zu parts, %zu tiers\n", camp.name.c_str(),
                all_configs.size(), parts.size(), camp.tiers.size());
    if (camp.max_run_wall_s > 0.0) {
        std::printf("  max_run_wall_s=%.0f (override all tiers)\n", camp.max_run_wall_s);
    } else {
        std::printf("  max_run_wall_s defaults: tier0/1=900s tier2+=2700s\n");
    }
    if (camp.max_pack_wall_s > 0.0) {
        std::printf("  max_pack_wall_s=%.0f\n", camp.max_pack_wall_s);
    }

    // M14 pack-level wall clock: do not start new runs once pack elapsed exceeds it.
    using pack_clock = std::chrono::steady_clock;
    const auto pack_t0 = pack_clock::now();
    bool pack_budget_hit = false;

    for (int tier = cp.tier; tier < static_cast<int>(camp.tiers.size()); ++tier) {
        if (pack_budget_hit) {
            break;
        }
        cp.tier = tier;
        const TierSpec& ts = camp.tiers[static_cast<std::size_t>(tier)];
        const double run_limit = run_wall_limit_s(camp, tier);
        std::printf("tier %d: h_scale=%.4g keep_frac=%.3g survivors=%zu max_run_wall_s=%.0f\n",
                    tier, ts.h_scale, ts.keep_frac, cp.survivors.size(), run_limit);

        // Ensure survivors are still known configs.
        std::vector<std::string> survivors;
        for (const auto& id : cp.survivors) {
            if (by_id.count(id)) {
                survivors.push_back(id);
            }
        }
        if (survivors.empty()) {
            for (const auto& c : all_configs) {
                survivors.push_back(c.id);
            }
        }
        cp.survivors = survivors;
        write_checkpoint(cp_path, cp);

        for (const auto& cfg_id : survivors) {
            if (pack_budget_hit) {
                break;
            }
            const Config& cfg = by_id.at(cfg_id);
            for (const auto& part : parts) {
                const std::string key = cfg_id + "|" + part.part + "|" + std::to_string(tier);
                if (done.count(key)) {
                    continue; // resume skip
                }

                // M14: pack ceiling — stop *starting* new runs (never abort in-flight).
                if (camp.max_pack_wall_s > 0.0) {
                    const double pack_elapsed =
                        std::chrono::duration<double>(pack_clock::now() - pack_t0).count();
                    if (pack_elapsed > camp.max_pack_wall_s) {
                        pack_budget_hit = true;
                        std::printf("  pack wall-clock exceeded (%.0f s > %.0f s); "
                                    "not starting further runs\n",
                                    pack_elapsed, camp.max_pack_wall_s);
                        break;
                    }
                }

                std::printf("  run %s part=%s tier=%d ...\n", cfg_id.c_str(),
                            part.part.c_str(), tier);
                std::fflush(stdout);
                fs::path wh_dir;
                if (camp.warehouse || cfg.adapt_passes > 0) {
                    wh_dir =
                        camp_dir / "runs" / cfg_id / part.part / ("t" + std::to_string(tier));
                }
                const RunOutcome ro = run_one(cfg, part, tier, ts.h_scale, progress_path,
                                              mesh_preview_path, wh_dir, run_limit, advisor,
                                              camp.host, camp.max_dof, camp.max_elems);
                results_app << ro.line.dump() << '\n';
                results_app.flush();
                // Fail loudly on a dropped row rather than report success with a
                // short summary. Same check-after-flush idiom as atomic_write().
                if (!results_app) {
                    throw std::runtime_error("failed appending row to " +
                                             results_path.string());
                }
                // Artifacts AFTER the row: the row cannot be regenerated, artifacts
                // can (scripts/advisor/rebuild_results.py), so an artifact failure
                // cannot cost a row.
                if (!wh_dir.empty()) {
                    write_warehouse_run(wh_dir, ro.line, ro.mesh ? &*ro.mesh : nullptr);
                }
                done.insert(key);
                ++cp.completed_runs;
                cp.updated_utc = utc_now();
                write_checkpoint(cp_path, cp);

                if (g_pause_requested.load(std::memory_order_relaxed)) {
                    cp.state = "paused";
                    write_checkpoint(cp_path, cp);
                    std::printf("paused after %d runs (SIGINT). resume with:\n"
                                "  polymesh_testlab resume %s\n",
                                cp.completed_runs, camp_dir.string().c_str());
                    return 0;
                }
            }
        }

        // Successive-halving trim (except after last tier — keep all that ran).
        auto scores = scores_from_results(results_path, camp, tier);
        if (tier + 1 < static_cast<int>(camp.tiers.size())) {
            cp.survivors = trim_survivors(cp.survivors, scores, ts.keep_frac);
            std::printf("  trim → %zu survivors for next tier\n", cp.survivors.size());
        }
        write_checkpoint(cp_path, cp);
    }

    cp.state = "finished";
    cp.updated_utc = utc_now();
    write_checkpoint(cp_path, cp);
    write_progress(progress_path, "done", 1.0, 0.0, "", "", cp.tier);
    std::printf("finished: %d runs → %s\n", cp.completed_runs, results_path.string().c_str());

    // Optional post-campaign hooks (ADR-0022). A hook failure never fails the
    // campaign (no hook touches results.jsonl) but is reported in the summary.
    std::vector<std::string> hook_failures;
    const auto run_hook = [&](const char* name, const char* script) {
        const std::string cmd = hook_python() + " " + script + " " + camp.name;
        const int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::fprintf(stderr, "on_finish %s exited %d\n", name, rc);
            hook_failures.push_back(std::string(name) + " (exit " + std::to_string(rc) + ")");
        }
    };
    if (camp.warehouse) {
        // mesh.vtu → wire.png for review; consumed by scripts/advisor/report_meshes.py
        // (WIRE_NAMES).
        run_hook("warehouse_shots", "scripts/warehouse_shots.py");
    }
    if (camp.on_finish_analyze) {
        run_hook("analyze", "scripts/analyze_campaign.py");
    }
    if (!hook_failures.empty()) {
        std::string joined;
        for (const auto& failure : hook_failures) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += failure;
        }
        // Reported on three surfaces: terminal summary, progress.json, and the
        // checkpoint beside `state: finished` (the marker consumers poll).
        std::printf("campaign summary: %d runs, POST-CAMPAIGN HOOKS FAILED: %s\n",
                    cp.completed_runs, joined.c_str());
        std::fprintf(stderr, "campaign summary: POST-CAMPAIGN HOOKS FAILED: %s\n",
                     joined.c_str());
        write_progress(progress_path, "done", 1.0, 0.0, "", "", cp.tier, -1, -1.0, 0, 0,
                       hook_failures);
        cp.hooks_failed = hook_failures;
        write_checkpoint(cp_path, cp);
    }
    return 0;
}

} // namespace polymesh::testlab::detail
