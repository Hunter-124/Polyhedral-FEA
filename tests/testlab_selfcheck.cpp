// SPDX-License-Identifier: BSD-3-Clause

// ImGui-free self-check for the GUI test-lab parsers (docs/dag/interfaces.md).
// Ensures the GUI side of the campaign/checkpoint/results/progress contract
// stays honest without needing a display or the harness binary.

#include "testlab_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using polymesh::gui::testlab::parse_campaign;
using polymesh::gui::testlab::parse_checkpoint;
using polymesh::gui::testlab::parse_progress;
using polymesh::gui::testlab::parse_result_line;
using polymesh::gui::testlab::CheckpointState;
using polymesh::gui::testlab::scan_campaigns;

namespace {

std::filesystem::path write_temp_campaign_tree() {
    const auto root = std::filesystem::temp_directory_path() / "polymesh_testlab_selfcheck";
    std::filesystem::remove_all(root);
    const auto camp = root / "hole-plate-frontier-1";
    std::filesystem::create_directories(camp);

    {
        std::ofstream out(camp / "campaign.json");
        out << R"({
  "name": "hole-plate-frontier-1",
  "parts": ["tests/fixtures/parts/plate_hole.case.json"],
  "tiers": [
    { "h_scale": 2.0, "keep_frac": 0.25 },
    { "h_scale": 1.0, "keep_frac": 0.5 },
    { "h_scale": 0.5, "keep_frac": 1.0 }
  ],
  "grid": {
    "mesher": ["hex", "graded_tet", "hybrid_zoo"],
    "curvature_turn_deg": [10, 15, 22.5],
    "feature_refine": [true, false],
    "snap_boundary": [true],
    "order": [1, 2],
    "element_tendency": [0.0]
  },
  "score": { "weights": { "accuracy": 0.5, "solve_ms": 0.25, "mesh_ms": 0.25 } },
  "resources": { "max_threads": 0, "max_mem_gb": 0 }
})";
    }
    {
        std::ofstream out(camp / "checkpoint.json");
        out << R"({
  "campaign": "hole-plate-frontier-1",
  "state": "paused",
  "tier": 1,
  "completed_runs": 137,
  "survivors": ["cfg-0007", "cfg-0012"],
  "started_utc": "2026-07-10T18:00:00Z",
  "updated_utc": "2026-07-10T18:22:31Z"
})";
    }
    {
        std::ofstream out(camp / "results.jsonl");
        out << R"({"cfg_id":"cfg-0007","config":{"mesher":"hybrid_zoo","curvature_turn_deg":15},"part":"plate_hole","tier":1,"mesh_ms":412,"solve_ms":1890,"n_elems":31956,"n_nodes":11935,"n_dof":35805,"quality":{"M1max":1.0e-11,"M2max":0.36,"M6":0.17,"score":0.42},"answers":{"sigma_max":9.12e7,"tip_deflection":null},"accuracy":{"metric":"scf","value":3.06,"truth":3.0,"rel_err":0.02},"status":"ok"})"
            << '\n';
        out << R"({"cfg_id":"cfg-0012","config":{"mesher":"hex"},"part":"plate_hole","tier":0,"mesh_ms":100,"solve_ms":200,"n_elems":1000,"n_nodes":500,"n_dof":1500,"status":"mesh_fail"})"
            << '\n';
    }
    {
        std::ofstream out(camp / "progress.json");
        out << R"({
  "phase": "solve",
  "phase_frac": 0.62,
  "elapsed_ms": 8400,
  "cg_iter": 310,
  "cg_resid": 3.2e-7,
  "run": { "cfg_id": "cfg-0007", "part": "plate_hole", "tier": 1 }
})";
    }
    return root;
}

} // namespace

TEST_CASE("testlab: parse campaign.json (interfaces §1)") {
    const auto spec = parse_campaign(R"({
  "name": "demo",
  "parts": ["a.case.json", "b.case.json"],
  "tiers": [{ "h_scale": 2.0, "keep_frac": 0.25 }],
  "grid": { "mesher": ["hex", "hybrid_zoo"], "order": [1, 2] },
  "score": { "weights": { "accuracy": 0.5, "solve_ms": 0.25, "mesh_ms": 0.25 } },
  "resources": { "max_threads": 4, "max_mem_gb": 8 }
})");
    CHECK(spec.name == "demo");
    REQUIRE(spec.parts.size() == 2);
    REQUIRE(spec.tiers.size() == 1);
    CHECK_THAT(spec.tiers[0].h_scale, Catch::Matchers::WithinAbs(2.0, 1e-12));
    CHECK_THAT(spec.tiers[0].keep_frac, Catch::Matchers::WithinAbs(0.25, 1e-12));
    REQUIRE(spec.grid.size() == 2);
    CHECK(spec.resources.max_threads == 4);
    CHECK_THAT(spec.resources.max_mem_gb, Catch::Matchers::WithinAbs(8.0, 1e-12));
}

TEST_CASE("testlab: parse checkpoint.json (interfaces §2)") {
    const auto cp = parse_checkpoint(R"({
  "campaign": "hole-plate-frontier-1",
  "state": "running",
  "tier": 1,
  "completed_runs": 137,
  "survivors": ["cfg-0007", "cfg-0012"],
  "started_utc": "2026-07-10T18:00:00Z",
  "updated_utc": "2026-07-10T18:22:31Z"
})");
    CHECK(cp.campaign == "hole-plate-frontier-1");
    CHECK(cp.state == CheckpointState::kRunning);
    CHECK(cp.tier == 1);
    CHECK(cp.completed_runs == 137);
    REQUIRE(cp.survivors.size() == 2);
    CHECK(cp.survivors[0] == "cfg-0007");
}

TEST_CASE("testlab: parse results.jsonl line (interfaces §3)") {
    const auto row = parse_result_line(
        R"({"cfg_id":"cfg-0007","config":{"mesher":"hybrid_zoo","curvature_turn_deg":15},"part":"plate_hole","tier":1,"mesh_ms":412,"solve_ms":1890,"n_elems":31956,"n_nodes":11935,"n_dof":35805,"quality":{"M1max":1.0e-11,"M2max":0.36,"M6":0.17,"score":0.42},"accuracy":{"metric":"scf","value":3.06,"truth":3.0,"rel_err":0.02},"status":"ok"})");
    CHECK(row.cfg_id == "cfg-0007");
    CHECK(row.part == "plate_hole");
    CHECK(row.tier == 1);
    CHECK(row.n_dof == 35805);
    CHECK(row.status == "ok");
    CHECK(row.accuracy.metric == "scf");
    CHECK_THAT(row.accuracy.rel_err, Catch::Matchers::WithinAbs(0.02, 1e-12));
    CHECK_THAT(row.quality.score, Catch::Matchers::WithinAbs(0.42, 1e-12));
    CHECK(row.config_summary.find("hybrid_zoo") != std::string::npos);
    CHECK_FALSE(row.health.present);
    CHECK_FALSE(row.scorecard.present);
    CHECK(std::isnan(row.n_pred_elems));
}

TEST_CASE("testlab: parse results.jsonl with health + scorecard (measure-first)") {
    const auto row = parse_result_line(
        R"({"cfg_id":"cfg-028399df","config":{"mesher":"varyhedron"},"part":"plate_hole","tier":0,"mesh_ms":100,"solve_ms":200,"n_elems":4608,"n_nodes":2064,"n_dof":6192,"n_pred_elems":355.5,"quality":{"M1max":0,"M2max":0.1,"M6":1.0,"score":0.5},"answers":{"strain_energy":0.0031,"tip_deflection":2.4e-6,"sigma_face_mean":3.75e6,"sigma_max":4.8e6},"health":{"free_residual_rel":1.2e-12,"reaction_sum_err":1e-13,"n_orphans":0,"load_area_ok":true,"ok":true},"scorecard":{"edge_hausdorff_over_h":0.04,"chordal_efficiency_max":1.2,"normal_dev_deg_max":8.2,"n_dof":6192,"accuracy_rel_err":0.02,"min_element_quality":1.0,"solve_residual_rel":1.2e-12,"health_ok":true},"accuracy":{"metric":"scf","value":3.06,"truth":3.0,"rel_err":0.02},"status":"ok"})");
    CHECK(row.cfg_id == "cfg-028399df");
    CHECK(row.status == "ok");
    CHECK(row.health.present);
    CHECK(row.health.ok);
    CHECK(row.health.has_load_area_ok);
    CHECK(row.health.load_area_ok);
    CHECK_THAT(row.health.free_residual_rel, Catch::Matchers::WithinAbs(1.2e-12, 1e-20));
    CHECK_THAT(row.health.reaction_sum_err, Catch::Matchers::WithinAbs(1e-13, 1e-20));
    CHECK(row.scorecard.present);
    CHECK(row.scorecard.has_health_ok);
    CHECK(row.scorecard.health_ok);
    CHECK_THAT(row.scorecard.edge_hausdorff_over_h, Catch::Matchers::WithinAbs(0.04, 1e-12));
    CHECK_THAT(row.scorecard.chordal_efficiency_max, Catch::Matchers::WithinAbs(1.2, 1e-12));
    CHECK_THAT(row.scorecard.normal_dev_deg_max, Catch::Matchers::WithinAbs(8.2, 1e-12));
    CHECK_THAT(row.answers.strain_energy, Catch::Matchers::WithinAbs(0.0031, 1e-12));
    CHECK_THAT(row.answers.tip_deflection, Catch::Matchers::WithinAbs(2.4e-6, 1e-15));
    CHECK_THAT(row.answers.sigma_face_mean, Catch::Matchers::WithinAbs(3.75e6, 1.0));
    CHECK_THAT(row.n_pred_elems, Catch::Matchers::WithinAbs(355.5, 1e-12));
}

TEST_CASE("testlab: parse solve_suspect + null scorecard fields") {
    const auto row = parse_result_line(
        R"({"cfg_id":"cfg-cyl","part":"cylinder","tier":0,"mesh_ms":10,"solve_ms":20,"n_elems":100,"n_nodes":50,"n_dof":150,"answers":{"strain_energy":0.009,"tip_deflection":1.4e-6,"sigma_face_mean":2e6},"health":{"free_residual_rel":1e-12,"reaction_sum_err":0.01,"load_area_ok":false,"ok":false},"scorecard":{"edge_hausdorff_over_h":null,"chordal_efficiency_max":null,"normal_dev_deg_max":12.0,"health_ok":false},"accuracy":{"metric":"strain_energy","value":0.009,"truth":0.004,"rel_err":1.4},"status":"solve_suspect"})");
    CHECK(row.status == "solve_suspect");
    CHECK(row.health.present);
    CHECK_FALSE(row.health.ok);
    CHECK(row.health.has_load_area_ok);
    CHECK_FALSE(row.health.load_area_ok);
    CHECK(row.scorecard.present);
    CHECK(row.scorecard.has_health_ok);
    CHECK_FALSE(row.scorecard.health_ok);
    CHECK(std::isnan(row.scorecard.edge_hausdorff_over_h));
    CHECK(std::isnan(row.scorecard.chordal_efficiency_max));
    CHECK_THAT(row.scorecard.normal_dev_deg_max, Catch::Matchers::WithinAbs(12.0, 1e-12));
    CHECK_THAT(row.answers.strain_energy, Catch::Matchers::WithinAbs(0.009, 1e-12));
}

TEST_CASE("testlab: parse handoff.json open_program_nodes") {
    const auto h = polymesh::gui::testlab::parse_handoff(R"({
  "campaign": "varyhedron-baseline-m9",
  "git_head": "abc123",
  "mode": "autonomous",
  "open_program_nodes": ["V10c", "M12", "G0"]
})");
    CHECK(h.campaign == "varyhedron-baseline-m9");
    CHECK(h.mode == "autonomous");
    REQUIRE(h.open_program_nodes.size() == 3);
    CHECK(h.open_program_nodes[0] == "V10c");
    CHECK(polymesh::gui::testlab::is_measure_first_baseline("varyhedron-baseline-m9"));
    CHECK_FALSE(polymesh::gui::testlab::is_measure_first_baseline("varyhedron-short-1"));
}

TEST_CASE("testlab: parse progress.json (interfaces §6)") {
    const auto p = parse_progress(R"({
  "phase": "solve",
  "phase_frac": 0.62,
  "elapsed_ms": 8400,
  "cg_iter": 310,
  "cg_resid": 3.2e-7,
  "run": { "cfg_id": "cfg-0007", "part": "plate_hole", "tier": 1 }
})");
    CHECK(p.phase == "solve");
    CHECK_THAT(p.phase_frac, Catch::Matchers::WithinAbs(0.62, 1e-12));
    CHECK(p.cg_iter == 310);
    CHECK(p.cfg_id == "cfg-0007");
    CHECK(p.part == "plate_hole");
    CHECK(p.tier == 1);
}

TEST_CASE("testlab: scan_campaigns + load round-trip") {
    const auto root = write_temp_campaign_tree();
    const auto list = scan_campaigns(root);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == "hole-plate-frontier-1");
    CHECK(list[0].has_campaign_json);
    CHECK(list[0].has_checkpoint);
    CHECK(list[0].has_results);
    CHECK(list[0].result_count == 2);
    CHECK(list[0].state == CheckpointState::kPaused);

    auto spec = polymesh::gui::testlab::load_campaign(list[0].dir);
    REQUIRE(spec.has_value());
    CHECK(spec->name == "hole-plate-frontier-1");
    CHECK(spec->tiers.size() == 3);

    auto cp = polymesh::gui::testlab::load_checkpoint(list[0].dir);
    REQUIRE(cp.has_value());
    CHECK(cp->completed_runs == 137);

    auto rows = polymesh::gui::testlab::load_results(list[0].dir);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].cfg_id == "cfg-0007");
    CHECK(rows[1].status == "mesh_fail");

    auto prog = polymesh::gui::testlab::load_progress(list[0].dir);
    REQUIRE(prog.has_value());
    CHECK(prog->phase == "solve");

    std::filesystem::remove_all(root);
}
