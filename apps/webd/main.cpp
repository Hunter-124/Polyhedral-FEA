// SPDX-License-Identifier: BSD-3-Clause
#include "http.hpp"
#include "jobs.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

namespace {

polymesh::webd::HttpServer* g_server = nullptr;

extern "C" void stop_server(int /*signal*/) {
    if (g_server != nullptr) {
        g_server->stop();
    }
}

std::filesystem::path executable_path(const char* argv0) {
    std::string buffer(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length > 0) {
        buffer.resize(static_cast<std::size_t>(length));
        return std::filesystem::path(buffer);
    }
    return std::filesystem::absolute(argv0);
}

std::filesystem::path choose_directory(const std::filesystem::path& preferred,
                                       const std::filesystem::path& fallback) {
    std::error_code error;
    if (std::filesystem::is_directory(preferred, error)) {
        return preferred;
    }
    return fallback;
}

std::filesystem::path source_examples(const std::filesystem::path& binary_dir) {
    const auto from_cwd = std::filesystem::current_path() / "bench/geometries/public";
    std::error_code error;
    if (std::filesystem::is_directory(from_cwd, error)) {
        return from_cwd;
    }
    for (auto directory = binary_dir; directory.has_parent_path();) {
        const auto candidate = directory / "bench/geometries/public";
        error.clear();
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return from_cwd;
}

std::uint16_t parse_port(std::string_view text) {
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("--port must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(value);
}

std::size_t parse_size(std::string_view text, std::string_view option) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::string(option) + " must be a nonnegative integer");
    }
    return value;
}

void usage() {
    std::fputs("usage: polymesh-webd [--host ADDRESS] [--port N] [--web-root DIR]\n"
               "                     [--examples-dir DIR] [--advisor DIR]\n"
               "                     [--max-mem-gb N] [--max-elems N] [--max-dof N]\n",
               stderr);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto executable = executable_path(argv[0]);
        const auto binary_dir = executable.parent_path();
        std::string host = "127.0.0.1";
        std::uint16_t port = 8770;
        std::filesystem::path web_root = choose_directory(
            binary_dir / "../share/polymesh/web", std::filesystem::current_path() / "web");
        std::filesystem::path examples_dir = choose_directory(
            binary_dir / "../share/polymesh/examples", source_examples(binary_dir));
        polymesh::webd::ServerOptions options;

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            const auto value = [&](std::string_view name) -> std::string_view {
                if (index + 1 >= argc) {
                    throw std::runtime_error(std::string(name) + " requires a value");
                }
                return argv[++index];
            };
            if (argument == "--host") {
                host = value(argument);
            } else if (argument == "--port") {
                port = parse_port(value(argument));
            } else if (argument == "--web-root") {
                web_root = value(argument);
            } else if (argument == "--examples-dir") {
                examples_dir = value(argument);
            } else if (argument == "--advisor") {
                options.advisor_dir = value(argument);
            } else if (argument == "--max-mem-gb") {
                const std::string text(value(argument));
                char* end = nullptr;
                options.max_mem_gb = std::strtod(text.c_str(), &end);
                if (end == text.c_str() || *end != '\0' ||
                    !std::isfinite(options.max_mem_gb) || options.max_mem_gb < 0.0) {
                    throw std::runtime_error("--max-mem-gb must be a nonnegative number");
                }
            } else if (argument == "--max-elems") {
                options.max_elems = parse_size(value(argument), argument);
            } else if (argument == "--max-dof") {
                options.max_dof = parse_size(value(argument), argument);
            } else if (argument == "--help" || argument == "-h") {
                usage();
                return 0;
            } else {
                throw std::runtime_error("unknown argument: " + std::string(argument));
            }
        }
        options.examples_dir = examples_dir.string();
        polymesh::webd::JobService jobs(std::move(options));
        polymesh::webd::HttpServer server(
            host, port,
            [&](const polymesh::webd::HttpRequest& request,
                polymesh::webd::HttpConnection& connection) {
                if (!jobs.handle(request, connection) &&
                    !polymesh::webd::serve_static(request, connection, web_root.string())) {
                    connection.send_response(
                        polymesh::webd::json_error(405, "method not allowed"));
                }
            });
        g_server = &server;
        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, stop_server);
        std::signal(SIGTERM, stop_server);
        server.run();
        g_server = nullptr;
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "polymesh-webd: %s\n", error.what());
        usage();
        return 2;
    }
}
