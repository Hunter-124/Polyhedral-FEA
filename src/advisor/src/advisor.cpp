// SPDX-License-Identifier: BSD-3-Clause
#include "advisor/advisor.hpp"
#include "advisor/calibration.hpp"

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <vector>

namespace polymesh::advisor {
namespace {

using json = nlohmann::json;

json read_json(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw AdvisorError("advisor: cannot open " + path.string());
    }
    try {
        json parsed;
        stream >> parsed;
        return parsed;
    } catch (const std::exception& e) {
        throw AdvisorError("advisor: " + path.string() + " is not valid JSON: " + e.what());
    }
}

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

Interval interval_of(const json& node, const char* key) {
    const auto& entry = node.at(key);
    if (!entry.is_array() || entry.size() != 2) {
        throw AdvisorError(std::string("advisor: clamps.json '") + key +
                           "' must be a [lo, hi] pair");
    }
    Interval out{entry[0].get<double>(), entry[1].get<double>()};
    if (!(out.lo <= out.hi)) {
        throw AdvisorError(std::string("advisor: clamps.json '") + key + "' has lo > hi");
    }
    return out;
}

/// Index of the largest logit in `values[begin, begin + count)`.
std::size_t argmax(const std::vector<double>& values, std::size_t begin, std::size_t count) {
    std::size_t best = begin;
    for (std::size_t i = begin + 1; i < begin + count; ++i) {
        if (values[i] > values[best]) {
            best = i;
        }
    }
    return best - begin;
}

double sigmoid(double x) {
    // Branch on the sign so the exponent argument is never positive: exp of a
    // large positive logit overflows to inf and turns the probability into NaN.
    if (x >= 0.0) {
        return 1.0 / (1.0 + std::exp(-x));
    }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

/// Ten to the power of a head output. A non-finite head output means the model
/// is broken or untrained; it is reported as NaN, never as 0. Zero would be the
/// most optimistic value in the range — a claim of zero error or zero cost —
/// from exactly the condition where the prediction is worthless.
double from_log10(double value) {
    if (!std::isfinite(value)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::pow(10.0, std::clamp(value, -30.0, 30.0));
}

// Must match `scripts/advisor/dataset.py:OUTPUT_NAMES` exactly, names and
// order. `rel_err_rel` is `rel_err` centred on its per-case median and is
// deliberately adjacent to it, so a future head insertion cannot separate the
// pair without this list failing the load-time check.
constexpr std::array<const char*, 12> kOutputNames{
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
constexpr std::array<const char*, 3> kActivationOutputNames{"trunk_input", "trunk_fc1",
                                                            "trunk_fc2"};

/// Sidecar schema tag. Refusing an unknown tag is the point: the layout decides
/// which neuron a drawn edge connects, and a future revision that reorders
/// `layers` or transposes `weights` would still parse.
constexpr const char* kActivationLayoutSchema = "polymesh.advisor.activation_layout/1";

/// The `layers` names, in the order `AdvisorNet.activations()` emits them.
constexpr std::array<const char*, 4> kLayerNames{"input", "trunk.fc1", "trunk.fc2", "heads"};

/// The twelve contract tensors, unpacked positionally. Index i is
/// `kOutputNames[i]`, and construction has already proven the graph agrees with
/// that list name-by-name, so these are not a guess.
AdvisorRawOutputs unpack(const std::vector<std::vector<float>>& raw) {
    AdvisorRawOutputs out;
    out.rel_err_log10 = static_cast<double>(raw[0][0]);
    out.rel_err_rel = static_cast<double>(raw[1][0]);
    out.geo_chamfer_log10 = static_cast<double>(raw[2][0]);
    out.geo_p99_log10 = static_cast<double>(raw[3][0]);
    out.dof_log10 = static_cast<double>(raw[4][0]);
    out.mesh_ms_log10 = static_cast<double>(raw[5][0]);
    out.solve_ms_log10 = static_cast<double>(raw[6][0]);
    out.solve_flops_log10 = static_cast<double>(raw[7][0]);
    out.solve_bytes_log10 = static_cast<double>(raw[8][0]);
    out.mesh_work_log10 = static_cast<double>(raw[9][0]);
    out.failure_logit = static_cast<double>(raw[10][0]);
    out.policy.assign(raw[11].begin(), raw[11].end());
    return out;
}

} // namespace

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

    Interval h_rel{};
    Interval eta_target{};
    Interval adapt_passes{};
    double veto_threshold = 0.5;
    AdvisorDecision default_decision;
    std::vector<std::string> action_dims;

    /// Discrete actions the chooser enumerates, from clamps.json:candidate_grid.
    /// Empty when the artifact predates the grid, in which case recommend()
    /// falls back to the single-shot policy read.
    std::vector<AdvisorDecision> candidates;
    /// Candidates whose predicted failure probability exceeds this are dropped
    /// before ranking. Distinct from `veto_threshold`, which refuses the whole
    /// recommendation after the fact: this one improves the choice, that one
    /// abandons it. Required from clamps.json -- deliberately no default, see
    /// load_clamps(). Measured on the rebuilt corpus (leave-one-family-out, 8
    /// families, 5 seeds), the gated family spans only 0.3233-0.3350 of held-out
    /// regret across thresholds 0.05-0.8, so the choice is made on pick-failure
    /// rate instead: 27.5% at 0.05 against 31.2% at 0.5.
    double gate_threshold = std::numeric_limits<double>::quiet_NaN();
    AdvisorObjective objective = AdvisorObjective::kAccuracy;
    std::optional<HostCalibration> calibration;

    /// Out-of-distribution test, from ood.json.
    ///
    /// Deliberately independent of the network's input vector. The distance is
    /// evaluated over `ood_columns` -- the FILE's column order, resolved by name
    /// from the caller's FeatureColumns, never a map iteration order, because a
    /// wrong-order Mahalanobis still yields plausible-looking distances. This
    /// cannot read `encode()`'s output even where the column sets overlap: the
    /// values arrive in raw units and are scaled by ood.json's own
    /// `center`/`scale`, a different standardization than normalization.json's
    /// `mean`/`std`, so the encoded network row is the wrong space regardless.
    /// (The shipped ONNX contract is the 62 columns of
    /// normalization.json:input_columns, geo_* exact-BRep descriptors included;
    /// ood.json:feature_columns is a separately fitted set that currently
    /// overlaps it entirely, but nothing here assumes that.)
    ///
    /// `ood_precision` is the k*k inverse of the shrunk training covariance in
    /// the file's scaled space, row-major. Fitting directly on raw columns
    /// instead gave a precision matrix at condition number 2.97e20, past
    /// float64's 1/eps; the file's own standardizer brings it to 1.25e11.
    /// Required, never defaulted -- see load_ood().
    std::vector<std::string> ood_columns;
    std::vector<double> ood_center;
    std::vector<double> ood_scale;
    std::vector<double> ood_precision;
    double ood_threshold = std::numeric_limits<double>::quiet_NaN();

    std::string input_name;
    std::vector<const char*> output_name_ptrs;

    /// The same nine names followed by `kActivationOutputNames`. Non-empty only
    /// when the graph exports the taps. Kept as a SECOND list so the production
    /// path keeps asking for exactly nine tensors: the taps are materialised
    /// intermediates, and a campaign that never draws anything should not pay
    /// for them.
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
    /// `activations_available == false` with a note, because an older model
    /// directory must still load and recommend exactly as it did before this
    /// facility existed.
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
    /// production path, unchanged: nine requested outputs. With a frame, and
    /// only when the taps are available, the SAME `Run` also returns the trunk
    /// tensors, so the recorded activations belong to the recorded outputs
    /// rather than to a second pass that might not agree with the first.
    [[nodiscard]] AdvisorRawOutputs forward(const FeatureColumns& columns,
                                            ActivationFrame* frame) const;
};

void Advisor::Impl::load_normalization(const std::filesystem::path& dir) {
    const json norm = read_json(dir / "normalization.json");
    input_columns = norm.at("input_columns").get<std::vector<std::string>>();
    mean = norm.at("mean").get<std::vector<double>>();
    stddev = norm.at("std").get<std::vector<double>>();
    if (norm.contains("impute")) {
        impute = norm.at("impute").get<std::vector<double>>();
    } else {
        impute.assign(input_columns.size(), 0.0);
    }
    if (mean.size() != input_columns.size() || stddev.size() != input_columns.size() ||
        impute.size() != input_columns.size()) {
        throw AdvisorError("advisor: normalization.json mean/std/impute length does not "
                           "match input_columns");
    }
    for (double& s : stddev) {
        if (!(s > 0.0) || !std::isfinite(s)) {
            s = 1.0;
        }
    }
    order_choices = norm.at("order_choices").get<std::vector<int>>();
    mesher_choices = norm.at("mesher_choices").get<std::vector<std::string>>();
    if (order_choices.empty() || mesher_choices.empty()) {
        throw AdvisorError("advisor: normalization.json has an empty categorical vocabulary");
    }
}

void Advisor::Impl::load_clamps(const std::filesystem::path& dir) {
    const json clamps = read_json(dir / "clamps.json");
    h_rel = interval_of(clamps, "h_rel");
    eta_target = interval_of(clamps, "eta_target");
    adapt_passes = interval_of(clamps, "adapt_passes");
    veto_threshold = clamps.value("veto_threshold", 0.5);
    action_dims = clamps.at("action_dims").get<std::vector<std::string>>();

    // The two artifacts carry the vocabularies independently. Only checking the
    // total width would let two files with swapped-but-equal-sized vocabularies
    // pass validation and then decode the policy head into the wrong category.
    const auto clamp_orders = clamps.at("order_choices").get<std::vector<int>>();
    const auto clamp_meshers = clamps.at("mesher_choices").get<std::vector<std::string>>();
    if (clamp_orders != order_choices || clamp_meshers != mesher_choices) {
        throw AdvisorError("advisor: clamps.json and normalization.json disagree on the "
                           "order/mesher vocabulary");
    }

    // 3 continuous dims + one logit per order + one per mesher. It was 4 + ...
    // while a p_elevate logit was carried; that dial is the same actuator as
    // `order >= 2` (apps/cli/main.cpp:805) and no longer exists.
    const std::size_t expected = 3 + order_choices.size() + mesher_choices.size();
    if (action_dims.size() != expected) {
        throw AdvisorError("advisor: clamps.json action_dims has " +
                           std::to_string(action_dims.size()) + " entries, expected " +
                           std::to_string(expected) +
                           " for the declared order/mesher vocabularies");
    }

    // A veto returns these, and the header promises every returned field is
    // inside the box. Validate rather than trust the exporter: a default
    // outside its own clamp interval would make the "safe fallback" the one
    // action the clamp table rejects.
    const json& defaults = clamps.at("defaults");
    default_decision.mesher = defaults.value("mesher", mesher_choices.front());
    default_decision.order = defaults.value("order", order_choices.front());
    default_decision.p_elevate = defaults.value("p_elevate", false);
    if (std::find(mesher_choices.begin(), mesher_choices.end(), default_decision.mesher) ==
        mesher_choices.end()) {
        throw AdvisorError("advisor: clamps.json defaults.mesher '" + default_decision.mesher +
                           "' is not in mesher_choices");
    }
    if (std::find(order_choices.begin(), order_choices.end(), default_decision.order) ==
        order_choices.end()) {
        throw AdvisorError("advisor: clamps.json defaults.order " +
                           std::to_string(default_decision.order) +
                           " is not in order_choices");
    }
    bool default_clamped = false;
    default_decision.h_rel = h_rel.clamp(defaults.value("h_rel", h_rel.lo), default_clamped);
    default_decision.eta_target =
        eta_target.clamp(defaults.value("eta_target", eta_target.lo), default_clamped);
    default_decision.adapt_passes = static_cast<int>(std::lround(adapt_passes.clamp(
        static_cast<double>(defaults.value("adapt_passes", 0)), default_clamped)));
    if (default_clamped) {
        throw AdvisorError("advisor: clamps.json defaults fall outside the clamp box the same "
                           "file declares; re-export the model");
    }

    // The candidate list is optional: an artifact exported before it existed
    // still loads, and recommend() then keeps the old single-shot behaviour.
    // Validating it here rather than at query time means a malformed list is a
    // construction error, never a silently degraded recommendation.
    //
    // A LIST of measured actions, not a cross product of dial levels: crossing
    // the dials would manufacture combinations the campaign never ran and ask
    // the regression heads to extrapolate to them.
    if (clamps.contains("candidate_grid") && clamps.at("candidate_grid").is_object()) {
        const json& grid = clamps.at("candidate_grid");
        if (grid.contains("actions") && grid.at("actions").is_array()) {
            for (const json& entry : grid.at("actions")) {
                AdvisorDecision candidate = default_decision;
                candidate.mesher = entry.value("mesher", default_decision.mesher);
                candidate.order = entry.value("order", default_decision.order);
                bool clamped = false;
                candidate.h_rel =
                    h_rel.clamp(entry.value("h_rel", default_decision.h_rel), clamped);
                candidate.eta_target = eta_target.clamp(
                    entry.value("eta_target", default_decision.eta_target), clamped);
                candidate.adapt_passes = static_cast<int>(std::lround(
                    adapt_passes.clamp(static_cast<double>(entry.value(
                                           "adapt_passes", default_decision.adapt_passes)),
                                       clamped)));
                if (clamped) {
                    throw AdvisorError(
                        "advisor: clamps.json candidate_grid action leaves the "
                        "clamp box the same file declares; re-export the model");
                }
                if (std::find(mesher_choices.begin(), mesher_choices.end(),
                              candidate.mesher) == mesher_choices.end()) {
                    throw AdvisorError("advisor: clamps.json candidate_grid names mesher '" +
                                       candidate.mesher + "', absent from mesher_choices");
                }
                if (std::find(order_choices.begin(), order_choices.end(), candidate.order) ==
                    order_choices.end()) {
                    throw AdvisorError("advisor: clamps.json candidate_grid names an order "
                                       "absent from order_choices");
                }
                candidates.push_back(candidate);
            }
        }
    }
    // Required, never defaulted. The gate and the veto are different decisions
    // with different correct values -- the gate filters candidates before
    // ranking, the veto abandons the whole recommendation afterwards -- so
    // inheriting one from the other produced a threshold that looked deliberate
    // and was not: the shipped rule silently ran at the veto's 0.5, the weakest
    // member of its own sweep. An absent key is a misconfiguration, not a
    // default, and it fails here rather than degrading a recommendation.
    if (!clamps.contains("gate_threshold") || !clamps.at("gate_threshold").is_number()) {
        throw AdvisorError(
            "advisor: clamps.json is missing a numeric 'gate_threshold'. It is "
            "required and must not be inherited from 'veto_threshold': the gate "
            "filters candidates before ranking, the veto refuses the whole "
            "recommendation. Re-export with python scripts/advisor/export_onnx.py");
    }
    gate_threshold = clamps.at("gate_threshold").get<double>();
    if (!(gate_threshold > 0.0) || !(gate_threshold < 1.0)) {
        throw AdvisorError("advisor: clamps.json gate_threshold " +
                           std::to_string(gate_threshold) +
                           " is not a probability in (0, 1); it gates a sigmoid output");
    }
}

// Out-of-distribution test (ood.json), fitted by scripts/advisor/calibration.py
// on the STANDARDIZED training matrix -- the same space `encode()` produces --
// over the geometry/BC context columns only. An unusual ACTION is a legal
// query; only an unfamiliar PART is out of distribution.
//
// Required, never defaulted, for the same reason `gate_threshold` is: the file
// carries a measured operating point (training q0.99, in-sample false-alarm
// rate 0.01, held-out-family detection 1.0 over 8 leave-one-family-out folds),
// and an advisor that silently runs without it answers questions about parts
// unlike anything it was trained on -- which is exactly what the abstention
// exists to prevent. An absent or malformed block is a misconfiguration.
void Advisor::Impl::load_ood(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "ood.json";
    const char* remedy =
        " The out-of-distribution veto is required and has no default: rebuild it with "
        "python scripts/advisor/calibration.py.";
    if (!std::filesystem::exists(path)) {
        throw AdvisorError("advisor: no ood.json in " + dir.string() + "." + remedy);
    }
    const json ood = read_json(path);
    if (!ood.contains("feature_columns") || !ood.at("feature_columns").is_array() ||
        ood.at("feature_columns").empty()) {
        throw AdvisorError("advisor: ood.json has no non-empty 'feature_columns' array." +
                           std::string(remedy));
    }
    const auto names = ood.at("feature_columns").get<std::vector<std::string>>();
    const std::size_t k = names.size();

    // Column ORDER is the contract, and it is pinned here rather than assumed:
    // the quadratic form is evaluated in the order ood.json lists, and every
    // value is fetched from the caller's FeatureColumns BY NAME at query time.
    // Two files iterating a map in incidentally-equal order is not a guarantee;
    // a permutation would silently score a different distance.
    //
    // Note what is NOT checked here: whether these names appear in
    // normalization.json:input_columns. The two feature sets are fitted
    // independently -- the OOD detector's columns are chosen for
    // distribution-shift sensitivity, the network's (62 columns in the shipped
    // contract, geo_* exact-BRep descriptors included) for held-out regret --
    // so overlapping but different sets are both expected and fine, and the
    // by-name resolution above is what keeps either side free to drift.
    ood_columns = names;

    const auto require_vector = [&](const char* key) {
        if (!ood.contains(key) || !ood.at(key).is_array()) {
            throw AdvisorError("advisor: ood.json has no '" + std::string(key) + "' array." +
                               remedy);
        }
        auto values = ood.at(key).get<std::vector<double>>();
        if (values.size() != k) {
            throw AdvisorError("advisor: ood.json '" + std::string(key) + "' has " +
                               std::to_string(values.size()) + " entries for " +
                               std::to_string(k) + " feature_columns." + remedy);
        }
        return values;
    };
    ood_center = require_vector("center");
    ood_scale = require_vector("scale");
    for (const double value : ood_scale) {
        // A zero or negative scale would divide the query into infinity. The
        // fitter collapses a constant column to exactly 1.0, so this can only
        // fire on a corrupt or hand-edited artifact.
        if (!(value > 0.0) || !std::isfinite(value)) {
            throw AdvisorError("advisor: ood.json 'scale' holds a non-positive entry; the "
                               "distance would be infinite." +
                               std::string(remedy));
        }
    }
    if (!ood.contains("precision") || !ood.at("precision").is_array() ||
        ood.at("precision").size() != k) {
        throw AdvisorError("advisor: ood.json 'precision' is not a " + std::to_string(k) +
                           "x" + std::to_string(k) + " matrix." + remedy);
    }
    ood_precision.assign(k * k, 0.0);
    for (std::size_t i = 0; i < k; ++i) {
        const json& row = ood.at("precision")[i];
        if (!row.is_array() || row.size() != k) {
            throw AdvisorError("advisor: ood.json 'precision' row " + std::to_string(i) +
                               " is not " + std::to_string(k) + " wide." + remedy);
        }
        for (std::size_t j = 0; j < k; ++j) {
            const double value = row[j].get<double>();
            if (!std::isfinite(value)) {
                throw AdvisorError(
                    "advisor: ood.json 'precision' holds a non-finite entry at (" +
                    std::to_string(i) + ", " + std::to_string(j) + ")." + remedy);
            }
            ood_precision[i * k + j] = value;
        }
    }
    for (const double value : ood_center) {
        if (!std::isfinite(value)) {
            throw AdvisorError("advisor: ood.json 'center' holds a non-finite entry." +
                               std::string(remedy));
        }
    }

    // The threshold is the VALIDATED operating point, not a quantile the C++
    // picks for itself. Reading it from `operating_point` keeps the shipped
    // rule and the measured false-alarm/detection rates the same object.
    if (!ood.contains("operating_point") || !ood.at("operating_point").is_object() ||
        !ood.at("operating_point").contains("threshold") ||
        !ood.at("operating_point").at("threshold").is_number()) {
        throw AdvisorError("advisor: ood.json has no numeric 'operating_point.threshold'." +
                           std::string(remedy));
    }
    ood_threshold = ood.at("operating_point").at("threshold").get<double>();
    if (!(ood_threshold > 0.0) || !std::isfinite(ood_threshold)) {
        throw AdvisorError("advisor: ood.json operating_point.threshold " +
                           std::to_string(ood_threshold) + " is not a positive distance." +
                           remedy);
    }
}

// Static picture of the deployed network (activation_layout.json), written by
// scripts/advisor/export_onnx.py beside the graph it describes.
//
// Unlike every other artifact this file reads, an absent or unusable one is NOT
// an error. The sidecar buys one thing -- the ability to DRAW the network -- and
// nothing else in the advisor depends on it, so a model directory exported
// before it existed must still load and recommend bit-for-bit as it did. The
// alternative discipline (reconstruct the layout from the graph's own shapes)
// was rejected: layer sizes are recoverable, but which weight row belongs to
// which of the nine heads, and which input column feeds which trunk neuron
// after the embedding concatenation, are not. A layout inferred that far would
// render a plausible network with its edges attached to the wrong neurons --
// a lie that looks exactly like the truth.
//
// Everything here is therefore validated against the graph, and any single
// disagreement drops the whole facility rather than half of it.
void Advisor::Impl::load_activation_layout(const std::filesystem::path& dir,
                                           bool graph_has_taps) {
    layout = NetworkLayout{};
    activations_available = false;
    const std::string remedy =
        ". Activations are unavailable; the advisor recommends normally without them. "
        "Re-export "
        "with python scripts/advisor/export_onnx.py to draw the network.";
    const std::filesystem::path path = dir / "activation_layout.json";
    if (!graph_has_taps) {
        activation_note = "advisor: model.onnx exports no trunk activation taps" + remedy;
        return;
    }
    if (!std::filesystem::exists(path)) {
        activation_note = "advisor: no activation_layout.json in " + dir.string() + remedy;
        return;
    }

    // Validation failures are thrown and caught here. They are genuine errors
    // about the sidecar, so they are raised where they are detected with the
    // detail attached, and converted to "no activations" at exactly one place.
    NetworkLayout parsed;
    try {
        const json doc = read_json(path);
        const std::string schema = doc.value("schema", std::string{});
        if (schema != kActivationLayoutSchema) {
            // A future revision is free to reorder `layers` or transpose
            // `weights` and would still parse cleanly here, so the tag is
            // checked rather than the shape alone.
            throw AdvisorError("advisor: activation_layout.json schema is '" + schema +
                               "', expected '" + kActivationLayoutSchema + "'");
        }
        const auto taps = doc.value("activation_outputs", std::vector<std::string>{});
        if (taps.size() != kActivationOutputNames.size()) {
            throw AdvisorError("advisor: activation_layout.json activation_outputs has " +
                               std::to_string(taps.size()) + " entries, expected " +
                               std::to_string(kActivationOutputNames.size()));
        }
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (taps[i] != kActivationOutputNames[i]) {
                throw AdvisorError("advisor: activation_layout.json activation_outputs[" +
                                   std::to_string(i) + "] is '" + taps[i] + "', expected '" +
                                   kActivationOutputNames[i] + "'");
            }
        }
        if (!doc.contains("hidden") || !doc.at("hidden").is_number_unsigned()) {
            throw AdvisorError("advisor: activation_layout.json has no unsigned 'hidden'");
        }
        const auto hidden = doc.at("hidden").get<std::size_t>();

        if (!doc.contains("layers") || !doc.at("layers").is_array() ||
            doc.at("layers").size() != kLayerNames.size()) {
            throw AdvisorError("advisor: activation_layout.json needs exactly " +
                               std::to_string(kLayerNames.size()) + " layers");
        }
        for (std::size_t i = 0; i < kLayerNames.size(); ++i) {
            const json& entry = doc.at("layers")[i];
            NetworkLayer layer;
            layer.name = entry.value("name", std::string{});
            if (layer.name != kLayerNames[i]) {
                throw AdvisorError("advisor: activation_layout.json layer " +
                                   std::to_string(i) + " is '" + layer.name + "', expected '" +
                                   kLayerNames[i] + "'");
            }
            if (!entry.contains("size") || !entry.at("size").is_number_unsigned() ||
                entry.at("size").get<std::size_t>() == 0) {
                throw AdvisorError("advisor: activation_layout.json layer '" + layer.name +
                                   "' has no positive 'size'");
            }
            layer.size = entry.at("size").get<std::size_t>();
            if (entry.contains("labels")) {
                layer.labels = entry.at("labels").get<std::vector<std::string>>();
                if (!layer.labels.empty() && layer.labels.size() != layer.size) {
                    throw AdvisorError("advisor: activation_layout.json layer '" + layer.name +
                                       "' has " + std::to_string(layer.labels.size()) +
                                       " labels for " + std::to_string(layer.size) + " units");
                }
            }
            parsed.layers.push_back(std::move(layer));
        }

        // The hidden layers are the trunk, both `hidden` wide by construction
        // (model.py:fc1/fc2). Checking both against the declared width catches a
        // sidecar copied from a model trained at a different capacity, which is
        // the shape of mistake that survives a directory being reassembled by
        // hand.
        if (parsed.layers[1].size != hidden || parsed.layers[2].size != hidden) {
            throw AdvisorError("advisor: activation_layout.json trunk layers are " +
                               std::to_string(parsed.layers[1].size) + "/" +
                               std::to_string(parsed.layers[2].size) +
                               " wide, not the declared "
                               "hidden width " +
                               std::to_string(hidden));
        }

        // Ten regressors plus the failure logit plus one policy dimension per
        // action dim -- the same accounting `evaluate()` unpacks, so a drawn
        // "heads" layer cannot have more or fewer circles than the graph has
        // outputs.
        const std::size_t expected_heads = kOutputNames.size() - 1 + action_dims.size();
        if (parsed.layers[3].size != expected_heads) {
            throw AdvisorError("advisor: activation_layout.json 'heads' has " +
                               std::to_string(parsed.layers[3].size) + " units, expected " +
                               std::to_string(expected_heads) + " for " +
                               std::to_string(kOutputNames.size() - 1) +
                               " scalar heads plus " + std::to_string(action_dims.size()) +
                               " policy dims");
        }
        // Head ORDER matters more than head count: the drawing labels the lit
        // circle that produced the recommendation, and a permuted heads layer
        // would attribute the decision to the wrong output.
        if (!parsed.layers[3].labels.empty()) {
            for (std::size_t i = 0; i + 1 < kOutputNames.size(); ++i) {
                if (parsed.layers[3].labels[i] != kOutputNames[i]) {
                    throw AdvisorError("advisor: activation_layout.json 'heads' label " +
                                       std::to_string(i) + " is '" +
                                       parsed.layers[3].labels[i] + "', expected '" +
                                       kOutputNames[i] + "'");
                }
            }
        }

        // Tie the input layer to the graph's own input width. The trunk input is
        // not the model input: model.py:trunk_input drops the two categorical
        // columns and appends one embedding block for each, so the width is
        // `input_columns - 2 + 2 * emb_dim`. `emb_dim` is not shipped to C++, so
        // what is checked is the part that does not depend on it: every named
        // input-layer label must be a real input column, exactly the two
        // categorical columns may be missing, and the unnamed remainder (the two
        // embedding blocks) must split evenly.
        const NetworkLayer& input_layer = parsed.layers[0];
        if (input_layer.labels.size() != input_layer.size) {
            throw AdvisorError(
                "advisor: activation_layout.json 'input' layer must label all " +
                std::to_string(input_layer.size) +
                " units; the labels are what attaches an edge to a column");
        }
        std::size_t named = 0;
        for (const std::string& label : input_layer.labels) {
            if (std::find(input_columns.begin(), input_columns.end(), label) !=
                input_columns.end()) {
                ++named;
            }
        }
        if (input_columns.size() < 2 || named != input_columns.size() - 2) {
            throw AdvisorError("advisor: activation_layout.json 'input' layer names " +
                               std::to_string(named) + " of the graph's " +
                               std::to_string(input_columns.size()) +
                               " input columns, expected all but the two categorical ones");
        }
        if ((input_layer.size - named) % 2 != 0) {
            throw AdvisorError("advisor: activation_layout.json 'input' layer has " +
                               std::to_string(input_layer.size - named) +
                               " embedding units, which is not two equal blocks");
        }

        // Widths as the GRAPH declares them, where it declares them statically.
        // This is the authoritative check that `trunk_input` really is
        // `layers[0]` wide: everything above is arithmetic on the sidecar's own
        // numbers, this compares them against the tensors that will actually
        // arrive. A dynamic dimension is left unchecked rather than assumed --
        // `forward()` re-checks every tap width per pass anyway.
        const std::array<std::size_t, 3> tap_expect{parsed.layers[0].size, hidden, hidden};
        for (std::size_t i = 0; i < kActivationOutputNames.size(); ++i) {
            const auto shape = session->GetOutputTypeInfo(kOutputNames.size() + i)
                                   .GetTensorTypeAndShapeInfo()
                                   .GetShape();
            if (shape.size() != 2) {
                throw AdvisorError("advisor: model.onnx tap '" +
                                   std::string(kActivationOutputNames[i]) + "' is rank " +
                                   std::to_string(shape.size()) + ", expected [batch, width]");
            }
            if (shape[1] > 0 && static_cast<std::size_t>(shape[1]) != tap_expect[i]) {
                throw AdvisorError("advisor: model.onnx tap '" +
                                   std::string(kActivationOutputNames[i]) + "' is " +
                                   std::to_string(shape[1]) +
                                   " wide, but activation_layout.json declares " +
                                   std::to_string(tap_expect[i]));
            }
        }

        // Weight blocks. `rows`/`cols` are validated against the layers they
        // join AND against the nesting of `weights`, because those are two
        // independent claims: the header pair is what a consumer indexes with,
        // the nesting is what the numbers actually are, and a mismatch between
        // them would transpose the drawn network.
        if (!doc.contains("edges") || !doc.at("edges").is_array() ||
            doc.at("edges").size() + 1 != kLayerNames.size()) {
            throw AdvisorError("advisor: activation_layout.json needs exactly " +
                               std::to_string(kLayerNames.size() - 1) + " edge blocks");
        }
        for (std::size_t i = 0; i + 1 < kLayerNames.size(); ++i) {
            const json& entry = doc.at("edges")[i];
            NetworkEdges edges;
            edges.from = entry.value("from", std::string{});
            edges.to = entry.value("to", std::string{});
            if (edges.from != kLayerNames[i] || edges.to != kLayerNames[i + 1]) {
                throw AdvisorError("advisor: activation_layout.json edge " +
                                   std::to_string(i) + " joins '" + edges.from + "' to '" +
                                   edges.to + "', expected '" + kLayerNames[i] + "' to '" +
                                   kLayerNames[i + 1] + "'");
            }
            if (!entry.contains("rows") || !entry.at("rows").is_number_unsigned() ||
                !entry.contains("cols") || !entry.at("cols").is_number_unsigned()) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' has no unsigned rows/cols");
            }
            edges.rows = entry.at("rows").get<std::size_t>();
            edges.cols = entry.at("cols").get<std::size_t>();
            if (edges.rows != parsed.layers[i + 1].size ||
                edges.cols != parsed.layers[i].size) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' is " + std::to_string(edges.rows) +
                                   "x" + std::to_string(edges.cols) + ", expected " +
                                   std::to_string(parsed.layers[i + 1].size) + "x" +
                                   std::to_string(parsed.layers[i].size) +
                                   " for the layers it joins");
            }
            if (!entry.contains("weights") || !entry.at("weights").is_array() ||
                entry.at("weights").size() != edges.rows) {
                throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                   "' -> '" + edges.to + "' has no " +
                                   std::to_string(edges.rows) + "-row 'weights' matrix");
            }
            edges.weights.resize(edges.rows * edges.cols);
            for (std::size_t r = 0; r < edges.rows; ++r) {
                const json& row = entry.at("weights")[r];
                if (!row.is_array() || row.size() != edges.cols) {
                    throw AdvisorError("advisor: activation_layout.json edge '" + edges.from +
                                       "' -> '" + edges.to + "' row " + std::to_string(r) +
                                       " is not " + std::to_string(edges.cols) + " wide");
                }
                for (std::size_t c = 0; c < edges.cols; ++c) {
                    const double value = row[c].get<double>();
                    if (!std::isfinite(value)) {
                        throw AdvisorError("advisor: activation_layout.json edge '" +
                                           edges.from + "' -> '" + edges.to +
                                           "' holds a non-finite weight at (" +
                                           std::to_string(r) + ", " + std::to_string(c) + ")");
                    }
                    // float32 on purpose: these are the drawing's line widths,
                    // the weights themselves are float32 in the graph, and the
                    // largest block here is 96x68.
                    edges.weights[r * edges.cols + c] = static_cast<float>(value);
                }
            }
            parsed.edges.push_back(std::move(edges));
        }
    } catch (const AdvisorError& error) {
        activation_note = std::string(error.what()) + remedy;
        return;
    } catch (const std::exception& error) {
        // Anything nlohmann raises on a payload that parses but is not shaped
        // like the schema. Prefixed, because those messages name a JSON type
        // and nothing else, and a user needs to know which file to re-export.
        activation_note =
            "advisor: activation_layout.json is malformed: " + std::string(error.what()) +
            remedy;
        return;
    }

    layout = std::move(parsed);
    activations_available = true;
    activation_note.clear();
}

/// Mahalanobis distance of one query from the training centre, over the ood.json
/// columns in the ood.json order. Mirrors
/// `scripts/advisor/calibration.py:ood_scores` exactly, including the clamp at
/// zero that guards the square root against a tiny negative quadratic form
/// produced by rounding in a near-degenerate direction.
///
/// Throws when the caller did not supply a column the file names. That is
/// deliberate and it is the whole point: `encode()` silently imputes a missing
/// column from the training median, which is right for a network input and fatal
/// for a distribution test -- imputing the median places an unknown part exactly
/// at the centre of the training distribution, i.e. it manufactures the answer
/// "perfectly familiar" for a part we know nothing about. A missing descriptor
/// must abort the test, never soften it.
double Advisor::Impl::mahalanobis(const FeatureColumns& columns) const {
    const std::size_t k = ood_columns.size();
    std::vector<double> scaled(k);
    for (std::size_t i = 0; i < k; ++i) {
        const auto it = columns.find(ood_columns[i]);
        if (it == columns.end()) {
            throw AdvisorError("advisor: the out-of-distribution test needs feature column '" +
                               ood_columns[i] +
                               "', which the caller did not supply. Imputing it "
                               "would place an unknown part at the centre of the training "
                               "distribution and report it as familiar.");
        }
        if (!std::isfinite(it->second)) {
            throw AdvisorError("advisor: feature column '" + ood_columns[i] +
                               "' is not finite; the out-of-distribution distance would be "
                               "meaningless.");
        }
        scaled[i] = (it->second - ood_center[i]) / ood_scale[i];
    }
    // Accumulated in double: the precision matrix is ill-conditioned by
    // construction (cond ~1.25e11) and a narrower accumulation here would be
    // indistinguishable from a real disagreement with the Python reference.
    double quadratic = 0.0;
    for (std::size_t i = 0; i < k; ++i) {
        double partial = 0.0;
        const double* row = ood_precision.data() + i * k;
        for (std::size_t j = 0; j < k; ++j) {
            partial += row[j] * scaled[j];
        }
        quadratic += scaled[i] * partial;
    }
    return std::sqrt(std::max(quadratic, 0.0));
}

std::vector<float> Advisor::Impl::encode(const FeatureColumns& columns) const {
    // Name-keyed so the exported column list, not a C++ ordering, decides the
    // layout. Any column the caller did not supply falls back to the training
    // median recorded in normalization.json.
    std::vector<float> row(input_columns.size());
    for (std::size_t i = 0; i < input_columns.size(); ++i) {
        double value = impute[i];
        if (const auto it = columns.find(input_columns[i]); it != columns.end()) {
            value = it->second;
        }
        if (!std::isfinite(value)) {
            value = impute[i];
        }
        row[i] = static_cast<float>((value - mean[i]) / stddev[i]);
    }
    return row;
}

void Advisor::Impl::apply_action(FeatureColumns& columns,
                                 const AdvisorDecision& action) const {
    // An out-of-vocabulary category is encoded as `size()`, the reserved
    // unknown-embedding slot the trainer allocates (`n_order_slots =
    // len(order_choices) + 1`). Returning 0 would silently score the row as if
    // it had asked for the FIRST category, which is a valid, wrong answer.
    const auto index_of = [](const auto& choices, const auto& value) -> double {
        const auto it = std::find(choices.begin(), choices.end(), value);
        return it == choices.end() ? static_cast<double>(choices.size())
                                   : static_cast<double>(std::distance(choices.begin(), it));
    };
    columns["h_rel"] = action.h_rel;
    columns["eta_target"] = action.eta_target;
    columns["adapt_passes"] = static_cast<double>(action.adapt_passes);
    columns["p_elevate"] = action.p_elevate ? 1.0 : 0.0;
    columns["order"] = static_cast<double>(action.order);
    columns["order_idx"] = index_of(order_choices, action.order);
    columns["mesher_idx"] = index_of(mesher_choices, action.mesher);

    // Scale-law inputs, mirroring `scripts/advisor/dataset.py:derived_features`.
    // They depend on the ACTION as well as the part (h = h_rel * diag), so they
    // are recomputed per candidate here rather than once in `to_columns`.
    // Leaving them out is not neutral: `encode` would fill all four from the
    // training median and score every part as if it were of average size.
    const auto lg = [](double value) {
        return value > 0.0 && std::isfinite(value) ? std::log10(value)
                                                   : std::numeric_limits<double>::quiet_NaN();
    };
    const auto column_of = [&columns](const char* name) {
        const auto it = columns.find(name);
        return it == columns.end() ? std::numeric_limits<double>::quiet_NaN() : it->second;
    };
    const double log_volume = lg(column_of("volume"));
    const double log_diag = lg(column_of("diag"));
    const double log_h = log_diag + lg(action.h_rel);
    columns["log10_volume"] = log_volume;
    columns["log10_diag"] = log_diag;
    columns["log10_h"] = log_h;
    columns["log10_cells"] = log_volume - 3.0 * log_h;
}

FeatureColumns to_columns(const pipeline::CaseFeatures& f) {
    FeatureColumns columns = {
        {"bbox_dx", f.bbox_dx},
        {"bbox_dy", f.bbox_dy},
        {"bbox_dz", f.bbox_dz},
        {"diag", f.diag},
        {"volume", f.volume},
        {"surface_area", f.surface_area},
        {"sa_over_v23", f.sa_over_v23},
        {"n_faces", static_cast<double>(f.n_faces)},
        {"n_sharp_edges", static_cast<double>(f.n_sharp_edges)},
        {"sharp_edge_len_total", f.sharp_edge_len_total},
        {"curved_frac", f.curved_frac},
        {"kappa_max_h", f.kappa_max_h},
        {"kappa_mean_h", f.kappa_mean_h},
        {"thin_min_over_diag", f.thin_min_over_diag},
        {"thin_p10_over_diag", f.thin_p10_over_diag},
        {"min_feature_h", f.min_feature_h},
        {"n_fix_faces", static_cast<double>(f.n_fix_faces)},
        {"n_load_faces", static_cast<double>(f.n_load_faces)},
        {"fix_area_frac", f.fix_area_frac},
        {"load_area_frac", f.load_area_frac},
        {"load_dir_x", f.load_dir_x},
        {"load_dir_y", f.load_dir_y},
        {"load_dir_z", f.load_dir_z},
        {"fix_load_dist_over_diag", f.fix_load_dist_over_diag},
        {"load_axis_alignment", f.load_axis_alignment},
        {"poisson", f.poisson},
        // The case_* columns duplicate what the feature extractor already knows;
        // the ones it cannot know (region counts, traction magnitude) are left
        // out on purpose so they are imputed rather than invented.
        {"case_poisson", f.poisson},
        {"case_load_dir_x", f.load_dir_x},
        {"case_load_dir_y", f.load_dir_y},
        {"case_load_dir_z", f.load_dir_z},
    };

    // Exact-BRep descriptors. The shipped ONNX contract is the 75 columns of
    // normalization.json:input_columns and these are among it, so encode()
    // consumes them when present (and imputes them when absent); they also
    // feed the out-of-distribution test in ood.json, which reads this map by
    // name in raw units under its own center/scale.
    //
    // Inserted only when the feature extractor actually measured them from a
    // BRep. When it did not -- no OpenCASCADE, or a model carrying no CAD -- the
    // keys stay ABSENT rather than zero, and mahalanobis() then refuses to score
    // rather than testing fabricated values. Zero is a legal measured value for
    // several of these descriptors (a fully planar part has
    // geo_curved_area_frac == 0), so a zero-filled block would be
    // indistinguishable from a real measurement of a simple part -- which is
    // precisely the part most likely to be out of distribution.
    if (f.geo_available) {
        columns.emplace("geo_curved_area_frac", f.geo_curved_area_frac);
        columns.emplace("geo_cyl_area_frac", f.geo_cyl_area_frac);
        columns.emplace("geo_plane_area_frac", f.geo_plane_area_frac);
        columns.emplace("geo_other_area_frac", f.geo_other_area_frac);
        columns.emplace("geo_min_curv_radius_rel", f.geo_min_curv_radius_rel);
        columns.emplace("geo_log_curv_radius_mean", f.geo_log_curv_radius_mean);
        columns.emplace("geo_log_curv_radius_std", f.geo_log_curv_radius_std);
        columns.emplace("geo_n_faces", f.geo_n_faces);
        columns.emplace("geo_n_edges", f.geo_n_edges);
        columns.emplace("geo_face_area_cv", f.geo_face_area_cv);
        columns.emplace("geo_aspect_max", f.geo_aspect_max);
        columns.emplace("geo_aspect_mid", f.geo_aspect_mid);
        columns.emplace("geo_volume_frac", f.geo_volume_frac);
        columns.emplace("geo_area_over_v23", f.geo_area_over_v23);
        columns.emplace("geo_min_face_size_rel", f.geo_min_face_size_rel);
        columns.emplace("geo_n_inner_loops", f.geo_n_inner_loops);
        columns.emplace("geo_hole_spacing_min_rel", f.geo_hole_spacing_min_rel);
        columns.emplace("geo_hole_spacing_p10_rel", f.geo_hole_spacing_p10_rel);
        columns.emplace("geo_feat_pair_dist_min_rel", f.geo_feat_pair_dist_min_rel);
        columns.emplace("geo_feat_pair_dist_p10_rel", f.geo_feat_pair_dist_p10_rel);
        columns.emplace("geo_feat_pair_dist_mean_rel", f.geo_feat_pair_dist_mean_rel);
        columns.emplace("geo_dihedral_p10", f.geo_dihedral_p10);
        columns.emplace("geo_dihedral_p50", f.geo_dihedral_p50);
        columns.emplace("geo_dihedral_p90", f.geo_dihedral_p90);
        columns.emplace("geo_singular_lambda_min", f.geo_singular_lambda_min);
        columns.emplace("load_to_feature_dist_min_rel",
                        f.load_to_feature_dist_min_rel);
        columns.emplace("fix_to_feature_dist_min_rel",
                        f.fix_to_feature_dist_min_rel);
        columns.emplace("case_load_multiaxiality", f.case_load_multiaxiality);
    }
    return columns;
}

std::vector<std::vector<float>>
Advisor::Impl::run(const std::vector<float>& row,
                   const std::vector<const char*>& names) const {
    const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(row.size())};
    Ort::Value input = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(row.data()),
                                                       row.size(), shape.data(), shape.size());
    const char* input_names[] = {input_name.c_str()};
    auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, &input, 1, names.data(),
                                names.size());

    std::vector<std::vector<float>> result;
    result.reserve(outputs.size());
    for (auto& tensor : outputs) {
        const auto info = tensor.GetTensorTypeAndShapeInfo();
        const std::size_t count = info.GetElementCount();
        const float* data = tensor.GetTensorData<float>();
        result.emplace_back(data, data + count);
    }
    return result;
}

AdvisorRawOutputs Advisor::Impl::forward(const FeatureColumns& columns,
                                         ActivationFrame* frame) const {
    const std::vector<float> row = encode(columns);
    const bool with_taps = frame != nullptr && activations_available;
    const std::vector<std::vector<float>> raw =
        run(row, with_taps ? tap_output_name_ptrs : output_name_ptrs);
    const AdvisorRawOutputs out = unpack(raw);
    if (frame == nullptr) {
        return out;
    }

    frame->outputs = out;
    // The `heads` layer of the drawing is the graph's own head outputs, in
    // `NetworkLayout` "heads" order: the ten regressors, the failure logit,
    // then the policy vector. Copied from the SAME tensors the decision is made
    // from, so a lit head circle and the recommendation it justifies cannot
    // disagree. Reported pre-activation, exactly as the graph emits them: the
    // regressors are log10 levels and `failure_logit` is a logit, and squashing
    // it here would make the drawn value a different number from the one the
    // gate compares.
    frame->heads.clear();
    frame->heads.reserve(kOutputNames.size() - 1 + out.policy.size());
    for (std::size_t i = 0; i + 1 < kOutputNames.size(); ++i) {
        frame->heads.push_back(raw[i][0]);
    }
    frame->heads.insert(frame->heads.end(), raw[kOutputNames.size() - 1].begin(),
                        raw[kOutputNames.size() - 1].end());
    if (!with_taps) {
        return out;
    }

    // Widths re-checked per pass rather than trusted from load time: the tap
    // dimensions may be dynamic in the graph, in which case construction could
    // not check them, and a frame whose vector length disagrees with the layout
    // would be drawn against the wrong circles. Left EMPTY on disagreement --
    // the drawing must show a dark layer, never a resized guess.
    std::vector<float>* const targets[] = {&frame->input, &frame->fc1, &frame->fc2};
    for (std::size_t i = 0; i < kActivationOutputNames.size(); ++i) {
        const std::vector<float>& tensor = raw[kOutputNames.size() + i];
        if (tensor.size() == layout.layers[i].size) {
            *targets[i] = tensor;
        }
    }
    return out;
}

Advisor::Advisor(const std::filesystem::path& model_dir, AdvisorObjective objective)
    : impl_(std::make_unique<Impl>()) {
    const std::filesystem::path graph = model_dir / "model.onnx";
    if (!std::filesystem::exists(graph)) {
        throw AdvisorError("advisor: no model.onnx in " + model_dir.string());
    }
    impl_->objective = objective;
    if (objective == AdvisorObjective::kEfficiency) {
        const std::filesystem::path calibration_path =
            model_dir / "hosts" / (local_host_name() + ".json");
        if (std::filesystem::exists(calibration_path)) {
            const json host = read_json(calibration_path);
            HostCalibration calibration;
            calibration.host = host.at("host").get<std::string>();
            calibration.flops_per_s = host.at("flops_per_s").get<double>();
            calibration.bytes_per_s = host.at("bytes_per_s").get<double>();
            calibration.ref_mesh_ms = host.at("ref_mesh_ms").get<double>();
            calibration.generated_utc = host.at("generated_utc").get<std::string>();
            if (!(calibration.flops_per_s > 0.0) || !(calibration.bytes_per_s > 0.0) ||
                !(calibration.ref_mesh_ms > 0.0)) {
                throw AdvisorError("advisor: invalid host calibration " +
                                   calibration_path.string());
            }
            impl_->calibration = std::move(calibration);
        }
    }
    impl_->load_normalization(model_dir);
    impl_->load_clamps(model_dir);
    impl_->load_ood(model_dir);
    // Determinism: one intra-op thread, sequential execution, CPU only. A
    // recommendation that changes with the thread pool is not reproducible
    // evidence, and campaign rows must be replayable.
    impl_->options.SetIntraOpNumThreads(1);
    impl_->options.SetInterOpNumThreads(1);
    impl_->options.SetExecutionMode(ORT_SEQUENTIAL);
    impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
    impl_->session =
        std::make_unique<Ort::Session>(impl_->env, graph.wstring().c_str(), impl_->options);
#else
    impl_->session =
        std::make_unique<Ort::Session>(impl_->env, graph.string().c_str(), impl_->options);
#endif

    Ort::AllocatorWithDefaultOptions allocator;
    if (impl_->session->GetInputCount() != 1) {
        throw AdvisorError("advisor: model.onnx must take exactly one input tensor");
    }
    impl_->input_name = impl_->session->GetInputNameAllocated(0, allocator).get();

    // The twelve contract outputs are still matched name-by-name and position-by
    // position: `evaluate()` unpacks them positionally, so this list is load
    // -time proof, not documentation. The three activation taps are the ONLY
    // permitted extras and only in their contract order -- anything else is a
    // graph this build cannot read positionally, which stays an error rather
    // than a best-effort load.
    const std::size_t n_out = impl_->session->GetOutputCount();
    const bool graph_has_taps = n_out == kOutputNames.size() + kActivationOutputNames.size();
    if (n_out != kOutputNames.size() && !graph_has_taps) {
        throw AdvisorError(
            "advisor: model.onnx has " + std::to_string(n_out) + " outputs, expected " +
            std::to_string(kOutputNames.size()) + " or " +
            std::to_string(kOutputNames.size() + kActivationOutputNames.size()) +
            " with the trunk activation taps appended");
    }
    for (std::size_t i = 0; i < kOutputNames.size(); ++i) {
        const std::string name = impl_->session->GetOutputNameAllocated(i, allocator).get();
        if (name != kOutputNames[i]) {
            throw AdvisorError("advisor: model.onnx output " + std::to_string(i) + " is '" +
                               name + "', expected '" + kOutputNames[i] + "'");
        }
    }
    impl_->output_name_ptrs.assign(kOutputNames.begin(), kOutputNames.end());
    if (graph_has_taps) {
        for (std::size_t i = 0; i < kActivationOutputNames.size(); ++i) {
            const std::string name =
                impl_->session->GetOutputNameAllocated(kOutputNames.size() + i, allocator)
                    .get();
            if (name != kActivationOutputNames[i]) {
                throw AdvisorError("advisor: model.onnx output " +
                                   std::to_string(kOutputNames.size() + i) + " is '" + name +
                                   "', expected the activation tap '" +
                                   kActivationOutputNames[i] + "'");
            }
        }
        impl_->tap_output_name_ptrs = impl_->output_name_ptrs;
        impl_->tap_output_name_ptrs.insert(impl_->tap_output_name_ptrs.end(),
                                           kActivationOutputNames.begin(),
                                           kActivationOutputNames.end());
    }

    const auto in_shape =
        impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (in_shape.size() != 2 || (in_shape[1] > 0 && static_cast<std::size_t>(in_shape[1]) !=
                                                        impl_->input_columns.size())) {
        throw AdvisorError("advisor: model.onnx input width does not match "
                           "normalization.json input_columns");
    }

    // Last, because it validates the sidecar against the session above.
    impl_->load_activation_layout(model_dir, graph_has_taps);
}

Advisor::~Advisor() = default;
Advisor::Advisor(Advisor&&) noexcept = default;
Advisor& Advisor::operator=(Advisor&&) noexcept = default;

AdvisorDecision Advisor::defaults() const { return impl_->default_decision; }

void Advisor::apply_action(FeatureColumns& columns, const AdvisorDecision& action) const {
    impl_->apply_action(columns, action);
}

AdvisorRawOutputs Advisor::evaluate(const FeatureColumns& columns) const {
    return impl_->forward(columns, nullptr);
}

bool Advisor::has_activations() const { return impl_->activations_available; }

const NetworkLayout& Advisor::layout() const { return impl_->layout; }

AdvisorDecision Advisor::recommend(const pipeline::CaseFeatures& features) const {
    return recommend(features, 0.0);
}

AdvisorDecision Advisor::recommend(const pipeline::CaseFeatures& features,
                                   double max_dof) const {
    return recommend(to_columns(features), max_dof);
}

AdvisorDecision Advisor::recommend(const FeatureColumns& columns) const {
    return recommend(columns, 0.0);
}

AdvisorDecision Advisor::recommend(const FeatureColumns& columns, double max_dof) const {
    // One chooser, two entry points. `recommend` is `decide` with nothing
    // watching, so there is no second ranking rule for the drawing to disagree
    // with -- and nothing in `decide` behaves differently when `trace` is null.
    return decide(columns, max_dof, nullptr);
}

AdvisorExplanation Advisor::explain(const pipeline::CaseFeatures& features,
                                    double max_dof) const {
    return explain(to_columns(features), max_dof);
}

AdvisorExplanation Advisor::explain(const FeatureColumns& columns, double max_dof) const {
    // Throws, unlike `recommend`, and deliberately: there is no honest reduced
    // answer here. A caller asking what the network did cannot be handed a
    // decision with empty activations and left to guess whether the network was
    // idle or the model directory was stale, so the reason is reported instead.
    if (!impl_->activations_available) {
        throw AdvisorError(impl_->activation_note);
    }
    AdvisorExplanation explanation;
    explanation.gate_threshold = impl_->gate_threshold;
    explanation.decision = decide(columns, max_dof, &explanation);
    return explanation;
}

ActivationTaps Advisor::taps(const FeatureColumns& columns) const {
    // Same guard and same reason as `explain`: no honest reduced answer.
    if (!impl_->activations_available) {
        throw AdvisorError(impl_->activation_note);
    }
    // Routed through the one `forward`, so this cannot become a second way of
    // reading the graph that disagrees with the one the decision uses.
    ActivationFrame frame;
    // The head outputs are already carried on the frame; the return value is
    // the unpacked form the chooser wants and this caller does not.
    static_cast<void>(impl_->forward(columns, &frame));
    return {std::move(frame.input), std::move(frame.fc1), std::move(frame.fc2),
            std::move(frame.heads)};
}

AdvisorDecision Advisor::decide(const FeatureColumns& columns, double max_dof,
                                AdvisorExplanation* trace) const {
    const Impl& impl = *impl_;
    FeatureColumns query = columns;

    AdvisorDecision decision = impl.default_decision;
    bool clamped = false;
    bool budget_refusal_pending = false;

    if (impl.candidates.empty()) {
        // Legacy artifact with no candidate grid: single-shot policy read. Kept
        // only so an old model directory still loads; measured on held-out
        // families this rule is the worst deployable chooser tested, losing
        // significantly to a zero-parameter constant configuration.
        impl.apply_action(query, impl.default_decision);
        ActivationFrame* frame = nullptr;
        if (trace != nullptr) {
            // There is no grid to index into on a legacy artifact, so this is
            // recorded as the first pass that ran. `gate_pass`, `over_budget`
            // and `ranked` stay false because this branch ranks nothing: it
            // reads the policy head and takes its word. Back-filling the
            // candidate bookkeeping here would draw a chooser that does not
            // exist.
            frame = &trace->frames.emplace_back();
            frame->candidate = 0;
            frame->action = impl.default_decision;
        }
        const AdvisorRawOutputs read = impl.forward(query, frame);
        if (frame != nullptr) {
            // The pass's own accuracy output, recorded because it is a real
            // number this pass produced -- not because anything ranked on it.
            frame->score = read.rel_err_rel;
        }
        const std::vector<double> policy = read.policy;
        if (policy.size() != impl.action_dims.size()) {
            AdvisorDecision vetoed = impl.default_decision;
            vetoed.vetoed = true;
            vetoed.note = "policy head width does not match clamps.json action_dims";
            return vetoed;
        }
        decision.h_rel = impl.h_rel.clamp(policy[0], clamped);
        decision.adapt_passes =
            static_cast<int>(std::lround(impl.adapt_passes.clamp(policy[1], clamped)));
        decision.eta_target = impl.eta_target.clamp(policy[2], clamped);
        const std::size_t order_begin = 3;
        const std::size_t mesher_begin = order_begin + impl.order_choices.size();
        decision.order =
            impl.order_choices[argmax(policy, order_begin, impl.order_choices.size())];
        decision.mesher =
            impl.mesher_choices[argmax(policy, mesher_begin, impl.mesher_choices.size())];
        decision.clamped = clamped;
    } else {
        // Enumerate the measured candidate grid, drop the candidates the
        // feasibility head expects to fail, and rank the survivors by predicted
        // per-case accuracy. Two heads, used in the order that can change the
        // decision -- the veto below can only refuse one after the fact.
        //
        // Every candidate is an action the campaign actually ran, so no query
        // asks the regression heads to extrapolate; that is the failure mode
        // that produced predicted_dof = 1.5e15 on an unseen part.
        struct EfficiencyCandidate {
            AdvisorDecision action;
            double accuracy_score = std::numeric_limits<double>::infinity();
            double predicted_seconds = std::numeric_limits<double>::infinity();
        };
        std::vector<EfficiencyCandidate> efficient_survivors;
        double best_score = std::numeric_limits<double>::infinity();
        double best_risk_score = std::numeric_limits<double>::infinity();
        AdvisorDecision best_survivor = impl.default_decision;
        AdvisorDecision best_any = impl.default_decision;
        bool have_survivor = false;
        bool have_any = false;
        bool saw_over_budget = false;

        for (std::size_t index = 0; index < impl.candidates.size(); ++index) {
            const AdvisorDecision& candidate = impl.candidates[index];
            impl.apply_action(query, candidate);
            ActivationFrame* frame = nullptr;
            if (trace != nullptr) {
                frame = &trace->frames.emplace_back();
                frame->candidate = static_cast<int>(index);
                frame->action = candidate;
            }
            const AdvisorRawOutputs out = impl.forward(query, frame);
            const double risk = sigmoid(out.failure_logit);
            const double score = out.rel_err_rel;
            if (frame != nullptr) {
                // The chooser's own values, not a second evaluation of the same
                // rules: `risk`, `score` and the threshold below are the ones
                // the branches underneath actually test.
                frame->score = score;
                frame->gate_pass = risk <= impl.gate_threshold;
                frame->ranked = std::isfinite(score);
            }
            if (!std::isfinite(score)) {
                continue;
            }
            // The max_dof budget is a hard feasibility filter, applied after
            // the feasibility head has scored the candidate and before any
            // ranking: an action the caller cannot afford is dropped from BOTH
            // pools, so it is never returned -- not even by the gate-binds
            // fallback below, which exists to give the veto the last word, not
            // to spend budget the caller does not have. The dof head is a
            // learned predictor (held-out MAE ~0.5 log10), so this is a filter,
            // not a guarantee.
            if (max_dof > 0.0 && from_log10(out.dof_log10) > max_dof) {
                if (frame != nullptr) {
                    frame->over_budget = true;
                }
                saw_over_budget = true;
                continue;
            }
            // Tracked separately so a query where the gate rejects everything
            // still returns the best ranked action rather than nothing:
            // refusing to act is the veto's job, not the gate's.
            if (score < best_risk_score) {
                best_risk_score = score;
                best_any = candidate;
                have_any = true;
            }
            if (risk <= impl.gate_threshold && score < best_score) {
                best_score = score;
                best_survivor = candidate;
                have_survivor = true;
            }
            if (risk <= impl.gate_threshold && impl.calibration.has_value()) {
                const double solve_seconds =
                    predicted_seconds(from_log10(out.solve_flops_log10),
                                      from_log10(out.solve_bytes_log10), *impl.calibration);
                const double mesh_seconds =
                    from_log10(out.mesh_work_log10) * impl.calibration->ref_mesh_ms / 1000.0;
                const double total_seconds = solve_seconds + mesh_seconds;
                if (std::isfinite(total_seconds)) {
                    efficient_survivors.push_back({.action = candidate,
                                                   .accuracy_score = score,
                                                   .predicted_seconds = total_seconds});
                }
            }
        }
        if (have_survivor && impl.objective == AdvisorObjective::kEfficiency &&
            impl.calibration.has_value()) {
            // A 5% envelope is multiplicative in the de-logged relative error.
            // In log10 space that is an additive log10(1.05), which remains
            // well-defined even when the centred score itself is negative.
            const double accuracy_limit = best_score + std::log10(1.05);
            double best_seconds = std::numeric_limits<double>::infinity();
            for (const EfficiencyCandidate& candidate : efficient_survivors) {
                if (candidate.accuracy_score <= accuracy_limit &&
                    candidate.predicted_seconds < best_seconds) {
                    best_seconds = candidate.predicted_seconds;
                    best_survivor = candidate.action;
                }
            }
            best_survivor.note =
                "efficiency objective: lowest calibrated cost within 5% of best accuracy";
        } else if (have_survivor && impl.objective == AdvisorObjective::kEfficiency) {
            best_survivor.note =
                "efficiency objective requested without host calibration; accuracy used";
        }

        if (have_survivor) {
            decision = best_survivor;
        } else if (have_any) {
            decision = best_any;
            decision.note = "every candidate exceeded the feasibility gate; "
                            "best-ranked action returned and left to the veto";
        } else if (saw_over_budget) {
            // Every scored candidate was over the caller's budget. Declined
            // below with the same suppression as the OOD refusal: the model's
            // predictions for actions we will not recommend are not
            // information the caller can act on.
            budget_refusal_pending = true;
        }
        clamped = decision.clamped;
    }

    // Final pass — re-score the action we are actually about to recommend, so
    // the reported predictions and the feasibility veto both describe the
    // recommendation rather than the default or some other candidate.
    impl.apply_action(query, decision);
    ActivationFrame* final_frame = nullptr;
    if (trace != nullptr) {
        // `recommended` marks the pass that scored the action the chooser was
        // about to recommend, which is what this pass is for. It stays true
        // through a refusal -- the pass really ran on that action -- and
        // `AdvisorExplanation::decision.vetoed` is what says the recommendation
        // did not survive the vetoes below.
        final_frame = &trace->frames.emplace_back();
        final_frame->candidate = -1;
        final_frame->recommended = true;
        final_frame->action = decision;
    }
    const AdvisorRawOutputs scored = impl.forward(query, final_frame);
    if (final_frame != nullptr) {
        final_frame->score = scored.rel_err_rel;
        // `gate_pass` is a predicate on this frame's own output against the same
        // threshold, so it is meaningful here even though the gate itself only
        // ran over the candidates. `ranked` and `over_budget` are candidate-loop
        // bookkeeping and stay false: this pass was never a candidate and was
        // never ranked or budgeted against one.
        final_frame->gate_pass = sigmoid(scored.failure_logit) <= impl.gate_threshold;
    }
    decision.predicted_rel_err = from_log10(scored.rel_err_log10);
    decision.predicted_chamfer_mean = from_log10(scored.geo_chamfer_log10);
    decision.predicted_dof = from_log10(scored.dof_log10);
    decision.predicted_mesh_ms = from_log10(scored.mesh_ms_log10);
    decision.predicted_solve_ms = from_log10(scored.solve_ms_log10);
    decision.predicted_solve_flops = from_log10(scored.solve_flops_log10);
    decision.predicted_solve_bytes = from_log10(scored.solve_bytes_log10);
    decision.predicted_mesh_work = from_log10(scored.mesh_work_log10);
    if (impl.calibration.has_value()) {
        const double seconds = predicted_seconds(
            decision.predicted_solve_flops, decision.predicted_solve_bytes, *impl.calibration);
        if (std::isfinite(seconds)) {
            decision.predicted_solve_seconds = seconds;
        }
    }
    // Reported RAW. This head is a log10 difference, not a log10 level, so
    // de-logging it would turn a per-case score into a meaningless ratio. A
    // non-finite output becomes NaN for the same reason `from_log10` does it:
    // lower is better here, so minus infinity would read as the best action
    // ever proposed, produced by exactly the condition where the prediction is
    // worthless.
    decision.predicted_rel_err_rel = std::isfinite(scored.rel_err_rel)
                                         ? scored.rel_err_rel
                                         : std::numeric_limits<double>::quiet_NaN();
    decision.failure_prob = sigmoid(scored.failure_logit);

    // The OOD test is evaluated over raw feature columns, NOT over encode()'s
    // output: ood.json standardizes with its own center/scale, a different
    // space than normalization.json's mean/std, so the encoded row cannot
    // serve it even though the shipped 62-column contract now includes every
    // column ood.json names.
    //
    // mahalanobis() throws when a named column is missing or non-finite, because
    // imputing it would place an unknown part at the centre of the training
    // distribution and call it familiar. But advisor.hpp promises that inference
    // on a loaded advisor never throws -- an unusable prediction becomes a veto --
    // and apps/cli/main.cpp calls this without a try/catch. So the failure is
    // converted here into the refusal it morally is. Declining to advise is the
    // correct outcome for a part we cannot assess; terminating the process is
    // strictly worse than declining.
    bool ood_assessable = true;
    std::string ood_failure;
    try {
        decision.ood_distance = impl.mahalanobis(columns);
    } catch (const AdvisorError& error) {
        ood_assessable = false;
        ood_failure = error.what();
        decision.ood_distance = std::numeric_limits<double>::quiet_NaN();
    }

    // Both refusals return the defaults, and both SUPPRESS the predictions.
    //
    // The predictions are exactly what must not survive a refusal. Beyond the
    // training support the regression heads do not degrade gracefully, they
    // diverge: the observed case that motivated this gate reported
    // predicted_mesh_ms = 1.66e14 -- about 5,300 years -- for a unit box, next to
    // a failure probability of 1e-65 claiming near-certain success. Printing a
    // number like that to a user is its own defect, independent of the meshing
    // decision, and NaN is the honest value: the model has no opinion here, and a
    // reader can see that it has none.
    //
    // `failure_prob` goes too. It is the output of the same extrapolating trunk,
    // and its confident 1e-65 on an out-of-distribution part is precisely the
    // evidence that it cannot be trusted there.
    //
    // `ood_distance` is retained: it is the measurement that produced the
    // refusal, it is meaningful by construction, and it is what a user or a
    // figure needs in order to see how far outside the part fell.
    const auto refuse = [&](const std::string& note) {
        const double unknown = std::numeric_limits<double>::quiet_NaN();
        AdvisorDecision vetoed = impl.default_decision;
        vetoed.predicted_rel_err = unknown;
        vetoed.predicted_rel_err_rel = unknown;
        vetoed.predicted_chamfer_mean = unknown;
        vetoed.predicted_dof = unknown;
        vetoed.predicted_mesh_ms = unknown;
        vetoed.predicted_solve_ms = unknown;
        vetoed.predicted_solve_flops = unknown;
        vetoed.predicted_solve_bytes = unknown;
        vetoed.predicted_mesh_work = unknown;
        vetoed.predicted_solve_seconds.reset();
        vetoed.failure_prob = unknown;
        vetoed.ood_distance = decision.ood_distance;
        vetoed.clamped = decision.clamped;
        vetoed.vetoed = true;
        vetoed.note = note;
        return vetoed;
    };

    // Two distinct refusals, deliberately not collapsed into one note. A user
    // must be able to tell "this part is unlike anything I was trained on" from
    // "I could not measure this part at all": the first is a statement about the
    // part, the second about our own instrumentation, and they call for different
    // actions. Same discipline as keeping the feasibility gate separate from the
    // OOD veto.
    if (!ood_assessable) {
        return refuse("out-of-distribution test unavailable, so no recommendation can be "
                      "assessed; defaults used (" +
                      ood_failure + ")");
    }

    // Out of distribution refuses the QUESTION, not the answer: beyond the
    // training support every head is extrapolating, the feasibility probability
    // included, so its opinion is no evidence that the action is safe.
    if (!(decision.ood_distance <= impl.ood_threshold)) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "out of distribution: mahalanobis %.4g exceeds the validated "
                      "operating point %.4g; defaults used",
                      decision.ood_distance, impl.ood_threshold);
        return refuse(detail);
    }
    // The budget refusal is checked after the OOD refusals (a part we cannot
    // assess at all is the more fundamental "no") and before the feasibility
    // veto (the budget, not the head, is the reason this recommendation is
    // being refused). `budget_refusal` is what lets a caller tell it apart.
    if (budget_refusal_pending) {
        AdvisorDecision refused = refuse("max_dof budget excluded every scored candidate "
                                         "action; defaults used");
        refused.budget_refusal = true;
        return refused;
    }
    if (decision.failure_prob > impl.veto_threshold) {
        return refuse("feasibility head vetoed the recommendation; defaults used");
    }
    if (clamped) {
        decision.note = "raw policy output projected onto the clamp box";
    }
    return decision;
}

AdvisorDecision advisor_recommend(const std::filesystem::path& model_dir,
                                  const pipeline::CaseFeatures& features) {
    return Advisor(model_dir).recommend(features);
}

std::string to_json(const AdvisorDecision& d) {
    const json out{{"mesher", d.mesher},
                   {"h_rel", d.h_rel},
                   {"order", d.order},
                   {"adapt_passes", d.adapt_passes},
                   {"eta_target", d.eta_target},
                   {"p_elevate", d.p_elevate},
                   {"predicted_rel_err", d.predicted_rel_err},
                   {"predicted_rel_err_rel", d.predicted_rel_err_rel},
                   {"predicted_chamfer_mean", d.predicted_chamfer_mean},
                   {"predicted_dof", d.predicted_dof},
                   {"predicted_mesh_ms", d.predicted_mesh_ms},
                   {"predicted_solve_ms", d.predicted_solve_ms},
                   {"predicted_solve_flops", d.predicted_solve_flops},
                   {"predicted_solve_bytes", d.predicted_solve_bytes},
                   {"predicted_mesh_work", d.predicted_mesh_work},
                   {"predicted_solve_seconds", d.predicted_solve_seconds.has_value()
                                                   ? json(*d.predicted_solve_seconds)
                                                   : json(nullptr)},
                   {"failure_prob", d.failure_prob},
                   {"ood_distance", d.ood_distance},
                   {"vetoed", d.vetoed},
                   {"budget_refusal", d.budget_refusal},
                   {"clamped", d.clamped},
                   {"note", d.note}};
    return out.dump();
}

} // namespace polymesh::advisor
