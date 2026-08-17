// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Learned mesh advisor inference (ADR-0027). Loads the exported multi-head
// network and turns geometry + boundary-condition features into a concrete,
// guardrailed mesh action.
//
// The model directory is the training package's output directory and must
// contain `model.onnx`, `normalization.json`, `clamps.json`, and `ood.json`.
// Those four files are the single source of truth shared with
// scripts/advisor/ — nothing about the feature order, the clamp box, the
// mesher vocabulary, or the out-of-distribution operating point is duplicated
// as a C++ constant.

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

    /// Centred accuracy score at the recommended action: the `rel_err` head's
    /// log10 prediction minus that case's median over the actions run, so it is
    /// reported RAW (a log10 difference), not de-logged like the fields above.
    ///
    /// This is a PER-CASE-RELATIVE score. It is comparable BETWEEN actions on
    /// the same case — lower is better, and it is the quantity that actually
    /// discriminates a good mesh from a bad one — and it is meaningless as an
    /// absolute number: the per-case reference-truth offset it subtracts out is
    /// exactly the part `predicted_rel_err` cannot generalize. Never threshold
    /// it, never compare it across cases.
    double predicted_rel_err_rel = 0.0;

    /// Feasibility head output in [0, 1]. Reported for audit; the candidate
    /// GATE (`clamps.json:gate_threshold`) uses it to filter candidates before
    /// ranking, and the residual `clamps.json:veto_threshold` refuses a
    /// recommendation the model expects to fail outright.
    double failure_prob = 0.0;

    /// Mahalanobis distance of this query's geometry features from the training
    /// distribution, in standardized feature space, using the mean and
    /// precision in `ood.json`. Above `ood.json:operating_point.threshold` the
    /// recommendation is refused: the model is being asked about a part unlike
    /// anything it was trained on, and its heads would be extrapolating.
    double ood_distance = 0.0;

    bool vetoed = false;

    /// True when the refusal was caused by the caller's `max_dof` budget: every
    /// scored candidate action's predicted dof head exceeded the budget, so
    /// the chooser returned the clamp-box defaults with all predictions
    /// suppressed exactly like the OOD/veto refusals. `vetoed` is also set —
    /// it says a refusal happened, this says WHY — so a caller can tell
    /// "nothing affordable exists" apart from "this part is out of
    /// distribution" or "the feasibility head vetoed".
    ///
    /// Treat the budget as a feasibility filter, not a guarantee: the dof head
    /// is a learned predictor whose held-out MAE is ~0.5 in log10 (about a 3x
    /// factor in linear dof), so a candidate just under the budget can land
    /// over it in reality.
    bool budget_refusal = false;

    /// True when a raw policy output fell outside the clamp box and was
    /// projected back onto it. The advisor is honest about being overruled.
    bool clamped = false;

    std::string note;
};

/// One-line JSON, suitable for a CLI log or a campaign row field.
[[nodiscard]] std::string to_json(const AdvisorDecision& decision);

/// Raw head outputs for one (case, action) pair, exactly as the graph emits
/// them: the seven regressors are log10 of their target, `failure_logit` is a
/// pre-sigmoid logit, and `policy` is the un-clamped action proposal.
///
/// `rel_err_rel` is the one regressor that is not a log10 level but a log10
/// DIFFERENCE — `rel_err` centred on its per-case median — so it is only
/// meaningful when compared against other actions on the same case.
struct AdvisorRawOutputs {
    double rel_err_log10 = 0.0;
    double rel_err_rel = 0.0;
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
    ///
    /// The `max_dof` overloads add a budget to the gated-enumeration chooser:
    /// after the feasibility gate and before the accuracy ranking, every
    /// candidate whose dof head (`10^dof_log10`) exceeds `max_dof` is dropped.
    /// `max_dof == 0` disables the budget and reproduces the historical
    /// decision exactly. A budget that empties the candidate set is a refusal
    /// (`budget_refusal`), never a relaxation: the advisor does not answer an
    /// affordability question with an unaffordable action. The budget applies
    /// to the enumerated candidate grid only; a legacy artifact without
    /// `clamps.json:candidate_grid` has no enumeration to filter and ignores it.
    [[nodiscard]] AdvisorDecision recommend(const FeatureColumns& columns) const;
    [[nodiscard]] AdvisorDecision recommend(const FeatureColumns& columns,
                                            double max_dof) const;
    [[nodiscard]] AdvisorDecision recommend(const pipeline::CaseFeatures& features) const;
    [[nodiscard]] AdvisorDecision recommend(const pipeline::CaseFeatures& features,
                                            double max_dof) const;

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
