// SPDX-License-Identifier: BSD-3-Clause

// polymesh_testlab — campaign runner with successive-halving, SIGINT pause,
// and atomic checkpointing. Normative schemas: docs/dag/interfaces.md.
//
// Anti-cheat: reference truths are loaded only from paths declared in case
// files (bench/reference/*). No numeric answers are embedded here.

#include "fea/backend.hpp"
#include "testlab_internal.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string_view>

namespace tl = polymesh::testlab::detail;

namespace {

int usage() {
    std::fputs("usage: polymesh_testlab run|resume|validate|pause-status <campaign_dir>\n"
               "                        [--advisor <model_dir>]\n"
               "\n"
               "  run           start (or restart) a campaign from campaign.json\n"
               "  resume        continue from checkpoint.json after pause / SIGINT\n"
               "  validate      parse campaign, grid, cases, and print maximum run count\n"
               "  pause-status  print checkpoint state (running|paused|finished)\n"
               "\n"
               "  --advisor DIR records what the learned advisor would have chosen for\n"
               "                each case as the row's advisor_decision field. The grid\n"
               "                still decides what actually runs — the decision is an\n"
               "                extra observable, never an override.\n"
               "\n"
               "Schemas: docs/dag/interfaces.md. Run from the repo root so case and\n"
               "bench/reference paths resolve. SIGINT after a run finishes → paused.\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    polymesh::fea::init_runtime_performance();
    if (argc < 3) {
        return usage();
    }
    const std::string_view cmd = argv[1];
    const std::filesystem::path camp_dir = argv[2];
    std::filesystem::path advisor_dir;
    for (int i = 3; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--advisor" && i + 1 < argc) {
            advisor_dir = argv[++i];
        } else {
            return usage();
        }
    }
    try {
        std::unique_ptr<tl::AdvisorScorer> advisor;
        if (!advisor_dir.empty()) {
            advisor = std::make_unique<tl::AdvisorScorer>(advisor_dir);
        }
        if (cmd == "run") {
            return tl::run_campaign(camp_dir, /*resume=*/false, advisor.get());
        }
        if (cmd == "resume") {
            return tl::run_campaign(camp_dir, /*resume=*/true, advisor.get());
        }
        if (cmd == "validate") {
            return tl::cmd_validate(camp_dir);
        }
        if (cmd == "pause-status") {
            return tl::cmd_pause_status(camp_dir);
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "polymesh_testlab: %s\n", e.what());
        return 1;
    }
}
