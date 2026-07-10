// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI. Subcommands grow with the phases; `check` is the P0 stub.

#include "fea/backend.hpp"
#include "geom/stl.hpp"

#include <cstdio>
#include <span>
#include <string_view>

namespace {

int usage() {
    std::fputs("usage: polymesh <command> [args]\n"
               "\n"
               "commands:\n"
               "  check <input.stl>   load a geometry file, report validity + statistics\n"
               "  backend             report the active compute backend\n",
               stderr);
    return 2;
}

int cmd_check(std::string_view input) {
    const auto surface = polymesh::geom::load_stl(input);
    surface.validate();
    std::printf("%.*s: OK — %zu vertices, %zu triangles\n", static_cast<int>(input.size()),
                input.data(), surface.vertices.size(), surface.triangles.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        return usage();
    }
    const std::string_view command = args[1];
    try {
        if (command == "check" && args.size() == 3) {
            return cmd_check(args[2]);
        }
        if (command == "backend" && args.size() == 2) {
            std::printf("%s\n", polymesh::fea::backend_description().c_str());
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
