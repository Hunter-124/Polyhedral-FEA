// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Durable artifact writing for polymesh_testlab (result.json / quality.json /
// checkpoint.json). Kept header-only so unit tests can exercise the two
// contracts that matter without linking the full campaign runner:
//
//   1. atomic_write() survives a TRANSIENT reader holding the destination open.
//      On Windows fs::rename is MoveFileExW(REPLACE_EXISTING), which fails with
//      a sharing violation while any other process has the target open. Our own
//      campaign monitoring -- peer agents reading result.json to compute
//      progress statistics -- was enough to make this fire in production.
//   2. write_run_json() NEVER throws. It is called from run_one()'s exception
//      handlers, where a throw would escape run_one entirely, past its own
//      catch-all, and abort the whole campaign process via main() -- losing the
//      status row that the handler exists to produce. An artifact is optional
//      and regenerable; the summary row is neither.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace polymesh::testlab {

namespace fs = std::filesystem;

/// Attempts (1 initial + kRenameRetries retries) before a transient rename
/// failure is reported as a real error. 5 ms doubling gives ~155 ms total,
/// which comfortably outlasts a stat/read by a monitoring process while staying
/// invisible next to a 12-180 s run.
inline constexpr int kRenameRetries = 5;
inline constexpr int kRenameBackoffMs = 5;

/// True for the error codes a *momentary* reader produces, as opposed to a
/// genuine I/O fault. On Windows these are ERROR_SHARING_VIOLATION (32),
/// ERROR_ACCESS_DENIED (5) and ERROR_LOCK_VIOLATION (33); comparing against
/// std::errc goes through default_error_condition so we do not hard-code the
/// Win32 numbers. A truly read-only destination also lands here, and costs the
/// full backoff before being reported -- which is the right trade: a real
/// failure still surfaces, it just surfaces ~155 ms later.
inline bool is_transient_rename_error(const std::error_code& ec) {
    return ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy ||
           ec == std::errc::no_lock_available ||
           ec == std::errc::resource_unavailable_try_again;
}

/// Write `text` to `path` via tmp + flush + checked + rename. Throws
/// std::runtime_error / fs::filesystem_error on genuine failure; callers that
/// must not fail are responsible for catching (see write_run_json).
inline void atomic_write(const fs::path& path, const std::string& text) {
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
    std::error_code ec;
    for (int attempt = 0;; ++attempt) {
        ec.clear();
        fs::rename(tmp, path, ec);
        if (!ec) {
            return;
        }
        if (attempt >= kRenameRetries || !is_transient_rename_error(ec)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRenameBackoffMs << attempt));
    }
    // Do not leave a .tmp behind to accumulate across a 3456-run campaign; the
    // exception message carries the reason.
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    throw std::runtime_error("cannot rename " + tmp.string() + " -> " + path.string() + ": " +
                             ec.message());
}

/// Write the per-run result.json (and quality.json when present) under
/// `run_dir`. Returns false and warns on stderr instead of throwing: this runs
/// inside run_one's exception handlers, where a throw would cost the status row
/// and the rest of the campaign. Never let bookkeeping outrank the result.
inline bool write_run_json(const fs::path& run_dir, const nlohmann::json& line) noexcept {
    try {
        std::error_code ec;
        fs::create_directories(run_dir, ec);
        // An ignored failure here guarantees the writes below fail for a reason
        // nobody can see, so report it and stop rather than compounding it.
        if (ec && !fs::is_directory(run_dir)) {
            std::fprintf(stderr, "warehouse: cannot create %s: %s\n", run_dir.string().c_str(),
                         ec.message().c_str());
            return false;
        }
        atomic_write(run_dir / "result.json", line.dump(2) + "\n");
        if (line.contains("quality")) {
            atomic_write(run_dir / "quality.json", line["quality"].dump(2) + "\n");
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "warehouse: result/quality write failed: %s\n", e.what());
        return false;
    } catch (...) {
        std::fprintf(stderr, "warehouse: result/quality write failed (unknown)\n");
        return false;
    }
}

} // namespace polymesh::testlab
