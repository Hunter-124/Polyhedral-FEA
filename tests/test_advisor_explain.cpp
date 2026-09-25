// SPDX-License-Identifier: BSD-3-Clause
// Learned mesh advisor: the deployed network explaining itself (ADR-0042).
//
// Two fixtures, on purpose. `advisor_tiny` is a graph WITHOUT the trunk taps
// and with no `activation_layout.json`, i.e. every model directory exported
// before this feature existed; it must still load and recommend, and must
// report that it cannot explain itself. `advisor_explain` is the same
// construction WITH the taps and the sidecar, and its `parity.json` carries
// float64 PyTorch's own trunk tensors, so the taps are checked against the
// reference implementation rather than against themselves.

#include "advisor/advisor.hpp"
#include "support/advisor_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using polymesh::test_support::columns_of;
using polymesh::test_support::fixture_present;
using polymesh::test_support::kFixtureDir;
using polymesh::test_support::load;
using polymesh::test_support::sigmoid;

const std::filesystem::path kExplainFixtureDir = "tests/fixtures/advisor_explain";

bool explain_fixture_present() {
    return std::filesystem::exists(kExplainFixtureDir / "model.onnx") &&
           std::filesystem::exists(kExplainFixtureDir / "parity.json") &&
           std::filesystem::exists(kExplainFixtureDir / "activation_layout.json");
}

} // namespace

TEST_CASE("advisor trunk taps match PyTorch, and the layout matches the graph", "[advisor]") {
    if (!explain_fixture_present()) {
        SKIP("advisor_explain fixture missing (python scripts/advisor/export_onnx.py "
             "--explain-fixture)");
    }
    const json parity = load(kExplainFixtureDir / "parity.json");
    const polymesh::advisor::Advisor advisor(kExplainFixtureDir);
    REQUIRE(advisor.has_activations());

    // The drawing is only honest if the nodes and the weight blocks describe the
    // graph that ran, so the sidecar is checked against the graph's own tensor
    // widths rather than trusted.
    const auto& layout = advisor.layout();
    REQUIRE(layout.layers.size() == 4);
    CHECK(layout.layers[0].name == "input");
    CHECK(layout.layers[1].name == "trunk.fc1");
    CHECK(layout.layers[2].name == "trunk.fc2");
    CHECK(layout.layers[3].name == "heads");
    REQUIRE(layout.edges.size() == 3);
    for (std::size_t e = 0; e < layout.edges.size(); ++e) {
        INFO("edge block " << e);
        CHECK(layout.edges[e].cols == layout.layers[e].size);
        CHECK(layout.edges[e].rows == layout.layers[e + 1].size);
        CHECK(layout.edges[e].weights.size() == layout.edges[e].rows * layout.edges[e].cols);
    }

    const double tolerance = parity.at("tolerance").at("relative").get<double>();
    const auto within = [tolerance](double actual, double expected) {
        return std::abs(actual - expected) / std::max(1.0, std::abs(expected)) <= tolerance;
    };
    const auto same_vector = [&](const std::vector<float>& actual, const json& expected,
                                 const char* what) {
        const auto reference = expected.get<std::vector<double>>();
        INFO(what << ": " << actual.size() << " values against " << reference.size());
        REQUIRE(actual.size() == reference.size());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            INFO(what << " unit " << i);
            CHECK(within(static_cast<double>(actual[i]), reference[i]));
        }
    };

    std::size_t checked = 0;
    for (const auto& fixture_case : parity.at("cases")) {
        INFO("case " << fixture_case.at("name").get<std::string>());
        // `taps` applies no policy, so this is the exact row the exporter
        // standardized. The passes `explain` runs each have their action columns
        // overwritten by the candidate being scored, so none of them is this
        // row and none of them could be compared against it.
        const auto taps = advisor.taps(columns_of(fixture_case.at("features")));
        same_vector(taps.input, fixture_case.at("trunk_input"), "trunk_input");
        same_vector(taps.fc1, fixture_case.at("trunk_fc1"), "trunk_fc1");
        same_vector(taps.fc2, fixture_case.at("trunk_fc2"), "trunk_fc2");

        // Widths must agree with what the drawing will index into.
        CHECK(taps.input.size() == layout.layers[0].size);
        CHECK(taps.fc1.size() == layout.layers[1].size);
        CHECK(taps.fc2.size() == layout.layers[2].size);
        CHECK(taps.heads.size() == layout.layers[3].size);

        // The heads vector is the same tensor set `evaluate` unpacks, so the
        // drawn head circles and the numbers the chooser ranks on are one
        // reading of the graph rather than two.
        const auto raw = advisor.evaluate(columns_of(fixture_case.at("features")));
        REQUIRE(taps.heads.size() >= 11);
        CHECK(static_cast<double>(taps.heads[0]) ==
              Catch::Approx(raw.rel_err_log10).epsilon(1e-6));
        CHECK(static_cast<double>(taps.heads[10]) ==
              Catch::Approx(raw.failure_logit).epsilon(1e-6));
        ++checked;
    }
    CHECK(checked == 4);
}

TEST_CASE("advisor explain reports the same decision recommend does", "[advisor]") {
    if (!explain_fixture_present()) {
        SKIP("advisor_explain fixture missing (python scripts/advisor/export_onnx.py "
             "--explain-fixture)");
    }
    const json parity = load(kExplainFixtureDir / "parity.json");
    const json clamps = load(kExplainFixtureDir / "clamps.json");
    const polymesh::advisor::Advisor advisor(kExplainFixtureDir);

    const std::size_t n_candidates = clamps.at("candidate_grid").at("actions").size();
    CHECK(
        advisor.explain(columns_of(parity.at("cases").at(0).at("features"))).gate_threshold ==
        Catch::Approx(clamps.at("gate_threshold").get<double>()));

    for (const auto& fixture_case : parity.at("cases")) {
        INFO("case " << fixture_case.at("name").get<std::string>());
        const auto columns = columns_of(fixture_case.at("features"));
        const auto decision = advisor.recommend(columns);
        const auto explanation = advisor.explain(columns);

        // Explaining a decision must not be able to change it: same action,
        // same predictions, same refusal state. If these two could disagree
        // the film would be narrating a decision the product never made.
        CHECK(explanation.decision.mesher == decision.mesher);
        CHECK(explanation.decision.order == decision.order);
        CHECK(explanation.decision.h_rel == Catch::Approx(decision.h_rel).margin(1e-12));
        CHECK(explanation.decision.adapt_passes == decision.adapt_passes);
        CHECK(explanation.decision.eta_target ==
              Catch::Approx(decision.eta_target).margin(1e-12));
        CHECK(explanation.decision.vetoed == decision.vetoed);
        CHECK(explanation.decision.budget_refusal == decision.budget_refusal);

        // One pass per enumerated candidate, then the re-score. The candidate
        // indices are the grid's own, in enumeration order.
        REQUIRE(explanation.frames.size() == n_candidates + 1);
        for (std::size_t i = 0; i < n_candidates; ++i) {
            INFO("frame " << i);
            CHECK(explanation.frames[i].candidate == static_cast<int>(i));
            CHECK_FALSE(explanation.frames[i].recommended);
        }
        CHECK(explanation.frames.back().candidate == -1);
        CHECK(explanation.frames.back().recommended);

        // The gate flag is not a separate opinion: it is the threshold applied
        // to the failure logit of that very pass.
        for (const auto& frame : explanation.frames) {
            const bool gate =
                sigmoid(frame.outputs.failure_logit) <= explanation.gate_threshold;
            CHECK(frame.gate_pass == gate);
        }
    }
}

TEST_CASE("an advisor without trunk taps recommends but declines to explain", "[advisor]") {
    if (!fixture_present()) {
        SKIP("advisor_tiny fixture missing (python scripts/advisor/export_onnx.py "
             "--tiny-fixture)");
    }
    // advisor_tiny is the pre-taps artifact shape, kept exactly so this path
    // stays covered: an older model directory must remain fully usable.
    const polymesh::advisor::Advisor advisor(kFixtureDir);
    CHECK_FALSE(advisor.has_activations());
    CHECK(advisor.layout().empty());

    const json parity = load(kFixtureDir / "parity.json");
    const auto columns = columns_of(parity.at("cases").at(0).at("features"));
    CHECK_NOTHROW(advisor.recommend(columns));
    // Refusing loudly is the point: a caller must not receive empty tensors and
    // draw an unlit network from them.
    CHECK_THROWS_AS(advisor.explain(columns), polymesh::advisor::AdvisorError);
}
