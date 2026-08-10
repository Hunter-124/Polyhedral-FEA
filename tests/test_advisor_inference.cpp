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

TEST_CASE("advisor guardrails clamp the box and honour the veto", "[advisor]") {
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
    const double passes_ceil_f = static_cast<double>(passes_ceil);
    const double veto_threshold = clamps.value("veto_threshold", 0.5);
    const auto order_choices = clamps.at("order_choices").get<std::vector<int>>();
    const auto mesher_choices = clamps.at("mesher_choices").get<std::vector<std::string>>();

    // The clamp box must contain its own defaults, or a "safe fallback" would
    // itself be an out-of-box action.
    CHECK(defaults.h_rel >= h_floor);
    CHECK(defaults.h_rel <= h_ceil);
    CHECK(defaults.eta_target >= eta_floor);
    CHECK(defaults.eta_target <= eta_ceil);

    bool saw_clamped_floor = false;
    bool saw_veto = false;
    bool saw_nominal = false;

    for (const auto& fixture_case : parity.at("cases")) {
        const std::string name = fixture_case.at("name").get<std::string>();
        INFO("case " << name);
        const auto columns = columns_of(fixture_case.at("features"));
        const auto decision = advisor.recommend(columns);

        // Guardrail 1 — whatever the heads said, the decision is in the box.
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

        // `recommend` queries the policy at the default action, so that is the
        // row whose policy output drives the decision. The fixture rows already
        // carry the default action, so this is the fixture row itself and the
        // forced policy values are exactly what the C++ sees.
        auto query = columns;
        advisor.apply_action(query, defaults);
        const auto policy = advisor.evaluate(query).policy;

        // The clamp is applied to the raw policy before anything else, and the
        // flag survives a veto, so it can be checked on every case.
        const double clamped_h = std::clamp(policy[0], h_floor, h_ceil);
        const bool expect_clamped = policy[0] != clamped_h ||
                                    policy[1] != std::clamp(policy[1], 0.0, passes_ceil_f) ||
                                    policy[2] != std::clamp(policy[2], eta_floor, eta_ceil);
        CHECK(decision.clamped == expect_clamped);
        if (policy[0] < h_floor) {
            saw_clamped_floor = true;
        }

        // Reconstruct the pre-veto recommendation: it is what pass 2 scores,
        // whether or not the veto then discards it.
        auto proposed = decision;
        proposed.h_rel = clamped_h;
        proposed.adapt_passes =
            static_cast<int>(std::lround(std::clamp(policy[1], 0.0, passes_ceil_f)));
        proposed.eta_target = std::clamp(policy[2], eta_floor, eta_ceil);
        proposed.p_elevate = policy[3] > 0.0;
        proposed.order = order_choices[static_cast<std::size_t>(std::distance(
            policy.begin() + 4,
            std::max_element(policy.begin() + 4,
                             policy.begin() + 4 +
                                 static_cast<std::ptrdiff_t>(order_choices.size()))))];
        const auto mesher_begin =
            policy.begin() + 4 + static_cast<std::ptrdiff_t>(order_choices.size());
        proposed.mesher = mesher_choices[static_cast<std::size_t>(std::distance(
            mesher_begin,
            std::max_element(mesher_begin,
                             mesher_begin +
                                 static_cast<std::ptrdiff_t>(mesher_choices.size()))))];

        auto scored = query;
        advisor.apply_action(scored, proposed);
        const double failure_prob = sigmoid(advisor.evaluate(scored).failure_logit);
        CHECK(decision.failure_prob == Catch::Approx(failure_prob).margin(1e-9));
        CHECK(decision.vetoed == (failure_prob > veto_threshold));

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

        // Not vetoed: the decision IS the clamped policy, exactly.
        saw_nominal = true;
        CHECK(decision.h_rel == Catch::Approx(clamped_h).margin(1e-12));
        CHECK(decision.eta_target == Catch::Approx(proposed.eta_target).margin(1e-12));
        CHECK(decision.adapt_passes == proposed.adapt_passes);
        CHECK(decision.order == proposed.order);
        CHECK(decision.mesher == proposed.mesher);
        CHECK(decision.p_elevate == proposed.p_elevate);
        if (policy[0] < h_floor) {
            CHECK(decision.h_rel == Catch::Approx(h_floor).margin(1e-12));
        }
    }

    // The fixture is only proof if it actually reaches both guardrails.
    CHECK(saw_clamped_floor);
    CHECK(saw_veto);
    CHECK(saw_nominal);
}
