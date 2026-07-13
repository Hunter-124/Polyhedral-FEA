// SPDX-License-Identifier: BSD-3-Clause

// polymesh_testlab — campaign runner with successive-halving, SIGINT pause,
// and atomic checkpointing. Normative schemas: docs/dag/interfaces.md.
//
// Anti-cheat: reference truths are loaded only from paths declared in case
// files (bench/reference/*). No numeric answers are embedded here.

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
#include "mesh/surface_metrics.hpp"
#include "pipeline/scene.hpp"
#include "probe_util.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/SparseCore>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

void atomic_write(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write " + tmp.string());
        }
        out << text;
        out.flush();
        if (!out) {
            throw std::runtime_error("failed writing " + tmp.string());
        }
    }
    fs::rename(tmp, path);
}

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
};

struct LoadSpec {
    Box3 box;
    Eigen::Vector3d traction = Eigen::Vector3d::Zero(); // N/m^2
};

struct ProbeSpec {
    std::string kind; // max_von_mises | max_vm_over_nominal | max_displacement
    double nominal = 0.0;
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
    bool feature_refine = false;
    double curvature_turn_deg = 15.0; // recorded; product path uses 15° today
    bool snap_boundary = true;
    int order = 1;
    double element_tendency = 0.0;
};

struct Checkpoint {
    std::string campaign;
    std::string state = "running"; // running | paused | finished
    int tier = 0;
    int completed_runs = 0;
    std::vector<std::string> survivors;
    std::string started_utc;
    std::string updated_utc;
};

// ── parsers ─────────────────────────────────────────────────────────────────

Box3 parse_box(const json& j) {
    // [[xmin,ymin,zmin],[xmax,ymax,zmax]]
    if (!j.is_array() || j.size() != 2 || !j[0].is_array() || !j[1].is_array() ||
        j[0].size() != 3 || j[1].size() != 3) {
        throw std::runtime_error("box must be [[xmin,ymin,zmin],[xmax,ymax,zmax]]");
    }
    Box3 b;
    b.lo = Eigen::Vector3d(j[0][0].get<double>(), j[0][1].get<double>(), j[0][2].get<double>());
    b.hi = Eigen::Vector3d(j[1][0].get<double>(), j[1][1].get<double>(), j[1][2].get<double>());
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
    return c;
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
            if (L.contains("traction") && L["traction"].is_array() && L["traction"].size() == 3) {
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
    throw std::runtime_error("unknown mesher '" + name + "'");
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
    atomic_write(path, j.dump(2) + "\n");
}

void write_progress(const fs::path& path, const std::string& phase, double phase_frac,
                    double elapsed_ms, const std::string& cfg_id, const std::string& part,
                    int tier, int cg_iter = -1, double cg_resid = -1.0, std::size_t n_elems = 0,
                    std::size_t n_nodes = 0) {
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
                           cfg_id_, part_, tier_, cg, cg_resid_.load(std::memory_order_relaxed),
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

fea::Dirichlet make_dirichlet(const fea::NodalMesh& mesh, const std::vector<BcSpec>& bcs) {
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

Eigen::VectorXd make_loads(const fea::NodalMesh& mesh, const std::vector<LoadSpec>& loads) {
    const auto all_faces = free_faces_as_surface(mesh);
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (const auto& L : loads) {
        std::vector<fea::SurfaceFace> selected;
        for (const auto& face : all_faces) {
            if (L.box.contains(face_centroid(mesh, face))) {
                selected.push_back(face);
            }
        }
        if (selected.empty()) {
            // Fallback: distribute total force F = traction * bbox_area_proxy onto
            // nodes in the box (node-lump). Area proxy is not exact; prefer faces.
            std::vector<std::uint32_t> nodes;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
                if (L.box.contains(mesh.nodes[i])) {
                    nodes.push_back(i);
                }
            }
            if (nodes.empty()) {
                continue;
            }
            // Approximate face area from node bbox on the selected set.
            Eigen::Vector3d lo = mesh.nodes[nodes.front()];
            Eigen::Vector3d hi = lo;
            for (auto n : nodes) {
                lo = lo.cwiseMin(mesh.nodes[n]);
                hi = hi.cwiseMax(mesh.nodes[n]);
            }
            const Eigen::Vector3d ext = (hi - lo).cwiseMax(1e-30);
            // Smallest extent is the face normal direction; area ≈ product of other two.
            const double area = ext[0] * ext[1] * ext[2] /
                                std::max({ext[0], ext[1], ext[2], 1e-30});
            const Eigen::Vector3d total = L.traction * area;
            const Eigen::Vector3d per = total / static_cast<double>(nodes.size());
            for (auto n : nodes) {
                f.segment<3>(3 * static_cast<Eigen::Index>(n)) += per;
            }
            continue;
        }
        const Eigen::Vector3d t = L.traction;
        f += fea::assemble_traction_load(mesh, selected,
                                         [&](const Eigen::Vector3d&) { return t; });
    }
    return f;
}

struct ProbeAnswers {
    double sigma_max = 0.0;          // global max von Mises (diagnostic / SCF)
    double tip_deflection = 0.0;     // PRIMARY: face-mean |u| on tip/load faces
    double tip_deflection_max = 0.0; // diagnostic: global max |u|
    double mean_u_component = 0.0;   // signed mean of dominant load-dir component
    double mean_ux = 0.0;            // signed mean ux on probe nodes
    double mean_uz = 0.0;            // signed mean uz on probe nodes
    int dominant_load_axis = 2;      // 0=x,1=y,2=z
    double reaction_sum_err = 0.0;   // |F+R| / max(|F|,eps) relative
    double free_residual_rel = 0.0;  // ||(Ku-f)_free|| / ||f||
    int n_orphan_nodes = 0;          // nodes never referenced by any element
    int n_bc_dofs = 0;
    int n_load_faces = 0;
    int n_probe_nodes = 0;
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

/// Unique nodes on boundary faces whose centroids fall in `box`.
std::vector<std::uint32_t> nodes_on_faces_in_box(const fea::NodalMesh& mesh,
                                                 const Box3& box,
                                                 int* n_faces_out = nullptr) {
    const auto all_faces = free_faces_as_surface(mesh);
    std::set<std::uint32_t> unique;
    int n_faces = 0;
    for (const auto& face : all_faces) {
        if (!box.contains(face_centroid(mesh, face))) {
            continue;
        }
        ++n_faces;
        for (const auto ni : face.nodes) {
            unique.insert(ni);
        }
    }
    if (n_faces_out != nullptr) {
        *n_faces_out = n_faces;
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
    a.reaction_sum_err =
        (F_sum + R_sum).norm() / std::max(F_sum.norm(), kEps);
    a.n_bc_dofs = static_cast<int>(bc.dof_values.size());
}

ProbeAnswers compute_probes(const fea::NodalMesh& mesh, const fea::Material& mat,
                            const Eigen::VectorXd& u,
                            const std::vector<LoadSpec>& loads,
                            const fea::Dirichlet& bc, const Eigen::VectorXd& f) {
    ProbeAnswers a;
    a.n_orphan_nodes = count_orphan_nodes(mesh);
    a.tip_deflection_max = tlab::global_max_displacement_mag(u, mesh.nodes.size());

    const auto stress = fea::recover_nodal_stress(mesh, mat, u);
    for (std::size_t i = 0; i < stress.size(); ++i) {
        a.sigma_max = std::max(a.sigma_max, fea::von_mises(stress[i]));
    }

    // PRIMARY tip probe: face-mean |u| on first load-box face set.
    std::vector<std::uint32_t> probe_nodes;
    if (!loads.empty()) {
        a.dominant_load_axis = tlab::dominant_axis(loads.front().traction);
        int n_faces = 0;
        probe_nodes = nodes_on_faces_in_box(mesh, loads.front().box, &n_faces);
        a.n_load_faces = n_faces;
        if (probe_nodes.empty()) {
            // Fall back to nodes in the load box — never global max as primary.
            probe_nodes = nodes_in_box(mesh, loads.front().box);
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
        // No load selector match at all — leave tip_deflection at 0 so the
        // metric fails closed rather than reporting a global-max lie.
        a.tip_deflection = 0.0;
    }

    compute_solve_health(mesh, mat, bc, f, u, a);
    return a;
}

double evaluate_probe(const ProbeSpec& probe, const ProbeAnswers& a) {
    // Canonical kinds (interfaces.md §5) plus short aliases used in hand-calcs.
    // Displacement probes use face-mean primary (not global max).
    if (probe.kind == "max_von_mises" || probe.kind == "max_vm") {
        return a.sigma_max;
    }
    if (probe.kind == "max_vm_over_nominal" || probe.kind == "scf") {
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("max_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_max / probe.nominal;
    }
    if (probe.kind == "max_displacement" || probe.kind == "tip_deflection") {
        return a.tip_deflection; // face-mean |u| on load select box
    }
    if (probe.kind == "mean_ux_on_face") {
        // Prefer signed dominant-load component when traction is primarily x.
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
    json normal_dev = nullptr;

    if (model.cad && !model.cad->empty() && h > 0.0) {
        try {
            const geom::CadTopology topo = geom::extract_topology(*model.cad, 8);
            if (!topo.empty()) {
                // Boundary nodes as crude sample set vs CAD edge polylines.
                std::set<std::uint32_t> bnodes;
                for (const auto& face : free_faces_as_surface(mesh)) {
                    for (const auto ni : face.nodes) {
                        bnodes.insert(ni);
                    }
                }
                std::vector<Eigen::Vector3d> samples;
                samples.reserve(bnodes.size());
                for (const auto ni : bnodes) {
                    samples.push_back(mesh.nodes[ni]);
                }
                if (!samples.empty()) {
                    const double hd = geom::edge_profile_hausdorff(topo, samples);
                    edge_hd = hd / h;
                }
            }
        } catch (...) {
            edge_hd = nullptr;
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

    return {{"edge_hausdorff_over_h", edge_hd}, {"normal_dev_deg_max", normal_dev}};
}

json geom_class_of(const pipeline::Model& model, double h_ref) {
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseAbs();
    const double min_ext = ext.minCoeff();
    const double max_ext = ext.maxCoeff();
    // Faceted "curved" proxy: many triangles relative to 12-tri box.
    const double ntri = static_cast<double>(model.surface.triangles.size());
    const double curved_frac = std::clamp((ntri - 12.0) / std::max(ntri, 1.0), 0.0, 1.0);
    const bool thin = (h_ref > 0.0) && (min_ext < 2.5 * h_ref);
    const double min_feature_h = (h_ref > 0.0) ? (min_ext / h_ref) : 0.0;
    (void)max_ext;
    return {{"curved_frac", curved_frac},
            {"thin", thin},
            {"min_feature_h", min_feature_h}};
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
    const auto m =
        mesh::evaluate_curved_mesh_quality(model.surface, mesh.nodes, faces, h, -1.0, -1.0,
                                           nullptr, tet_ptr);
    return {{"M1max", m.m1_max},
            {"M2max", m.m2_max},
            {"M6", m.m6_min_boundary_aspect},
            {"score", m.composite_score}};
}

// ── single run ──────────────────────────────────────────────────────────────

struct RunOutcome {
    json line; // results.jsonl object
    double accuracy_score = 0.0; // 0..1 mean over metrics
    double mesh_ms = 0.0;
    double solve_ms = 0.0;
};

void write_warehouse_run(const fs::path& run_dir, const json& line,
                         const pipeline::VolumeMeshOutput* vol) {
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    atomic_write(run_dir / "result.json", line.dump(2) + "\n");
    if (line.contains("quality")) {
        atomic_write(run_dir / "quality.json", line["quality"].dump(2) + "\n");
    }
    if (vol != nullptr) {
        try {
            fea::write_vtu(run_dir / "mesh.vtu", vol->mesh);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warehouse: write_vtu failed: %s\n", e.what());
        }
    }
}

RunOutcome run_one(const Config& cfg, const PartCase& part, int tier, double h_scale,
                   const fs::path& progress_path, const fs::path& mesh_preview_path,
                   const fs::path& warehouse_run_dir = {}) {
    using clock = std::chrono::steady_clock;
    const auto t_all0 = clock::now();

    RunOutcome out;
    out.line["cfg_id"] = cfg.id;
    out.line["config"] = cfg.values;
    out.line["part"] = part.part;
    out.line["tier"] = tier;

    ProgressHeartbeat beat(progress_path, "mesh", cfg.id, part.part, tier, t_all0);

    try {
        const auto model = pipeline::Model::load(part.geometry);
        const auto resolved = pipeline::resolve_mesh_size(model, 0.0);
        const double h = std::max(resolved.h * h_scale, 1e-9);
        out.line["geom_class"] = geom_class_of(model, resolved.h);

        const auto t_mesh0 = clock::now();
        // skin_layers=2, feature_refine from grid; empty refine_seeds; tendency dial.
        auto vol = pipeline::volume_mesh(model, h, cfg.mesher, /*skin_layers=*/2,
                                         cfg.feature_refine, /*refine_seeds=*/{},
                                         /*seed_band=*/0.0, cfg.element_tendency);
        // Validity is mandatory before any solve (anti-cheat / engineering rule).
        vol.mesh.check_validity();
        if (cfg.order >= 2) {
            vol.mesh = fea::promote_to_quadratic(vol.mesh);
            vol.mesh.check_validity();
        }
        const auto t_mesh1 = clock::now();
        out.mesh_ms =
            std::chrono::duration<double, std::milli>(t_mesh1 - t_mesh0).count();

        out.line["n_elems"] = vol.mesh.elements.size();
        out.line["n_nodes"] = vol.mesh.nodes.size();
        const long long n_dof = 3 * static_cast<long long>(vol.mesh.nodes.size());
        out.line["n_dof"] = n_dof;
        out.line["quality"] = quality_of(model, vol.mesh, h);

        beat.set_mesh_stats(vol.mesh.elements.size(), vol.mesh.nodes.size());
        write_mesh_preview(mesh_preview_path, vol);

        // Campaign budget: skip pathological meshes so one hybrid_vem case cannot
        // pin the overnight runner for tens of minutes. Tunable later via campaign.json.
        constexpr long long kMaxCampaignDof = 80000;
        constexpr long long kMaxCampaignElems = 60000;
        if (n_dof > kMaxCampaignDof ||
            static_cast<long long>(vol.mesh.elements.size()) > kMaxCampaignElems) {
            out.line["status"] = "over_budget";
            out.line["error"] = "mesh exceeds campaign DOF/elem budget (" +
                                std::to_string(n_dof) + " dof, " +
                                std::to_string(vol.mesh.elements.size()) + " elems)";
            out.line["mesh_ms"] = out.mesh_ms;
            out.line["solve_ms"] = 0.0;
            out.accuracy_score = 0.0;
            if (!warehouse_run_dir.empty()) {
                write_warehouse_run(warehouse_run_dir, out.line, &vol);
            }
            beat.set_phase("done", 1.0);
            return out;
        }

        beat.set_phase("assemble", 0.0);

        const fea::Material mat{.youngs_modulus = part.E, .poissons_ratio = part.nu};
        const auto bc = make_dirichlet(vol.mesh, part.bcs);
        if (bc.dof_values.empty()) {
            throw std::runtime_error("no Dirichlet DOFs matched BC boxes for part " +
                                     part.part);
        }
        const auto loads = make_loads(vol.mesh, part.loads);
        if (loads.norm() == 0.0) {
            throw std::runtime_error("zero load vector for part " + part.part);
        }

        beat.set_phase("solve", 0.0);

        const auto t_solve0 = clock::now();
        fea::SolveOptions sopt;
        // Short campaigns prefer robust direct LDLT; CG was hanging / failing
        // on poorly conditioned hybrid meshes at moderate free DOF.
        sopt.method = fea::SolveMethod::kDirect;
        sopt.on_progress = [&](int iter, int max_iters, double resid) {
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
        out.solve_ms =
            std::chrono::duration<double, std::milli>(t_solve1 - t_solve0).count();

        beat.set_phase("recover", 0.5);

        const ProbeAnswers ans =
            compute_probes(vol.mesh, mat, u, part.loads, bc, loads);
        out.line["answers"] = {{"sigma_max", ans.sigma_max},
                               {"tip_deflection", ans.tip_deflection},
                               {"tip_deflection_max", ans.tip_deflection_max},
                               {"mean_u_component", ans.mean_u_component},
                               {"n_probe_nodes", ans.n_probe_nodes},
                               {"n_load_faces", ans.n_load_faces}};

        // Health gates: residual / reaction / orphans. Fail-closed for accuracy.
        constexpr double kFreeResidTolDirect = 1e-6;
        constexpr double kReactionSumTol = 0.05;
        const bool health_ok =
            (ans.n_orphan_nodes == 0) &&
            (ans.free_residual_rel <= kFreeResidTolDirect) &&
            (ans.reaction_sum_err <= kReactionSumTol);
        out.line["health"] = {{"free_residual_rel", ans.free_residual_rel},
                              {"reaction_sum_err", ans.reaction_sum_err},
                              {"n_orphans", ans.n_orphan_nodes},
                              {"n_bc_dofs", ans.n_bc_dofs},
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
            const double rel =
                (std::abs(truth) > 0.0) ? std::abs(measured - truth) / std::abs(truth)
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

        if (!warehouse_run_dir.empty()) {
            write_warehouse_run(warehouse_run_dir, out.line, &vol);
        }

        beat.set_phase("done", 1.0);
    } catch (const fea::FeaError& e) {
        out.line["status"] = "solve_fail";
        out.line["error"] = e.what();
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        if (!warehouse_run_dir.empty()) {
            write_warehouse_run(warehouse_run_dir, out.line, nullptr);
        }
    } catch (const std::exception& e) {
        // Mesh / I/O / validity failures.
        const std::string msg = e.what();
        out.line["status"] =
            (msg.find("mesh") != std::string::npos || msg.find("validity") != std::string::npos)
                ? "mesh_fail"
                : "solve_fail";
        out.line["error"] = msg;
        out.line["mesh_ms"] = out.mesh_ms;
        out.line["solve_ms"] = out.solve_ms;
        out.accuracy_score = 0.0;
        if (!warehouse_run_dir.empty()) {
            write_warehouse_run(warehouse_run_dir, out.line, nullptr);
        }
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
    std::sort(candidates.begin(), candidates.end(), [&](const std::string& a, const std::string& b) {
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
    std::fputs(
        "usage: polymesh_testlab run|resume|pause-status <campaign_dir>\n"
        "\n"
        "  run           start (or restart) a campaign from campaign.json\n"
        "  resume        continue from checkpoint.json after pause / SIGINT\n"
        "  pause-status  print checkpoint state (running|paused|finished)\n"
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

int run_campaign(const fs::path& camp_dir, bool resume) {
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
        {
            std::ofstream trunc(results_path, std::ios::trunc);
        }
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

    for (int tier = cp.tier; tier < static_cast<int>(camp.tiers.size()); ++tier) {
        cp.tier = tier;
        const TierSpec& ts = camp.tiers[static_cast<std::size_t>(tier)];
        std::printf("tier %d: h_scale=%.4g keep_frac=%.3g survivors=%zu\n", tier, ts.h_scale,
                    ts.keep_frac, cp.survivors.size());

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
            const Config& cfg = by_id.at(cfg_id);
            for (const auto& part : parts) {
                const std::string key =
                    cfg_id + "|" + part.part + "|" + std::to_string(tier);
                if (done.count(key)) {
                    continue; // resume skip
                }

                std::printf("  run %s part=%s tier=%d ...\n", cfg_id.c_str(), part.part.c_str(),
                            tier);
                std::fflush(stdout);
                fs::path wh_dir;
                if (camp.warehouse) {
                    wh_dir = camp_dir / "runs" / cfg_id / part.part /
                             ("t" + std::to_string(tier));
                }
                const RunOutcome ro = run_one(cfg, part, tier, ts.h_scale, progress_path,
                                              mesh_preview_path, wh_dir);
                results_app << ro.line.dump() << '\n';
                results_app.flush();
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

    // Optional post-campaign hooks (ADR-0022). Failures here do not fail the campaign.
    if (camp.warehouse) {
        // V9b: mesh.vtu → wire.png for HANDOFF / review (best-effort).
        const int rc =
            std::system(("python3 scripts/warehouse_shots.py " + camp.name).c_str());
        if (rc != 0) {
            std::fprintf(stderr, "on_finish warehouse_shots exited %d\n", rc);
        }
    }
    if (camp.on_finish_analyze) {
        const int rc = std::system(("python3 scripts/analyze_campaign.py " + camp.name).c_str());
        if (rc != 0) {
            std::fprintf(stderr, "on_finish analyze exited %d\n", rc);
        }
    }
    if (camp.on_finish_grok) {
        const int rc =
            std::system(("python3 scripts/write_grok_handoff.py " + camp.name).c_str());
        if (rc != 0) {
            std::fprintf(stderr, "on_finish grok handoff exited %d\n", rc);
        }
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
    try {
        if (cmd == "run") {
            return run_campaign(camp_dir, /*resume=*/false);
        }
        if (cmd == "resume") {
            return run_campaign(camp_dir, /*resume=*/true);
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
