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

        CHECK(decision.failure_prob == Catch::Approx(failure_prob).margin(1e-9));
        CHECK(decision.vetoed == (failure_prob > veto_threshold));
        // Reported RAW: this head is a log10 difference, not a level, and it is
        // carried across the veto branch so a vetoed row still says what the
        // discarded recommendation scored.
        CHECK(decision.predicted_rel_err_rel ==
              Catch::Approx(scored_raw.rel_err_rel).margin(1e-9));

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
        const auto index = static_cast<std::size_t>(q * (micros.size() - 1));
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
