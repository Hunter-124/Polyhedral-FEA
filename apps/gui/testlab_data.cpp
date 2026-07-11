// SPDX-License-Identifier: BSD-3-Clause
#include "testlab_data.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace polymesh::gui::testlab {
namespace {

using json = nlohmann::json;

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(std::format("cannot open {}", path.string()));
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string json_value_to_string(const json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return std::to_string(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        return std::format("{}", v.get<double>());
    }
    if (v.is_null()) {
        return "null";
    }
    return v.dump();
}

std::string summarize_config(const json& config) {
    if (!config.is_object()) {
        return json_value_to_string(config);
    }
    std::ostringstream oss;
    bool first = true;
    for (auto it = config.begin(); it != config.end(); ++it) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << it.key() << '=' << json_value_to_string(it.value());
    }
    return oss.str();
}

double opt_double(const json& j, const char* key, double def = 0.0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    return j.at(key).get<double>();
}

int opt_int(const json& j, const char* key, int def = 0) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    return j.at(key).get<int>();
}

std::string opt_string(const json& j, const char* key, const std::string& def = {}) {
    if (!j.contains(key) || j[key].is_null()) {
        return def;
    }
    return j.at(key).get<std::string>();
}

} // namespace

CheckpointState parse_checkpoint_state(const std::string& s) {
    if (s == "running") {
        return CheckpointState::kRunning;
    }
    if (s == "paused") {
        return CheckpointState::kPaused;
    }
    if (s == "finished") {
        return CheckpointState::kFinished;
    }
    return CheckpointState::kUnknown;
}

const char* checkpoint_state_cstr(CheckpointState s) {
    switch (s) {
    case CheckpointState::kRunning:
        return "running";
    case CheckpointState::kPaused:
        return "paused";
    case CheckpointState::kFinished:
        return "finished";
    case CheckpointState::kUnknown:
    default:
        return "unknown";
    }
}

CampaignSpec parse_campaign(const std::string& json_text) {
    try {
        const json j = json::parse(json_text);
        CampaignSpec out;
        out.name = j.at("name").get<std::string>();
        if (j.contains("parts") && j["parts"].is_array()) {
            for (const auto& p : j["parts"]) {
                out.parts.push_back(p.get<std::string>());
            }
        }
        if (j.contains("tiers") && j["tiers"].is_array()) {
            for (const auto& t : j["tiers"]) {
                CampaignTier tier;
                tier.h_scale = opt_double(t, "h_scale", 1.0);
                tier.keep_frac = opt_double(t, "keep_frac", 1.0);
                out.tiers.push_back(tier);
            }
        }
        if (j.contains("grid") && j["grid"].is_object()) {
            for (auto it = j["grid"].begin(); it != j["grid"].end(); ++it) {
                GridAxis axis;
                axis.key = it.key();
                if (it.value().is_array()) {
                    for (const auto& v : it.value()) {
                        axis.values.push_back(json_value_to_string(v));
                    }
                }
                out.grid.push_back(std::move(axis));
            }
        }
        if (j.contains("score") && j["score"].is_object()) {
            const auto& sc = j["score"];
            if (sc.contains("weights") && sc["weights"].is_object()) {
                const auto& w = sc["weights"];
                out.score.accuracy = opt_double(w, "accuracy", 0.5);
                out.score.solve_ms = opt_double(w, "solve_ms", 0.25);
                out.score.mesh_ms = opt_double(w, "mesh_ms", 0.25);
            }
        }
        if (j.contains("resources") && j["resources"].is_object()) {
            const auto& r = j["resources"];
            out.resources.max_threads = opt_int(r, "max_threads", 0);
            out.resources.max_mem_gb = opt_double(r, "max_mem_gb", 0.0);
        }
        return out;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::format("malformed campaign JSON: {}", e.what()));
    }
}

Checkpoint parse_checkpoint(const std::string& json_text) {
    try {
        const json j = json::parse(json_text);
        Checkpoint out;
        out.campaign = opt_string(j, "campaign");
        out.state = parse_checkpoint_state(opt_string(j, "state"));
        out.tier = opt_int(j, "tier", 0);
        out.completed_runs = opt_int(j, "completed_runs", 0);
        out.started_utc = opt_string(j, "started_utc");
        out.updated_utc = opt_string(j, "updated_utc");
        if (j.contains("survivors") && j["survivors"].is_array()) {
            for (const auto& s : j["survivors"]) {
                out.survivors.push_back(s.get<std::string>());
            }
        }
        return out;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::format("malformed checkpoint JSON: {}", e.what()));
    }
}

ResultRow parse_result_line(const std::string& line) {
    try {
        const json j = json::parse(line);
        ResultRow out;
        out.cfg_id = opt_string(j, "cfg_id");
        out.part = opt_string(j, "part");
        out.tier = opt_int(j, "tier", 0);
        out.mesh_ms = opt_double(j, "mesh_ms");
        out.solve_ms = opt_double(j, "solve_ms");
        out.n_elems = opt_int(j, "n_elems");
        out.n_nodes = opt_int(j, "n_nodes");
        out.n_dof = opt_int(j, "n_dof");
        out.status = opt_string(j, "status", "ok");
        if (j.contains("quality") && j["quality"].is_object()) {
            const auto& q = j["quality"];
            out.quality.M1max = opt_double(q, "M1max");
            out.quality.M2max = opt_double(q, "M2max");
            out.quality.M6 = opt_double(q, "M6");
            out.quality.score = opt_double(q, "score");
        }
        if (j.contains("accuracy") && j["accuracy"].is_object()) {
            const auto& a = j["accuracy"];
            out.accuracy.metric = opt_string(a, "metric");
            out.accuracy.value = opt_double(a, "value");
            out.accuracy.truth = opt_double(a, "truth");
            out.accuracy.rel_err = opt_double(a, "rel_err");
        }
        if (j.contains("config")) {
            out.config_summary = summarize_config(j["config"]);
        }
        return out;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::format("malformed results.jsonl line: {}", e.what()));
    }
}

LiveProgress parse_progress(const std::string& json_text) {
    try {
        const json j = json::parse(json_text);
        LiveProgress out;
        out.phase = opt_string(j, "phase");
        out.phase_frac = opt_double(j, "phase_frac");
        out.elapsed_ms = opt_double(j, "elapsed_ms");
        out.cg_iter = opt_int(j, "cg_iter");
        out.cg_resid = opt_double(j, "cg_resid");
        out.n_elems = static_cast<std::size_t>(std::max(0, opt_int(j, "n_elems")));
        out.n_nodes = static_cast<std::size_t>(std::max(0, opt_int(j, "n_nodes")));
        if (j.contains("run") && j["run"].is_object()) {
            const auto& r = j["run"];
            out.cfg_id = opt_string(r, "cfg_id");
            out.part = opt_string(r, "part");
            out.tier = opt_int(r, "tier");
        }
        return out;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::format("malformed progress JSON: {}", e.what()));
    }
}

std::optional<CampaignSpec> load_campaign(const std::filesystem::path& dir) {
    const auto path = dir / "campaign.json";
    if (!std::filesystem::is_regular_file(path)) {
        return std::nullopt;
    }
    CampaignSpec spec = parse_campaign(read_file_text(path));
    spec.dir = dir;
    spec.path = path;
    if (spec.name.empty()) {
        spec.name = dir.filename().string();
    }
    return spec;
}

std::optional<Checkpoint> load_checkpoint(const std::filesystem::path& dir) {
    const auto path = dir / "checkpoint.json";
    if (!std::filesystem::is_regular_file(path)) {
        return std::nullopt;
    }
    return parse_checkpoint(read_file_text(path));
}

std::vector<ResultRow> load_results(const std::filesystem::path& dir) {
    const auto path = dir / "results.jsonl";
    std::vector<ResultRow> rows;
    if (!std::filesystem::is_regular_file(path)) {
        return rows;
    }
    std::ifstream in(path);
    if (!in) {
        return rows;
    }
    std::string line;
    while (std::getline(in, line)) {
        // Trim trailing CR (Windows) and skip blank / comment lines.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        rows.push_back(parse_result_line(line));
    }
    return rows;
}

std::optional<LiveProgress> load_progress(const std::filesystem::path& dir) {
    const auto path = dir / "progress.json";
    if (!std::filesystem::is_regular_file(path)) {
        return std::nullopt;
    }
    try {
        return parse_progress(read_file_text(path));
    } catch (...) {
        // progress.json is rewritten atomically; a torn read is non-fatal.
        return std::nullopt;
    }
}

std::optional<MeshPreview> load_mesh_preview(const std::filesystem::path& dir) {
    const auto path = dir / "mesh_preview.pmp";
    if (!std::filesystem::is_regular_file(path)) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    char magic[4]{};
    in.read(magic, 4);
    if (magic[0] != 'P' || magic[1] != 'M' || magic[2] != 'P' || magic[3] != '1') {
        return std::nullopt;
    }
    std::uint32_t n_nodes = 0;
    std::uint32_t n_quads = 0;
    std::uint64_t n_elems = 0;
    in.read(reinterpret_cast<char*>(&n_nodes), sizeof(n_nodes));
    in.read(reinterpret_cast<char*>(&n_quads), sizeof(n_quads));
    in.read(reinterpret_cast<char*>(&n_elems), sizeof(n_elems));
    if (!in || n_nodes > 50'000'000u || n_quads > 50'000'000u) {
        return std::nullopt;
    }
    MeshPreview out;
    out.n_elems = static_cast<std::size_t>(n_elems);
    out.nodes.resize(n_nodes);
    for (std::uint32_t i = 0; i < n_nodes; ++i) {
        float xyz[3]{};
        in.read(reinterpret_cast<char*>(xyz), sizeof(xyz));
        out.nodes[i] = {static_cast<double>(xyz[0]), static_cast<double>(xyz[1]),
                        static_cast<double>(xyz[2])};
    }
    out.quads.resize(n_quads);
    for (std::uint32_t i = 0; i < n_quads; ++i) {
        std::uint32_t ids[4]{};
        in.read(reinterpret_cast<char*>(ids), sizeof(ids));
        out.quads[i] = {ids[0], ids[1], ids[2], ids[3]};
    }
    if (!in) {
        return std::nullopt;
    }
    out.note = std::format("campaign preview · {} nodes · {} faces · {} elems", n_nodes,
                           n_quads, n_elems);
    return out;
}

int count_result_lines(const std::filesystem::path& results_path) {
    if (!std::filesystem::is_regular_file(results_path)) {
        return 0;
    }
    std::ifstream in(results_path);
    if (!in) {
        return 0;
    }
    int n = 0;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())))) {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            ++n;
        }
    }
    return n;
}

std::vector<CampaignSummary> scan_campaigns(const std::filesystem::path& root) {
    std::vector<CampaignSummary> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory()) {
            continue;
        }
        CampaignSummary s;
        s.dir = entry.path();
        s.name = entry.path().filename().string();
        s.has_campaign_json = std::filesystem::is_regular_file(s.dir / "campaign.json");
        s.has_checkpoint = std::filesystem::is_regular_file(s.dir / "checkpoint.json");
        s.has_results = std::filesystem::is_regular_file(s.dir / "results.jsonl");
        if (!s.has_campaign_json && !s.has_checkpoint && !s.has_results) {
            continue;
        }
        if (s.has_campaign_json) {
            try {
                auto spec = load_campaign(s.dir);
                if (spec && !spec->name.empty()) {
                    s.name = spec->name;
                }
            } catch (...) {
                // keep folder name
            }
        }
        if (s.has_results) {
            s.result_count = count_result_lines(s.dir / "results.jsonl");
        }
        if (s.has_checkpoint) {
            try {
                if (auto cp = load_checkpoint(s.dir)) {
                    s.state = cp->state;
                }
            } catch (...) {
                s.state = CheckpointState::kUnknown;
            }
        }
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const CampaignSummary& a, const CampaignSummary& b) { return a.name < b.name; });
    return out;
}

} // namespace polymesh::gui::testlab
