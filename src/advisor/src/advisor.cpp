// SPDX-License-Identifier: BSD-3-Clause
#include "advisor/advisor.hpp"

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
constexpr std::array<const char*, 9> kOutputNames{
    "rel_err", "rel_err_rel", "geo_chamfer",   "geo_p99", "dof",
    "mesh_ms", "solve_ms",    "failure_logit", "policy"};

} // namespace

struct Advisor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "polymesh_advisor"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    Ort::MemoryInfo memory =
        Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

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
    /// abandons it. Measured, filtering first cuts held-out regret 0.4413 ->
    /// 0.3468 and the rate of recommending an action that then fails outright
    /// 22.7% -> 10.0%, and it is insensitive to the threshold over [0.05, 0.5].
    double gate_threshold = 0.5;

    std::string input_name;
    std::vector<const char*> output_name_ptrs;

    void load_normalization(const std::filesystem::path& dir);
    void load_clamps(const std::filesystem::path& dir);
    [[nodiscard]] std::vector<float> encode(const FeatureColumns& columns) const;
    void apply_action(FeatureColumns& columns, const AdvisorDecision& action) const;
    [[nodiscard]] std::vector<std::vector<float>> run(const std::vector<float>& row) const;
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
    default_decision.adapt_passes = static_cast<int>(std::lround(
        adapt_passes.clamp(static_cast<double>(defaults.value("adapt_passes", 0)),
                           default_clamped)));
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
                candidate.h_rel = h_rel.clamp(entry.value("h_rel", default_decision.h_rel),
                                              clamped);
                candidate.eta_target =
                    eta_target.clamp(entry.value("eta_target", default_decision.eta_target),
                                     clamped);
                candidate.adapt_passes = static_cast<int>(std::lround(adapt_passes.clamp(
                    static_cast<double>(entry.value("adapt_passes",
                                                    default_decision.adapt_passes)),
                    clamped)));
                if (clamped) {
                    throw AdvisorError("advisor: clamps.json candidate_grid action leaves the "
                                       "clamp box the same file declares; re-export the model");
                }
                if (std::find(mesher_choices.begin(), mesher_choices.end(), candidate.mesher) ==
                    mesher_choices.end()) {
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
    gate_threshold = clamps.value("gate_threshold", veto_threshold);
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

void Advisor::Impl::apply_action(FeatureColumns& columns, const AdvisorDecision& action) const {
    // An out-of-vocabulary category is encoded as `size()`, the reserved
    // unknown-embedding slot the trainer allocates (`n_order_slots =
    // len(order_choices) + 1`). Returning 0 would silently score the row as if
    // it had asked for the FIRST category, which is a valid, wrong answer.
    const auto index_of = [](const auto& choices, const auto& value) -> double {
        const auto it = std::find(choices.begin(), choices.end(), value);
        return it == choices.end()
                   ? static_cast<double>(choices.size())
                   : static_cast<double>(std::distance(choices.begin(), it));
    };
    columns["h_rel"] = action.h_rel;
    columns["eta_target"] = action.eta_target;
    columns["adapt_passes"] = static_cast<double>(action.adapt_passes);
    columns["p_elevate"] = action.p_elevate ? 1.0 : 0.0;
    columns["order"] = static_cast<double>(action.order);
    columns["order_idx"] = index_of(order_choices, action.order);
    columns["mesher_idx"] = index_of(mesher_choices, action.mesher);
}

FeatureColumns to_columns(const pipeline::CaseFeatures& f) {
    return {
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
}

std::vector<std::vector<float>> Advisor::Impl::run(const std::vector<float>& row) const {
    const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(row.size())};
    Ort::Value input = Ort::Value::CreateTensor<float>(
        memory, const_cast<float*>(row.data()), row.size(), shape.data(), shape.size());
    const char* input_names[] = {input_name.c_str()};
    auto outputs = session->Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                                output_name_ptrs.data(), output_name_ptrs.size());

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

Advisor::Advisor(const std::filesystem::path& model_dir) : impl_(std::make_unique<Impl>()) {
    const std::filesystem::path graph = model_dir / "model.onnx";
    if (!std::filesystem::exists(graph)) {
        throw AdvisorError("advisor: no model.onnx in " + model_dir.string());
    }
    impl_->load_normalization(model_dir);
    impl_->load_clamps(model_dir);

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

    const std::size_t n_out = impl_->session->GetOutputCount();
    if (n_out != kOutputNames.size()) {
        throw AdvisorError("advisor: model.onnx has " + std::to_string(n_out) +
                           " outputs, expected " + std::to_string(kOutputNames.size()));
    }
    for (std::size_t i = 0; i < n_out; ++i) {
        const std::string name = impl_->session->GetOutputNameAllocated(i, allocator).get();
        if (name != kOutputNames[i]) {
            throw AdvisorError("advisor: model.onnx output " + std::to_string(i) + " is '" +
                               name + "', expected '" + kOutputNames[i] + "'");
        }
    }
    impl_->output_name_ptrs.assign(kOutputNames.begin(), kOutputNames.end());

    const auto in_shape =
        impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (in_shape.size() != 2 ||
        (in_shape[1] > 0 &&
         static_cast<std::size_t>(in_shape[1]) != impl_->input_columns.size())) {
        throw AdvisorError("advisor: model.onnx input width does not match "
                           "normalization.json input_columns");
    }
}

Advisor::~Advisor() = default;
Advisor::Advisor(Advisor&&) noexcept = default;
Advisor& Advisor::operator=(Advisor&&) noexcept = default;

AdvisorDecision Advisor::defaults() const {
    return impl_->default_decision;
}

void Advisor::apply_action(FeatureColumns& columns, const AdvisorDecision& action) const {
    impl_->apply_action(columns, action);
}

AdvisorRawOutputs Advisor::evaluate(const FeatureColumns& columns) const {
    const auto raw = impl_->run(impl_->encode(columns));
    // Index i is `kOutputNames[i]`, and construction has already proven the
    // graph agrees with that list name-by-name, so these are not a guess.
    AdvisorRawOutputs out;
    out.rel_err_log10 = static_cast<double>(raw[0][0]);
    out.rel_err_rel = static_cast<double>(raw[1][0]);
    out.geo_chamfer_log10 = static_cast<double>(raw[2][0]);
    out.geo_p99_log10 = static_cast<double>(raw[3][0]);
    out.dof_log10 = static_cast<double>(raw[4][0]);
    out.mesh_ms_log10 = static_cast<double>(raw[5][0]);
    out.solve_ms_log10 = static_cast<double>(raw[6][0]);
    out.failure_logit = static_cast<double>(raw[7][0]);
    out.policy.assign(raw[8].begin(), raw[8].end());
    return out;
}

AdvisorDecision Advisor::recommend(const pipeline::CaseFeatures& features) const {
    return recommend(to_columns(features));
}

AdvisorDecision Advisor::recommend(const FeatureColumns& columns) const {
    const Impl& impl = *impl_;
    FeatureColumns query = columns;

    AdvisorDecision decision = impl.default_decision;
    bool clamped = false;

    if (impl.candidates.empty()) {
        // Legacy artifact with no candidate grid: single-shot policy read. Kept
        // only so an old model directory still loads; measured on held-out
        // families this rule is the worst deployable chooser tested, losing
        // significantly to a zero-parameter constant configuration.
        impl.apply_action(query, impl.default_decision);
        const std::vector<double> policy = evaluate(query).policy;
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
        double best_score = std::numeric_limits<double>::infinity();
        double best_risk_score = std::numeric_limits<double>::infinity();
        AdvisorDecision best_survivor = impl.default_decision;
        AdvisorDecision best_any = impl.default_decision;
        bool have_survivor = false;
        bool have_any = false;

        for (const AdvisorDecision& candidate : impl.candidates) {
                            impl.apply_action(query, candidate);
                            const AdvisorRawOutputs out = evaluate(query);
                            const double risk = sigmoid(out.failure_logit);
                            const double score = out.rel_err_rel;
                            if (!std::isfinite(score)) {
                                continue;
                            }
                            // Tracked separately so a query where the gate
                            // rejects everything still returns the best ranked
                            // action rather than nothing: refusing to act is the
                            // veto's job, not the gate's.
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
        }

        if (have_survivor) {
            decision = best_survivor;
        } else if (have_any) {
            decision = best_any;
            decision.note = "every candidate exceeded the feasibility gate; "
                            "best-ranked action returned and left to the veto";
        }
        clamped = decision.clamped;
    }

    // Final pass — re-score the action we are actually about to recommend, so
    // the reported predictions and the feasibility veto both describe the
    // recommendation rather than the default or some other candidate.
    impl.apply_action(query, decision);
    const AdvisorRawOutputs scored = evaluate(query);
    decision.predicted_rel_err = from_log10(scored.rel_err_log10);
    decision.predicted_chamfer_mean = from_log10(scored.geo_chamfer_log10);
    decision.predicted_dof = from_log10(scored.dof_log10);
    decision.predicted_mesh_ms = from_log10(scored.mesh_ms_log10);
    decision.predicted_solve_ms = from_log10(scored.solve_ms_log10);
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

    if (decision.failure_prob > impl.veto_threshold) {
        AdvisorDecision vetoed = impl.default_decision;
        vetoed.predicted_rel_err = decision.predicted_rel_err;
        vetoed.predicted_rel_err_rel = decision.predicted_rel_err_rel;
        vetoed.predicted_chamfer_mean = decision.predicted_chamfer_mean;
        vetoed.predicted_dof = decision.predicted_dof;
        vetoed.predicted_mesh_ms = decision.predicted_mesh_ms;
        vetoed.predicted_solve_ms = decision.predicted_solve_ms;
        vetoed.failure_prob = decision.failure_prob;
        vetoed.clamped = decision.clamped;
        vetoed.vetoed = true;
        vetoed.note = "feasibility head vetoed the recommendation; defaults used";
        return vetoed;
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
                   {"failure_prob", d.failure_prob},
                   {"vetoed", d.vetoed},
                   {"clamped", d.clamped},
                   {"note", d.note}};
    return out.dump();
}

} // namespace polymesh::advisor
