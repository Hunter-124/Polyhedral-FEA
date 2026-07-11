// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Minimal process supervisor for the external polymesh_testlab harness.
// The GUI never links apps/testlab — it only fork/execs the binary and talks
// through on-disk interfaces (docs/dag/interfaces.md). Pause = SIGINT so the
// harness can write an atomic checkpoint before stopping (same contract as a
// terminal Ctrl-C).

#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace polymesh::gui {

class ProcessRunner {
  public:
    enum class State : int {
        kIdle = 0,
        kRunning,
        kExited,
    };

    ProcessRunner() = default;
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    /// Spawn `binary` with `args` (argv[1..]; argv[0] is the binary name).
    /// Optional `cwd` changes the child working directory. Returns false and
    /// sets last_error() on failure. If already running, returns false.
    bool start(const std::filesystem::path& binary, const std::vector<std::string>& args,
               const std::filesystem::path& cwd = {});

    /// Cooperative pause: send SIGINT (Unix) / CTRL_BREAK (Windows). The
    /// harness is expected to checkpoint and exit; does not force-kill.
    bool request_pause();

    /// Reap a finished child (non-blocking). Call from the UI frame loop.
    void poll();

    /// Force-kill if still running (last resort; prefer request_pause).
    void force_kill();

    State state() const { return state_; }
    int exit_code() const { return exit_code_; }
    const std::string& last_error() const { return last_error_; }
    bool is_running() const { return state_ == State::kRunning; }

#if !defined(_WIN32)
    pid_t pid() const { return pid_; }
#endif

  private:
    State state_ = State::kIdle;
    int exit_code_ = 0;
    std::string last_error_;

#if defined(_WIN32)
    HANDLE process_ = nullptr;
    DWORD pid_win_ = 0;
#else
    pid_t pid_ = -1;
#endif
};

/// Locate polymesh_testlab: explicit override, then common build paths relative
/// to `repo_root` (or CWD), then PATH. Empty if not found.
std::filesystem::path find_testlab_binary(const std::filesystem::path& override_path = {},
                                          const std::filesystem::path& repo_root = {});

} // namespace polymesh::gui
