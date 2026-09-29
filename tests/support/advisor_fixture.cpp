// SPDX-License-Identifier: BSD-3-Clause
#include "support/advisor_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>

namespace polymesh::test_support {

const std::filesystem::path kFixtureDir = "tests/fixtures/advisor_tiny";

nlohmann::json load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    REQUIRE(stream.good());
    nlohmann::json parsed;
    stream >> parsed;
    return parsed;
}

bool fixture_present() {
    return std::filesystem::exists(kFixtureDir / "model.onnx") &&
           std::filesystem::exists(kFixtureDir / "parity.json") &&
           std::filesystem::exists(kFixtureDir / "clamps.json") &&
           std::filesystem::exists(kFixtureDir / "normalization.json");
}

advisor::FeatureColumns columns_of(const nlohmann::json& raw) {
    advisor::FeatureColumns columns;
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

} // namespace polymesh::test_support
