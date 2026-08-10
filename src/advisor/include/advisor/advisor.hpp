// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Learned mesh advisor inference (ADR-0027). Loads the exported multi-head
// network and turns geometry + boundary-condition features into a concrete,
// guardrailed mesh action.
//
// The model directory is the training package's output directory and must
// contain `model.onnx`, `normalization.json`, and `clamps.json`. Those three
// files are the single source of truth shared with scripts/advisor/ — nothing
// about the feature order, the clamp box, or the mesher vocabulary is
// duplicated as a C++ constant.

#include "pipeline/scene.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace polymesh::advisor {

/// Raised for an unusable model directory (missing/ill-formed artifacts, a
/// graph whose I/O does not match `normalization.json`). Inference on a loaded
/// advisor never throws: an unusable prediction becomes a veto instead.
class AdvisorError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A mesh action plus the predictions that justify it. Every field is already
/// inside the clamp box from `clamps.json`.
struct AdvisorDecision {
    std::string mesher = "hybrid_zoo";
    double h_rel = 0.1;
    int order = 1;
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = false;

    /// Predictions at the recommended action (linear units, de-logged).
    double predicted_rel_err = 0.0;
    double predicted_chamfer_mean = 0.0;
    double predicted_dof = 0.0;
    double predicted_mesh_ms = 0.0;
    double predicted_solve_ms = 0.0;

    /// Feasibility head output in [0, 1]. Above `clamps.json:veto_threshold`
    /// the recommendation is discarded and `defaults` are returned instead.
    double failure_prob = 0.0;
    bool vetoed = false;

    /// True when a raw policy output fell outside the clamp box and was
    /// projected back onto it. The advisor is honest about being overruled.
    bool clamped = false;

    std::string note;
};

/// One-line JSON, suitable for a CLI log or a campaign row field.
[[nodiscard]] std::string to_json(const AdvisorDecision& decision);

/// Raw head outputs for one (case, action) pair, exactly as the graph emits
/// them: the six regressors are log10 of their target, `failure_logit` is a
/// pre-sigmoid logit, and `policy` is the un-clamped action proposal.
struct AdvisorRawOutputs {
    double rel_err_log10 = 0.0;
    double geo_chamfer_log10 = 0.0;
    double geo_p99_log10 = 0.0;
    double dof_log10 = 0.0;
    double mesh_ms_log10 = 0.0;
    double solve_ms_log10 = 0.0;
    double failure_logit = 0.0;
    std::vector<double> policy;
};

/// Column-keyed model input. Keys are `normalization.json:input_columns`
/// names; any column left out is filled from that file's training median, so a
/// caller that only knows part of the row is well defined rather than wrong.
using FeatureColumns = std::map<std::string, double>;

/// The columns a `CaseFeatures` can supply, by their dataset names.
[[nodiscard]] FeatureColumns to_columns(const pipeline::CaseFeatures& features);

/// A loaded advisor. Construction is the expensive part (ORT session + graph
/// validation); `recommend` is a pair of single-row forward passes.
///
/// Deterministic by construction: CPU execution provider only, one intra-op
/// thread, no parallel execution mode.
class Advisor {
public:
    explicit Advisor(const std::filesystem::path& model_dir);
    ~Advisor();
    Advisor(Advisor&&) noexcept;
    Advisor& operator=(Advisor&&) noexcept;
    Advisor(const Advisor&) = delete;
    Advisor& operator=(const Advisor&) = delete;

    /// Two passes: the policy head is queried at the default action, then the
    /// outcome heads are re-evaluated at the action the policy proposed, so
    /// `failure_prob` scores the action actually being recommended. Action
    /// columns present in `columns` are overwritten by the queried action.
    [[nodiscard]] AdvisorDecision recommend(const FeatureColumns& columns) const;
    [[nodiscard]] AdvisorDecision recommend(const pipeline::CaseFeatures& features) const;

    /// One unmodified forward pass on exactly these columns. This is the
    /// what-if entry point — scoring a configuration the advisor did not
    /// choose — and the surface the parity tests replay, because it applies no
    /// policy of its own.
    [[nodiscard]] AdvisorRawOutputs evaluate(const FeatureColumns& columns) const;

    /// Overwrite the action columns of `columns` in place with `action`,
    /// including the categorical index columns. Exposed so a caller can build
    /// the exact row a given action would produce.
    void apply_action(FeatureColumns& columns, const AdvisorDecision& action) const;

    /// The clamp-box defaults, i.e. what a veto returns.
    [[nodiscard]] AdvisorDecision defaults() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Convenience one-shot: load and recommend. Prefer `Advisor` when advising
/// more than one case, so the session is built once.
[[nodiscard]] AdvisorDecision advisor_recommend(const std::filesystem::path& model_dir,
                                                const pipeline::CaseFeatures& features);

} // namespace polymesh::advisor
