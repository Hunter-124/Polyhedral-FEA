// SPDX-License-Identifier: BSD-3-Clause
// Learned mesh advisor: C++/PyTorch parity and the two guardrails that stand
// between a raw network output and a mesh (ADR-0027).
//
// The fixture under tests/fixtures/advisor_tiny/ is produced by
// `python scripts/advisor/export_onnx.py --tiny-fixture`. It carries the graph,
// the normalization/clamp artifacts, and PyTorch's own outputs for four inputs
// chosen to reach the nominal, clamped, vetoed, and imputed paths. Everything
// here is compared against that exporter rather than against a hand-copied
// number, so the test fails if either side drifts.

#include "advisor/advisor.hpp"
#include "pipeline/scene.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "geom/cad_geometry_features.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

const std::filesystem::path kFixtureDir = "tests/fixtures/advisor_tiny";

json load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    REQUIRE(stream.good());
    json parsed;
    stream >> parsed;
    return parsed;
}

bool fixture_present() {
    return std::filesystem::exists(kFixtureDir / "model.onnx") &&
           std::filesystem::exists(kFixtureDir / "parity.json") &&
           std::filesystem::exists(kFixtureDir / "clamps.json") &&
           std::filesystem::exists(kFixtureDir / "normalization.json");
}

/// The fixture's raw, pre-standardization row, exactly as the exporter fed it.
/// `imputed_defaults` deliberately omits five columns so the C++ impute path is
/// the thing under test for that case.
polymesh::advisor::FeatureColumns columns_of(const json& raw) {
    polymesh::advisor::FeatureColumns columns;
    for (const auto& [name, value] : raw.items()) {
        if (value.is_number()) {
            columns[name] = value.get<double>();
        } else if (value.is_boolean()) {
            columns[name] = value.get<bool>() ? 1.0 : 0.0;
        }
    }
    return columns;
}

double sigmoid(double x) {
    return x >= 0.0 ? 1.0 / (1.0 + std::exp(-x)) : std::exp(x) / (1.0 + std::exp(x));
}

} // namespace

TEST_CASE("advisor rejects an unusable model directory", "[advisor]") {
    // A missing directory is a configuration error the operator must see, not
    // something to paper over with silent defaults.
    CHECK_THROWS_AS(polymesh::advisor::Advisor("bench/advisor/definitely-not-here"),
                    polymesh::advisor::AdvisorError);
}

TEST_CASE("advisor reads gate_threshold from clamps.json and rejects its absence",
          "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }

    // The feasibility gate and the OOD/abstention veto are different decisions
    // with different correct values. The gate used to inherit the veto's number
    // whenever the key was absent, which shipped the gate at 0.5 -- the weakest
    // member of its own measured sweep -- while looking like a deliberate
    // choice. An absent key is a misconfiguration, so it must fail loudly.
    const json clamps = load(kFixtureDir / "clamps.json");
    REQUIRE(clamps.contains("gate_threshold"));
    REQUIRE(clamps.contains("veto_threshold"));

    // Distinct values, so a fallback cannot masquerade as a correct read: if the
    // product ever silently reused the veto again, the value below would move.
    const double gate = clamps.at("gate_threshold").get<double>();
    const double veto = clamps.at("veto_threshold").get<double>();
    CHECK(gate != veto);
    CHECK(gate > 0.0);
    CHECK(gate < 1.0);

    // Loads with the key present.
    CHECK_NOTHROW(polymesh::advisor::Advisor(kFixtureDir));

    // Now stage a copy of the fixture with the key removed, and require that the
    // constructor refuses it rather than defaulting.
    const std::filesystem::path staged =
        std::filesystem::temp_directory_path() / "polymesh_advisor_gate_missing";
    std::filesystem::remove_all(staged);
    std::filesystem::create_directories(staged);
    for (const auto& entry : std::filesystem::directory_iterator(kFixtureDir)) {
        std::filesystem::copy_file(entry.path(), staged / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing);
    }
    json stripped = clamps;
    stripped.erase("gate_threshold");
    REQUIRE_FALSE(stripped.contains("gate_threshold"));
    REQUIRE(stripped.contains("veto_threshold")); // the tempting fallback is still there
    {
        std::ofstream out(staged / "clamps.json");
        REQUIRE(out.good());
        out << stripped.dump(2) << "\n";
    }
    CHECK_THROWS_AS(polymesh::advisor::Advisor(staged), polymesh::advisor::AdvisorError);

    // A non-probability is equally a misconfiguration.
    json out_of_range = clamps;
    out_of_range["gate_threshold"] = 1.5;
    {
        std::ofstream out(staged / "clamps.json");
        REQUIRE(out.good());
        out << out_of_range.dump(2) << "\n";
    }
    CHECK_THROWS_AS(polymesh::advisor::Advisor(staged), polymesh::advisor::AdvisorError);

    std::filesystem::remove_all(staged);
}

TEST_CASE("advisor requires ood.json and refuses rather than imputes", "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }

    const json ood = load(kFixtureDir / "ood.json");
    REQUIRE(ood.contains("feature_columns"));
    REQUIRE(ood.contains("center"));
    REQUIRE(ood.contains("scale"));
    REQUIRE(ood.at("operating_point").contains("threshold"));

    // The distance is fitted and evaluated in RAW feature units with the file's
    // own center/scale, deliberately NOT reusing normalization.json: the shipped
    // ONNX contract (62 columns, geo_* exact-BRep descriptors included)
    // standardizes with normalization.json's mean/std, a different space than
    // ood.json's, so the OOD vector is assembled by name from the raw columns
    // rather than read from encode()'s output.
    const auto names = ood.at("feature_columns").get<std::vector<std::string>>();
    REQUIRE_FALSE(names.empty());
    CHECK(ood.at("center").size() == names.size());
    CHECK(ood.at("scale").size() == names.size());
    CHECK(ood.at("precision").size() == names.size());

    // A model directory without an OOD block must not load. The whole defect this
    // guards is a validated detector sitting inert on disk while the product runs
    // without it, so the requirement is defended by a test and not only by the
    // loader.
    const std::filesystem::path staged =
        std::filesystem::temp_directory_path() / "polymesh_advisor_ood_missing";
    std::filesystem::remove_all(staged);
    std::filesystem::create_directories(staged);
    for (const auto& entry : std::filesystem::directory_iterator(kFixtureDir)) {
        std::filesystem::copy_file(entry.path(), staged / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing);
    }
    CHECK_NOTHROW(polymesh::advisor::Advisor(staged));
    std::filesystem::remove(staged / "ood.json");
    CHECK_THROWS_AS(polymesh::advisor::Advisor(staged), polymesh::advisor::AdvisorError);
    std::filesystem::remove_all(staged);

    // Inference must NEVER throw, even when the OOD test cannot be performed:
    // advisor.hpp promises an unusable prediction becomes a veto. A query missing
    // a required descriptor is refused, not imputed -- imputing the training
    // median would place an unknown part at the centre of the training
    // distribution and report it as maximally familiar.
    const polymesh::advisor::Advisor advisor(kFixtureDir);
    polymesh::advisor::FeatureColumns empty;
    polymesh::advisor::AdvisorDecision decision;
    REQUIRE_NOTHROW(decision = advisor.recommend(empty));
    CHECK(decision.vetoed);
    CHECK(decision.note.find("out-of-distribution test unavailable") != std::string::npos);
    // Every prediction is suppressed on a refusal: an extrapolating head reported
    // predicted_mesh_ms = 1.66e14 (about 5,300 years) on the case that motivated
    // this gate, and printing that to a user is its own defect.
    CHECK_FALSE(std::isfinite(decision.predicted_mesh_ms));
    CHECK_FALSE(std::isfinite(decision.predicted_rel_err));
    CHECK_FALSE(std::isfinite(decision.failure_prob));
}

TEST_CASE("advisor head outputs match PyTorch within the exported tolerance", "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }
    const json parity = load(kFixtureDir / "parity.json");
    const polymesh::advisor::Advisor advisor(kFixtureDir);

    // Parity is RELATIVE, and the tolerance ships with the fixture instead of
    // being hardcoded here. ONNX Runtime and PyTorch use different float32 GEMM
    // kernels and accumulation orders, so absolute error scales with output
    // magnitude: the fixture's failure_logit of +6.5 lands ~4.8e-6 away in
    // absolute terms while being 3.9e-7 in relative terms.
    const double tolerance = parity.at("tolerance").at("relative").get<double>();
    const auto within = [tolerance](double actual, double expected) {
        return std::abs(actual - expected) / std::max(1.0, std::abs(expected)) <= tolerance;
    };

    std::size_t checked = 0;
    for (const auto& fixture_case : parity.at("cases")) {
        INFO("case " << fixture_case.at("name").get<std::string>());
        // `evaluate` applies no policy of its own, so this is the same single
        // forward pass the exporter ran — real parity, not a re-derivation.
        const auto raw = advisor.evaluate(columns_of(fixture_case.at("features")));
        const json& expected = fixture_case.at("outputs");

        CHECK(within(raw.rel_err_log10, expected.at("rel_err").get<double>()));
        // The centred accuracy head. It is a log10 difference rather than a
        // level, so it is compared exactly as the exporter emitted it — no
        // de-logging on either side.
        CHECK(within(raw.rel_err_rel, expected.at("rel_err_rel").get<double>()));
        CHECK(within(raw.geo_chamfer_log10, expected.at("geo_chamfer").get<double>()));
        CHECK(within(raw.geo_p99_log10, expected.at("geo_p99").get<double>()));
        CHECK(within(raw.dof_log10, expected.at("dof").get<double>()));
        CHECK(within(raw.mesh_ms_log10, expected.at("mesh_ms").get<double>()));
        CHECK(within(raw.solve_ms_log10, expected.at("solve_ms").get<double>()));
        CHECK(within(raw.failure_logit, expected.at("failure_logit").get<double>()));

        const auto expected_policy = expected.at("policy").get<std::vector<double>>();
        REQUIRE(raw.policy.size() == expected_policy.size());
        for (std::size_t i = 0; i < expected_policy.size(); ++i) {
            INFO("policy dim " << i);
            CHECK(within(raw.policy[i], expected_policy[i]));
        }
        ++checked;
    }
    // nominal, clamped_low_h_rel, vetoed_failure, imputed_defaults.
    CHECK(checked == 4);
}

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
    // Read strictly, exactly as Advisor does. Mirroring the old
    // `value("gate_threshold", veto_threshold)` fallback here would let the test
    // keep passing against a clamps.json the product now rejects.
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

        // A refusal now SUPPRESSES every prediction rather than carrying it, and
        // there are two independent causes of refusal, so neither the predicted
        // values nor `vetoed` can be predicted from the feasibility head alone.
        //
        // Suppression is the point: beyond the training support the heads diverge
        // rather than degrade -- the case that motivated the OOD gate reported
        // predicted_mesh_ms = 1.66e14, about 5,300 years, beside a failure
        // probability of 1e-65 claiming near-certain success. NaN is the honest
        // value and a reader can see the model has no opinion.
        if (decision.vetoed) {
            CHECK_FALSE(std::isfinite(decision.failure_prob));
            CHECK_FALSE(std::isfinite(decision.predicted_rel_err_rel));
            CHECK_FALSE(std::isfinite(decision.predicted_mesh_ms));
            CHECK_FALSE(std::isfinite(decision.predicted_dof));
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

    // The same de-logging as advisor.cpp's from_log10, reimplemented here so
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
    const auto score_candidates =
        [&](const polymesh::advisor::FeatureColumns& columns) {
            std::vector<Scored> table;
            for (const auto& action : actions) {
                auto candidate = defaults;
                candidate.mesher = action.value("mesher", defaults.mesher);
                candidate.order = action.value("order", defaults.order);
                candidate.h_rel = action.value("h_rel", defaults.h_rel);
                candidate.eta_target = action.value("eta_target", defaults.eta_target);
                candidate.adapt_passes =
                    action.value("adapt_passes", defaults.adapt_passes);
                auto query = columns;
                advisor.apply_action(query, candidate);
                const auto raw = advisor.evaluate(query);
                if (!std::isfinite(raw.rel_err_rel)) {
                    continue;
                }
                table.push_back({candidate, raw.rel_err_rel,
                                 sigmoid(raw.failure_logit), dof_of(raw.dof_log10)});
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
    // historical unfiltered one bit-for-bit on every parity case.
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
        CHECK(same_number(unfiltered.predicted_rel_err_rel,
                          budget_off.predicted_rel_err_rel));
        CHECK(same_number(unfiltered.predicted_chamfer_mean,
                          budget_off.predicted_chamfer_mean));
        CHECK(same_number(unfiltered.predicted_dof, budget_off.predicted_dof));
        CHECK(same_number(unfiltered.predicted_mesh_ms, budget_off.predicted_mesh_ms));
        CHECK(same_number(unfiltered.predicted_solve_ms, budget_off.predicted_solve_ms));
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
    // The fixture is only proof if it actually reached both arms.
    CHECK(exercised_pick);
    CHECK(exercised_refusal);
}

// Not run by default: it is a measurement, not a pass/fail assertion, and the
// number is machine-dependent. Run with
//   build/tests/polymesh_tests.exe "[advisor][!benchmark]"
// The cost matters because the shipped rule went from roughly two forward passes
// to one per candidate action, and that trade needed quantifying before we
// called it shippable rather than after.
TEST_CASE("advisor recommend() latency", "[advisor][!benchmark]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing");
    }
    const json clamps = load(kFixtureDir / "clamps.json");
    const json parity = load(kFixtureDir / "parity.json");
    const std::size_t candidates =
        clamps.contains("candidate_grid")
            ? clamps.at("candidate_grid").value("n_candidates", std::size_t{0})
            : std::size_t{0};

    const polymesh::advisor::Advisor advisor(kFixtureDir);
    const auto columns = columns_of(parity.at("cases")[0].at("features"));

    // Warm the session and the allocator so the first call's one-off costs do
    // not land in the distribution.
    for (int i = 0; i < 5; ++i) {
        (void)advisor.recommend(columns);
    }

    constexpr int kRuns = 200;
    std::vector<double> micros;
    micros.reserve(kRuns);
    for (int i = 0; i < kRuns; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto decision = advisor.recommend(columns);
        const auto end = std::chrono::steady_clock::now();
        (void)decision;
        micros.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(micros.begin(), micros.end());
    const auto pct = [&](double q) {
        const auto index =
            static_cast<std::size_t>(q * static_cast<double>(micros.size() - 1));
        return micros[index];
    };

    const auto model_bytes = std::filesystem::file_size(kFixtureDir / "model.onnx");
    WARN("advisor recommend() over " << kRuns << " calls, " << candidates
         << " candidate actions, single-threaded:"
         << "\n  p50 " << pct(0.50) << " us"
         << "\n  p90 " << pct(0.90) << " us"
         << "\n  p99 " << pct(0.99) << " us"
         << "\n  max " << micros.back() << " us"
         << "\n  per candidate (p50) " << (candidates ? pct(0.50) / static_cast<double>(candidates) : 0.0)
         << " us"
         << "\n  fixture model.onnx " << model_bytes << " bytes");

    // A guard, not a benchmark target: if a recommendation ever costs more than
    // a tenth of a second it is no longer negligible against a solve and the
    // candidate list should be pruned via max_candidates.
    CHECK(pct(0.99) < 100000.0);
}

// Emits, for every corpus part: the 15 exact-BRep descriptors, the ASSEMBLED
// out-of-distribution vector in ood.json's own column order, and the
// ood_distance the C++ actually computed. Diffed against the Python reference
// (`bench/advisor/geometry_features.csv` from geometry_features.py, and
// `calibration.py:ood_scores`).
//
// Not a pass/fail assertion and not run by default: it produces evidence.
//
// The distance and the vector are emitted because descriptor agreement alone
// proves nothing about the decision. Every descriptor can match to six figures
// while the distance diverges through a precision matrix whose condition number
// is 9.15e10, and the distance is what decides whether the product refuses.
// Emitting only descriptors and then asserting distance agreement -- computing
// both sides in Python from those descriptors -- would not exercise the C++
// quadratic form at all. It has to be the number the C++ produced.
//
// The full production path is used: CAD -> extract_case_features -> to_columns ->
// Advisor::recommend, so feature assembly and name resolution are covered too,
// not just the descriptor extractor.
TEST_CASE("advisor descriptor dump", "[advisor][!benchmark]") {
    const std::filesystem::path corpus = "bench/geometries/corpus/primitives";
    const std::filesystem::path model_dir = "bench/advisor";
    if (!std::filesystem::is_directory(corpus) ||
        !std::filesystem::exists(model_dir / "ood.json")) {
        SKIP("corpus STEP directory or bench/advisor/ood.json missing");
    }
    if (!polymesh::geom::occ_enabled()) {
        SKIP("built without OpenCASCADE");
    }

    std::vector<std::filesystem::path> steps;
    for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
        if (entry.path().extension() == ".step") {
            steps.push_back(entry.path());
        }
    }
    std::sort(steps.begin(), steps.end());
    REQUIRE_FALSE(steps.empty());

    // The column ORDER is read from the artifact, not hardcoded, so the emitted
    // vector is directly comparable to what mahalanobis() indexes.
    const json ood = load(model_dir / "ood.json");
    const auto ood_names = ood.at("feature_columns").get<std::vector<std::string>>();
    const polymesh::advisor::Advisor advisor(model_dir);

    json out = json::object();
    out["ood_column_order"] = ood_names;
    out["ood_threshold"] = ood.at("operating_point").at("threshold").get<double>();
    json parts = json::object();
    for (const std::filesystem::path& step : steps) {
        auto model = polymesh::pipeline::Model::load(step.string());
        // No BC regions and a nominal load direction: the OOD fit deliberately
        // excludes boundary-condition columns, so the distance must not depend on
        // them. Emitting it this way makes that property checkable rather than
        // asserted.
        const auto features = polymesh::pipeline::extract_case_features(
            model, {}, {}, Eigen::Vector3d(1.0, 0.0, 0.0), 0.3);
        const auto columns = polymesh::advisor::to_columns(features);
        const auto decision = advisor.recommend(columns);

        json record;
        record["geo_available"] = features.geo_available;
        record["ood_distance"] = decision.ood_distance;
        record["vetoed"] = decision.vetoed;
        record["note"] = decision.note;
        json vector = json::object();
        for (const std::string& name : ood_names) {
            const auto it = columns.find(name);
            vector[name] = it == columns.end() ? json() : json(it->second);
        }
        record["ood_vector"] = vector;
        parts[step.stem().string()] = record;
    }
    out["parts"] = parts;

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "polymesh_cpp_descriptors.json";
    std::ofstream stream(path);
    REQUIRE(stream.good());
    stream << out.dump(2) << "\n";
    stream.close();
    WARN("wrote C++ OOD vectors and distances for " << steps.size() << " parts to "
         << path.string());
}
