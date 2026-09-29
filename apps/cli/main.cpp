// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.
// Command dispatcher; each command lives in its own commands_*.cpp.

#include "cli_common.hpp"

#include "fea/backend.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string_view>

int main(int argc, char** argv) {
    namespace cli = polymesh::cli;
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() == 2 &&
        (std::strcmp(args[1], "--version") == 0 || std::strcmp(args[1], "-V") == 0)) {
        std::printf("polymesh %s\n", POLYMESH_VERSION);
        return 0;
    }
    polymesh::fea::init_runtime_performance();
    if (args.size() < 2) {
        return cli::usage();
    }
    const std::string_view command = args[1];
    try {
        if (command == "check") {
            return cli::cmd_check(args);
        }
        if (command == "mesh") {
            return cli::cmd_mesh(args);
        }
        if (command == "solve") {
            return cli::cmd_solve(args);
        }
        if (command == "diag") {
            return cli::cmd_diag(args);
        }
        if (command == "render") {
            return cli::cmd_render(args);
        }
        if (command == "calibrate") {
            return cli::cmd_calibrate(args);
        }
        if (command == "backend" && args.size() == 2) {
            return cli::cmd_backend();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return cli::usage();
}
