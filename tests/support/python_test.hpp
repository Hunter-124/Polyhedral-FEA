// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Launcher for tests that execute repo Python scripts. Each test keeps its own
// payload and assertions; this only writes, runs and reports. The working
// directory is the repo root (catch_discover_tests WORKING_DIRECTORY).

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace polymesh::testsupport {

/// Interpreter command. On Windows prefer a real `python`: the WindowsApps
/// `python3` may be a stub.
inline const char* python_exe() {
#if defined(_WIN32)
    if (std::system("python -c \"import sys\" >nul 2>&1") == 0) {
        return "python";
    }
    return "python3";
#else
    return "python3";
#endif
}

/// Writes `source` to <temp>/<name>.py and runs it with stdout+stderr captured
/// in <temp>/<name>.txt. A non-zero exit (e.g. a failed assert inside the
/// payload) fails the test with the captured output.
inline void run_python_script(const std::string& name, const std::string& source) {
    namespace fs = std::filesystem;
    const fs::path script = fs::temp_directory_path() / (name + ".py");
    const fs::path out = fs::temp_directory_path() / (name + ".txt");
    {
        std::ofstream stream(script);
        REQUIRE(stream.good());
        stream << source;
    }
    const std::string cmd = std::string(python_exe()) + " \"" + script.string() + "\" > \"" +
                            out.string() + "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::ifstream in(out);
        std::ostringstream text;
        text << in.rdbuf();
        FAIL("python payload failed:\n" << text.str());
    }
}

} // namespace polymesh::testsupport
