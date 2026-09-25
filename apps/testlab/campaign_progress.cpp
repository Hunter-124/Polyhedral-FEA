// SPDX-License-Identifier: BSD-3-Clause

// progress.json heartbeat and mesh_preview.pmp for the GUI live view
// (docs/dag/interfaces.md §6).

#include "run_artifacts.hpp"
#include "testlab_internal.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace polymesh::testlab::detail {

void write_progress(const fs::path& path, const std::string& phase, double phase_frac,
                    double elapsed_ms, const std::string& cfg_id, const std::string& part,
                    int tier, int cg_iter, double cg_resid, std::size_t n_elems,
                    std::size_t n_nodes, const std::vector<std::string>& hooks_failed) {
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

ProgressHeartbeat::ProgressHeartbeat(fs::path path, std::string phase, std::string cfg_id,
                                     std::string part, int tier,
                                     std::chrono::steady_clock::time_point t0)
    : path_(std::move(path)), phase_(std::move(phase)), cfg_id_(std::move(cfg_id)),
      part_(std::move(part)), tier_(tier), t0_(t0), stop_(false) {
    thr_ = std::thread([this] { loop(); });
}

void ProgressHeartbeat::set_phase(std::string phase, double phase_frac) {
    {
        const std::lock_guard lock(mu_);
        phase_ = std::move(phase);
    }
    phase_frac_.store(phase_frac, std::memory_order_relaxed);
    tick_now();
}

void ProgressHeartbeat::set_frac(double f) { phase_frac_.store(f, std::memory_order_relaxed); }

void ProgressHeartbeat::set_cg(int iter, double resid) {
    cg_iter_.store(iter, std::memory_order_relaxed);
    cg_resid_.store(resid, std::memory_order_relaxed);
}

void ProgressHeartbeat::set_mesh_stats(std::size_t n_elems, std::size_t n_nodes) {
    n_elems_.store(n_elems, std::memory_order_relaxed);
    n_nodes_.store(n_nodes, std::memory_order_relaxed);
}

ProgressHeartbeat::~ProgressHeartbeat() {
    stop_.store(true, std::memory_order_relaxed);
    if (thr_.joinable()) {
        thr_.join();
    }
}

void ProgressHeartbeat::tick_now() {
    try {
        std::string phase;
        {
            const std::lock_guard lock(mu_);
            phase = phase_;
        }
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_)
                .count();
        const int cg = cg_iter_.load(std::memory_order_relaxed);
        write_progress(path_, phase, phase_frac_.load(std::memory_order_relaxed), ms, cfg_id_,
                       part_, tier_, cg, cg_resid_.load(std::memory_order_relaxed),
                       n_elems_.load(std::memory_order_relaxed),
                       n_nodes_.load(std::memory_order_relaxed));
    } catch (...) {
        // Progress is best-effort; never fail the run for a status write.
    }
}

void ProgressHeartbeat::loop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        tick_now();
        for (int i = 0; i < 10 && !stop_.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    tick_now(); // final stamp
}

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

} // namespace polymesh::testlab::detail
