// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to polymesh_advisor: the state behind `Advisor` and the helpers more
// than one advisor translation unit uses. Never included outside src/advisor/src.

#include "advisor/advisor.hpp"
#include "advisor/calibration.hpp"

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace polymesh::advisor {
namespace detail {

using json = nlohmann::json;

/// Parse one model-directory artifact. Throws AdvisorError naming the file when
/// it cannot be opened or is not valid JSON.
json read_json(const std::filesystem::path& path);

/// Continuous clamp interval read from clamps.json.
struct Interval {
    double lo = 0.0;
    double hi = 0.0;

    [[nodiscard]] double clamp(double value, bool& clamped) const {
        if (!std::isfinite(value)) {
            clamped = true;
            return lo;
        }
        if (value < lo || value > hi) {
            clamped = true;
        }
        return std::clamp(value, lo, hi);
    }
};

// Must match `scripts/advisor/dataset.py:OUTPUT_NAMES` exactly, names and
// order. `rel_err_rel` is `rel_err` centred on its per-case median and is
// deliberately adjacent to it, so a future head insertion cannot separate the
// pair without this list failing the load-time check.
inline constexpr std::array<const char*, 12> kOutputNames{
    "rel_err",  "rel_err_rel", "geo_chamfer", "geo_p99",   "dof",           "mesh_ms",
    "solve_ms", "solve_flops", "solve_bytes", "mesh_work", "failure_logit", "policy"};

// The activation taps, in the order `scripts/advisor/export_onnx.py` appends
// them AFTER the twelve contract outputs. Appended, never interleaved: the
// contract indices above are what `evaluate()` unpacks positionally, so a tap
// inserted among them would silently re-label every head.
//
// `trunk_input` is the post-embedding concatenation (the trunk's real input
// width, narrower than `input_columns` by the two categorical columns and wider
// by the two embedding blocks); `trunk_fc1` and `trunk_fc2` are the POST-GELU
// hidden tensors.
inline constexpr std::array<const char*, 3> kActivationOutputNames{"trunk_input", "trunk_fc1",
                                                                   "trunk_fc2"};

} // namespace detail

struct Advisor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "polymesh_advisor"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

    std::vector<std::string> input_columns;
    std::vector<double> mean;
    std::vector<double> stddev;
    std::vector<double> impute;
    std::vector<int> order_choices;
    std::vector<std::string> mesher_choices;

    detail::Interval h_rel{};
    detail::Interval eta_target{};
    detail::Interval adapt_passes{};
    double veto_threshold = 0.5;
    AdvisorDecision default_decision;
    std::vector<std::string> action_dims;

    /// Discrete actions the chooser enumerates, from clamps.json:candidate_grid.
    /// Empty when the artifact has no grid, in which case recommend() uses the
    /// single-shot policy read.
    std::vector<AdvisorDecision> candidates;
    /// Candidates whose predicted failure probability exceeds this are dropped
    /// before ranking. Distinct from `veto_threshold`, which refuses the whole
    /// recommendation after the fact: this one improves the choice, that one
    /// abandons it. Required from clamps.json -- deliberately no default, see
    /// load_clamps().
    double gate_threshold = std::numeric_limits<double>::quiet_NaN();
    AdvisorObjective objective = AdvisorObjective::kAccuracy;
    std::optional<HostCalibration> calibration;

    /// Out-of-distribution test, from ood.json.
    ///
    /// Independent of the network's input vector: evaluated over `ood_columns`
    /// in the FILE's order, each value resolved by name from the caller's
    /// FeatureColumns (a wrong-order Mahalanobis still yields plausible-looking
    /// distances), in raw units scaled by ood.json's own `center`/`scale`. That
    /// is a different standardization than normalization.json's `mean`/`std`,
    /// so encode()'s row is the wrong space even where the column sets overlap.
    /// Neither set's width or membership is assumed; each comes from its file.
    ///
    /// `ood_precision` is the k*k inverse of the shrunk training covariance in
    /// the file's scaled space, row-major; the scaling keeps it far better
    /// conditioned than a raw-unit fit (ood.json records
    /// `precision_condition_number`). Required, never defaulted -- see load_ood().
    std::vector<std::string> ood_columns;
    std::vector<double> ood_center;
    std::vector<double> ood_scale;
    std::vector<double> ood_precision;
    double ood_threshold = std::numeric_limits<double>::quiet_NaN();

    std::string input_name;
    std::vector<const char*> output_name_ptrs;

    /// The contract output names followed by `kActivationOutputNames`.
    /// Non-empty only when the graph exports the taps. Kept as a SECOND list so
    /// the production path keeps asking for only the contract outputs: the taps
    /// are materialised intermediates, and a campaign that never draws anything
    /// should not pay for them.
    std::vector<const char*> tap_output_name_ptrs;

    /// Set when `activation_layout.json` loaded AND agreed with the graph.
    /// Everything the drawing needs is either fully consistent or absent; there
    /// is no partial mode, because a layout that half-matches the graph draws
    /// edges between neurons that are not connected.
    bool activations_available = false;
    /// Why not, when `activations_available` is false. Reported through the
    /// `explain()` error rather than swallowed, so a stale model directory says
    /// what to re-export instead of silently rendering nothing.
    std::string activation_note =
        "advisor: this model directory was loaded without activation "
        "taps";
    NetworkLayout layout;

    void load_normalization(const std::filesystem::path& dir);
    void load_clamps(const std::filesystem::path& dir);
    void load_ood(const std::filesystem::path& dir);
    /// Read and validate `activation_layout.json` against the graph. Never
    /// throws: a missing or inconsistent sidecar leaves
    /// `activations_available == false` with a note, because a model directory
    /// without it must still load and recommend unchanged.
    void load_activation_layout(const std::filesystem::path& dir, bool graph_has_taps);
    /// Mahalanobis distance of one query, in raw feature units. Accumulates in
    /// double regardless of storage width: the precision matrix is
    /// ill-conditioned by construction and a float32 accumulation here would be
    /// indistinguishable from a real disagreement with the Python reference.
    [[nodiscard]] double mahalanobis(const FeatureColumns& columns) const;
    [[nodiscard]] std::vector<float> encode(const FeatureColumns& columns) const;
    void apply_action(FeatureColumns& columns, const AdvisorDecision& action) const;
    [[nodiscard]] std::vector<std::vector<float>>
    run(const std::vector<float>& row, const std::vector<const char*>& names) const;
    /// One forward pass on these columns. With `frame == nullptr` this is the
    /// production path: only the contract outputs are requested. With a frame, and
    /// only when the taps are available, the SAME `Run` also returns the trunk
    /// tensors, so the recorded activations belong to the recorded outputs
    /// rather than to a second pass that might not agree with the first.
    [[nodiscard]] AdvisorRawOutputs forward(const FeatureColumns& columns,
                                            ActivationFrame* frame) const;
};

} // namespace polymesh::advisor
