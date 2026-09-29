// SPDX-License-Identifier: BSD-3-Clause
// The chooser: candidate enumeration, feasibility gate, budget, ranking, final
// re-score, and the OOD / budget / veto refusals.
#include "advisor_internal.hpp"

#include "advisor/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace polymesh::advisor {
namespace {

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

} // namespace

AdvisorDecision Advisor::decide(const FeatureColumns& columns, double max_dof,
                                AdvisorExplanation* trace) const {
    const Impl& impl = *impl_;
    FeatureColumns query = columns;

    AdvisorDecision decision = impl.default_decision;
    bool clamped = false;
    bool budget_refusal_pending = false;

    if (impl.candidates.empty()) {
        // Artifact with no candidate grid: single-shot policy read, kept only
        // so such a model directory still loads.
        impl.apply_action(query, impl.default_decision);
        ActivationFrame* frame = nullptr;
        if (trace != nullptr) {
            // There is no grid to index into on a grid-less artifact, so this is
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
        // asks the regression heads to extrapolate.
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
            // learned predictor, so this is a filter, not a guarantee.
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
    // space than normalization.json's mean/std.
    //
    // mahalanobis() throws when a named column is missing or non-finite, but
    // advisor.hpp promises that inference on a loaded advisor never throws --
    // an unusable prediction becomes a veto -- and the CLI solve command
    // (apps/cli/commands_solve.cpp) calls this without a try/catch. So the
    // failure is converted here into a refusal: declining to advise a part we
    // cannot assess, not terminating the process.
    bool ood_assessable = true;
    std::string ood_failure;
    try {
        decision.ood_distance = impl.mahalanobis(columns);
    } catch (const AdvisorError& error) {
        ood_assessable = false;
        ood_failure = error.what();
        decision.ood_distance = std::numeric_limits<double>::quiet_NaN();
    }

    // Both refusals return the defaults, and both SUPPRESS the predictions
    // (NaN): beyond the training support the regression heads diverge rather
    // than degrade, and `failure_prob` is the output of the same extrapolating
    // trunk. `ood_distance` is retained: it is the measurement that produced
    // the refusal and shows how far outside the part fell.
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

    // Two distinct refusals, deliberately not collapsed into one note: "this
    // part is unlike anything I was trained on" is a statement about the part,
    // "I could not measure this part at all" one about our own
    // instrumentation, and they call for different actions.
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

} // namespace polymesh::advisor
