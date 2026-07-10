// SPDX-License-Identifier: BSD-3-Clause

// D6 harness smoke: script --help, dry-run paths, committed result JSON schema.
// Does not run the multi-second/minute L-domain suite.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int run_cmd(const std::string& cmd) { return std::system(cmd.c_str()); }

} // namespace

TEST_CASE("D6 run_tier3.py --help exits 0") {
    // Working directory is repo root (catch_discover_tests WORKING_DIRECTORY).
    const int rc =
        run_cmd("python3 bench/d6/run_tier3.py --help > /tmp/polymesh_d6_help.txt 2>&1");
    REQUIRE(rc == 0);
    const auto out = slurp("/tmp/polymesh_d6_help.txt");
    REQUIRE(out.find("D6") != std::string::npos);
    REQUIRE(out.find("--quick") != std::string::npos);
    REQUIRE(out.find("--full") != std::string::npos);
}

TEST_CASE("D6 run_tier3.py --dry-run prints artifact paths") {
    const int rc =
        run_cmd("python3 bench/d6/run_tier3.py --dry-run > /tmp/polymesh_d6_dry.txt 2>&1");
    REQUIRE(rc == 0);
    const auto out = slurp("/tmp/polymesh_d6_dry.txt");
    REQUIRE(out.find("polymesh-d6-l-domain") != std::string::npos);
}

TEST_CASE("D6 scoreboard JSON has competitive schema fields") {
    const auto text = slurp("bench/results/polymesh-d6-l-domain.json");
    // Minimal structural checks without pulling a JSON library into tests.
    REQUIRE(text.find("\"solver\"") != std::string::npos);
    REQUIRE(text.find("\"case_id\"") != std::string::npos);
    REQUIRE(text.find("\"dofs\"") != std::string::npos);
    REQUIRE(text.find("\"wall_time_s\"") != std::string::npos);
    REQUIRE(text.find("\"accuracy\"") != std::string::npos);
    REQUIRE(text.find("\"label\"") != std::string::npos);
    REQUIRE(text.find("\"timestamp\"") != std::string::npos);
    REQUIRE(text.find("l-domain-d6") != std::string::npos);
    REQUIRE(text.find("dof_ratio_uniform_over_graded") != std::string::npos);
}

TEST_CASE("D6 raw JSON has summary ratios") {
    const auto text = slurp("bench/d6/out/polymesh-d6-l-domain-raw.json");
    REQUIRE(text.find("\"summary\"") != std::string::npos);
    REQUIRE(text.find("dof_ratio_uniform_over_graded") != std::string::npos);
    REQUIRE(text.find("time_ratio_uniform_over_graded") != std::string::npos);
    REQUIRE(text.find("meets_dof_target") != std::string::npos);
    REQUIRE(text.find("tier3_dof_target") != std::string::npos);
}
