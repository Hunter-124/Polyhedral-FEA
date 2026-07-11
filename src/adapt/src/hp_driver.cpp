// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/hp_driver.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace polymesh::adapt {
namespace {

constexpr double kPi = 3.14159265358979323846;

double turn_threshold_rad(const HpDriverPolicy& p) {
    return p.turn_angle_deg * kPi / 180.0;
}

bool is_thin_wall(double thickness) {
    return std::isfinite(thickness) && thickness > 0.0 &&
           thickness < 0.5 * std::numeric_limits<double>::max();
}

/// Dimensionless geometry severity: ≥1 means geometry gate forces h.
double geometry_severity(const ElementHpSignal& s, const HpDriverPolicy& policy) {
    double sev = 0.0;
    const double theta = turn_threshold_rad(policy);
    if (theta > 0.0 && s.h > 0.0 && s.kappa > 0.0) {
        const double turn = s.h * s.kappa; // radians of surface turn through cell
        sev = std::max(sev, turn / theta);
    }
    if (is_thin_wall(s.thickness) && s.h > 0.0 && policy.thin_wall_fraction > 0.0) {
        const double target = policy.thin_wall_fraction * s.thickness;
        if (target > 0.0) {
            sev = std::max(sev, s.h / target);
        }
    }
    return sev;
}

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

ShapeTendency best_shape_vote(const ElementHpSignal& s) {
    // Prefer the fitness winner if it clearly beats the second place.
    const double hx = s.hex_fit;
    const double tt = s.tet_fit;
    const double py = s.poly_fit;
    const double m = std::max({hx, tt, py});
    const double second = (m == hx) ? std::max(tt, py) : (m == tt ? std::max(hx, py) : std::max(hx, tt));
    if (m - second < 0.08) {
        return ShapeTendency::kKeep; // ambiguous
    }
    if (m == hx) {
        return ShapeTendency::kPreferHex;
    }
    if (m == tt) {
        return ShapeTendency::kPreferTet;
    }
    return ShapeTendency::kPreferPoly;
}

double shape_awkwardness(const ElementHpSignal& s) {
    // High when the best non-keep fit is much better than a balanced 0.5, or
    // when geometry is moderately curved and hex fitness is poor.
    const double best = std::max({s.hex_fit, s.tet_fit, s.poly_fit});
    const double spread = best - std::min({s.hex_fit, s.tet_fit, s.poly_fit});
    double awkward = clamp01(spread);
    if (s.kappa > 0.0 && s.h > 0.0) {
        // Mild curvature + weak hex → poly/tet more attractive.
        const double turn = s.h * s.kappa;
        if (s.hex_fit < 0.4 && turn > 0.05) {
            awkward = std::max(awkward, 0.35);
        }
    }
    return awkward;
}

double at_or_broadcast(std::span<const double> v, std::size_t i, double fallback) {
    if (v.empty()) {
        return fallback;
    }
    if (v.size() == 1) {
        return v[0];
    }
    return v[i];
}

int at_or_broadcast_int(std::span<const int> v, std::size_t i, int fallback) {
    if (v.empty()) {
        return fallback;
    }
    if (v.size() == 1) {
        return v[0];
    }
    return v[i];
}

} // namespace

ElementHpDecision decide_element(const ElementHpSignal& s, const HpDriverPolicy& policy,
                                 double max_eta) {
    ElementHpDecision d;
    d.p_next = s.p;
    d.h_next = s.h;
    d.shape = ShapeTendency::kKeep;

    if (!(s.h > 0.0) || s.p < 1) {
        d.reason = "invalid";
        return d;
    }

    const double eta = std::max(0.0, s.eta);
    const double surplus = std::max(0.0, s.surplus);
    const double max_e = std::max(max_eta, 0.0);
    if (max_e > 0.0 && eta < policy.eta_rel_floor * max_e) {
        d.reason = "floor";
        return d;
    }
    // Zero global error and no geometry pressure → stay put.
    const double geo = geometry_severity(s, policy);
    if (eta <= 0.0 && geo < policy.geometry_force_h) {
        d.reason = "zero-eta";
        return d;
    }

    const double eps = 1e-30;
    const double surplus_ratio = surplus / std::max(eta, eps);
    const bool can_raise_p = s.p < s.p_max && s.p_max > 0;

    // --- Benefit estimates (dimensionless error-reduction proxies) ---
    // Geometry-driven h: severity above gate, scaled by residual (or pure geo).
    const double benefit_h_geo =
        (geo >= policy.geometry_force_h)
            ? geo * std::max(eta, 0.1 * std::max(max_e, 1e-6))
            : 0.0;
    // Non-smooth residual: large η, small surplus ratio → h refinement pays.
    const double nonsmooth = clamp01(1.0 - surplus_ratio / std::max(policy.smooth_surplus_ratio, 1e-6));
    const double benefit_h_nonsmooth =
        (geo < policy.geometry_force_h) ? eta * nonsmooth * nonsmooth : 0.0;
    const double benefit_h = std::max(benefit_h_geo, benefit_h_nonsmooth);

    // Smooth interior: flat geometry + surplus captures residual → p-raise.
    double benefit_p = 0.0;
    if (can_raise_p && geo < policy.geometry_force_h) {
        const double smooth =
            clamp01(surplus_ratio / std::max(policy.smooth_surplus_ratio, 1e-6));
        // Prefer p only when smoothness is competitive (ratio near/above threshold).
        if (surplus_ratio >= 0.5 * policy.smooth_surplus_ratio) {
            benefit_p = eta * smooth * smooth * (1.0 - 0.15 * static_cast<double>(s.p));
        }
    }

    // Shape: only when fitness is decisive and residual or mild geometry exists.
    const ShapeTendency vote = best_shape_vote(s);
    double benefit_shape = 0.0;
    if (vote != ShapeTendency::kKeep) {
        const double awkward = shape_awkwardness(s);
        benefit_shape = awkward * std::max(eta, 0.05 * std::max(max_e, 1e-6));
        // Geometry gate already owns strong curvature; shape is secondary there.
        if (geo >= policy.geometry_force_h) {
            benefit_shape *= 0.25;
        }
    }

    const double cost_h = std::max(policy.cost_h, 1e-6);
    const double cost_p =
        std::max(policy.cost_p + 0.4 * static_cast<double>(std::max(s.p, 1)), 1e-6);
    const double cost_shape = std::max(policy.cost_shape, 1e-6);

    d.utility_h = benefit_h / cost_h;
    d.utility_p = benefit_p / cost_p;
    d.utility_shape = benefit_shape / cost_shape;

    // Geometry hard gate: never choose p over h when geometry severity forces h.
    if (geo >= policy.geometry_force_h) {
        d.utility_p = 0.0;
    }

    const double uh = d.utility_h;
    const double up = d.utility_p;
    const double us = d.utility_shape;
    const double best = std::max({uh, up, us});

    if (best < policy.min_utility) {
        d.reason = "low-utility";
        return d;
    }

    // Tie-break order: h > p > shape (geometry / non-smooth first).
    if (uh >= up && uh >= us) {
        d.action = HpAction::kHRefine;
        d.h_next = s.h * policy.h_refine_factor;
        if (policy.h_min > 0.0) {
            d.h_next = std::max(d.h_next, policy.h_min);
        }
        d.reason = (benefit_h_geo >= benefit_h_nonsmooth) ? "geometry-h" : "nonsmooth-h";
        return d;
    }
    if (up >= us) {
        d.action = HpAction::kPRaise;
        d.p_next = s.p + 1;
        d.reason = "smooth-p";
        return d;
    }
    d.action = HpAction::kShapeChange;
    d.shape = vote;
    d.reason = "shape-fit";
    return d;
}

std::vector<double> estimate_surplus_from_zz(const std::vector<double>& element_eta,
                                             double dorfler_theta) {
    const auto n = element_eta.size();
    std::vector<double> surplus(n, 0.0);
    if (n == 0) {
        return surplus;
    }
    const auto high = dorfler_mark(element_eta, dorfler_theta);
    std::vector<char> is_high(n, 0);
    for (auto i : high) {
        if (i < n) {
            is_high[i] = 1;
        }
    }
    // Smooth complement: surplus ≈ η (next order would capture residual).
    // High-η set: surplus ≈ 0.1 η (singular / non-smooth).
    for (std::size_t i = 0; i < n; ++i) {
        const double e = std::max(0.0, element_eta[i]);
        surplus[i] = is_high[i] ? 0.1 * e : e;
    }
    // When total η² is zero, dorfler returns empty and is_high is all 0 → surplus 0.
    // mark_smooth would return all indices; we keep surplus 0 so decide_element
    // exits via zero-eta rather than spuriously raising p.
    return surplus;
}

std::vector<ElementHpSignal> make_hp_signals(std::span<const double> h,
                                             std::span<const double> kappa,
                                             std::span<const double> thickness,
                                             std::span<const double> eta_zz,
                                             std::span<const double> surplus,
                                             std::span<const int> p_orders,
                                             std::span<const double> hex_fit,
                                             std::span<const double> tet_fit,
                                             std::span<const double> poly_fit,
                                             const HpDriverPolicy& policy) {
    const std::size_t n = eta_zz.size();
    if (n == 0) {
        return {};
    }
    auto check_len = [n](std::span<const double> v, const char* name) {
        if (!v.empty() && v.size() != 1 && v.size() != n) {
            throw std::invalid_argument(std::string("make_hp_signals: length mismatch: ") +
                                        name);
        }
    };
    check_len(h, "h");
    check_len(kappa, "kappa");
    check_len(thickness, "thickness");
    check_len(surplus, "surplus");
    check_len(hex_fit, "hex_fit");
    check_len(tet_fit, "tet_fit");
    check_len(poly_fit, "poly_fit");
    if (!p_orders.empty() && p_orders.size() != 1 && p_orders.size() != n) {
        throw std::invalid_argument("make_hp_signals: length mismatch: p");
    }

    std::vector<double> surplus_use;
    if (surplus.empty()) {
        surplus_use = estimate_surplus_from_zz(
            std::vector<double>(eta_zz.begin(), eta_zz.end()), policy.dorfler_theta);
    }

    std::vector<ElementHpSignal> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        ElementHpSignal& s = out[i];
        s.h = at_or_broadcast(h, i, 1.0);
        s.kappa = at_or_broadcast(kappa, i, 0.0);
        s.thickness = at_or_broadcast(thickness, i, 0.0);
        s.eta = eta_zz[i];
        s.surplus = surplus.empty() ? surplus_use[i] : at_or_broadcast(surplus, i, 0.0);
        s.p = at_or_broadcast_int(p_orders, i, 1);
        s.p_max = policy.p_max_default;
        s.hex_fit = at_or_broadcast(hex_fit, i, 0.5);
        s.tet_fit = at_or_broadcast(tet_fit, i, 0.5);
        s.poly_fit = at_or_broadcast(poly_fit, i, 0.5);
    }
    return out;
}

HpDriverPlan drive_hp(std::span<const ElementHpSignal> signals, const HpDriverPolicy& policy,
                      std::span<const Eigen::Vector3d> centroids, double h_uniform) {
    HpDriverPlan plan;
    const auto n = signals.size();
    if (!centroids.empty() && centroids.size() != n) {
        throw std::invalid_argument("drive_hp: centroids size mismatch");
    }
    plan.decisions.resize(n);
    if (n == 0) {
        return plan;
    }

    double max_eta = 0.0;
    for (const auto& s : signals) {
        max_eta = std::max(max_eta, s.eta);
    }

    double h_ref = h_uniform;
    if (!(h_ref > 0.0)) {
        // Median-ish of provided h values.
        double sum = 0.0;
        for (const auto& s : signals) {
            sum += s.h;
        }
        h_ref = sum / static_cast<double>(n);
    }

    std::vector<double> h_eta(n, 0.0); // η restricted to h-candidates for Dörfler
    for (std::size_t i = 0; i < n; ++i) {
        plan.decisions[i] = decide_element(signals[i], policy, max_eta);
        switch (plan.decisions[i].action) {
        case HpAction::kHRefine:
            plan.h_mark.push_back(i);
            // Geometry-only marks may have tiny η; give them weight so Dörfler
            // does not drop pure a-priori h candidates.
            h_eta[i] = std::max(signals[i].eta, 0.0);
            if (h_eta[i] <= 0.0) {
                h_eta[i] = (max_eta > 0.0) ? 0.05 * max_eta : 1.0;
            }
            ++plan.n_h;
            break;
        case HpAction::kPRaise:
            plan.p_mark.push_back(i);
            ++plan.n_p;
            break;
        case HpAction::kShapeChange:
            plan.shape_mark.push_back(i);
            ++plan.n_shape;
            break;
        case HpAction::kNone:
        default:
            ++plan.n_none;
            break;
        }
    }

    // Optional: shrink h_mark with Dörfler on h-candidates so the seed set stays
    // focused when many mild geometry marks fire.
    if (plan.h_mark.size() > 1) {
        const auto focused = dorfler_mark(h_eta, policy.dorfler_theta);
        if (!focused.empty()) {
            std::unordered_set<std::size_t> keep(focused.begin(), focused.end());
            std::vector<std::size_t> filtered;
            filtered.reserve(focused.size());
            for (auto i : plan.h_mark) {
                if (keep.contains(i)) {
                    filtered.push_back(i);
                }
            }
            // Keep at least the original strongest if filter emptied somehow.
            if (!filtered.empty()) {
                plan.h_mark = std::move(filtered);
                plan.n_h = plan.h_mark.size();
            }
        }
    }

    // Global shape tendency: majority vote among shape marks; ties → kKeep.
    if (!plan.shape_mark.empty()) {
        int n_hex = 0, n_tet = 0, n_poly = 0;
        for (auto i : plan.shape_mark) {
            switch (plan.decisions[i].shape) {
            case ShapeTendency::kPreferHex:
                ++n_hex;
                break;
            case ShapeTendency::kPreferTet:
                ++n_tet;
                break;
            case ShapeTendency::kPreferPoly:
                ++n_poly;
                break;
            default:
                break;
            }
        }
        const int m = std::max({n_hex, n_tet, n_poly});
        int winners = (n_hex == m) + (n_tet == m) + (n_poly == m);
        if (m > 0 && winners == 1) {
            if (n_hex == m) {
                plan.global_shape = ShapeTendency::kPreferHex;
            } else if (n_tet == m) {
                plan.global_shape = ShapeTendency::kPreferTet;
            } else {
                plan.global_shape = ShapeTendency::kPreferPoly;
            }
        }
    }

    // h_suggestion: reuse AdaptSuggestion layout for pipeline/CLI.
    AdaptSuggestion sug;
    sug.n_marked = plan.h_mark.size();
    sug.marked_fraction =
        n > 0 ? static_cast<double>(sug.n_marked) / static_cast<double>(n) : 0.0;
    sug.seed_band = 1.5 * h_ref;
    if (sug.n_marked == 0 || sug.marked_fraction < 0.05) {
        sug.h_next = h_ref;
    } else {
        sug.h_next = h_ref * policy.h_refine_factor;
        if (policy.h_min > 0.0) {
            sug.h_next = std::max(sug.h_next, policy.h_min);
        }
    }
    if (!centroids.empty()) {
        sug.refine_seeds.reserve(plan.h_mark.size());
        for (auto i : plan.h_mark) {
            sug.refine_seeds.push_back(centroids[i]);
        }
    }
    plan.h_suggestion = std::move(sug);

    // Predicted relative DOF cost of applying the plan (rough v1).
    const double frac_h =
        n > 0 ? static_cast<double>(plan.h_mark.size()) / static_cast<double>(n) : 0.0;
    const double frac_p =
        n > 0 ? static_cast<double>(plan.p_mark.size()) / static_cast<double>(n) : 0.0;
    const double frac_s =
        n > 0 ? static_cast<double>(plan.shape_mark.size()) / static_cast<double>(n) : 0.0;
    plan.predicted_dof_factor =
        1.0 + frac_h * (policy.cost_h - 1.0) + frac_p * (policy.cost_p - 1.0) +
        frac_s * 0.25 * (policy.cost_shape - 1.0);

    (void)policy.seed; // reserved for campaign-noise experiments
    return plan;
}

const char* shape_tendency_tag(ShapeTendency t) {
    switch (t) {
    case ShapeTendency::kPreferHex:
        return "hex";
    case ShapeTendency::kPreferTet:
        return "tet";
    case ShapeTendency::kPreferPoly:
        return "poly";
    case ShapeTendency::kKeep:
    default:
        return "keep";
    }
}

std::string summarize_hp_plan(const HpDriverPlan& plan) {
    return std::format("hp-driver: h={} p={} shape={} none={} tendency={} dof×{:.2f}",
                       plan.n_h, plan.n_p, plan.n_shape, plan.n_none,
                       shape_tendency_tag(plan.global_shape), plan.predicted_dof_factor);
}

} // namespace polymesh::adapt
