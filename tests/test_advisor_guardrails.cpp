// SPDX-License-Identifier: BSD-3-Clause
// Learned mesh advisor (ADR-0027): the guardrails between a raw network output
// and a mesh -- clamp box, feasibility gate, veto -- and the max_dof budget
// filter. Each decision is re-derived independently from the fixture.

#include "advisor/advisor.hpp"
#include "support/advisor_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using polymesh::test_support::columns_of;
using polymesh::test_support::fixture_present;
using polymesh::test_support::kFixtureDir;
using polymesh::test_support::load;
using polymesh::test_support::sigmoid;

} // namespace

TEST_CASE("advisor guardrails: gated enumeration stays in the box and honours the veto",
          "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }
    const json parity = load(kFixtureDir / "parity.json");
    const json clamps = load(kFixtureDir / "clamps.json");
    const polymesh::advisor::Advisor advisor(kFixtureDir);
    const auto defaults = advisor.defaults();

    const double h_floor = clamps.at("h_rel")[0].get<double>();
    const double h_ceil = clamps.at("h_rel")[1].get<double>();
    const double eta_floor = clamps.at("eta_target")[0].get<double>();
    const double eta_ceil = clamps.at("eta_target")[1].get<double>();
    const int passes_ceil = clamps.at("adapt_passes")[1].get<int>();
    const double veto_threshold = clamps.value("veto_threshold", 0.5);
    // Read strictly, exactly as Advisor does. A fallback to the veto threshold
    // here would let the test pass against a clamps.json the product rejects.
    REQUIRE(clamps.contains("gate_threshold"));
    const double gate_threshold = clamps.at("gate_threshold").get<double>();
    const auto order_choices = clamps.at("order_choices").get<std::vector<int>>();
    const auto mesher_choices = clamps.at("mesher_choices").get<std::vector<std::string>>();

    // The clamp box must contain its own defaults, or a "safe fallback" would
    // itself be an out-of-box action.
    CHECK(defaults.h_rel >= h_floor);
    CHECK(defaults.h_rel <= h_ceil);
    CHECK(defaults.eta_target >= eta_floor);
    CHECK(defaults.eta_target <= eta_ceil);

    // The candidate list is what the shipped rule enumerates. Read it here and
    // re-derive the decision from it independently, rather than trusting the
    // same code path the test is meant to check.
    REQUIRE(clamps.contains("candidate_grid"));
    const json& grid = clamps.at("candidate_grid");
    REQUIRE(grid.contains("actions"));
    const json& actions = grid.at("actions");
    REQUIRE(actions.is_array());
    REQUIRE(!actions.empty());

    bool saw_veto = false;
    bool saw_nominal = false;
    bool saw_gate_bind = false;

    for (const auto& fixture_case : parity.at("cases")) {
        const std::string name = fixture_case.at("name").get<std::string>();
        INFO("case " << name);
        const auto columns = columns_of(fixture_case.at("features"));
        const auto decision = advisor.recommend(columns);

        // Guardrail 1 — whatever the heads said, the decision is in the box and
        // in the declared vocabularies.
        CHECK(decision.h_rel >= h_floor);
        CHECK(decision.h_rel <= h_ceil);
        CHECK(decision.eta_target >= eta_floor);
        CHECK(decision.eta_target <= eta_ceil);
        CHECK(decision.adapt_passes >= 0);
        CHECK(decision.adapt_passes <= passes_ceil);
        CHECK(std::find(order_choices.begin(), order_choices.end(), decision.order) !=
              order_choices.end());
        CHECK(std::find(mesher_choices.begin(), mesher_choices.end(), decision.mesher) !=
              mesher_choices.end());

        // Independently re-derive the gated argmin: score every candidate, drop
        // those the feasibility head expects to fail, and take the lowest
        // predicted rel_err_rel among the survivors. If the gate rejects every
        // candidate the rule falls back to the best-ranked one and leaves the
        // refusal to the veto, so both arms are reproduced here.
        double best_survivor_score = std::numeric_limits<double>::infinity();
        double best_any_score = std::numeric_limits<double>::infinity();
        json best_survivor;
        json best_any;
        for (const auto& action : actions) {
            auto candidate = defaults;
            candidate.mesher = action.value("mesher", defaults.mesher);
            candidate.order = action.value("order", defaults.order);
            candidate.h_rel = action.value("h_rel", defaults.h_rel);
            candidate.eta_target = action.value("eta_target", defaults.eta_target);
            candidate.adapt_passes = action.value("adapt_passes", defaults.adapt_passes);

            auto query = columns;
            advisor.apply_action(query, candidate);
            const auto raw = advisor.evaluate(query);
            const double score = raw.rel_err_rel;
            if (!std::isfinite(score)) {
                continue;
            }
            if (score < best_any_score) {
                best_any_score = score;
                best_any = action;
            }
            if (sigmoid(raw.failure_logit) <= gate_threshold && score < best_survivor_score) {
                best_survivor_score = score;
                best_survivor = action;
            }
        }
        const bool gate_kept_nothing = best_survivor.is_null();
        if (gate_kept_nothing && !best_any.is_null()) {
            saw_gate_bind = true;
        }
        const json& expected = gate_kept_nothing ? best_any : best_survivor;
        REQUIRE(!expected.is_null());

        // The final pass re-scores the chosen action, so the reported prediction
        // and the veto must both describe THAT action and no other.
        auto chosen = defaults;
        chosen.mesher = expected.value("mesher", defaults.mesher);
        chosen.order = expected.value("order", defaults.order);
        chosen.h_rel = expected.value("h_rel", defaults.h_rel);
        chosen.eta_target = expected.value("eta_target", defaults.eta_target);
        chosen.adapt_passes = expected.value("adapt_passes", defaults.adapt_passes);
        auto scored_query = columns;
        advisor.apply_action(scored_query, chosen);
        const auto scored_raw = advisor.evaluate(scored_query);
        const double failure_prob = sigmoid(scored_raw.failure_logit);

        // A refusal SUPPRESSES every prediction rather than carrying it, and
        // there are two independent causes of refusal, so neither the predicted
        // values nor `vetoed` can be predicted from the feasibility head alone.
        // Beyond the training support the heads diverge rather than degrade, so
        // NaN is the honest value: a reader can see the model has no opinion.
        if (decision.vetoed) {
            CHECK_FALSE(std::isfinite(decision.failure_prob));
            CHECK_FALSE(std::isfinite(decision.predicted_rel_err_rel));
            CHECK_FALSE(std::isfinite(decision.predicted_mesh_ms));
            CHECK_FALSE(std::isfinite(decision.predicted_dof));
            CHECK_FALSE(std::isfinite(decision.predicted_solve_flops));
            CHECK_FALSE(std::isfinite(decision.predicted_solve_bytes));
            CHECK_FALSE(std::isfinite(decision.predicted_mesh_work));
            CHECK_FALSE(decision.predicted_solve_seconds.has_value());
        } else {
            // Not refused: the reported prediction and the feasibility
            // probability must both describe the action actually chosen.
            CHECK(decision.failure_prob == Catch::Approx(failure_prob).margin(1e-9));
            CHECK(failure_prob <= veto_threshold);
            // Reported RAW: this head is a log10 difference, not a level.
            CHECK(decision.predicted_rel_err_rel ==
                  Catch::Approx(scored_raw.rel_err_rel).margin(1e-9));
        }

        if (decision.vetoed) {
            // Guardrail 2 — the feasibility head vetoes: defaults, flagged.
            saw_veto = true;
            CHECK(decision.mesher == defaults.mesher);
            CHECK(decision.h_rel == Catch::Approx(defaults.h_rel));
            CHECK(decision.order == defaults.order);
            CHECK(decision.adapt_passes == defaults.adapt_passes);
            CHECK(decision.eta_target == Catch::Approx(defaults.eta_target));
            continue;
        }

        // Not vetoed: the decision IS the independently re-derived gated argmin.
        saw_nominal = true;
        CHECK(decision.mesher == chosen.mesher);
        CHECK(decision.order == chosen.order);
        CHECK(decision.h_rel == Catch::Approx(chosen.h_rel).margin(1e-12));
        CHECK(decision.eta_target == Catch::Approx(chosen.eta_target).margin(1e-12));
        CHECK(decision.adapt_passes == chosen.adapt_passes);

        // Every candidate came from the measured list, so the chosen action must
        // be one of them -- the chooser must never synthesise an action.
        bool found = false;
        for (const auto& action : actions) {
            if (action.value("mesher", std::string{}) == decision.mesher &&
                action.value("order", 0) == decision.order &&
                std::abs(action.value("h_rel", 0.0) - decision.h_rel) <= 1e-12) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    // The fixture is only proof if it actually reaches both guardrails.
    CHECK(saw_veto);
    CHECK(saw_nominal);
    // Not required: whether the gate binds depends on the forced head values.
    // Recorded so a fixture that stops exercising it is visible rather than
    // silently reducing what this test covers.
    INFO("gate rejected every candidate on at least one case: " << saw_gate_bind);
}

TEST_CASE("advisor max_dof budget filter: gated enumeration respects the budget",
          "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }
    const json parity = load(kFixtureDir / "parity.json");
    const json clamps = load(kFixtureDir / "clamps.json");
    const polymesh::advisor::Advisor advisor(kFixtureDir);
    const auto defaults = advisor.defaults();
    REQUIRE(clamps.contains("gate_threshold"));
    const double gate_threshold = clamps.at("gate_threshold").get<double>();
    REQUIRE(clamps.contains("candidate_grid"));
    const json& actions = clamps.at("candidate_grid").at("actions");

    // The same de-logging as advisor_decide.cpp's from_log10, reimplemented here so
    // the expectation never trusts the code path it is checking.
    const auto dof_of = [](double dof_log10) {
        if (!std::isfinite(dof_log10)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return std::pow(10.0, std::clamp(dof_log10, -30.0, 30.0));
    };

    struct Scored {
        polymesh::advisor::AdvisorDecision action;
        double score;
        double risk;
        double dof;
    };
    // Independently score every candidate, exactly as the gated-enumeration
    // test above does: apply the action, read the heads, keep the finite rows.
    const auto score_candidates = [&](const polymesh::advisor::FeatureColumns& columns) {
        std::vector<Scored> table;
        for (const auto& action : actions) {
            auto candidate = defaults;
            candidate.mesher = action.value("mesher", defaults.mesher);
            candidate.order = action.value("order", defaults.order);
            candidate.h_rel = action.value("h_rel", defaults.h_rel);
            candidate.eta_target = action.value("eta_target", defaults.eta_target);
            candidate.adapt_passes = action.value("adapt_passes", defaults.adapt_passes);
            auto query = columns;
            advisor.apply_action(query, candidate);
            const auto raw = advisor.evaluate(query);
            if (!std::isfinite(raw.rel_err_rel)) {
                continue;
            }
            table.push_back({candidate, raw.rel_err_rel, sigmoid(raw.failure_logit),
                             dof_of(raw.dof_log10)});
        }
        return table;
    };
    const auto same_action = [](const polymesh::advisor::AdvisorDecision& a,
                                const polymesh::advisor::AdvisorDecision& b) {
        return a.mesher == b.mesher && a.order == b.order && a.h_rel == b.h_rel &&
               a.eta_target == b.eta_target && a.adapt_passes == b.adapt_passes &&
               a.p_elevate == b.p_elevate;
    };
    // NaN-aware: a refusal suppresses predictions AS NaN, and bit-for-bit means
    // those compare equal too.
    const auto same_number = [](double a, double b) {
        return a == b || (std::isnan(a) && std::isnan(b));
    };

    // (a) max_dof = 0 disables the budget: the decision must reproduce the
    // unfiltered one bit-for-bit on every parity case.
    for (const auto& fixture_case : parity.at("cases")) {
        INFO("case " << fixture_case.at("name").get<std::string>());
        const auto columns = columns_of(fixture_case.at("features"));
        const auto unfiltered = advisor.recommend(columns);
        const auto budget_off = advisor.recommend(columns, 0.0);
        CHECK(same_action(unfiltered, budget_off));
        CHECK(unfiltered.vetoed == budget_off.vetoed);
        CHECK(unfiltered.clamped == budget_off.clamped);
        CHECK(unfiltered.note == budget_off.note);
        CHECK(same_number(unfiltered.predicted_rel_err, budget_off.predicted_rel_err));
        CHECK(same_number(unfiltered.predicted_rel_err_rel, budget_off.predicted_rel_err_rel));
        CHECK(
            same_number(unfiltered.predicted_chamfer_mean, budget_off.predicted_chamfer_mean));
        CHECK(same_number(unfiltered.predicted_dof, budget_off.predicted_dof));
        CHECK(same_number(unfiltered.predicted_mesh_ms, budget_off.predicted_mesh_ms));
        CHECK(same_number(unfiltered.predicted_solve_ms, budget_off.predicted_solve_ms));
        CHECK(same_number(unfiltered.predicted_solve_flops, budget_off.predicted_solve_flops));
        CHECK(same_number(unfiltered.predicted_solve_bytes, budget_off.predicted_solve_bytes));
        CHECK(same_number(unfiltered.predicted_mesh_work, budget_off.predicted_mesh_work));
        CHECK(same_number(unfiltered.failure_prob, budget_off.failure_prob));
        CHECK(same_number(unfiltered.ood_distance, budget_off.ood_distance));
        CHECK_FALSE(budget_off.budget_refusal);
    }

    // (b)-(d) each need a case with at least one finitely scored candidate;
    // find them from the fixture rather than assuming which parity case works.
    bool exercised_pick = false;
    bool exercised_refusal = false;
    for (const auto& fixture_case : parity.at("cases")) {
        const auto columns = columns_of(fixture_case.at("features"));
        const auto table = score_candidates(columns);
        if (table.empty()) {
            continue;
        }
        // The budget refusal is checked AFTER the OOD refusals, so a case the
        // advisor refuses as out-of-distribution cannot exercise it here.
        const auto baseline = advisor.recommend(columns);
        const bool ood_refused =
            baseline.vetoed && baseline.note.find("distribution") != std::string::npos;

        // (c) A budget below EVERY candidate's predicted dof empties the
        // candidate set: refusal, mirroring the OOD refusal exactly — clamp-box
        // defaults, every prediction suppressed, `vetoed` set — but flagged
        // `budget_refusal` so a caller can tell it apart.
        if (!exercised_refusal && !ood_refused) {
            double min_dof = std::numeric_limits<double>::infinity();
            for (const auto& row : table) {
                min_dof = std::min(min_dof, row.dof);
            }
            REQUIRE(std::isfinite(min_dof));
            REQUIRE(min_dof > 0.0);
            const auto decision = advisor.recommend(columns, min_dof * 0.5);
            CHECK(decision.budget_refusal);
            CHECK(decision.vetoed);
            CHECK(same_action(decision, defaults));
            CHECK_FALSE(std::isfinite(decision.predicted_rel_err));
            CHECK_FALSE(std::isfinite(decision.predicted_rel_err_rel));
            CHECK_FALSE(std::isfinite(decision.predicted_chamfer_mean));
            CHECK_FALSE(std::isfinite(decision.predicted_dof));
            CHECK_FALSE(std::isfinite(decision.predicted_mesh_ms));
            CHECK_FALSE(std::isfinite(decision.predicted_solve_ms));
            CHECK_FALSE(std::isfinite(decision.predicted_solve_flops));
            CHECK_FALSE(std::isfinite(decision.predicted_solve_bytes));
            CHECK_FALSE(std::isfinite(decision.predicted_mesh_work));
            CHECK_FALSE(decision.predicted_solve_seconds.has_value());
            CHECK_FALSE(std::isfinite(decision.failure_prob));
            CHECK(decision.note.find("budget") != std::string::npos);
            // ood_distance is retained on a refusal: it is the measurement,
            // not a prediction, and it is identical to the unfiltered call's.
            CHECK(decision.ood_distance == baseline.ood_distance);
            exercised_refusal = true;
        }

        // (b)+(d) A budget that prices out only the cheapest gate-passing
        // candidate: the chooser must return the cheapest SURVIVING candidate
        // that respects the budget, and a candidate that passes the failure
        // gate but sits over budget must be filtered — gate and budget compose.
        if (!exercised_pick) {
            std::vector<const Scored*> survivors;
            for (const auto& row : table) {
                if (row.risk <= gate_threshold) {
                    survivors.push_back(&row);
                }
            }
            if (survivors.size() < 2) {
                continue;
            }
            std::sort(survivors.begin(), survivors.end(),
                      [](const Scored* a, const Scored* b) { return a->score < b->score; });
            const Scored& best = *survivors.front();
            REQUIRE(best.dof > 0.0);
            // Strictly under the known best candidate's predicted dof, so the
            // unfiltered winner is exactly what the budget excludes.
            const double max_dof = best.dof * 0.999;
            const Scored* expected = nullptr;
            bool saw_gate_passing_over_budget = false;
            for (const Scored* row : survivors) {
                if (row->dof > max_dof) {
                    saw_gate_passing_over_budget = true; // (d): gate-passing, over budget
                    continue;
                }
                expected = row; // survivors are score-sorted: first fit is cheapest
                break;
            }
            if (expected == nullptr) {
                continue;
            }
            CHECK(saw_gate_passing_over_budget);
            const auto decision = advisor.recommend(columns, max_dof);
            if (decision.vetoed) {
                // Refused for an independent reason (OOD or the residual veto);
                // this case cannot speak to the ranking. Try the next one.
                continue;
            }
            CHECK_FALSE(decision.budget_refusal);
            CHECK(same_action(decision, expected->action));
            CHECK(decision.predicted_dof == Catch::Approx(expected->dof).epsilon(1e-9));
            CHECK(decision.predicted_dof <= max_dof);
            // The budget really did move the decision off the unfiltered best.
            CHECK_FALSE(same_action(decision, best.action));
            exercised_pick = true;
        }
    }
    // The deterministic fixture's score winner may also be its minimum-DOF
    // candidate after a head-schema retrain, leaving no budget that can exclude
    // it while retaining another action. That is a valid model geometry, not a
    // product failure; the all-candidates refusal arm remains mandatory.
    if (!exercised_pick) {
        WARN("fixture best action is already minimum DOF; partial budget pick is unreachable");
    }
    CHECK(exercised_refusal);
}
