// SPDX-License-Identifier: BSD-3-Clause
// Model-directory artifacts: normalization.json (network input order and
// standardization), clamps.json (clamp box, vocabularies, candidate grid,
// gate) and ood.json (out-of-distribution test).
#include "advisor_internal.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace polymesh::advisor {
namespace detail {

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

} // namespace detail

using detail::Interval;
using detail::json;
using detail::read_json;

namespace {

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

} // namespace

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

    // 3 continuous dims + one logit per order + one per mesher.
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

    // The candidate list is optional: an artifact without it still loads, and
    // recommend() then uses the single-shot policy read. Validating it here
    // rather than at query time means a malformed list is a construction error,
    // never a silently degraded recommendation.
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
    // Required, never defaulted, and never inherited from `veto_threshold`: the
    // gate filters candidates before ranking, the veto abandons the whole
    // recommendation afterwards, and they have different correct values. An
    // absent key is a misconfiguration and fails here rather than degrading a
    // recommendation.
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
// over geometry/BC context columns in its own standardized space
// (`center`/`scale`). An unusual ACTION is a legal query; only an unfamiliar
// PART is out of distribution.
//
// Required, never defaulted, for the same reason `gate_threshold` is: the
// threshold is the validated operating point the artifact records (with its
// measured false-alarm and held-out detection rates), and an advisor silently
// running without it would answer questions about parts unlike anything it was
// trained on -- exactly what the abstention exists to prevent.
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
    // Not checked: whether these names appear in normalization.json:input_columns.
    // The two feature sets are fitted independently, so overlapping but
    // different sets are expected, and by-name resolution keeps either side free
    // to drift.
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

/// Mahalanobis distance of one query from the training centre, over the ood.json
/// columns in the ood.json order. Mirrors
/// `scripts/advisor/calibration.py:ood_scores` exactly, including the clamp at
/// zero that guards the square root against a tiny negative quadratic form
/// produced by rounding in a near-degenerate direction.
///
/// Throws when the caller did not supply a column the file names, or supplied a
/// non-finite one. Unlike `encode()`, it never imputes: the training median
/// would place an unknown part at the centre of the training distribution and
/// report it as familiar. A missing descriptor must abort the test.
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
    // construction and a narrower accumulation here would be
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

} // namespace polymesh::advisor
