// SPDX-License-Identifier: BSD-3-Clause
// ORT session construction and graph I/O validation, the forward pass, and the
// public entry points that are not the chooser or the drawing.
#include "advisor/advisor.hpp"
#include "advisor/calibration.hpp"
#include "advisor_internal.hpp"

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::advisor {

using detail::json;
using detail::kActivationOutputNames;
using detail::kOutputNames;
using detail::read_json;

namespace {

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
