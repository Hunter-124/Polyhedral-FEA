// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/graded_sizing.hpp"

#include "geom/indicators.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace polymesh::adapt {

GradedSizing::GradedSizing(std::vector<SizeSource> sources, double h_min, double h_max,
                           double beta)
    : sources_(std::move(sources)), h_min_(h_min), h_max_(h_max), beta_(beta) {
    if (!(h_min_ > 0.0) || !(h_max_ >= h_min_) || !(beta_ > 0.0)) {
        throw std::invalid_argument("GradedSizing: need 0 < h_min <= h_max and beta > 0");
    }
    // Clamp source targets into range up front so size_at stays branch-light.
    for (auto& s : sources_) {
        s.h = std::clamp(s.h, h_min_, h_max_);
    }
}

double GradedSizing::size_at(const Eigen::Vector3d& point) const {
    if (sources_.empty()) {
        return h_max_;
    }
    double h = h_max_;
    for (const auto& s : sources_) {
        // Lower envelope: h_i + beta * distance. The min over sources is
        // Lipschitz-beta, so the field grades smoothly with no post-limiter.
        const double cand = s.h + beta_ * (point - s.x).norm();
        if (cand < h) {
            h = cand;
            if (h <= h_min_) {
                return h_min_; // cannot go finer; early out
            }
        }
    }
    return std::clamp(h, h_min_, h_max_);
}

std::vector<SizeSource> geometry_size_sources(const geom::TriSurface& surface, double h_min,
                                              double h_max, double curvature_fraction,
                                              double thickness_fraction) {
    std::vector<SizeSource> out;
    if (surface.vertices.empty()) {
        return out;
    }
    const auto curv = geom::estimate_vertex_curvature(surface);
    const auto thick = geom::estimate_local_thickness(surface);
    out.reserve(surface.vertices.size() / 4 + 1);
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        double h = h_max;
        const double kappa = (i < curv.kappa.size()) ? curv.kappa[i] : 0.0;
        if (kappa > 1e-12) {
            // h ≈ c / κ (κ ≈ 1/R ⇒ h ≈ c·R): resolve the local turn.
            h = std::min(h, curvature_fraction / kappa);
        }
        if (i < thick.thickness.size() && geom::has_finite_thickness(thick.thickness[i])) {
            h = std::min(h, thickness_fraction * thick.thickness[i]);
        }
        if (h < h_max) { // only geometrically interesting vertices become sources
            out.push_back(SizeSource{surface.vertices[i], std::clamp(h, h_min, h_max)});
        }
    }
    return out;
}

std::vector<SizeSource> point_size_sources(std::span<const Eigen::Vector3d> points, double h) {
    std::vector<SizeSource> out;
    out.reserve(points.size());
    for (const auto& p : points) {
        out.push_back(SizeSource{p, h});
    }
    return out;
}

SeedPlan seed_plan(std::span<const SizeSource> sources, double h_coarse, double band_frac) {
    SeedPlan plan;
    plan.h_fine = h_coarse;
    for (const auto& s : sources) {
        if (s.h < h_coarse) {
            plan.refine_seeds.push_back(s.x);
            plan.h_fine = std::min(plan.h_fine, s.h);
        }
    }
    if (!plan.refine_seeds.empty()) {
        plan.seed_band = band_frac * h_coarse;
    }
    return plan;
}

} // namespace polymesh::adapt
