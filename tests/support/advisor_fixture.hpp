// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Access to the advisor_tiny fixture (tests/fixtures/advisor_tiny/), produced by
// `python scripts/advisor/export_onnx.py --tiny-fixture`. It carries the graph,
// the normalization/clamp artifacts, and PyTorch's own outputs for four inputs
// chosen to reach the nominal, clamped, vetoed, and imputed paths, so advisor
// tests compare against the exporter rather than hand-copied numbers.

#include "advisor/advisor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace polymesh::test_support {

extern const std::filesystem::path kFixtureDir;

/// Parse a JSON file; REQUIREs that it opens.
nlohmann::json load(const std::filesystem::path& path);

/// True when model.onnx, parity.json, clamps.json and normalization.json exist.
bool fixture_present();

/// The fixture's raw, pre-standardization row, exactly as the exporter fed it.
/// `imputed_defaults` deliberately omits five columns so the C++ impute path is
/// the thing under test for that case.
advisor::FeatureColumns columns_of(const nlohmann::json& raw);

double sigmoid(double x);

} // namespace polymesh::test_support
