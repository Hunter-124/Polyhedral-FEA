// SPDX-License-Identifier: BSD-3-Clause

// campaign.json / case / reference / grid / checkpoint parsing and writing.
//
// Anti-cheat: reference truths are loaded only from paths declared in case
// files (bench/reference/*). No numeric answers are embedded here.

#include "advisor/calibration.hpp"
#include "run_artifacts.hpp"
#include "testlab_internal.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::testlab::detail {
namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ── stable config id ────────────────────────────────────────────────────────

// FNV-1a 64-bit over a canonical JSON dump of the config object.
std::string cfg_id_of(const json& config) {
    // Sort keys via dump of a fresh object in key order (nlohmann sorts by default
    // only with ordered_json; we rebuild from a std::map for stability).
    std::map<std::string, json> ordered;
    for (auto it = config.begin(); it != config.end(); ++it) {
        ordered[it.key()] = it.value();
    }
    json canon = ordered;
    const std::string s = canon.dump();
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "cfg-%08x", static_cast<unsigned>(h & 0xffffffffu));
    return buf;
}

Box3 parse_box(const json& j) {
    // [[xmin,ymin,zmin],[xmax,ymax,zmax]]
    if (!j.is_array() || j.size() != 2 || !j[0].is_array() || !j[1].is_array() ||
        j[0].size() != 3 || j[1].size() != 3) {
        throw std::runtime_error("box must be [[xmin,ymin,zmin],[xmax,ymax,zmax]]");
    }
    Box3 b;
    b.lo =
        Eigen::Vector3d(j[0][0].get<double>(), j[0][1].get<double>(), j[0][2].get<double>());
    b.hi =
        Eigen::Vector3d(j[1][0].get<double>(), j[1][1].get<double>(), j[1][2].get<double>());
    return b;
}

std::vector<MetricSpec> load_metrics(const fs::path& ref_path) {
    const json j = json::parse(read_file(ref_path));
    std::vector<MetricSpec> out;
    // interfaces.md format: { "part", "metrics": [ {name,value,tol,probe} ] }
    if (j.contains("metrics") && j["metrics"].is_array()) {
        for (const auto& m : j["metrics"]) {
            MetricSpec ms;
            ms.name = m.at("name").get<std::string>();
            ms.value = m.at("value").get<double>();
            ms.tol = m.value("tol", 0.05);
            if (m.contains("probe")) {
                ms.probe.kind = m["probe"].at("kind").get<std::string>();
                ms.probe.nominal = m["probe"].value("nominal", 0.0);
                if (m["probe"].contains("select") && m["probe"]["select"].contains("box")) {
                    ms.probe.select = parse_box(m["probe"]["select"]["box"]);
                }
            }
            // Raw nodal sigma_vm_max is singularity-sensitive (it grows with
            // refinement at a clamped end), so it is a diagnostic and never a
            // score (ADR-0023, docs/validation/hand-calcs.md).
            if (ms.probe.kind == "max_von_mises" || ms.probe.kind == "max_vm") {
                throw std::runtime_error(
                    "reference " + ref_path.string() + " metric '" + ms.name +
                    "' scores probe kind '" + ms.probe.kind +
                    "'. Raw nodal max von "
                    "Mises is singularity-sensitive and is a DIAGNOSTIC, never a "
                    "score (ADR-0023). Use strain_energy, tip_deflection, "
                    "sigma_p99, or a face-mean ratio.");
            }
            ms.derivation = m.value("derivation", "");
            out.push_back(std::move(ms));
        }
        return out;
    }
    // Legacy bench/reference format: { name, citation, values: {key: number} }
    // Treat each value as a max_von_mises-style metric only when probe is absent —
    // not usable without probe kinds. Require metrics[] for the harness.
    throw std::runtime_error(
        "reference " + ref_path.string() +
        " must use interfaces.md metrics[] (name/value/tol/probe); legacy values-only not "
        "supported by testlab");
}

/// Campaign configs name meshers with the vocabulary in
/// `pipeline::mesher_from_name`, the single table beside the enum, so a mesher
/// name this binary prints into a results row round-trips.
pipeline::VolumeMesher parse_mesher(const std::string& name) {
    const auto parsed = pipeline::mesher_from_name(name);
    if (!parsed) {
        throw std::runtime_error("unknown mesher '" + name + "'");
    }
    return *parsed;
}

} // namespace

// ── time ────────────────────────────────────────────────────────────────────

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Campaign load_campaign(const fs::path& path) {
    const json j = json::parse(read_file(path));
    Campaign c;
    c.name = j.at("name").get<std::string>();
    c.host = j.value("host", std::string{});
    if (c.host.empty()) {
        c.host = polymesh::advisor::local_host_name();
    }
    for (const auto& p : j.at("parts")) {
        c.parts.push_back(p.get<std::string>());
    }
    for (const auto& t : j.at("tiers")) {
        TierSpec ts;
        ts.h_scale = t.at("h_scale").get<double>();
        ts.keep_frac = t.value("keep_frac", 1.0);
        if (!(ts.keep_frac > 0.0) || ts.keep_frac > 1.0) {
            throw std::runtime_error("keep_frac must be in (0,1]");
        }
        c.tiers.push_back(ts);
    }
    if (c.tiers.empty()) {
        throw std::runtime_error("campaign needs at least one tier");
    }
    c.grid = j.at("grid");
    if (!c.grid.is_object()) {
        throw std::runtime_error("grid must be an object");
    }
    static const std::set<std::string> kGridKeys{
        "mesher",           "feature_refine", "order",
        "element_tendency", "bc_grading",     "curvature_turn_deg",
        "snap_boundary",    "skin_layers",    "adapt_passes",
        "eta_target",       "p_elevate",      "adapt_leb_waves",
        "spectral_smooth",  "h_rel",          "cost_only"};
    for (auto it = c.grid.begin(); it != c.grid.end(); ++it) {
        if (!kGridKeys.contains(it.key())) {
            throw std::runtime_error("unknown grid key '" + it.key() + "'");
        }
    }
    if (j.contains("score") && j["score"].contains("weights")) {
        const auto& w = j["score"]["weights"];
        c.w_accuracy = w.value("accuracy", 0.5);
        c.w_solve_ms = w.value("solve_ms", 0.25);
        c.w_mesh_ms = w.value("mesh_ms", 0.25);
    }
    c.warehouse = j.value("warehouse", false);
    if (j.contains("on_finish") && j["on_finish"].is_object()) {
        c.on_finish_analyze = j["on_finish"].value("analyze", false);
        // Other on_finish keys are ignored, so campaign.json files naming retired hooks load.
    }
    // M14 resources (interfaces.md §1): wall-clock kills + pack ceiling.
    if (j.contains("resources") && j["resources"].is_object()) {
        const auto& r = j["resources"];
        c.max_run_wall_s = r.value("max_run_wall_s", 0.0);
        c.max_pack_wall_s = r.value("max_pack_wall_s", 0.0);
        c.max_dof = r.value("max_dof", 0LL);
        c.max_elems = r.value("max_elems", 0LL);
    }
    return c;
}

PartCase load_case(const fs::path& path) {
    const json j = json::parse(read_file(path));
    PartCase c;
    c.part = j.at("part").get<std::string>();
    c.geometry = j.at("geometry").get<std::string>();
    if (j.contains("material")) {
        const auto& mat = j["material"];
        c.E = mat.value("E", mat.value("youngs_modulus", 200e9));
        c.nu = mat.value("nu", mat.value("poissons_ratio", 0.3));
        c.rho = mat.value("rho", 7850.0);
    }
    if (j.contains("bcs")) {
        for (const auto& b : j["bcs"]) {
            BcSpec bc;
            bc.box = parse_box(b.at("select").at("box"));
            if (b.contains("fix") && b["fix"].is_array() && b["fix"].size() == 3) {
                bc.fix = {b["fix"][0].get<bool>(), b["fix"][1].get<bool>(),
                          b["fix"][2].get<bool>()};
            }
            c.bcs.push_back(bc);
        }
    }
    if (j.contains("loads")) {
        for (const auto& L : j["loads"]) {
            LoadSpec ls;
            ls.box = parse_box(L.at("select").at("box"));
            if (L["select"].contains("expected_area")) {
                ls.expected_area = L["select"]["expected_area"].get<double>();
            }
            if (L["select"].contains("normal_min_dot")) {
                ls.normal_min_dot = L["select"]["normal_min_dot"].get<double>();
            }
            if (L.contains("traction") && L["traction"].is_array() &&
                L["traction"].size() == 3) {
                ls.traction = Eigen::Vector3d(L["traction"][0].get<double>(),
                                              L["traction"][1].get<double>(),
                                              L["traction"][2].get<double>());
            }
            c.loads.push_back(ls);
        }
    }
    c.reference_path = j.at("reference").get<std::string>();
    c.metrics = load_metrics(c.reference_path);
    return c;
}

// Full-factorial expansion of campaign.grid → Config list.
std::vector<Config> expand_grid(const json& grid) {
    // Collect keys and value lists.
    std::vector<std::string> keys;
    std::vector<std::vector<json>> axes;
    for (auto it = grid.begin(); it != grid.end(); ++it) {
        if (!it.value().is_array() || it.value().empty()) {
            throw std::runtime_error("grid." + it.key() + " must be a non-empty array");
        }
        keys.push_back(it.key());
        axes.push_back(it.value().get<std::vector<json>>());
    }
    if (keys.empty()) {
        throw std::runtime_error("grid is empty");
    }
    // Cartesian product.
    std::vector<std::size_t> idx(keys.size(), 0);
    std::vector<Config> out;
    for (;;) {
        json values = json::object();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            values[keys[i]] = axes[i][idx[i]];
        }
        Config cfg;
        cfg.values = values;
        cfg.id = cfg_id_of(values);
        if (values.contains("mesher")) {
            cfg.mesher = parse_mesher(values["mesher"].get<std::string>());
        }
        if (values.contains("feature_refine")) {
            cfg.feature_refine = values["feature_refine"].get<bool>();
        }
        if (values.contains("bc_grading")) {
            cfg.bc_grading = values["bc_grading"].get<bool>();
        }
        if (values.contains("spectral_smooth")) {
            cfg.spectral_smooth = values["spectral_smooth"].get<bool>();
        }
        if (values.contains("curvature_turn_deg")) {
            cfg.curvature_turn_deg = values["curvature_turn_deg"].get<double>();
            (void)cfg.curvature_turn_deg; // recorded only (see Config)
        }
        if (values.contains("snap_boundary")) {
            cfg.snap_boundary = values["snap_boundary"].get<bool>();
            (void)cfg.snap_boundary; // recorded only (see Config)
        }
        if (values.contains("order")) {
            cfg.order = values["order"].get<int>();
            if (cfg.order < 1) {
                cfg.order = 1;
            }
        }
        if (values.contains("element_tendency")) {
            cfg.element_tendency = values["element_tendency"].get<double>();
        }
        if (values.contains("skin_layers")) {
            cfg.skin_layers = values["skin_layers"].get<int>();
            if (cfg.skin_layers < 1) {
                throw std::runtime_error("grid.skin_layers values must be >= 1");
            }
        }
        if (values.contains("adapt_passes")) {
            cfg.adapt_passes = values["adapt_passes"].get<int>();
            if (cfg.adapt_passes < 0) {
                throw std::runtime_error("grid.adapt_passes values must be >= 0");
            }
        }
        if (values.contains("eta_target")) {
            cfg.eta_target = values["eta_target"].get<double>();
            if (!(cfg.eta_target >= 0.0) || !std::isfinite(cfg.eta_target)) {
                throw std::runtime_error("grid.eta_target values must be finite and >= 0");
            }
        }
        if (values.contains("p_elevate")) {
            cfg.p_elevate = values["p_elevate"].get<bool>();
        }
        if (values.contains("adapt_leb_waves")) {
            cfg.adapt_leb_waves = values["adapt_leb_waves"].get<int>();
            if (cfg.adapt_leb_waves < 1 || cfg.adapt_leb_waves > 4) {
                throw std::runtime_error("grid.adapt_leb_waves values must be in [1,4]");
            }
        }
        if (values.contains("cost_only")) {
            cfg.cost_only = values["cost_only"].get<bool>();
        }
        if (values.contains("h_rel")) {
            const double value = values["h_rel"].get<double>();
            if (!(value > 0.0) || !std::isfinite(value)) {
                throw std::runtime_error("grid.h_rel values must be finite and > 0");
            }
            cfg.h_rel = value;
        }
        if (cfg.cost_only && cfg.adapt_passes > 0) {
            throw std::runtime_error(
                "grid.cost_only=true requires adapt_passes=0 because adaptation needs ZZ");
        }
        out.push_back(std::move(cfg));

        // Odometer increment.
        std::size_t k = 0;
        for (; k < idx.size(); ++k) {
            ++idx[k];
            if (idx[k] < axes[k].size()) {
                break;
            }
            idx[k] = 0;
        }
        if (k == idx.size()) {
            break;
        }
    }
    // Dedup by id (identical value sets).
    std::map<std::string, Config> uniq;
    for (auto& c : out) {
        uniq.emplace(c.id, std::move(c));
    }
    out.clear();
    for (auto& [id, c] : uniq) {
        (void)id;
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(),
              [](const Config& a, const Config& b) { return a.id < b.id; });
    return out;
}

// ── checkpoint ──────────────────────────────────────────────────────────────

Checkpoint load_checkpoint(const fs::path& path) {
    const json j = json::parse(read_file(path));
    Checkpoint cp;
    cp.campaign = j.at("campaign").get<std::string>();
    cp.state = j.at("state").get<std::string>();
    cp.tier = j.value("tier", 0);
    cp.completed_runs = j.value("completed_runs", 0);
    if (j.contains("survivors")) {
        for (const auto& s : j["survivors"]) {
            cp.survivors.push_back(s.get<std::string>());
        }
    }
    cp.started_utc = j.value("started_utc", utc_now());
    cp.updated_utc = j.value("updated_utc", utc_now());
    if (j.contains("hooks_failed")) {
        for (const auto& h : j["hooks_failed"]) {
            cp.hooks_failed.push_back(h.get<std::string>());
        }
    }
    return cp;
}

void write_checkpoint(const fs::path& path, const Checkpoint& cp) {
    json j;
    j["campaign"] = cp.campaign;
    j["state"] = cp.state;
    j["tier"] = cp.tier;
    j["completed_runs"] = cp.completed_runs;
    j["survivors"] = cp.survivors;
    j["started_utc"] = cp.started_utc;
    j["updated_utc"] = utc_now();
    if (!cp.hooks_failed.empty()) {
        j["hooks_failed"] = cp.hooks_failed;
    }
    atomic_write(path, j.dump(2) + "\n");
}

} // namespace polymesh::testlab::detail
