// SPDX-License-Identifier: BSD-3-Clause
// Learned mesh advisor (ADR-0027): C++/PyTorch head-output parity against the
// advisor_tiny fixture, plus two opt-in [!benchmark] diagnostics (recommend()
// latency and the corpus OOD descriptor dump).

#include "advisor/advisor.hpp"
#include "geom/step.hpp"
#include "pipeline/scene.hpp"
#include "support/advisor_fixture.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using polymesh::test_support::columns_of;
using polymesh::test_support::fixture_present;
using polymesh::test_support::kFixtureDir;
using polymesh::test_support::load;

} // namespace

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
    // magnitude.
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
        CHECK(within(raw.solve_flops_log10, expected.at("solve_flops").get<double>()));
        CHECK(within(raw.solve_bytes_log10, expected.at("solve_bytes").get<double>()));
        CHECK(within(raw.mesh_work_log10, expected.at("mesh_work").get<double>()));
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

// Not run by default: it is a measurement, not a pass/fail assertion, and the
// number is machine-dependent. Run with
//   build/tests/polymesh_tests.exe "[advisor][!benchmark]"
// Quantifies the per-candidate forward-pass cost of the enumerating rule.
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
        micros.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    std::sort(micros.begin(), micros.end());
    const auto pct = [&](double q) {
        const auto index =
            static_cast<std::size_t>(q * static_cast<double>(micros.size() - 1));
        return micros[index];
    };

    const auto model_bytes = std::filesystem::file_size(kFixtureDir / "model.onnx");
    WARN("advisor recommend() over "
         << kRuns << " calls, " << candidates << " candidate actions, single-threaded:"
         << "\n  p50 " << pct(0.50) << " us"
         << "\n  p90 " << pct(0.90) << " us"
         << "\n  p99 " << pct(0.99) << " us"
         << "\n  max " << micros.back() << " us"
         << "\n  per candidate (p50) "
         << (candidates ? pct(0.50) / static_cast<double>(candidates) : 0.0) << " us"
         << "\n  fixture model.onnx " << model_bytes << " bytes");

    // A guard, not a benchmark target: if a recommendation ever costs more than
    // a tenth of a second it is no longer negligible against a solve and the
    // candidate list should be pruned via max_candidates.
    CHECK(pct(0.99) < 100000.0);
}

// Evidence generator, not run by default and asserting nothing: for every
// corpus part, emits the 15 exact-BRep descriptors, the ASSEMBLED
// out-of-distribution vector in ood.json's own column order, and the
// ood_distance the C++ actually computed, for diffing against the Python
// reference (`bench/advisor/geometry_features.csv`, `calibration.py:ood_scores`).
// The distance must be the C++ number: descriptor agreement alone does not
// exercise the C++ quadratic form (docs/advisor/0004-model-card.md).
//
// The full production path is used: CAD -> extract_case_features -> to_columns ->
// Advisor::recommend, so feature assembly and name resolution are covered too.
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
