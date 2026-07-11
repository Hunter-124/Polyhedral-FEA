// SPDX-License-Identifier: BSD-3-Clause
#include "process_runner.hpp"

#include <cstdlib>
#include <format>

#if defined(_WIN32)
#include <string>
#else
#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

namespace polymesh::gui {
namespace {

bool is_executable_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) {
        return false;
    }
#if defined(_WIN32)
    const auto ext = p.extension().string();
    return ext == ".exe" || ext == ".EXE" || ext.empty();
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

} // namespace

ProcessRunner::~ProcessRunner() {
    if (is_running()) {
        force_kill();
        poll();
    }
}

#if defined(_WIN32)

bool ProcessRunner::start(const std::filesystem::path& binary,
                          const std::vector<std::string>& args,
                          const std::filesystem::path& cwd) {
    if (state_ == State::kRunning) {
        last_error_ = "process already running";
        return false;
    }
    if (binary.empty()) {
        last_error_ = "empty binary path";
        return false;
    }

    // Build a single command line: "binary" arg1 arg2 ...
    std::string cmd = "\"" + binary.string() + "\"";
    for (const auto& a : args) {
        cmd.push_back(' ');
        // Quote args that need it.
        if (a.find_first_of(" \t\"") != std::string::npos) {
            cmd.push_back('"');
            for (char c : a) {
                if (c == '"') {
                    cmd += "\\\"";
                } else {
                    cmd.push_back(c);
                }
            }
            cmd.push_back('"');
        } else {
            cmd += a;
        }
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cwd_str = cwd.empty() ? std::string() : cwd.string();
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    const BOOL ok = CreateProcessA(
        nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr,
        cwd_str.empty() ? nullptr : cwd_str.c_str(), &si, &pi);
    if (!ok) {
        last_error_ = std::format("CreateProcess failed (err {})", GetLastError());
        state_ = State::kIdle;
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_win_ = pi.dwProcessId;
    state_ = State::kRunning;
    exit_code_ = 0;
    last_error_.clear();
    return true;
}

bool ProcessRunner::request_pause() {
    if (state_ != State::kRunning || process_ == nullptr) {
        last_error_ = "no running process to pause";
        return false;
    }
    // CTRL_BREAK_EVENT is the Windows analogue of a cooperative interrupt when
    // the child is started in a new process group.
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid_win_)) {
        // Fall back to soft terminate request.
        if (!TerminateProcess(process_, 130)) {
            last_error_ = std::format("pause signal failed (err {})", GetLastError());
            return false;
        }
    }
    last_error_.clear();
    return true;
}

void ProcessRunner::poll() {
    if (state_ != State::kRunning || process_ == nullptr) {
        return;
    }
    const DWORD wait = WaitForSingleObject(process_, 0);
    if (wait == WAIT_TIMEOUT) {
        return;
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    CloseHandle(process_);
    process_ = nullptr;
    pid_win_ = 0;
    state_ = State::kExited;
}

void ProcessRunner::force_kill() {
    if (state_ != State::kRunning || process_ == nullptr) {
        return;
    }
    TerminateProcess(process_, 1);
    poll();
}

#else // Unix

bool ProcessRunner::start(const std::filesystem::path& binary,
                          const std::vector<std::string>& args,
                          const std::filesystem::path& cwd) {
    if (state_ == State::kRunning) {
        last_error_ = "process already running";
        return false;
    }
    if (binary.empty()) {
        last_error_ = "empty binary path";
        return false;
    }

    // Build argv: binary name + args + nullptr. Keep owned strings alive.
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(binary.string());
    for (const auto& a : args) {
        owned.push_back(a);
    }
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        last_error_ = std::format("fork failed: {}", std::strerror(errno));
        state_ = State::kIdle;
        return false;
    }
    if (child == 0) {
        // Child.
        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) {
                std::fprintf(stderr, "polymesh-gui: chdir(%s) failed: %s\n", cwd.c_str(),
                             std::strerror(errno));
                _exit(127);
            }
        }
        // New process group so SIGINT can target the harness alone.
        ::setpgid(0, 0);
        ::execvp(argv[0], argv.data());
        std::fprintf(stderr, "polymesh-gui: execvp(%s) failed: %s\n", argv[0],
                     std::strerror(errno));
        _exit(127);
    }

    // Parent.
    ::setpgid(child, child); // race-safe; ignore EACCES if child already set it
    pid_ = child;
    state_ = State::kRunning;
    exit_code_ = 0;
    last_error_.clear();
    return true;
}

bool ProcessRunner::request_pause() {
    if (state_ != State::kRunning || pid_ <= 0) {
        last_error_ = "no running process to pause";
        return false;
    }
    // SIGINT to the process group — same as terminal Ctrl-C for the harness.
    if (::kill(-pid_, SIGINT) != 0) {
        // Fall back to the single process if the group is gone.
        if (::kill(pid_, SIGINT) != 0) {
            last_error_ = std::format("SIGINT failed: {}", std::strerror(errno));
            return false;
        }
    }
    last_error_.clear();
    return true;
}

void ProcessRunner::poll() {
    if (state_ != State::kRunning || pid_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) {
        return; // still running
    }
    if (r < 0) {
        if (errno == ECHILD) {
            // Already reaped elsewhere.
            pid_ = -1;
            state_ = State::kExited;
            return;
        }
        last_error_ = std::format("waitpid failed: {}", std::strerror(errno));
        return;
    }
    if (WIFEXITED(status)) {
        exit_code_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code_ = 128 + WTERMSIG(status);
    } else {
        exit_code_ = -1;
    }
    pid_ = -1;
    state_ = State::kExited;
}

void ProcessRunner::force_kill() {
    if (state_ != State::kRunning || pid_ <= 0) {
        return;
    }
    ::kill(-pid_, SIGKILL);
    ::kill(pid_, SIGKILL);
    // Blocking reap so we do not leave a zombie.
    int status = 0;
    ::waitpid(pid_, &status, 0);
    if (WIFEXITED(status)) {
        exit_code_ = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code_ = 128 + WTERMSIG(status);
    }
    pid_ = -1;
    state_ = State::kExited;
}

#endif

std::filesystem::path find_testlab_binary(const std::filesystem::path& override_path,
                                          const std::filesystem::path& repo_root) {
    if (!override_path.empty() && is_executable_file(override_path)) {
        return override_path;
    }

    const std::filesystem::path roots[] = {
        repo_root,
        std::filesystem::current_path(),
        std::filesystem::current_path() / "..",
        std::filesystem::current_path() / "../..",
    };

    const char* candidates[] = {
        "build/apps/testlab/polymesh_testlab",
        "build/apps/testlab/polymesh_testlab.exe",
        "build/polymesh_testlab",
        "build/polymesh_testlab.exe",
        "apps/testlab/polymesh_testlab",
        "polymesh_testlab",
        "polymesh_testlab.exe",
    };

    for (const auto& root : roots) {
        if (root.empty()) {
            continue;
        }
        for (const char* rel : candidates) {
            const auto p = root / rel;
            if (is_executable_file(p)) {
                return std::filesystem::weakly_canonical(p);
            }
        }
    }

    // PATH lookup via which-style: try bare name through access after PATH split
    // is overkill; leave empty and let execvp search PATH when the UI passes
    // "polymesh_testlab" as the binary string.
    return {};
}

} // namespace polymesh::gui
