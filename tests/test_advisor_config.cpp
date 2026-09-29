// SPDX-License-Identifier: BSD-3-Clause
// Learned mesh advisor (ADR-0027): model-directory validation. A misconfigured
// artifact must refuse to load rather than run on silent defaults.

#include "advisor/advisor.hpp"
#include "support/advisor_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using polymesh::test_support::fixture_present;
using polymesh::test_support::kFixtureDir;
using polymesh::test_support::load;

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
    // with different correct values. An absent gate key is a misconfiguration,
    // so it must fail loudly rather than inherit the veto's number.
    const json clamps = load(kFixtureDir / "clamps.json");
    REQUIRE(clamps.contains("gate_threshold"));
    REQUIRE(clamps.contains("veto_threshold"));

    // Distinct values, so a fallback cannot masquerade as a correct read: if the
    // product ever silently reused the veto, the value below would move.
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

    // A model directory without an OOD block must not load: a validated
    // detector must never sit inert on disk while the product runs without it.
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
    // Every prediction is suppressed on a refusal: an extrapolating head's
    // output is meaningless beyond the training support and must not be shown.
    CHECK_FALSE(std::isfinite(decision.predicted_mesh_ms));
    CHECK_FALSE(std::isfinite(decision.predicted_solve_flops));
    CHECK_FALSE(std::isfinite(decision.predicted_solve_bytes));
    CHECK_FALSE(std::isfinite(decision.predicted_mesh_work));
    CHECK_FALSE(decision.predicted_solve_seconds.has_value());
    CHECK_FALSE(std::isfinite(decision.predicted_rel_err));
    CHECK_FALSE(std::isfinite(decision.failure_prob));
}
