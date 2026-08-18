// SPDX-License-Identifier: BSD-3-Clause

// polymesh_testlab — campaign runner with successive-halving, SIGINT pause,
// and atomic checkpointing. Normative schemas: docs/dag/interfaces.md.
//
// Anti-cheat: reference truths are loaded only from paths declared in case
// files (bench/reference/*). No numeric answers are embedded here.

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif
#include "fea/assembly.hpp"
#include "fea/backend.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/material.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/cad_topology.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "mesh/surface_metrics.hpp"
#include "pipeline/scene.hpp"
#include "load_area.hpp"
#include "probe_util.hpp"
#include "run_artifacts.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/SparseCore>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace fea = polymesh::fea;
namespace pipeline = polymesh::pipeline;
namespace mesh = polymesh::mesh;
namespace geom = polymesh::geom;
namespace tlab = polymesh::testlab;

namespace {

// ── SIGINT → pause after the current run finishes ───────────────────────────

std::atomic<bool> g_pause_requested{false};

void on_sigint(int /*sig*/) {
    g_pause_requested.store(true, std::memory_order_relaxed);
    // Restore default so a second SIGINT aborts hard (user impatience).
    std::signal(SIGINT, SIG_DFL);
}

// ── time / atomic I/O ───────────────────────────────────────────────────────

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

using polymesh::testlab::atomic_write;

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

// ── campaign / case / reference models ──────────────────────────────────────

struct TierSpec {
    double h_scale = 1.0;
    double keep_frac = 1.0;
};

struct Campaign {
    std::string name;
    std::vector<std::string> parts; // case file paths (repo-relative)
    std::vector<TierSpec> tiers;
    json grid; // object of arrays
    double w_accuracy = 0.5;
    double w_solve_ms = 0.25;
    double w_mesh_ms = 0.25;
    bool warehouse = false; // ADR-0022 full experiment warehouse
    bool on_finish_analyze = false;
    bool on_finish_grok = false;
    /// M14: per-run wall-clock (s). 0 → tier defaults 900 / 900 / 2700.
    double max_run_wall_s = 0.0;
    /// M14: pack-level ceiling (s). 0 = unlimited (do not start new runs past it).
    double max_pack_wall_s = 0.0;
    /// Post-mesh DOF / element ceilings for a run. 0 → the throughput defaults
    /// below. A reference campaign legitimately wants a bigger budget than a
    /// throughput sweep: the whole point of an overkill solve is to be larger
    /// than anything the training grid produces.
    long long max_dof = 0;
    long long max_elems = 0;
};

struct Box3 {
    Eigen::Vector3d lo = Eigen::Vector3d::Constant(-1e300);
    Eigen::Vector3d hi = Eigen::Vector3d::Constant(1e300);
    bool contains(const Eigen::Vector3d& p) const {
        return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1] &&
               p[2] >= lo[2] && p[2] <= hi[2];
    }
};

struct BcSpec {
    Box3 box;
    std::array<bool, 3> fix{{true, true, true}};
    std::vector<std::uint32_t> cad_face_ids;
};

struct LoadSpec {
    Box3 box;
    Eigen::Vector3d traction = Eigen::Vector3d::Zero(); // N/m^2
    /// Optional guard (m²): selected face area must match within ±2% (advisor Q7).
    std::optional<double> expected_area;
    /// Keep faces whose unit normal aligns with traction (n·t̂ > min_dot).
    /// Default 0.7 when traction is nonzero; ignored when traction ≈ 0.
    double normal_min_dot = 0.7;
    // Exact trimmed BRep faces represented by this selector, resolved from the
    // CAD model once per run. cad_face_area is the sum of the WHOLE trimmed areas
    // of those faces; it drives the face-replacement fallback, but it is NOT a
    // verification target because the selector box may cover only part of a face.
    std::vector<std::uint32_t> cad_face_ids;
    std::optional<double> cad_face_area;
    /// Area of the loaded region obtained by applying THIS case's own selection
    /// rule (load box + |n·t̂| > normal_min_dot) to the exact CAD tessellation.
    /// Being the continuum limit of the same rule, it is mesh-independent and is
    /// both the rescale target for the traction and the basis for the reported
    /// fidelity deficit. Empty when there is no CAD to measure against.
    std::optional<double> cad_rule_area;
};

struct ProbeSpec {
    // Scoring kinds: mean_vm_over_nominal | tip_deflection | strain_energy | ...
    // Diagnostic-only (not preferred for score): max_von_mises, max_vm_over_nominal
    std::string kind;
    double nominal = 0.0;
    /// Optional spatial select for face-mean stress (hole patch, tip face, …).
    std::optional<Box3> select;
};

struct MetricSpec {
    std::string name;
    double value = 0.0;
    double tol = 0.05;
    ProbeSpec probe;
    std::string derivation;
};

struct PartCase {
    std::string part;
    std::string geometry; // path
    double E = 200e9;
    double nu = 0.3;
    double rho = 7850;
    std::vector<BcSpec> bcs;
    std::vector<LoadSpec> loads;
    std::string reference_path;
    std::vector<MetricSpec> metrics; // filled after loading reference
};

struct Config {
    std::string id;
    json values; // original grid values
    pipeline::VolumeMesher mesher = pipeline::VolumeMesher::kHybrid;
    bool feature_refine = true;
    double curvature_turn_deg = 15.0; // recorded; product path uses 15° today
    bool snap_boundary = true;
    int order = 1;
    double element_tendency = 0.0;
    /// A-priori geometry+BC grading (ADR-0021): refine toward BC/load boxes and
    /// geometry features before the solve. OFF by default so frozen campaign
    /// baselines are unchanged; a campaign opts in via `"bc_grading": true`.
    bool bc_grading = false;
    /// Spectral sizing (ADR-0034): FFT-denoise CAD-edge curvature sources and
    /// energy-truncate the fused size field. OFF by default so frozen campaign
    /// baselines are unchanged; a campaign opts in via `"spectral_smooth": true`.
    bool spectral_smooth = false;
    int skin_layers = 2;
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = false;
    int adapt_leb_waves = 2;
    std::optional<double> h_rel;
};

struct Checkpoint {
    std::string campaign;
    std::string state = "running"; // running | paused | finished
    int tier = 0;
    int completed_runs = 0;
    std::vector<std::string> survivors;
    std::string started_utc;
    std::string updated_utc;
    /// Post-campaign hooks that failed, empty when all ran (or none were asked
    /// for). Recorded beside `state` because `state == "finished"` is what a
    /// consumer polls to conclude success, and three post-processing steps failed
    /// silently behind exactly that marker for an entire regeneration.
    std::vector<std::string> hooks_failed;
};

// ── parsers ─────────────────────────────────────────────────────────────────

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

Campaign load_campaign(const fs::path& path) {
    const json j = json::parse(read_file(path));
    Campaign c;
    c.name = j.at("name").get<std::string>();
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
        "mesher",          "feature_refine", "order",          "element_tendency",
        "bc_grading",      "curvature_turn_deg", "snap_boundary", "skin_layers",
        "adapt_passes",    "eta_target",      "p_elevate",     "adapt_leb_waves",
        "spectral_smooth", "h_rel"};
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
        c.on_finish_grok = j["on_finish"].value("grok_handoff", false);
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

/// Thrown from solve progress (or after mesh) when max_run_wall_s is exceeded.
struct WallClockBudgetExceeded : std::runtime_error {
    explicit WallClockBudgetExceeded(double elapsed_s)
        : std::runtime_error("wall-clock budget exceeded (" + std::to_string(elapsed_s) +
                             " s)"),
          elapsed_s(elapsed_s) {}
    double elapsed_s = 0.0;
};

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
            // ADR-0023 and docs/validation/hand-calcs.md both say raw nodal
            // sigma_vm_max is diagnostic only and prohibited as a score, and
            // evaluate_probe carries the same comment — but nothing enforced it,
            // so two fixture references kept scoring it. Measured consequence:
            // smoke_bar's sigma_max rises with resolution (Spearman(DOF,
            // rel_err) = +0.70; order 2 medians 2.7 decades of "error" against a
            // 1 MPa uniform-bar hand-calc, peaking at 12.05) because a fully
            // clamped end is a stress singularity the metric resolves better the
            // finer the mesh. Every advisor trained on those rows learned that
            // refining is catastrophic, and that one case dominated the
            // corpus-wide macro-mean regret headline (docs/advisor/0008 S4).
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

pipeline::VolumeMesher parse_mesher(const std::string& name) {
    if (name == "hex") {
        return pipeline::VolumeMesher::kHexFill;
    }
    if (name == "tet" || name == "tet_fill") {
        return pipeline::VolumeMesher::kTetFill;
    }
    if (name == "graded_tet" || name == "graded") {
        return pipeline::VolumeMesher::kGradedTet;
    }
    if (name == "hybrid_zoo" || name == "hybrid" || name == "zoo") {
        return pipeline::VolumeMesher::kHybrid;
    }
    if (name == "hybrid_vem" || name == "hybridvem" || name == "hybrid-vem") {
        return pipeline::VolumeMesher::kHybridVem;
    }
    if (name == "hexpyr" || name == "transition") {
        return pipeline::VolumeMesher::kHexPyramid;
    }
    if (name == "prism" || name == "sweep") {
        return pipeline::VolumeMesher::kPrismSweep;
    }
    if (name == "hexvem" || name == "vem") {
        return pipeline::VolumeMesher::kHexVem;
    }
    if (name == "octa" || name == "octahedral") {
        return pipeline::VolumeMesher::kOctahedral;
    }
    if (name == "varyhedron" || name == "vary") {
        return pipeline::VolumeMesher::kVaryhedron;
    }
    if (name == "cvt_poly" || name == "cvt" || name == "restricted_cvt") {
        return pipeline::VolumeMesher::kCvtPoly;
    }
    throw std::runtime_error("unknown mesher '" + name + "'");
}

std::string mesher_name(pipeline::VolumeMesher mesher) {
    switch (mesher) {
    case pipeline::VolumeMesher::kTetFill:
        return "tet";
    case pipeline::VolumeMesher::kHexFill:
        return "hex";
    case pipeline::VolumeMesher::kHexVem:
        return "hex_vem";
    case pipeline::VolumeMesher::kGradedTet:
        return "graded_tet";
    case pipeline::VolumeMesher::kHexPyramid:
        return "hexpyr";
    case pipeline::VolumeMesher::kPrismSweep:
        return "prism";
    case pipeline::VolumeMesher::kHybrid:
        return "hybrid_zoo";
    case pipeline::VolumeMesher::kOctahedral:
        return "octa";
    case pipeline::VolumeMesher::kHybridVem:
        return "hybrid_vem";
    case pipeline::VolumeMesher::kVaryhedron:
        return "varyhedron";
    case pipeline::VolumeMesher::kCvtPoly:
        return "cvt_poly";
    }
    return "unknown";
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
            (void)cfg.curvature_turn_deg; // product volume_mesh uses fixed 15° today
        }
        if (values.contains("snap_boundary")) {
            cfg.snap_boundary = values["snap_boundary"].get<bool>();
            (void)cfg.snap_boundary; // product path always snaps; recorded for feedback
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
        if (values.contains("h_rel")) {
            const double value = values["h_rel"].get<double>();
            if (!(value > 0.0) || !std::isfinite(value)) {
                throw std::runtime_error("grid.h_rel values must be finite and > 0");
            }
            cfg.h_rel = value;
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

void write_progress(const fs::path& path, const std::string& phase, double phase_frac,
                    double elapsed_ms, const std::string& cfg_id, const std::string& part,
                    int tier, int cg_iter = -1, double cg_resid = -1.0,
                    std::size_t n_elems = 0, std::size_t n_nodes = 0,
                    const std::vector<std::string>& hooks_failed = {}) {
    json j;
    j["phase"] = phase;
    j["phase_frac"] = phase_frac;
    j["elapsed_ms"] = elapsed_ms;
    if (cg_iter >= 0) {
        j["cg_iter"] = cg_iter;
        j["cg_resid"] = cg_resid;
    } else {
        j["cg_iter"] = nullptr;
        j["cg_resid"] = nullptr;
    }
    if (n_elems > 0) {
        j["n_elems"] = n_elems;
        j["n_nodes"] = n_nodes;
    }
    j["run"] = {{"cfg_id", cfg_id}, {"part", part}, {"tier", tier}};
    if (!hooks_failed.empty()) {
        // Only present when a post-campaign hook failed, so a reader that polls
        // this file sees the failure rather than having to read run.log.
        j["hooks_failed"] = hooks_failed;
    }
    atomic_write(path, j.dump(2) + "\n");
}

/// Background progress.json heartbeats so the GUI "live progress" box moves
/// during long mesh/assemble/solve stretches (interfaces.md §6 ~500 ms).
class ProgressHeartbeat {
  public:
    ProgressHeartbeat(fs::path path, std::string phase, std::string cfg_id, std::string part,
                      int tier, std::chrono::steady_clock::time_point t0)
        : path_(std::move(path)), phase_(std::move(phase)), cfg_id_(std::move(cfg_id)),
          part_(std::move(part)), tier_(tier), t0_(t0), stop_(false) {
        thr_ = std::thread([this] { loop(); });
    }
    ProgressHeartbeat(const ProgressHeartbeat&) = delete;
    ProgressHeartbeat& operator=(const ProgressHeartbeat&) = delete;

    void set_phase(std::string phase, double phase_frac = 0.0) {
        {
            const std::lock_guard lock(mu_);
            phase_ = std::move(phase);
        }
        phase_frac_.store(phase_frac, std::memory_order_relaxed);
        tick_now();
    }
    void set_frac(double f) { phase_frac_.store(f, std::memory_order_relaxed); }
    void set_cg(int iter, double resid) {
        cg_iter_.store(iter, std::memory_order_relaxed);
        cg_resid_.store(resid, std::memory_order_relaxed);
    }
    void set_mesh_stats(std::size_t n_elems, std::size_t n_nodes) {
        n_elems_.store(n_elems, std::memory_order_relaxed);
        n_nodes_.store(n_nodes, std::memory_order_relaxed);
    }

    ~ProgressHeartbeat() {
        stop_.store(true, std::memory_order_relaxed);
        if (thr_.joinable()) {
            thr_.join();
        }
    }

  private:
    void tick_now() {
        try {
            std::string phase;
            {
                const std::lock_guard lock(mu_);
                phase = phase_;
            }
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0_)
                                .count();
            const int cg = cg_iter_.load(std::memory_order_relaxed);
            write_progress(path_, phase, phase_frac_.load(std::memory_order_relaxed), ms,
                           cfg_id_, part_, tier_, cg,
                           cg_resid_.load(std::memory_order_relaxed),
                           n_elems_.load(std::memory_order_relaxed),
                           n_nodes_.load(std::memory_order_relaxed));
        } catch (...) {
            // Progress is best-effort; never fail the run for a status write.
        }
    }
    void loop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            tick_now();
            for (int i = 0; i < 10 && !stop_.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        tick_now(); // final stamp
    }

    fs::path path_;
    std::string phase_;
    std::string cfg_id_;
    std::string part_;
    int tier_ = 0;
    std::chrono::steady_clock::time_point t0_{};
    std::mutex mu_;
    std::atomic<bool> stop_;
    std::atomic<double> phase_frac_{0.0};
    std::atomic<int> cg_iter_{-1};
    std::atomic<double> cg_resid_{-1.0};
    std::atomic<std::size_t> n_elems_{0};
    std::atomic<std::size_t> n_nodes_{0};
    std::thread thr_;
};

/// Lightweight boundary mesh for GUI live viewport (campaign dir).
/// Magic "PMP1": nodes float32 xyz, quads uint32×4. No full element dump.
void write_mesh_preview(const fs::path& path, const pipeline::VolumeMeshOutput& vol) {
    const auto n_nodes = static_cast<std::uint32_t>(vol.mesh.nodes.size());
    const auto n_quads = static_cast<std::uint32_t>(vol.boundary_quads.size());
    const auto n_elems = static_cast<std::uint64_t>(vol.mesh.elements.size());
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        const char magic[4] = {'P', 'M', 'P', '1'};
        out.write(magic, 4);
        out.write(reinterpret_cast<const char*>(&n_nodes), sizeof(n_nodes));
        out.write(reinterpret_cast<const char*>(&n_quads), sizeof(n_quads));
        out.write(reinterpret_cast<const char*>(&n_elems), sizeof(n_elems));
        for (const auto& p : vol.mesh.nodes) {
            const float xyz[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]),
                                  static_cast<float>(p[2])};
            out.write(reinterpret_cast<const char*>(xyz), sizeof(xyz));
        }
        for (const auto& q : vol.boundary_quads) {
            const std::uint32_t ids[4] = {q[0], q[1], q[2], q[3]};
            out.write(reinterpret_cast<const char*>(ids), sizeof(ids));
        }
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

// ── geometry helpers for BC / loads / probes ────────────────────────────────
std::vector<fea::SurfaceFace>
select_exact_cad_load_faces(const fea::NodalMesh& mesh, const geom::CadModel& cad, double h,
                            std::span<const std::uint32_t> cad_face_ids);


fea::Dirichlet make_dirichlet(const fea::NodalMesh& mesh, const std::vector<BcSpec>& bcs,
                              const geom::CadModel* cad, double h) {
    // Node-in-box plus free-face centroid-in-box (surface snap can pull end-face
    // nodes slightly off the CAD plane so pure node-in-box misses fixtures).
    fea::Dirichlet bc;
    auto fix_node = [&](std::uint32_t i, const BcSpec& b) {
        for (int a = 0; a < 3; ++a) {
            if (b.fix[static_cast<std::size_t>(a)]) {
                bc.dof_values[3 * static_cast<Eigen::Index>(i) + a] = 0.0;
            }
        }
    };
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
        const Eigen::Vector3d& p = mesh.nodes[i];
        for (const auto& b : bcs) {
            if (b.box.contains(p)) {
                fix_node(i, b);
            }
        }
    }
    const auto faces = fea::extract_boundary_faces(mesh);
    for (const auto& q : faces) {
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            // Degenerate tri quads use q[2]==q[3].
            if (k == 3 && q[2] == q[3]) {
                break;
            }
            c += mesh.nodes[q[static_cast<std::size_t>(k)]];
            ++n;
        }
        if (n <= 0) {
            continue;
        }
        c /= static_cast<double>(n);
        for (const auto& b : bcs) {
            if (!b.box.contains(c)) {
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                if (k == 3 && q[2] == q[3]) {
                    break;
                }
                fix_node(q[static_cast<std::size_t>(k)], b);
            }
        }
    }
    // Fallback: if still under-constrained, pin every node inside each BC box
    // expanded by 2% of mesh bbox diagonal (coarse graded meshes often leave
    // the CAD face plane by a fraction of h after snap).
    if (bc.dof_values.size() < 9 && !mesh.nodes.empty()) {
        Eigen::Vector3d lo = mesh.nodes.front();
        Eigen::Vector3d hi = lo;
        for (const auto& p : mesh.nodes) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        const double pad = 0.02 * std::max((hi - lo).norm(), 1e-9);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
            const Eigen::Vector3d& p = mesh.nodes[i];
            for (const auto& b : bcs) {
                Box3 exp = b.box;
                exp.lo -= Eigen::Vector3d::Constant(pad);
                exp.hi += Eigen::Vector3d::Constant(pad);
                if (exp.contains(p)) {
                    fix_node(i, b);
                }
            }
        }
    }
    // Exact-support fallback is deliberately gated on total legacy failure:
    // every currently passing fixture set remains byte-for-byte unchanged.
    if (bc.dof_values.empty() && cad != nullptr) {
        for (const auto& b : bcs) {
            for (const auto& face :
                 select_exact_cad_load_faces(mesh, *cad, h, b.cad_face_ids)) {
                for (const auto node : face.nodes) {
                    fix_node(node, b);
                }
            }
        }
    }
    return bc;
}

std::vector<fea::SurfaceFace> free_faces_as_surface(const fea::NodalMesh& mesh) {
    const auto quads = fea::extract_boundary_faces(mesh);
    std::vector<fea::SurfaceFace> faces;
    faces.reserve(quads.size());
    for (const auto& q : quads) {
        fea::SurfaceFace f;
        if (q[2] == q[3]) {
            f.type = fea::FaceType::kTri3;
            f.nodes = {q[0], q[1], q[2]};
        } else {
            f.type = fea::FaceType::kQuad4;
            f.nodes = {q[0], q[1], q[2], q[3]};
        }
        faces.push_back(std::move(f));
    }
    return faces;
}

Eigen::Vector3d face_centroid(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (auto n : f.nodes) {
        c += mesh.nodes[n];
    }
    return c / static_cast<double>(f.nodes.size());
}

/// Unit normal from first triangle of a surface face (right-hand order of nodes).
/// Returns zero if the face is degenerate.
Eigen::Vector3d face_unit_normal(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    if (f.nodes.size() < 3) {
        return Eigen::Vector3d::Zero();
    }
    const Eigen::Vector3d& p0 = mesh.nodes[f.nodes[0]];
    const Eigen::Vector3d& p1 = mesh.nodes[f.nodes[1]];
    const Eigen::Vector3d& p2 = mesh.nodes[f.nodes[2]];
    Eigen::Vector3d n = (p1 - p0).cross(p2 - p0);
    const double nn = n.norm();
    if (!(nn > 1e-30)) {
        return Eigen::Vector3d::Zero();
    }
    return n / nn;
}

/// Boundary faces in `box`, optionally filtered so n·t̂ > min_dot when traction
/// is nonzero. Falls back to box-only if the normal filter empties the set.
double legacy_surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& f);
double surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& face);

std::vector<fea::SurfaceFace>
select_load_faces(const fea::NodalMesh& mesh, const Box3& box, const Eigen::Vector3d& traction,
                  double normal_min_dot, std::optional<double> expected_area = std::nullopt) {
    const auto all_faces = free_faces_as_surface(mesh);
    std::vector<fea::SurfaceFace> in_box;
    in_box.reserve(all_faces.size());
    for (const auto& face : all_faces) {
        if (box.contains(face_centroid(mesh, face))) {
            in_box.push_back(face);
        }
    }
    const double tnorm = traction.norm();
    const Eigen::Vector3d t_hat =
        tnorm > 1e-30 ? Eigen::Vector3d(traction / tnorm) : Eigen::Vector3d::Zero();
    if (in_box.empty() || !tlab::load_rule_filters(normal_min_dot, traction)) {
        return in_box;
    }
    std::vector<fea::SurfaceFace> filtered;
    filtered.reserve(in_box.size());
    // The normal test itself lives in tlab::load_rule_keeps_normal so the CAD-side
    // rule area cannot drift from it. |n·t̂| there tolerates inverted winding on
    // mixed/hex skins; box-only fallback below if nothing survives.
    for (const auto& face : in_box) {
        const Eigen::Vector3d n = face_unit_normal(mesh, face);
        if (n.norm() < 0.5) {
            continue; // degenerate
        }
        if (tlab::load_rule_keeps_normal(normal_min_dot, traction, n)) {
            filtered.push_back(face);
        }
    }
    if (filtered.empty()) {
        filtered = in_box;
    }
    // When CAD expected_area is known and the mesh free-skin overshoots (RVD
    // jagged tip, dual interfaces), keep the faces most aligned with traction
    // and farthest along t̂ until cumulative area is closest to expected.
    // Traction magnitude is still applied per-face; this only prunes which
    // free faces carry the load (same total force if area matches CAD).
    if (expected_area && *expected_area > 0.0 && filtered.size() > 1) {
        double total = 0.0;
        for (const auto& f : filtered) {
            total += legacy_surface_face_area(mesh, f);
        }
        const double exp = *expected_area;
        if (total > 1.05 * exp) {
            struct Ranked {
                fea::SurfaceFace face;
                double score = 0.0;
                double area = 0.0;
            };
            std::vector<Ranked> ranked;
            ranked.reserve(filtered.size());
            for (const auto& face : filtered) {
                const Eigen::Vector3d c = face_centroid(mesh, face);
                const Eigen::Vector3d n = face_unit_normal(mesh, face);
                Ranked r;
                r.face = face;
                r.area = legacy_surface_face_area(mesh, face);
                // Prefer outer tip: large c·t̂ and strong normal alignment.
                r.score = c.dot(t_hat) + 0.1 * std::abs(n.dot(t_hat));
                ranked.push_back(std::move(r));
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            std::vector<fea::SurfaceFace> kept;
            double acc = 0.0;
            double best_err = 1e300;
            std::size_t best_n = 0;
            for (std::size_t i = 0; i < ranked.size(); ++i) {
                acc += ranked[i].area;
                kept.push_back(ranked[i].face);
                const double err = std::abs(acc - exp);
                if (err < best_err) {
                    best_err = err;
                    best_n = kept.size();
                }
                // Stop once we are past expected and error is growing.
                if (acc > exp && err > best_err) {
                    break;
                }
            }
            if (best_n > 0 && best_n <= kept.size()) {
                kept.resize(best_n);
                return kept;
            }
        }
    }
    return filtered;
}

struct ResolvedLoadFaces {
    std::vector<fea::SurfaceFace> faces;
    /// Area the traction is applied over. In the exact-CAD fallback this is
    /// REPLACED by the exact CAD area, because the traction is rescaled so the
    /// resultant matches the true face; it is then a statement of intent, not a
    /// measurement, and comparing it against the CAD area is vacuous.
    double reported_area = 0.0;
    /// Area the MESH actually selected, always measured, never substituted. This
    /// is the only value that can verify mesh fidelity against the CAD.
    double mesh_selected_area = 0.0;
    double traction_scale = 1.0;
    bool used_exact_fallback = false;
};

std::vector<fea::SurfaceFace>
select_exact_cad_load_faces(const fea::NodalMesh& mesh, const geom::CadModel& cad, double h,
                            std::span<const std::uint32_t> cad_face_ids) {
    if (cad_face_ids.empty()) {
        return {};
    }
    std::set<std::uint32_t> selected_ids(cad_face_ids.begin(), cad_face_ids.end());
    std::vector<mesh::BoundarySupport> provenance;
    mesh::BoundaryProjectionContext projection;
    if (!pipeline::make_boundary_projection(cad, h, &projection, &provenance)) {
        return {};
    }

    const auto all_faces = fea::boundary_surface_faces(mesh);
    provenance.resize(mesh.nodes.size());
    std::set<std::uint32_t> boundary_nodes;
    for (const auto& face : all_faces) {
        boundary_nodes.insert(face.nodes.begin(), face.nodes.end());
    }
    for (const auto node : boundary_nodes) {
        mesh::BoundarySupport owner;
        (void)projection.target(mesh.nodes[node], owner);
        provenance[node] = owner;
    }

    std::vector<fea::SurfaceFace> selected;
    selected.reserve(all_faces.size());
    for (const auto& face : all_faces) {
        std::size_t selected_votes = 0;
        std::size_t other_face_votes = 0;
        for (const auto node : face.nodes) {
            if (node >= provenance.size() ||
                provenance[node].kind != mesh::BoundarySupportKind::kCadFace) {
                continue;
            }
            if (selected_ids.contains(provenance[node].id)) {
                ++selected_votes;
            } else {
                ++other_face_votes;
            }
        }
        bool keep = selected_votes > 0 && selected_votes >= other_face_votes;
        if (!keep) {
            const auto exact = geom::project_point_on_surface(cad, face_centroid(mesh, face));
            keep = exact && exact->face_id != geom::kInvalidCadSupportId &&
                   selected_ids.contains(exact->face_id);
        }
        if (keep) {
            selected.push_back(face);
        }
    }
    return selected;
}

std::vector<ResolvedLoadFaces>
resolve_load_faces(const fea::NodalMesh& mesh, const geom::CadModel* cad, double h,
                   const std::vector<LoadSpec>& loads) {
    std::vector<ResolvedLoadFaces> out;
    out.reserve(loads.size());
    for (const auto& load : loads) {
        ResolvedLoadFaces resolved;
        resolved.faces = select_load_faces(mesh, load.box, load.traction, load.normal_min_dot,
                                           load.expected_area);
        for (const auto& face : resolved.faces) {
            resolved.mesh_selected_area += legacy_surface_face_area(mesh, face);
        }

        bool legacy_failed_area_gate = false;
        if (load.expected_area && *load.expected_area > 0.0) {
            legacy_failed_area_gate =
                std::abs(resolved.mesh_selected_area - *load.expected_area) /
                    *load.expected_area >
                0.05;
        }
        if ((resolved.faces.empty() || legacy_failed_area_gate) && cad != nullptr &&
            load.cad_face_area && *load.cad_face_area > 0.0 &&
            !load.cad_face_ids.empty()) {
            // Face REPLACEMENT: the box selection found nothing usable, so take the
            // faces the exact CAD ids resolve to instead.
            auto exact_faces =
                select_exact_cad_load_faces(mesh, *cad, h, load.cad_face_ids);
            double mesh_area = 0.0;
            for (const auto& face : exact_faces) {
                mesh_area += surface_face_area(mesh, face);
            }
            if (!exact_faces.empty() && mesh_area > 0.0) {
                resolved.faces = std::move(exact_faces);
                resolved.mesh_selected_area = mesh_area;
                resolved.used_exact_fallback = true;
            }
        }

        // Resultant-preserving rescale. Whichever face set we ended up with, the
        // applied resultant is made equal to traction x cad_rule_area, so it no
        // longer depends on how well this mesh happened to resolve a curved loaded
        // surface. A coarse mesh then solves the RIGHT problem badly instead of the
        // WRONG problem, and mesh_selected_area keeps the measured deficit so the
        // remaining distribution error stays visible in the row.
        if (load.cad_rule_area && *load.cad_rule_area > 0.0 &&
            resolved.mesh_selected_area > 0.0) {
            resolved.reported_area = *load.cad_rule_area;
            resolved.traction_scale = *load.cad_rule_area / resolved.mesh_selected_area;
        } else {
            resolved.reported_area = resolved.mesh_selected_area;
        }
        out.push_back(std::move(resolved));
    }
    return out;
}

Eigen::VectorXd make_loads(const fea::NodalMesh& mesh, const std::vector<LoadSpec>& loads,
                           const std::vector<ResolvedLoadFaces>& resolved_loads) {
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (std::size_t load_index = 0; load_index < loads.size(); ++load_index) {
        const auto& L = loads[load_index];
        const auto& resolved = resolved_loads[load_index];
        const auto& selected = resolved.faces;
        if (selected.empty()) {
            // Preserve the legacy node-lump fallback when neither selector can
            // identify a boundary face.
            std::vector<std::uint32_t> nodes;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
                if (L.box.contains(mesh.nodes[i])) {
                    nodes.push_back(i);
                }
            }
            if (nodes.empty()) {
                continue;
            }
            Eigen::Vector3d lo = mesh.nodes[nodes.front()];
            Eigen::Vector3d hi = lo;
            for (auto n : nodes) {
                lo = lo.cwiseMin(mesh.nodes[n]);
                hi = hi.cwiseMax(mesh.nodes[n]);
            }
            const Eigen::Vector3d ext = (hi - lo).cwiseMax(1e-30);
            const double area =
                ext[0] * ext[1] * ext[2] / std::max({ext[0], ext[1], ext[2], 1e-30});
            const Eigen::Vector3d per =
                L.traction * area / static_cast<double>(nodes.size());
            for (auto n : nodes) {
                f.segment<3>(3 * static_cast<Eigen::Index>(n)) += per;
            }
            continue;
        }
        const Eigen::Vector3d t = resolved.traction_scale * L.traction;
        f += fea::assemble_traction_load(mesh, selected,
                                         [&](const Eigen::Vector3d&) { return t; });
    }
    return f;
}


void hash_mix(std::uint64_t& hash, std::uint64_t value) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xffu;
        hash *= kPrime;
    }
}

std::uint64_t selected_face_set_hash(const std::vector<ResolvedLoadFaces>& selections) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t load_index = 0; load_index < selections.size(); ++load_index) {
        hash_mix(hash, load_index);
        std::vector<std::vector<std::uint32_t>> faces;
        faces.reserve(selections[load_index].faces.size());
        for (const auto& face : selections[load_index].faces) {
            auto nodes = face.nodes;
            std::sort(nodes.begin(), nodes.end());
            faces.push_back(std::move(nodes));
        }
        std::sort(faces.begin(), faces.end());
        hash_mix(hash, faces.size());
        for (const auto& face : faces) {
            hash_mix(hash, face.size());
            for (const auto node : face) {
                hash_mix(hash, node);
            }
        }
    }
    return hash;
}

std::uint64_t selected_node_set_hash(const std::vector<ResolvedLoadFaces>& selections) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t load_index = 0; load_index < selections.size(); ++load_index) {
        hash_mix(hash, load_index);
        std::set<std::uint32_t> nodes;
        for (const auto& face : selections[load_index].faces) {
            nodes.insert(face.nodes.begin(), face.nodes.end());
        }
        hash_mix(hash, nodes.size());
        for (const auto node : nodes) {
            hash_mix(hash, node);
        }
    }
    return hash;
}

std::uint64_t load_vector_hash(const Eigen::VectorXd& loads) {
    std::uint64_t hash = 1469598103934665603ull;
    hash_mix(hash, static_cast<std::uint64_t>(loads.size()));
    for (Eigen::Index i = 0; i < loads.size(); ++i) {
        hash_mix(hash, std::bit_cast<std::uint64_t>(loads[i]));
    }
    return hash;
}

std::uint64_t dirichlet_node_set_hash(const fea::Dirichlet& bc) {
    std::set<std::uint64_t> nodes;
    for (const auto& [dof, value] : bc.dof_values) {
        (void)value;
        nodes.insert(static_cast<std::uint64_t>(dof / 3));
    }
    std::uint64_t hash = 1469598103934665603ull;
    hash_mix(hash, nodes.size());
    for (const auto node : nodes) {
        hash_mix(hash, node);
    }
    return hash;
}

struct ProbeAnswers {
    double sigma_max = 0.0;          // DIAGNOSTIC only: global nodal max VM
    double sigma_face_mean = 0.0;    // PRIMARY stress: area-wtd face-region VM (centroid)
    double sigma_box_max = 0.0;      // box-selected peak element-centroid VM
    double sigma_p99 = 0.0;          // DIAGNOSTIC: p99 element-centroid VM, quality-filtered
    double strain_energy = 0.0;      // 1/2 u^T K u (J)
    double tip_deflection = 0.0;     // face-mean |u| on tip/load faces
    double tip_deflection_max = 0.0; // diagnostic: global max |u|
    double mean_u_component = 0.0;   // signed mean of dominant load-dir component
    double mean_ux = 0.0;
    double mean_uz = 0.0;
    int dominant_load_axis = 2; // 0=x,1=y,2=z
    double reaction_sum_err = 0.0;
    double free_residual_rel = 0.0;
    int n_orphan_nodes = 0;
    int n_bc_dofs = 0;
    int n_load_faces = 0;
    int n_probe_nodes = 0;
    int n_quality_excluded = 0; // elements below quality floor for p99
    double load_face_area = 0.0;
    /// Area the mesh actually selected, never substituted (see ResolvedLoadFaces).
    double mesh_selected_area = 0.0;
    /// |A_mesh - A_expected|/A_expected. EMPTY when no expected area could be
    /// established: a gate that cannot verify anything must not report 0.0, which
    /// reads as a perfect match and manufactured false confidence in exactly the
    /// runs that were most wrong (a mesh missing 28% of its loaded face recorded
    /// load_area_rel_err = 0.0 and passed).
    std::optional<double> load_area_rel_err;
    tlab::LoadAreaStatus load_area_status = tlab::LoadAreaStatus::kUnverified;
    /// Mesh-INDEPENDENT case-definition cross-check: authored expected_area vs the
    /// CAD-rule area. Kept separate from load_area_rel_err because a disagreement
    /// here is a case or geometry bug, not a mesh-quality one.
    std::optional<double> authored_area_rel_diff;
    bool authored_area_consistent = true;
    bool authored_area_checked = false;
    /// Health contribution. FALSE only when the area was actually verified and is
    /// out of tolerance. Unverified and partial-selection are not passes -- they
    /// are reported as their own status -- but they are not solve failures either,
    /// so they must not silently sink a row that is otherwise healthy.
    bool load_area_ok = true;
};

int count_orphan_nodes(const fea::NodalMesh& mesh) {
    if (mesh.nodes.empty()) {
        return 0;
    }
    std::vector<char> used(mesh.nodes.size(), 0);
    for (const auto& el : mesh.elements) {
        for (const auto ni : el.nodes) {
            if (ni < used.size()) {
                used[ni] = 1;
            }
        }
    }
    int n = 0;
    for (const char u : used) {
        if (!u) {
            ++n;
        }
    }
    return n;
}

/// Unique nodes on the exact face set used to assemble the traction load.
std::vector<std::uint32_t> nodes_on_load_faces(const ResolvedLoadFaces& resolved,
                                               int* n_faces_out = nullptr) {
    std::set<std::uint32_t> unique;
    for (const auto& face : resolved.faces) {
        unique.insert(face.nodes.begin(), face.nodes.end());
    }
    if (n_faces_out != nullptr) {
        *n_faces_out = static_cast<int>(resolved.faces.size());
    }
    return std::vector<std::uint32_t>(unique.begin(), unique.end());
}

/// Fallback: unique nodes whose coordinates fall in `box`.
std::vector<std::uint32_t> nodes_in_box(const fea::NodalMesh& mesh, const Box3& box) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
        if (box.contains(mesh.nodes[i])) {
            out.push_back(i);
        }
    }
    return out;
}

/// Free residual + reaction force balance (health gates for accuracy trust).
void compute_solve_health(const fea::NodalMesh& mesh, const fea::Material& mat,
                          const fea::Dirichlet& bc, const Eigen::VectorXd& f,
                          const Eigen::VectorXd& u, ProbeAnswers& a) {
    const auto K = fea::assemble_stiffness(mesh, mat);
    const Eigen::VectorXd r = K * u - f;

    double free_r2 = 0.0;
    double f2 = 0.0;
    Eigen::Vector3d F_sum = Eigen::Vector3d::Zero();
    Eigen::Vector3d R_sum = Eigen::Vector3d::Zero();
    const Eigen::Index n_dof = u.size();
    for (Eigen::Index dof = 0; dof < n_dof; ++dof) {
        f2 += f[dof] * f[dof];
        const int axis = static_cast<int>(dof % 3);
        if (bc.dof_values.contains(dof)) {
            R_sum[axis] += r[dof];
        } else {
            free_r2 += r[dof] * r[dof];
            F_sum[axis] += f[dof];
        }
    }
    constexpr double kEps = 1e-30;
    a.free_residual_rel = std::sqrt(free_r2) / std::max(std::sqrt(f2), kEps);
    // Equilibrium: sum(F_applied on free) + sum(reactions on constrained) ≈ 0.
    a.reaction_sum_err = (F_sum + R_sum).norm() / std::max(F_sum.norm(), kEps);
    a.n_bc_dofs = static_cast<int>(bc.dof_values.size());
}

/// Legacy corner-only area. Keep this on the committed selection path so a
/// passing row cannot silently change its face ranking or load vector.
double legacy_surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    if (f.nodes.size() < 3) {
        return 0.0;
    }
    const Eigen::Vector3d& p0 = mesh.nodes[f.nodes[0]];
    const Eigen::Vector3d& p1 = mesh.nodes[f.nodes[1]];
    const Eigen::Vector3d& p2 = mesh.nodes[f.nodes[2]];
    double area = 0.5 * (p1 - p0).cross(p2 - p0).norm();
    if (f.nodes.size() >= 4 && f.nodes[2] != f.nodes[3]) {
        const Eigen::Vector3d& p3 = mesh.nodes[f.nodes[3]];
        area += 0.5 * (p2 - p0).cross(p3 - p0).norm();
    }
    return area;
}

/// Isoparametric face area, including tri6/quad8 mid-side geometry.
double surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& face) {
    return fea::integrated_face_area(mesh, std::vector<fea::SurfaceFace>{face});
}

/// Quality floor for p99 / face-mean stress (exclude slivers that invent 1e20 VM).
/// `ElementCentroidStress::quality` is now a measured shape quality for EVERY
/// element type (fea::cell_quality) and is 0 for a cell that could not be
/// measured, so this floor finally bites on hex/prism/pyramid/poly meshes — it
/// used to be a no-op there because non-tet samples were hard-assigned 1.0.
/// 0.02 on the scaled-Jacobian / aspect scale ≈ a 50:1 degenerate cell.
constexpr double kStressQualityFloor = 0.02;

ProbeAnswers compute_probes(const fea::NodalMesh& mesh, const fea::Material& mat,
                            const Eigen::VectorXd& u, const std::vector<LoadSpec>& loads,
                            const std::vector<ResolvedLoadFaces>& resolved_loads,
                            const std::vector<MetricSpec>& metrics, const fea::Dirichlet& bc,
                            const Eigen::VectorXd& f) {
    ProbeAnswers a;
    a.n_orphan_nodes = count_orphan_nodes(mesh);
    a.tip_deflection_max = tlab::global_max_displacement_mag(u, mesh.nodes.size());
    a.strain_energy = fea::strain_energy(mesh, mat, u);

    // DIAGNOSTIC: nodal max (can spike on slivers — never the campaign score).
    const auto nodal = fea::recover_nodal_stress(mesh, mat, u);
    for (const auto& s : nodal) {
        a.sigma_max = std::max(a.sigma_max, fea::von_mises(s));
    }

    // Stress select box: first metric probe.select, else first load box.
    Box3 stress_box;
    bool have_stress_box = false;
    for (const auto& m : metrics) {
        if (m.probe.select) {
            stress_box = *m.probe.select;
            have_stress_box = true;
            break;
        }
    }
    if (!have_stress_box && !loads.empty()) {
        stress_box = loads.front().box;
        have_stress_box = true;
    }

    // Element-centroid (interior) stress samples — scoring path.
    const auto elem_s = fea::recover_element_centroid_stress(mesh, mat, u);
    std::vector<double> vm_quality;
    vm_quality.reserve(elem_s.size());
    for (const auto& es : elem_s) {
        if (es.quality < kStressQualityFloor) {
            ++a.n_quality_excluded;
            continue;
        }
        const double vm = fea::von_mises(es.stress);
        vm_quality.push_back(vm);
        if (have_stress_box && stress_box.contains(es.centroid)) {
            a.sigma_box_max = std::max(a.sigma_box_max, vm);
        }
    }
    if (!vm_quality.empty()) {
        std::sort(vm_quality.begin(), vm_quality.end());
        const std::size_t idx = std::min(
            vm_quality.size() - 1,
            static_cast<std::size_t>(0.99 * static_cast<double>(vm_quality.size() - 1)));
        a.sigma_p99 = vm_quality[idx];
    }

    // Area-weighted mean VM: for each boundary face in stress box, use nearest
    // quality-passing element-centroid sample (never nodal extrapolation).
    if (have_stress_box && !elem_s.empty()) {
        double wsum = 0.0;
        double vsum = 0.0;
        for (const auto& face : free_faces_as_surface(mesh)) {
            const Eigen::Vector3d c = face_centroid(mesh, face);
            if (!stress_box.contains(c)) {
                continue;
            }
            const double area = legacy_surface_face_area(mesh, face);
            if (!(area > 0.0)) {
                continue;
            }
            double best_d2 = std::numeric_limits<double>::infinity();
            double best_vm = 0.0;
            bool found = false;
            for (const auto& es : elem_s) {
                if (es.quality < kStressQualityFloor) {
                    continue;
                }
                const double d2 = (es.centroid - c).squaredNorm();
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_vm = fea::von_mises(es.stress);
                    found = true;
                }
            }
            if (found) {
                wsum += area;
                vsum += area * best_vm;
            }
        }
        if (wsum > 0.0) {
            a.sigma_face_mean = vsum / wsum;
        }
    }

    // Tip / load face mean displacement + optional expected-area guard.
    // Uses the same box + normal-aligned face set as assemble_traction_load so
    // lateral wall faces near a tip slab are not counted as load area.
    std::vector<std::uint32_t> probe_nodes;
    if (!loads.empty() && !resolved_loads.empty()) {
        const LoadSpec& L0 = loads.front();
        const ResolvedLoadFaces& selected = resolved_loads.front();
        a.dominant_load_axis = tlab::dominant_axis(L0.traction);
        int n_faces = 0;
        probe_nodes = nodes_on_load_faces(selected, &n_faces);
        a.n_load_faces = n_faces;
        a.load_face_area = selected.reported_area;
        a.mesh_selected_area = selected.mesh_selected_area;

        // Expected area comes from the case's own selection rule applied to the
        // exact CAD (cad_rule_area) when available -- that is what the traction was
        // rescaled onto -- else from an authored expected_area. The old rule
        // consulted neither on a CAD-backed run whose case omitted expected_area,
        // and left rel_err at its 0.0 default, which reads as a perfect match.
        // Policy and reasoning live in load_area.hpp so they can be unit-tested.
        const tlab::LoadAreaAssessment area = tlab::assess_load_area(
            L0.expected_area, L0.cad_rule_area, a.mesh_selected_area);
        a.load_area_status = area.status;
        a.load_area_rel_err = area.rel_err;
        a.load_area_ok = area.ok;

        // Mesh-independent: does the authored case definition still agree with the
        // CAD? Reported separately from the deficit above, never folded into it.
        const tlab::AuthoredAreaCheck authored =
            tlab::check_authored_area(L0.expected_area, L0.cad_rule_area);
        a.authored_area_checked = authored.checked;
        a.authored_area_rel_diff = authored.rel_diff;
        a.authored_area_consistent = authored.consistent;
        if (probe_nodes.empty()) {
            probe_nodes = nodes_in_box(mesh, L0.box);
        }
    }
    a.n_probe_nodes = static_cast<int>(probe_nodes.size());
    if (!probe_nodes.empty()) {
        a.tip_deflection = tlab::face_mean_displacement_mag(u, probe_nodes);
        a.mean_u_component =
            tlab::face_mean_displacement_component(u, probe_nodes, a.dominant_load_axis);
        a.mean_ux = tlab::face_mean_displacement_component(u, probe_nodes, 0);
        a.mean_uz = tlab::face_mean_displacement_component(u, probe_nodes, 2);
    } else {
        a.tip_deflection = 0.0;
    }

    compute_solve_health(mesh, mat, bc, f, u, a);
    return a;
}

double evaluate_probe(const ProbeSpec& probe, const ProbeAnswers& a) {
    // Score path: face-mean stress, energy, tip face-mean. Raw max is diagnostic.
    if (probe.kind == "mean_vm" || probe.kind == "mean_von_mises" ||
        probe.kind == "face_mean_vm") {
        return a.sigma_face_mean;
    }
    if (probe.kind == "peak_vm") {
        return a.sigma_box_max;
    }
    if (probe.kind == "peak_vm_over_nominal") {
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("peak_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_box_max / probe.nominal;
    }
    if (probe.kind == "mean_vm_over_nominal" || probe.kind == "scf_mean" ||
        probe.kind == "scf") {
        // "scf" now means face-mean VM / nominal (not nodal max).
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("mean_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_face_mean / probe.nominal;
    }
    if (probe.kind == "max_von_mises" || probe.kind == "max_vm") {
        // Diagnostic only — references should not score this.
        return a.sigma_max;
    }
    if (probe.kind == "max_vm_over_nominal") {
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("max_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_max / probe.nominal;
    }
    if (probe.kind == "sigma_p99" || probe.kind == "p99_vm") {
        return a.sigma_p99;
    }
    if (probe.kind == "strain_energy" || probe.kind == "energy") {
        return a.strain_energy;
    }
    if (probe.kind == "max_displacement" || probe.kind == "tip_deflection") {
        return a.tip_deflection;
    }
    if (probe.kind == "mean_ux_on_face") {
        return (a.dominant_load_axis == 0) ? a.mean_u_component : a.mean_ux;
    }
    if (probe.kind == "mean_uz_on_face") {
        return (a.dominant_load_axis == 2) ? a.mean_u_component : a.mean_uz;
    }
    throw std::runtime_error("unknown probe kind '" + probe.kind + "'");
}

/// Optional geometry scorecard fields (null when CAD/surface unavailable).
json compute_scorecard_geom(const pipeline::Model& model, const fea::NodalMesh& mesh,
                            double h) {
    json edge_hd = nullptr;
    json chord_eff = nullptr;
    json normal_dev = nullptr;

    if (model.cad && !model.cad->empty() && h > 0.0) {
        try {
            const geom::CadTopology topo = geom::extract_topology(*model.cad, 8);
            if (!topo.empty()) {
                // Free-boundary undirected edges near sharp CAD features.
                // (Do not treat unordered node lists as a polyline — that
                // invents nonsense segments and inflates chordal efficiency.)
                const auto faces = free_faces_as_surface(mesh);
                std::set<std::pair<std::uint32_t, std::uint32_t>> bedges;
                std::set<std::uint32_t> bnodes;
                auto add_edge = [&](std::uint32_t i, std::uint32_t j) {
                    if (i > j) {
                        std::swap(i, j);
                    }
                    bedges.insert({i, j});
                };
                for (const auto& face : faces) {
                    for (const auto ni : face.nodes) {
                        bnodes.insert(ni);
                    }
                    const auto& n = face.nodes;
                    if (n.size() == 3) {
                        add_edge(n[0], n[1]);
                        add_edge(n[1], n[2]);
                        add_edge(n[2], n[0]);
                    } else if (n.size() >= 4) {
                        add_edge(n[0], n[1]);
                        add_edge(n[1], n[2]);
                        add_edge(n[2], n[3]);
                        add_edge(n[3], n[0]);
                    }
                }
                std::vector<Eigen::Vector3d> samples;
                samples.reserve(bnodes.size());
                for (const auto ni : bnodes) {
                    if (ni < mesh.nodes.size()) {
                        samples.push_back(mesh.nodes[ni]);
                    }
                }
                const double near_band = 0.75 * h;
                std::vector<geom::MeshEdgeSegment> segs;
                segs.reserve(bedges.size());
                for (const auto& [ia, ib] : bedges) {
                    if (ia >= mesh.nodes.size() || ib >= mesh.nodes.size()) {
                        continue;
                    }
                    const Eigen::Vector3d& pa = mesh.nodes[ia];
                    const Eigen::Vector3d& pb = mesh.nodes[ib];
                    const auto qa = geom::closest_edge(topo, pa, /*sharp_only=*/true);
                    const auto qb = geom::closest_edge(topo, pb, /*sharp_only=*/true);
                    if (!qa || !qb || qa->distance > near_band || qb->distance > near_band) {
                        continue;
                    }
                    segs.push_back(geom::MeshEdgeSegment{pa, pb});
                }
                if (!samples.empty()) {
                    const double hd = geom::edge_profile_hausdorff_filtered(
                        topo, samples, /*sharp_only=*/true);
                    edge_hd = hd / std::max(h, 1e-15);
                }
                if (!segs.empty()) {
                    const auto chord = geom::chordal_edge_metrics_segments(
                        topo, segs, /*sharp_edges_only=*/true);
                    chord_eff = chord.max_efficiency;
                    if (edge_hd.is_null() && chord.hausdorff > 0.0) {
                        edge_hd = chord.hausdorff / std::max(h, 1e-15);
                    }
                }
            }
        } catch (...) {
            edge_hd = nullptr;
            chord_eff = nullptr;
        }
    }

    // v1 normal deviation: max angle (deg) between boundary face normal and a
    // nearby, orientation-compatible model.surface triangle. Skip faces with
    // no good match; leave null if nothing reliable is found.
    if (!model.surface.triangles.empty() && !model.surface.vertices.empty() && h > 0.0) {
        try {
            const double max_match_d2 = (2.0 * h) * (2.0 * h);
            // Require at least ~cos(30°) alignment to count as same-face match.
            constexpr double kMinAbsDot = 0.866; // cos 30°
            double max_deg = 0.0;
            bool any = false;
            for (const auto& face : free_faces_as_surface(mesh)) {
                if (face.nodes.size() < 3) {
                    continue;
                }
                const Eigen::Vector3d& p0 = mesh.nodes[face.nodes[0]];
                const Eigen::Vector3d& p1 = mesh.nodes[face.nodes[1]];
                const Eigen::Vector3d& p2 = mesh.nodes[face.nodes[2]];
                const Eigen::Vector3d e01 = p1 - p0;
                const Eigen::Vector3d e02 = p2 - p0;
                Eigen::Vector3d fn = e01.cross(e02);
                const double fn_n = fn.norm();
                if (!(fn_n > 1e-30)) {
                    continue;
                }
                fn /= fn_n;
                const Eigen::Vector3d c = face_centroid(mesh, face);
                double best_abs_dot = -1.0;
                for (const auto& tri : model.surface.triangles) {
                    const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
                    const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
                    const Eigen::Vector3d& c3 = model.surface.vertices[tri[2]];
                    const Eigen::Vector3d tc = (a + b + c3) / 3.0;
                    if ((tc - c).squaredNorm() > max_match_d2) {
                        continue;
                    }
                    const Eigen::Vector3d ab = b - a;
                    const Eigen::Vector3d ac = c3 - a;
                    const Eigen::Vector3d sn = ab.cross(ac);
                    const double snn = sn.norm();
                    if (!(snn > 1e-30)) {
                        continue;
                    }
                    best_abs_dot = std::max(best_abs_dot, std::abs(fn.dot(sn / snn)));
                }
                // No nearby co-oriented surface triangle → skip (do not report 90°).
                if (best_abs_dot < kMinAbsDot) {
                    continue;
                }
                const double cosang = std::clamp(best_abs_dot, 0.0, 1.0);
                const double deg = std::acos(cosang) * (180.0 / 3.14159265358979323846);
                max_deg = std::max(max_deg, deg);
                any = true;
            }
            if (any) {
                normal_dev = max_deg;
            }
        } catch (...) {
            normal_dev = nullptr;
        }
    }

    return {{"edge_hausdorff_over_h", edge_hd},
            {"chordal_efficiency_max", chord_eff},
            {"normal_dev_deg_max", normal_dev}};
}

/// Crude N_pred ≈ C · V / h³ before meshing (ADR-0023 M4 diagnosis).
/// If N_pred already busts the tier, blame sizing not the mesher.
double predict_elem_count(const pipeline::Model& model, double h) {
    if (!(h > 0.0)) {
        return 0.0;
    }
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseMax(0.0);
    const double vol = std::max(ext[0] * ext[1] * ext[2], 0.0);
    // C~6 for tet packing density in a bounding box (order-of-magnitude only).
    constexpr double kC = 6.0;
    return kC * vol / (h * h * h);
}

json geom_class_of(const pipeline::Model& model, double h_ref) {
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseAbs();
    const double min_ext = ext.minCoeff();
    const double max_ext = ext.maxCoeff();
    // Curved-vertex fraction: the SAME definition the serving side reports in
    // pipeline::CaseFeatures (src/pipeline/src/scene.cpp), so this campaign-row
    // field and the advisor's live feature agree. Fraction of vertices whose
    // curvature scaled by the bounding-box diagonal exceeds 1e-8, estimated by
    // geom::estimate_vertex_curvature and guarded so a malformed or empty
    // surface yields the finite zero default. The faceted proxy this replaces
    // -- (ntri - 12) / ntri -- saturated to ~1.0 on any real triangulation.
    double curved_frac = 0.0;
    try {
        const auto curvature = geom::estimate_vertex_curvature(model.surface);
        const double bbox_diag = ext.norm();
        std::size_t n = 0;
        std::size_t curved = 0;
        for (const double kappa : curvature.kappa) {
            if (!(kappa >= 0.0) || !std::isfinite(kappa)) {
                continue;
            }
            ++n;
            if (kappa * bbox_diag > 1e-8) {
                ++curved;
            }
        }
        if (n > 0) {
            curved_frac = static_cast<double>(curved) / static_cast<double>(n);
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero default.
    }
    const bool thin = (h_ref > 0.0) && (min_ext < 2.5 * h_ref);
    const double min_feature_h = (h_ref > 0.0) ? (min_ext / h_ref) : 0.0;
    (void)max_ext;
    return {{"curved_frac", curved_frac}, {"thin", thin}, {"min_feature_h", min_feature_h}};
}

json case_features_json(const pipeline::CaseFeatures& f) {
    return {{"bbox_dx", f.bbox_dx},
            {"bbox_dy", f.bbox_dy},
            {"bbox_dz", f.bbox_dz},
            {"diag", f.diag},
            {"volume", f.volume},
            {"surface_area", f.surface_area},
            {"sa_over_v23", f.sa_over_v23},
            {"n_faces", f.n_faces},
            {"n_sharp_edges", f.n_sharp_edges},
            {"sharp_edge_len_total", f.sharp_edge_len_total},
            {"curved_frac", f.curved_frac},
            {"kappa_max_h", f.kappa_max_h},
            {"kappa_mean_h", f.kappa_mean_h},
            {"thin_min_over_diag", f.thin_min_over_diag},
            {"thin_p10_over_diag", f.thin_p10_over_diag},
            {"min_feature_h", f.min_feature_h},
            {"n_fix_faces", f.n_fix_faces},
            {"n_load_faces", f.n_load_faces},
            {"fix_area_frac", f.fix_area_frac},
            {"load_area_frac", f.load_area_frac},
            {"load_dir_x", f.load_dir_x},
            {"load_dir_y", f.load_dir_y},
            {"load_dir_z", f.load_dir_z},
            {"fix_load_dist_over_diag", f.fix_load_dist_over_diag},
            {"load_axis_alignment", f.load_axis_alignment},
            {"poisson", f.poisson}};
}

json action_json(const Config& cfg, double h, double h_rel) {
    return {{"h", h},
            {"h_rel", h_rel},
            {"mesher", mesher_name(cfg.mesher)},
            {"element_tendency", cfg.element_tendency},
            {"skin_layers", cfg.skin_layers},
            {"feature_refine", cfg.feature_refine},
            {"bc_grading", cfg.bc_grading},
            {"spectral_smooth", cfg.spectral_smooth},
            {"adapt_passes", cfg.adapt_passes},
            {"eta_target", cfg.eta_target},
            {"p_elevate", cfg.p_elevate},
            {"adapt_leb_waves", cfg.adapt_leb_waves},
            {"order", cfg.order}};
}

PartCase with_exact_cad_selections(const pipeline::Model& model, const PartCase& source) {
    PartCase resolved = source;
    if (!model.cad) {
        return resolved;
    }
    const geom::CadTopology topology = geom::extract_topology(*model.cad, 4);
    const auto& surface = model.surface;
    for (auto& bc : resolved.bcs) {
        std::set<std::uint32_t> face_ids;
        Eigen::Index slab_axis = 0;
        (bc.box.hi - bc.box.lo).cwiseAbs().minCoeff(&slab_axis);
        Eigen::Vector3d slab_direction = Eigen::Vector3d::Zero();
        slab_direction[slab_axis] = 1.0;
        for (const auto& tri : surface.triangles) {
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!bc.box.contains(centroid)) {
                continue;
            }
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            if (!(cross.norm() > 0.0) ||
                std::abs(cross.normalized().dot(slab_direction)) <= 0.7) {
                continue;
            }
            const auto exact = geom::project_point_on_surface(*model.cad, centroid);
            if (exact && exact->face_id != geom::kInvalidCadSupportId) {
                face_ids.insert(exact->face_id);
            }
        }
        bc.cad_face_ids.assign(face_ids.begin(), face_ids.end());
    }
    for (auto& load : resolved.loads) {
        std::set<std::uint32_t> box_faces;
        std::set<std::uint32_t> aligned_faces;
        const double traction_norm = load.traction.norm();
        Eigen::Vector3d cap_direction = Eigen::Vector3d::Zero();
        if (traction_norm > 0.0) {
            cap_direction = load.traction / traction_norm;
        }
        double cap_min_dot = load.normal_min_dot;
        if (!(cap_min_dot > -1.0)) {
            // WHY this substitution exists, and why it is now scoped: a transverse
            // end load has no reason to align with the end-face normal, so with the
            // normal filter disabled the traction direction cannot tell an end cap
            // from the lateral walls that share the slab. The slab's thin axis can,
            // so it is used to pick WHICH CAD faces the face-replacement fallback
            // may substitute.
            //
            // It must NOT reach cad_rule_area. That is the rescale target, and the
            // case asked for every in-box face; silently measuring a 0.7-filtered
            // cap instead made the two sides disagree by 130.7% on sphere_box_s2_c1
            // and rescaled the traction 2.3x onto a region nobody requested. The
            // rule area below therefore uses the case's own normal_min_dot.
            Eigen::Index slab_axis = 0;
            (load.box.hi - load.box.lo).cwiseAbs().minCoeff(&slab_axis);
            cap_direction = Eigen::Vector3d::Zero();
            cap_direction[slab_axis] = 1.0;
            cap_min_dot = 0.7;
        }
        // CAD face ids that resolve, so the rule area counts the same tessellation
        // the cap sets are built from and stays comparable with earlier runs.
        std::set<std::uint32_t> topology_ids;
        for (const auto& face : topology.faces) {
            topology_ids.insert(face.id);
        }
        // The case's rule as written, mirroring select_load_faces through the
        // shared predicate: box-only when normal_min_dot <= -1, else the filter,
        // with a fallback to box-only if the filter selects nothing.
        double box_rule_area = 0.0;
        double filtered_rule_area = 0.0;
        for (const auto& tri : surface.triangles) {
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!load.box.contains(centroid)) {
                continue;
            }
            const auto exact = geom::project_point_on_surface(*model.cad, centroid);
            if (!exact || exact->face_id == geom::kInvalidCadSupportId) {
                continue;
            }
            box_faces.insert(exact->face_id);
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            const double twice_area = cross.norm();
            const double tri_area = 0.5 * twice_area;
            // Cap set: heuristic direction, used ONLY to pick face ids for the
            // face-replacement fallback.
            if (cap_direction.norm() <= 0.0 ||
                (twice_area > 0.0 &&
                 std::abs((cross / twice_area).dot(cap_direction)) > cap_min_dot)) {
                aligned_faces.insert(exact->face_id);
            }
            // Rule area: the case's OWN normal_min_dot and traction, through the
            // same predicate the mesh selector uses.
            if (topology_ids.contains(exact->face_id)) {
                box_rule_area += tri_area;
                if (tlab::load_rule_keeps_normal(load.normal_min_dot, load.traction, cross)) {
                    filtered_rule_area += tri_area;
                }
            }
        }
        const bool use_aligned = !aligned_faces.empty();
        const auto& selected = use_aligned ? aligned_faces : box_faces;
        double exact_area = 0.0;
        for (const auto face_id : selected) {
            const auto it = std::find_if(topology.faces.begin(), topology.faces.end(),
                                         [&](const geom::CadFace& face) {
                                             return face.id == face_id;
                                         });
            if (it != topology.faces.end()) {
                load.cad_face_ids.push_back(face_id);
                exact_area += it->area;
            }
        }
        if (exact_area > 0.0) {
            load.cad_face_area = exact_area;
        }
        // Mirror select_load_faces at the set level too: a filter that selects
        // nothing falls back to the whole in-box set.
        const double rule_area =
            (tlab::load_rule_filters(load.normal_min_dot, load.traction) &&
             filtered_rule_area > 0.0)
                ? filtered_rule_area
                : box_rule_area;
        if (rule_area > 0.0) {
            // The case's rule evaluated on the exact CAD tessellation rather than on
            // the candidate mesh. On a curved loaded surface the mesh's answer is
            // quantised to facet size -- 15 facets on a spherical boss put the
            // 45-degree latitude cut-off anywhere -- while this is the rule's
            // continuum limit, so it is what the traction is rescaled onto.
            load.cad_rule_area = rule_area;
        }
        // Loud once per part: an authored expected_area that no longer matches the
        // CAD is a case-definition or geometry bug, and every row it produces is
        // scored against a load the reference did not assume.
        const auto authored = tlab::check_authored_area(load.expected_area,
                                                       load.cad_rule_area);
        if (authored.checked && !authored.consistent) {
            std::fprintf(stderr,
                         "WARNING %s: authored select.expected_area %.9g disagrees with the "
                         "exact CAD rule area %.9g by %.3g%% (tol %.3g%%). This is a case "
                         "definition or geometry drift, not mesh quality: the reference "
                         "truth assumes a different loaded region than the run applies.\n",
                         resolved.part.c_str(), *load.expected_area, *load.cad_rule_area,
                         100.0 * *authored.rel_diff, 100.0 * tlab::kAuthoredAreaTol);
        }
    }
    return resolved;
}

pipeline::SimSetup adaptive_setup(const pipeline::Model& model, const PartCase& part,
                                  const Config& cfg, double h) {
    pipeline::SimSetup setup;
    setup.youngs_modulus = part.E;
    setup.poissons_ratio = part.nu;
    setup.mesh_size = h;
    setup.use_feature_grading = cfg.feature_refine;
    setup.bc_grading = cfg.bc_grading;
    setup.spectral_smooth = cfg.spectral_smooth;
    setup.adapt_passes = cfg.adapt_passes;
    setup.eta_target = cfg.eta_target;
    setup.p_elevate = cfg.p_elevate || cfg.order >= 2;
    setup.adapt_leb_waves = cfg.adapt_leb_waves;
    setup.skin_layers = cfg.skin_layers;
    setup.mesher = cfg.mesher;
    setup.element_tendency = cfg.element_tendency;
    const auto& surface = model.surface;
    for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] < 0) {
            continue;
        }
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d centroid =
            (surface.vertices[tri[0]] + surface.vertices[tri[1]] + surface.vertices[tri[2]]) /
            3.0;
        for (const auto& bc : part.bcs) {
            if (bc.box.contains(centroid)) {
                setup.fixtures.insert(model.triangle_region[ti]);
                break;
            }
        }
    }
    for (const auto& load : part.loads) {
        std::map<int, double> box_area;
        std::map<int, double> aligned_area;
        const double traction_norm = load.traction.norm();
        Eigen::Vector3d direction = Eigen::Vector3d::Zero();
        if (traction_norm > 0.0) {
            direction = load.traction / traction_norm;
        }
        for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
            if (ti >= model.triangle_region.size() || model.triangle_region[ti] < 0) {
                continue;
            }
            const auto& tri = surface.triangles[ti];
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!load.box.contains(centroid)) {
                continue;
            }
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            const double twice_area = cross.norm();
            if (!(twice_area > 0.0)) {
                continue;
            }
            const int region = model.triangle_region[ti];
            const double area = 0.5 * twice_area;
            box_area[region] += area;
            if (traction_norm <= 0.0 ||
                std::abs((cross / twice_area).dot(direction)) > load.normal_min_dot) {
                aligned_area[region] += area;
            }
        }
        const auto& selected = aligned_area.empty() ? box_area : aligned_area;
        for (const auto& [region, area] : selected) {
            setup.loads[region].force += load.traction * area;
        }
    }
    return setup;
}

/// M11: flag sharp CAD edges that want h_edge = L/3 below tier h_min, and count
/// features shorter than 2·h. Detector only — no OCC defeaturing (ADR-0024 Q10).
struct HminFeatureReport {
    json feature_flags = json::array();
    int n_features_below_h_min = 0;
};

HminFeatureReport detect_hmin_features(const pipeline::Model& model, double h) {
    HminFeatureReport rep;
    if (!(h > 0.0) || !geom::occ_enabled() || !model.cad || model.cad->empty()) {
        return rep;
    }
    // Tier-implied floor: ~1/4 of campaign h (resolved.h * h_scale).
    const double h_min = h * 0.25;
    try {
        const geom::CadTopology topo = geom::extract_topology(*model.cad, 4);
        for (const auto& e : topo.edges) {
            if (e.feature != geom::CadEdgeFeature::kSharp) {
                continue;
            }
            if (!(e.length > 0.0) || !std::isfinite(e.length)) {
                continue;
            }
            // Sharp edges shorter than 2·h → count as below h_min class.
            if (e.length < 2.0 * h) {
                ++rep.n_features_below_h_min;
            }
            // Would-want h_edge = L/3 under the floor → explicit feature_flags entry.
            const double h_edge = e.length / 3.0;
            if (h_edge < h_min) {
                rep.feature_flags.push_back({{"edge_id", e.id},
                                             {"reason", "below_h_min"},
                                             {"length", e.length},
                                             {"h_edge", h_edge},
                                             {"h_min", h_min}});
            }
        }
    } catch (...) {
        // Surface-only / extract failure: leave flags empty.
    }
    return rep;
}

json quality_of(const pipeline::Model& model, const fea::NodalMesh& mesh, double h) {
    const auto quads = fea::extract_boundary_faces(mesh);
    std::vector<mesh::FreeFace> faces(quads.begin(), quads.end());
    std::vector<std::array<std::uint32_t, 4>> tets;
    for (const auto& el : mesh.elements) {
        if (el.type == fea::ElementType::kTet4 && el.nodes.size() >= 4) {
            tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
        }
    }
    const auto* tet_ptr = tets.empty() ? nullptr : &tets;
    const auto m = mesh::evaluate_curved_mesh_quality(model.surface, mesh.nodes, faces, h,
                                                      -1.0, -1.0, nullptr, tet_ptr);
    return {{"M1max", m.m1_max},
            {"M2max", m.m2_max},
            {"M6", m.m6_min_boundary_aspect},
            {"score", m.composite_score}};
}

/// Per-run mesh-vs-BRep fidelity row fields. The metric itself lives in
/// mesh::brep_fidelity_summary; this only serializes it, so the campaign
/// columns and the diagnostic sweep can never drift apart.
json geo_fidelity_of(const pipeline::Model& model, const fea::NodalMesh& nodal, double h) {
    mesh::BrepFidelitySummary summary;
    if (geom::occ_enabled() && model.cad && !model.cad->empty()) {
        const auto quads = fea::extract_boundary_faces(nodal);
        const std::vector<mesh::FreeFace> faces(quads.begin(), quads.end());
        summary = mesh::brep_fidelity_summary(*model.cad, nodal.nodes, faces, h);
    }
    return {{"available", summary.available},
            {"chamfer_mean", summary.chamfer_mean},
            {"dist_p95", summary.dist_p95},
            {"dist_p99", summary.dist_p99},
            {"dist_max", summary.dist_max},
            {"normal_angle_p95_rad", summary.normal_angle_p95_rad},
            {"rel_volume_err", summary.rel_volume_err},
            {"n_samples", summary.n_samples}};
}

/// Records what the learned advisor would have chosen for a case, so a
/// campaign can score the policy against the grid it actually ran (ADR-0027).
///
/// The advisor deliberately does NOT override the campaign action: the grid is
/// the experiment. Letting the model pick the config would make every row
/// self-confirming and destroy the comparison the campaign exists to produce.
#ifdef POLYMESH_WITH_ADVISOR
class AdvisorScorer {
public:
    explicit AdvisorScorer(const fs::path& model_dir) : advisor_(model_dir) {}

    [[nodiscard]] json decision_json(const pipeline::CaseFeatures& features) const {
        return json::parse(polymesh::advisor::to_json(advisor_.recommend(features)));
    }

private:
    polymesh::advisor::Advisor advisor_;
};
#else
class AdvisorScorer {
public:
    explicit AdvisorScorer(const fs::path&) {
        throw std::runtime_error("--advisor needs a build with POLYMESH_WITH_ADVISOR=ON");
    }
    [[nodiscard]] json decision_json(const pipeline::CaseFeatures&) const {
        return json::object();
    }
};
#endif

// ── single run ──────────────────────────────────────────────────────────────

struct RunOutcome {
    json line;                   // results.jsonl object
    // The mesh travels out so the CALLER can write artifacts AFTER the summary
    // row is appended. Present only when a mesh was actually built: five of the
    // eight former write sites had none, and std::optional makes handing over a
    // default-constructed mesh as if it were real unrepresentable.
    std::optional<pipeline::VolumeMeshOutput> mesh;
    double accuracy_score = 0.0; // 0..1 mean over metrics
    double mesh_ms = 0.0;
    double solve_ms = 0.0;
};
// The row-first ordering depends on moving the mesh out of run_one, never copying
// it. Enforce the precondition in the compiler rather than in a review comment.
static_assert(std::is_move_constructible_v<pipeline::VolumeMeshOutput>,
              "RunOutcome moves the mesh out of run_one; a non-movable "
              "VolumeMeshOutput would silently copy a whole mesh per run");
static_assert(std::is_move_assignable_v<pipeline::VolumeMeshOutput>,
              "out.mesh = std::move(vol) must move, not copy");

// Never throws: every caller is either run_one's success path or one of its
// exception handlers, and a throw from inside a handler escapes run_one past
// its own catch-all and aborts the campaign, losing the status row.
void write_warehouse_run(const fs::path& run_dir, const json& line,
                         const pipeline::VolumeMeshOutput* vol) noexcept {
    if (!polymesh::testlab::write_run_json(run_dir, line)) {
        return;
    }
    if (vol != nullptr) {
        try {
            fea::write_vtu(run_dir / "mesh.vtu", vol->mesh);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warehouse: write_vtu failed: %s\n", e.what());
        }
    }
}

// Never throws: this runs on the SolveJob worker thread via job.on_pass, where
// a throw is caught as a solve failure and would silently downgrade a healthy
// run to a mesh_fail row -- corrupting the training set rather than crashing.
void write_adapt_trace(const fs::path& run_dir,
                       const std::vector<pipeline::PassTrace>& traces) noexcept {
    if (run_dir.empty() || traces.empty()) {
        return;
    }
    static constexpr std::array<const char*, 4> kShapeNames{"keep", "hex", "tet", "poly"};
    std::ostringstream text;
    for (const auto& trace : traces) {
        const std::size_t shape = static_cast<std::size_t>(std::clamp(trace.global_shape, 0, 3));
        const json row{{"pass", trace.pass},
                       {"n_elems", trace.n_elems},
                       {"n_nodes", trace.n_nodes},
                       {"n_dof", trace.n_dof},
                       {"global_eta", trace.global_eta},
                       {"eta_p50", trace.eta_p50},
                       {"eta_p90", trace.eta_p90},
                       {"eta_max", trace.eta_max},
                       {"n_h_mark", trace.n_h_mark},
                       {"n_p_mark", trace.n_p_mark},
                       {"n_shape_mark", trace.n_shape_mark},
                       {"global_shape", kShapeNames[shape]},
                       {"predicted_dof_factor", trace.predicted_dof_factor},
                       {"mesh_ms", trace.mesh_ms},
                       {"solve_ms", trace.solve_ms}};
        text << row.dump() << '\n';
    }
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec && !fs::is_directory(run_dir)) {
        std::fprintf(stderr, "warehouse: cannot create %s: %s\n", run_dir.string().c_str(),
                     ec.message().c_str());
        return;
    }
    try {
        atomic_write(run_dir / "adapt_trace.jsonl", text.str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warehouse: adapt_trace write failed: %s\n", e.what());
    }
}

RunOutcome run_one(const Config& cfg, const PartCase& part, int tier, double h_scale,
                   const fs::path& progress_path, const fs::path& mesh_preview_path,
                   const fs::path& warehouse_run_dir = {}, double max_run_wall_s = 900.0,
                   const AdvisorScorer* advisor = nullptr, long long budget_dof = 0,
                   long long budget_elems = 0) {
    using clock = std::chrono::steady_clock;
    const auto t_all0 = clock::now();
    const auto wall_elapsed_s = [&]() -> double {
        return std::chrono::duration<double>(clock::now() - t_all0).count();
    };
    const auto stamp_wall = [&](json& line) {
        line["wall_time_s"] = wall_elapsed_s();
        line["max_run_wall_s"] = max_run_wall_s;
    };

    RunOutcome out;
    out.line["cfg_id"] = cfg.id;
    out.line["config"] = cfg.values;
    out.line["part"] = part.part;
    out.line["schema"] = "advisor-row-v3";
    out.line["action"] = action_json(cfg, 0.0, cfg.h_rel.value_or(0.0));
    out.line["features"] = case_features_json(pipeline::CaseFeatures{});
    out.line["tier"] = tier;

    ProgressHeartbeat beat(progress_path, "mesh", cfg.id, part.part, tier, t_all0);

    try {
        const auto model = pipeline::Model::load(part.geometry);
        const PartCase selection_part = with_exact_cad_selections(model, part);
        const auto auto_resolved = pipeline::resolve_mesh_size(model, 0.0);
        const double bbox_diag = (model.bbox_max - model.bbox_min).norm();
        const double h = cfg.h_rel
                             ? std::max(*cfg.h_rel * bbox_diag, 1e-9)
                             : std::max(auto_resolved.h * h_scale, 1e-9);
        const auto resolved =
            cfg.h_rel ? pipeline::resolve_mesh_size(model, h) : auto_resolved;
        json gc = geom_class_of(model, auto_resolved.h);
        std::vector<pipeline::RefineRegion> fix_regions;
        std::vector<pipeline::RefineRegion> load_regions;
        fix_regions.reserve(part.bcs.size());
        load_regions.reserve(part.loads.size());
        for (const auto& bc : part.bcs) {
            fix_regions.push_back({bc.box.lo, bc.box.hi, 0.5});
        }
        Eigen::Vector3d load_dir = Eigen::Vector3d::Zero();
        for (const auto& load : part.loads) {
            load_regions.push_back({load.box.lo, load.box.hi, 0.25});
            load_dir += load.traction;
        }
        const pipeline::CaseFeatures features = pipeline::extract_case_features(
            model, fix_regions, load_regions, load_dir, part.nu);
        out.line["features"] = case_features_json(features);
        if (advisor != nullptr) {
            // The advisor is an observation, so its failure must not be
            // reported as an FEA failure. Without this the outer handler would
            // stamp status=solve_fail on a perfectly good run because an ORT
            // session threw, and the training set would learn that fake
            // solver failure.
            try {
                out.line["advisor_decision"] = advisor->decision_json(features);
            } catch (const std::exception& e) {
                out.line["advisor_error"] = e.what();
            }
        }
        const double actual_h_rel = bbox_diag > 0.0 ? h / bbox_diag : 0.0;
        out.line["action"] = action_json(cfg, h, actual_h_rel);

        // M11: h_min feature flag (virtual-topology detector; no OCC suppress).
        const HminFeatureReport hmin = detect_hmin_features(model, h);
        gc["n_features_below_h_min"] = hmin.n_features_below_h_min;
        out.line["geom_class"] = std::move(gc);
        out.line["feature_flags"] = hmin.feature_flags;
        out.line["n_features_below_h_min"] = hmin.n_features_below_h_min;
        if (!resolved.note.empty()) {
            out.line["mesher_note"] = resolved.note;
        }

        // M4: predicted element count from bbox volume / h³ before meshing.
        // Defaults keep one pathological config from pinning an overnight
        // throughput runner; a campaign may raise them (see Campaign::max_dof).
        constexpr long long kDefaultMaxCampaignDof = 80000;
        constexpr long long kDefaultMaxCampaignElems = 60000;
        const long long kMaxCampaignDof = budget_dof > 0 ? budget_dof : kDefaultMaxCampaignDof;
        const long long kMaxCampaignElems =
            budget_elems > 0 ? budget_elems : kDefaultMaxCampaignElems;
        const double n_pred = predict_elem_count(model, h);
        out.line["n_pred_elems"] = n_pred;
        out.line["h"] = h;
        if (n_pred > 2.0 * static_cast<double>(kMaxCampaignElems)) {
            // Sizing field alone already busts budget — do not mesh.
            out.line["status"] = "over_budget";
            out.line["over_budget_cause"] = "sizing"; // N_pred ≫ tier → fix auto-h
            out.line["error"] = "N_pred=" + std::to_string(static_cast<long long>(n_pred)) +
                                " already exceeds 2× elem budget (sizing, not mesher)";
            out.line["mesh_ms"] = 0.0;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            beat.set_phase("done", 1.0);
            return out;
        }

        // M14: if already over wall before meshing (unlikely), skip mesh+solve.
        if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
            throw WallClockBudgetExceeded(wall_elapsed_s());
        }

        const auto t_mesh0 = clock::now();
        // Optional a-priori geometry+BC grading (ADR-0021): refine toward the
        // load/fixture selection boxes (loads finest) fused with geometry
        // features, so the mesh reflects the simulation setup. Off by default.
        std::vector<Eigen::Vector3d> refine_seeds;
        double refine_band = 0.0;
        mesh::SizeFieldFn size_field;
        if (cfg.bc_grading) {
            std::vector<pipeline::RefineRegion> regions = load_regions;
            regions.insert(regions.end(), fix_regions.begin(), fix_regions.end());
            const auto plan =
                pipeline::build_refinement_plan(model, h, regions, cfg.feature_refine);
            refine_seeds = plan.refine_seeds;
            refine_band = plan.seed_band;
            size_field = plan.size_field;
        }
        pipeline::VolumeMeshOutput vol;
        std::vector<pipeline::PassTrace> adapt_traces;
        if (cfg.adapt_passes > 0) {
            pipeline::SimSetup setup = adaptive_setup(model, selection_part, cfg, h);
            setup.max_elems = static_cast<std::size_t>(kMaxCampaignElems);
            setup.max_dof = static_cast<std::size_t>(kMaxCampaignDof);
            pipeline::SolveJob job;
            job.on_pass = [&](const pipeline::PassTrace& trace) {
                adapt_traces.push_back(trace);
                write_adapt_trace(warehouse_run_dir, adapt_traces);
            };
            job.start(model, setup);
            bool wall_exceeded = false;
            while (job.state() == pipeline::SolveJob::State::kMeshing ||
                   job.state() == pipeline::SolveJob::State::kSolving) {
                if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
                    wall_exceeded = true;
                    job.request_cancel();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (wall_exceeded) {
                throw WallClockBudgetExceeded(wall_elapsed_s());
            }
            auto result = job.take_result();
            if (!result) {
                throw std::runtime_error("adaptive SolveJob failed: " + job.status_text());
            }
            vol.mesh = std::move(result->volume_mesh);
            vol.boundary_quads = std::move(result->boundary_quads);
            vol.mesher_note = std::move(result->mesh_note);
            vol.fill_geometry_volume = result->fill_geometry_volume;
            vol.solved_geometry_volume = result->solved_geometry_volume;

            for (const auto& trace : adapt_traces) {
                out.mesh_ms += trace.mesh_ms;
                out.solve_ms += trace.solve_ms;
            }
            write_adapt_trace(warehouse_run_dir, adapt_traces);
        } else {
            vol = pipeline::volume_mesh(
                model, h, cfg.mesher, cfg.skin_layers, cfg.feature_refine, refine_seeds,
                refine_band, cfg.element_tendency, 0, 0, 0, {}, size_field);
            const std::string combined_mesher_note =
                tlab::combine_mesher_notes(resolved.note, vol.mesher_note);
            if (!combined_mesher_note.empty()) {
                out.line["mesher_note"] = combined_mesher_note;
            }
            vol.mesh.check_validity();
            if (cfg.order >= 2 || cfg.p_elevate) {
                // Use the product's own curved-geometry construction rather than
                // a local promote+project copy: pyramid split, exact face and
                // sharp-edge mids, shape-floor rollback and h-refinement
                // fallback all have to be identical, or the advisor learns from
                // meshes the shipped pipeline never builds.
                auto shaped = pipeline::curve_volume_geometry(model, vol.mesh, h);
                vol.mesh = std::move(shaped.mesh);
                vol.boundary_quads = fea::extract_boundary_faces(vol.mesh);
                vol.mesher_note += std::format(
                    " | curved_volume promoted={} pyramid_split={} projected={}"
                    " partial={} reverted={} h_refined={}",
                    shaped.n_promoted, shaped.n_pyramids_split, shaped.n_projected,
                    shaped.n_partial, shaped.n_reverted, shaped.n_h_refined);
                vol.mesh.check_validity();
            }
            pipeline::update_solved_geometry_volume(model, vol);

            const auto t_mesh1 = clock::now();
            out.mesh_ms =
                std::chrono::duration<double, std::milli>(t_mesh1 - t_mesh0).count();
        }
        if (!vol.mesher_note.empty()) {
            out.line["mesher_note"] = vol.mesher_note;
        }
        // Validity is mandatory before any solve (anti-cheat / engineering rule).
        vol.mesh.check_validity();

        out.line["n_elems"] = vol.mesh.elements.size();
        out.line["n_nodes"] = vol.mesh.nodes.size();
        const long long n_dof = 3 * static_cast<long long>(vol.mesh.nodes.size());
        out.line["n_dof"] = n_dof;
        out.line["quality"] = quality_of(model, vol.mesh, h);
        out.line["geo_fidelity"] = geo_fidelity_of(model, vol.mesh, h);
        if (vol.fill_geometry_volume.available) {
            out.line["geometry_fill_volume_err"] =
                vol.fill_geometry_volume.relative_error;
        }
        if (vol.solved_geometry_volume.available) {
            out.line["geometry_volume_err"] = vol.solved_geometry_volume.relative_error;
        }
        if (n_pred > 0.0) {
            out.line["n_elems_over_pred"] =
                static_cast<double>(vol.mesh.elements.size()) / n_pred;
        }

        beat.set_mesh_stats(vol.mesh.elements.size(), vol.mesh.nodes.size());
        write_mesh_preview(mesh_preview_path, vol);

        // M14: after mesh — if wall budget blown, skip remaining solve.
        if (max_run_wall_s > 0.0 && wall_elapsed_s() > max_run_wall_s) {
            out.line["status"] = "over_budget";
            out.line["over_budget_cause"] = "wall_clock";
            out.line["error"] = "wall-clock exceeded after mesh (" +
                                std::to_string(wall_elapsed_s()) + " s > " +
                                std::to_string(max_run_wall_s) + " s); solve skipped";
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            out.mesh = std::move(vol);
            beat.set_phase("done", 1.0);
            return out;
        }

        // Campaign budget: skip pathological meshes so one hybrid_vem case cannot
        // pin the overnight runner for tens of minutes. Tunable later via campaign.json.
        if (n_dof > kMaxCampaignDof ||
            static_cast<long long>(vol.mesh.elements.size()) > kMaxCampaignElems) {
            out.line["status"] = "over_budget";
            // N_pred OK but N_actual high → mesher (recovery/protect cascades).
            out.line["over_budget_cause"] =
                (n_pred > 0.0 && static_cast<double>(vol.mesh.elements.size()) > 3.0 * n_pred)
                    ? "mesher"
                    : "budget";
            out.line["error"] =
                "mesh exceeds campaign DOF/elem budget (" + std::to_string(n_dof) + " dof, " +
                std::to_string(vol.mesh.elements.size()) +
                " elems, N_pred=" + std::to_string(static_cast<long long>(n_pred)) + ")";
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            stamp_wall(out.line);
            out.mesh = std::move(vol);
            beat.set_phase("done", 1.0);
            return out;
        }

        beat.set_phase("assemble", 0.0);

        const fea::Material mat{.youngs_modulus = part.E, .poissons_ratio = part.nu};
        const auto bc = make_dirichlet(vol.mesh, selection_part.bcs,
                                       model.cad ? &*model.cad : nullptr, h);
        if (bc.dof_values.empty()) {
            throw std::runtime_error("no Dirichlet DOFs matched BC boxes for part " +
                                     part.part);
        }
        const auto resolved_loads =
            resolve_load_faces(vol.mesh, model.cad ? &*model.cad : nullptr, h,
                               selection_part.loads);
        const auto loads = make_loads(vol.mesh, selection_part.loads, resolved_loads);
        if (loads.norm() == 0.0) {
            throw std::runtime_error("zero load vector for part " + part.part);
        }
        if (std::getenv("POLYMESH_SELECTION_AUDIT") != nullptr) {
            const auto legacy_bc =
                make_dirichlet(vol.mesh, part.bcs, nullptr, h);
            const auto legacy_selections =
                resolve_load_faces(vol.mesh, nullptr, h, part.loads);
            const auto legacy_loads =
                make_loads(vol.mesh, part.loads, legacy_selections);
            const bool used_fixture_fallback =
                legacy_bc.dof_values.empty() && !bc.dof_values.empty();
            const bool used_load_fallback =
                std::any_of(resolved_loads.begin(), resolved_loads.end(),
                            [](const ResolvedLoadFaces& selection) {
                                return selection.used_exact_fallback;
                            });
            const std::uint64_t legacy_fixture_hash =
                dirichlet_node_set_hash(legacy_bc);
            const std::uint64_t selected_fixture_hash =
                dirichlet_node_set_hash(bc);
            const std::uint64_t legacy_face_hash =
                selected_face_set_hash(legacy_selections);
            const std::uint64_t selected_face_hash =
                selected_face_set_hash(resolved_loads);
            const std::uint64_t legacy_node_hash =
                selected_node_set_hash(legacy_selections);
            const std::uint64_t selected_node_hash =
                selected_node_set_hash(resolved_loads);
            const std::uint64_t legacy_vector_hash =
                load_vector_hash(legacy_loads);
            const std::uint64_t selected_vector_hash =
                load_vector_hash(loads);
            const bool unchanged =
                legacy_fixture_hash == selected_fixture_hash &&
                legacy_face_hash == selected_face_hash &&
                legacy_node_hash == selected_node_hash &&
                legacy_vector_hash == selected_vector_hash;
            if (!used_fixture_fallback && !used_load_fallback && !unchanged) {
                throw std::logic_error(
                    "selection audit: a non-fallback row changed its BC/load selection");
            }
            out.line["selection_audit"] = {
                {"used_fixture_fallback", used_fixture_fallback},
                {"used_load_fallback", used_load_fallback},
                {"legacy_fixture_node_hash", legacy_fixture_hash},
                {"selected_fixture_node_hash", selected_fixture_hash},
                {"legacy_face_hash", legacy_face_hash},
                {"selected_face_hash", selected_face_hash},
                {"legacy_node_hash", legacy_node_hash},
                {"selected_node_hash", selected_node_hash},
                {"legacy_load_vector_hash", legacy_vector_hash},
                {"selected_load_vector_hash", selected_vector_hash},
                {"unchanged", unchanged}};
        }

        beat.set_phase("solve", 0.0);

        const auto t_solve0 = clock::now();
        fea::SolveOptions sopt;
        // kAuto is fine now: the direct/CG cliff that used to force kDirect here
        // ("CG was hanging / failing on poorly conditioned hybrid meshes at
        // moderate free DOF") sat at 8000 free DOF. kAuto keeps LDLT to 50000
        // and bounds CG above that, so campaign meshes take the same direct path
        // they did before without the unbounded factorisation on the huge ones.
        sopt.on_progress = [&](int iter, int max_iters, double resid) {
            // M14 mid-solve wall-clock kill (when progress callbacks fire).
            if (max_run_wall_s > 0.0) {
                const double elapsed = wall_elapsed_s();
                if (elapsed > max_run_wall_s) {
                    throw WallClockBudgetExceeded(elapsed);
                }
            }
            const double frac =
                max_iters > 0
                    ? std::clamp(static_cast<double>(iter) / static_cast<double>(max_iters),
                                 0.0, 1.0)
                    : 0.0;
            beat.set_cg(iter, resid);
            beat.set_frac(frac);
        };
        const Eigen::VectorXd u = fea::solve_elastostatics(vol.mesh, mat, bc, loads, sopt);
        const auto t_solve1 = clock::now();
        out.solve_ms += std::chrono::duration<double, std::milli>(t_solve1 - t_solve0).count();

        beat.set_phase("recover", 0.5);

        const ProbeAnswers ans =
            compute_probes(vol.mesh, mat, u, selection_part.loads, resolved_loads,
                           part.metrics, bc, loads);
        // INVARIANT: this block must carry every ProbeAnswers field that
        // evaluate_probe() can read. scripts/build_advisor_dataset.py re-derives
        // each row's accuracy from `answers` against the CURRENT references, so a
        // probe input that is not recorded here makes that row permanently
        // unscoreable — changing truth would need a campaign re-run instead of a
        // dataset rebuild. sigma_box_max (the box-windowed peak VM behind the SCF
        // probe) was missing for exactly that reason; mean_ux/mean_uz and
        // dominant_load_axis back the axis-conditional displacement probes.
        out.line["answers"] = {{"sigma_max", ans.sigma_max},
                               {"sigma_face_mean", ans.sigma_face_mean},
                               {"sigma_box_max", ans.sigma_box_max},
                               {"sigma_p99", ans.sigma_p99},
                               {"strain_energy", ans.strain_energy},
                               {"tip_deflection", ans.tip_deflection},
                               {"tip_deflection_max", ans.tip_deflection_max},
                               {"mean_u_component", ans.mean_u_component},
                               {"mean_ux", ans.mean_ux},
                               {"mean_uz", ans.mean_uz},
                               {"dominant_load_axis", ans.dominant_load_axis},
                               {"n_probe_nodes", ans.n_probe_nodes},
                               {"n_load_faces", ans.n_load_faces},
                               {"n_quality_excluded", ans.n_quality_excluded},
                               {"load_face_area", ans.load_face_area},
                               {"mesh_selected_area", ans.mesh_selected_area},
                               {"load_area_status",
                                std::string(tlab::load_area_status_name(
                                    ans.load_area_status))},
                               // Explicit null, never 0.0, when nothing could be
                               // verified. A number here reads as a pass.
                               {"load_area_rel_err",
                                ans.load_area_rel_err ? json(*ans.load_area_rel_err)
                                                      : json(nullptr)},
                               // Case-definition cross-check, deliberately a
                               // separate field from the mesh deficit above.
                               {"authored_area_checked", ans.authored_area_checked},
                               {"authored_area_consistent", ans.authored_area_consistent},
                               {"authored_area_rel_diff",
                                ans.authored_area_rel_diff
                                    ? json(*ans.authored_area_rel_diff)
                                    : json(nullptr)}};

        // Health gates: residual / reaction / orphans / load-area guard.
        // load_area_ok is false only when the area was genuinely verified and is
        // out of tolerance, so an UNVERIFIABLE area no longer silently satisfies
        // the gate and no longer sinks a healthy row either -- it is visible as
        // load_area_status instead.
        constexpr double kFreeResidTolDirect = 1e-6;
        constexpr double kReactionSumTol = 0.05;
        const bool health_ok = (ans.n_orphan_nodes == 0) &&
                               (ans.free_residual_rel <= kFreeResidTolDirect) &&
                               (ans.reaction_sum_err <= kReactionSumTol) && ans.load_area_ok;
        out.line["health"] = {{"free_residual_rel", ans.free_residual_rel},
                              {"reaction_sum_err", ans.reaction_sum_err},
                              {"n_orphans", ans.n_orphan_nodes},
                              {"n_bc_dofs", ans.n_bc_dofs},
                              {"load_area_ok", ans.load_area_ok},
                              {"load_area_status",
                               std::string(tlab::load_area_status_name(
                                   ans.load_area_status))},
                              {"load_area_verified",
                               ans.load_area_status == tlab::LoadAreaStatus::kVerified},
                              {"authored_area_consistent", ans.authored_area_consistent},
                              {"load_area_rel_err",
                               ans.load_area_rel_err ? json(*ans.load_area_rel_err)
                                                     : json(nullptr)},
                              {"ok", health_ok}};

        // Accuracy vs hand-calc truths (loaded from bench/reference via the case).
        // When health fails, still record measured answers/rel_err but zero scores
        // so ranking never trusts a singular / residual-broken solve.
        double acc_sum = 0.0;
        int acc_n = 0;
        json acc_detail = json::array();
        for (const auto& m : part.metrics) {
            const double measured = evaluate_probe(m.probe, ans);
            const double truth = m.value;
            const double rel = (std::abs(truth) > 0.0)
                                   ? std::abs(measured - truth) / std::abs(truth)
                                   : std::abs(measured);
            const double tol = (m.tol > 0.0) ? m.tol : 1e-12;
            const double s = health_ok ? (1.0 / (1.0 + rel / tol)) : 0.0;
            acc_sum += s;
            ++acc_n;
            acc_detail.push_back({{"metric", m.name},
                                  {"value", measured},
                                  {"truth", truth},
                                  {"rel_err", rel},
                                  {"score", s},
                                  {"trusted", health_ok}});
        }
        out.accuracy_score = (acc_n > 0) ? (acc_sum / static_cast<double>(acc_n)) : 0.0;
        // Primary metric for results.jsonl (first metric or aggregate).
        if (!acc_detail.empty()) {
            out.line["accuracy"] = acc_detail.front();
            out.line["accuracy"]["all"] = acc_detail;
        } else {
            out.line["accuracy"] = {{"metric", "none"},
                                    {"value", nullptr},
                                    {"truth", nullptr},
                                    {"rel_err", nullptr}};
        }

        // Five-metric campaign scorecard (Q4).
        json scorecard = compute_scorecard_geom(model, vol.mesh, h);
        scorecard["n_dof"] = n_dof;
        if (!acc_detail.empty() && acc_detail.front().contains("rel_err")) {
            scorecard["accuracy_rel_err"] = acc_detail.front()["rel_err"];
        } else {
            scorecard["accuracy_rel_err"] = nullptr;
        }
        if (out.line.contains("quality") && out.line["quality"].contains("M6")) {
            scorecard["min_element_quality"] = out.line["quality"]["M6"];
        } else if (out.line.contains("quality") && out.line["quality"].contains("score")) {
            scorecard["min_element_quality"] = out.line["quality"]["score"];
        } else {
            scorecard["min_element_quality"] = nullptr;
        }
        scorecard["solve_residual_rel"] = ans.free_residual_rel;
        scorecard["health_ok"] = health_ok;
        out.line["scorecard"] = std::move(scorecard);

        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        // solve_suspect: residual/reaction/orphan gate failed — answers recorded
        // but accuracy scores zeroed so analyze can filter untrusted runs.
        out.line["status"] = health_ok ? "ok" : "solve_suspect";
        stamp_wall(out.line);

        out.mesh = std::move(vol);

        beat.set_phase("done", 1.0);
    } catch (const WallClockBudgetExceeded& e) {
        // M14: mid-solve (or pre-mesh) wall-clock kill.
        out.line["status"] = "over_budget";
        out.line["over_budget_cause"] = "wall_clock";
        out.line["error"] = e.what();
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
        beat.set_phase("done", 1.0);
    } catch (const pipeline::GeometryVolumeLimitError& e) {
        out.line["status"] = "mesh_fail";
        out.line["error"] = e.what();
        // A resolution refusal fires before any mesh exists, so there is nothing
        // to measure and `available` is false. Emitting relative_error's 0.0 there
        // would state a value this guard never computed, and 0.0 reads as a
        // PERFECT volume match to anyone who does not also check a sibling flag.
        // Say "not measured" explicitly instead; null is the honest answer.
        const char* const volume_field =
            e.solved_stage ? "geometry_volume_err" : "geometry_fill_volume_err";
        if (e.assessment.available) {
            out.line[volume_field] = e.assessment.relative_error;
        } else {
            out.line[volume_field] = nullptr;
        }
        out.line["geometry_volume_measured"] = e.assessment.available;
        out.line["advisor_training_eligible"] = false;
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
        beat.set_phase("done", 1.0);
    } catch (const fea::FeaError& e) {
        out.line["status"] = "solve_fail";
        out.line["error"] = e.what();
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
    } catch (const std::exception& e) {
        // Mesh / I/O / validity failures.
        const std::string msg = e.what();
        out.line["status"] = (msg.find("mesh") != std::string::npos ||
                              msg.find("validity") != std::string::npos)
                                 ? "mesh_fail"
                                 : "solve_fail";
        out.line["error"] = msg;
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        stamp_wall(out.line);
    }
    return out;
}

double scalar_score(const Campaign& camp, double accuracy, double mesh_ms, double solve_ms) {
    // Soft inverse-time maps ms → (0,1]; accuracy already in [0,1].
    const double s_mesh = 1.0 / (1.0 + mesh_ms / 1000.0);
    const double s_solve = 1.0 / (1.0 + solve_ms / 1000.0);
    return camp.w_accuracy * accuracy + camp.w_mesh_ms * s_mesh + camp.w_solve_ms * s_solve;
}

// ── campaign orchestration ──────────────────────────────────────────────────

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

int usage() {
    std::fputs("usage: polymesh_testlab run|resume|validate|pause-status <campaign_dir>\n"
               "                        [--advisor <model_dir>]\n"
               "\n"
               "  run           start (or restart) a campaign from campaign.json\n"
               "  resume        continue from checkpoint.json after pause / SIGINT\n"
               "  validate      parse campaign, grid, cases, and print maximum run count\n"
               "  pause-status  print checkpoint state (running|paused|finished)\n"
               "\n"
               "  --advisor DIR records what the learned advisor would have chosen for\n"
               "                each case as the row's advisor_decision field. The grid\n"
               "                still decides what actually runs — the decision is an\n"
               "                extra observable, never an override.\n"
               "\n"
               "Schemas: docs/dag/interfaces.md. Run from the repo root so case and\n"
               "bench/reference paths resolve. SIGINT after a run finishes → paused.\n",
               stderr);
    return 2;
}

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

/// Interpreter for the post-campaign hooks (ADR-0022).
///
/// `python3` is NOT a safe default. On this workstation it resolves to a bare
/// C:\Python314 that has no numpy, while every other tool in the repo runs under
/// the Python311 that does -- so `warehouse_shots.py` raised ModuleNotFoundError
/// and rendered nothing for an entire campaign regeneration, reporting it only as
/// an exit code in a log. Resolution order:
///   1. $POLYMESH_PYTHON -- scripts/advisor/run_batch.py sets this to its own
///      sys.executable, so a campaign driven by the runner uses EXACTLY the
///      interpreter the runner itself is running under.
///   2. `python`, when it runs at all. Mirrors python_exe() in the Catch2 tests,
///      which has picked the working interpreter on Windows all along.
///   3. `python3`, the POSIX fallback.
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
                const RunOutcome ro =
                    run_one(cfg, part, tier, ts.h_scale, progress_path, mesh_preview_path,
                            wh_dir, run_limit, advisor, camp.max_dof, camp.max_elems);
                results_app << ro.line.dump() << '\n';
                results_app.flush();
                // A silently dropped row is worse than a stopped campaign: the
                // run is finished and its per-run result.json is already on
                // disk, so failing here loses nothing recoverable (see
                // scripts/advisor/rebuild_results.py) whereas continuing would
                // report success while the summary quietly lost work. Same
                // check-after-flush idiom as atomic_write() above.
                if (!results_app) {
                    throw std::runtime_error("failed appending row to " +
                                             results_path.string());
                }
                // Artifacts AFTER the row: the row is the record we cannot
                // regenerate, the artifacts are rebuildable from it (and by
                // scripts/advisor/rebuild_results.py in reverse). Writing them
                // second makes an artifact failure unable to cost a row by
                // construction, not merely by the guard inside the writer.
                if (!wh_dir.empty()) {
                    write_warehouse_run(wh_dir, ro.line,
                                        ro.mesh ? &*ro.mesh : nullptr);
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

    // Optional post-campaign hooks (ADR-0022). A hook failure does not fail the
    // campaign -- no hook touches results.jsonl -- but it IS reported in the
    // summary below, because an exit code alone went unnoticed for thousands of
    // runs while warehouse_shots rendered nothing.
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
        // V9b: mesh.vtu → wire.png for HANDOFF / review, consumed by
        // write_grok_handoff.py (shots) and scripts/advisor/report.py (WIRE_NAMES).
        run_hook("warehouse_shots", "scripts/warehouse_shots.py");
    }
    if (camp.on_finish_analyze) {
        run_hook("analyze", "scripts/analyze_campaign.py");
    }
    if (camp.on_finish_grok) {
        run_hook("grok handoff", "scripts/write_grok_handoff.py");
    }
    if (!hook_failures.empty()) {
        std::string joined;
        for (const auto& failure : hook_failures) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += failure;
        }
        // Three surfaces, because an exit code and 153 stack traces in run.log were
        // already there and still went unread for a whole regeneration: the terminal
        // summary, progress.json, and the checkpoint beside `state: finished` --
        // which is the marker a downstream consumer polls to conclude success.
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

} // namespace

int main(int argc, char** argv) {
    fea::init_runtime_performance();
    if (argc < 3) {
        return usage();
    }
    const std::string_view cmd = argv[1];
    const fs::path camp_dir = argv[2];
    fs::path advisor_dir;
    for (int i = 3; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--advisor" && i + 1 < argc) {
            advisor_dir = argv[++i];
        } else {
            return usage();
        }
    }
    try {
        std::unique_ptr<AdvisorScorer> advisor;
        if (!advisor_dir.empty()) {
            advisor = std::make_unique<AdvisorScorer>(advisor_dir);
        }
        if (cmd == "run") {
            return run_campaign(camp_dir, /*resume=*/false, advisor.get());
        }
        if (cmd == "resume") {
            return run_campaign(camp_dir, /*resume=*/true, advisor.get());
        }
        if (cmd == "validate") {
            return cmd_validate(camp_dir);
        }
        if (cmd == "pause-status") {
            return cmd_pause_status(camp_dir);
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "polymesh_testlab: %s\n", e.what());
        return 1;
    }
}
