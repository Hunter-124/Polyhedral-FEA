// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/graded_sizing.hpp"

#include "geom/indicators.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    build_grid();
}

void GradedSizing::build_grid() {
    cell_start_.clear();
    items_.clear();
    grid_nx_ = grid_ny_ = grid_nz_ = 0;
    if (sources_.empty()) {
        return;
    }
    Eigen::Vector3d lo = sources_[0].x;
    Eigen::Vector3d hi = sources_[0].x;
    for (const auto& s : sources_) {
        lo = lo.cwiseMin(s.x);
        hi = hi.cwiseMax(s.x);
    }
    grid_origin_ = lo;
    const Eigen::Vector3d ext = (hi - lo).cwiseMax(0.0);
    // Cell ≥ the influence radius (h_max−h_min)/beta so a source outside the
    // 3×3×3 neighbourhood can never lower the envelope (proof: its cand ≥ h_max).
    // Also ≥ mean spacing so the grid stays O(#sources) cells, not huge.
    const double rmax = (h_max_ - h_min_) / beta_;
    const double vol = std::max(ext.x() * ext.y() * ext.z(), 1e-300);
    const double mean_spacing =
        std::cbrt(vol / static_cast<double>(sources_.size()));
    grid_cell_ = std::max({rmax, mean_spacing, 1e-12});
    const double inv = 1.0 / grid_cell_;
    grid_nx_ = std::max(1, static_cast<std::int32_t>(std::floor(ext.x() * inv)) + 1);
    grid_ny_ = std::max(1, static_cast<std::int32_t>(std::floor(ext.y() * inv)) + 1);
    grid_nz_ = std::max(1, static_cast<std::int32_t>(std::floor(ext.z() * inv)) + 1);
    const std::size_t ncells = static_cast<std::size_t>(grid_nx_) *
                               static_cast<std::size_t>(grid_ny_) *
                               static_cast<std::size_t>(grid_nz_);
    auto bucket = [&](const Eigen::Vector3d& p) -> std::size_t {
        auto ci = [&](double v, double o, std::int32_t n) {
            std::int32_t i = static_cast<std::int32_t>(std::floor((v - o) * inv));
            return i < 0 ? 0 : (i >= n ? n - 1 : i);
        };
        const std::int32_t ix = ci(p.x(), grid_origin_.x(), grid_nx_);
        const std::int32_t iy = ci(p.y(), grid_origin_.y(), grid_ny_);
        const std::int32_t iz = ci(p.z(), grid_origin_.z(), grid_nz_);
        return (static_cast<std::size_t>(iz) * static_cast<std::size_t>(grid_ny_) +
                static_cast<std::size_t>(iy)) *
                   static_cast<std::size_t>(grid_nx_) +
               static_cast<std::size_t>(ix);
    };
    cell_start_.assign(ncells + 1, 0);
    for (const auto& s : sources_) {
        ++cell_start_[bucket(s.x) + 1];
    }
    for (std::size_t c = 0; c < ncells; ++c) {
        cell_start_[c + 1] += cell_start_[c];
    }
    items_.resize(sources_.size());
    std::vector<std::uint32_t> cursor(cell_start_.begin(), cell_start_.end() - 1);
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        items_[cursor[bucket(sources_[i].x)]++] = static_cast<std::uint32_t>(i);
    }
}

double GradedSizing::size_at(const Eigen::Vector3d& point) const {
    if (sources_.empty()) {
        return h_max_;
    }
    if (h_max_ - h_min_ <= 1e-30) {
        return h_min_;
    }
    const double inv = 1.0 / grid_cell_;
    auto ci = [&](double v, double o, std::int32_t n) {
        std::int32_t i = static_cast<std::int32_t>(std::floor((v - o) * inv));
        return i < 0 ? 0 : (i >= n ? n - 1 : i);
    };
    const std::int32_t cx = ci(point.x(), grid_origin_.x(), grid_nx_);
    const std::int32_t cy = ci(point.y(), grid_origin_.y(), grid_ny_);
    const std::int32_t cz = ci(point.z(), grid_origin_.z(), grid_nz_);
    double h = h_max_;
    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        const std::int32_t iz = cz + dz;
        if (iz < 0 || iz >= grid_nz_) {
            continue;
        }
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            const std::int32_t iy = cy + dy;
            if (iy < 0 || iy >= grid_ny_) {
                continue;
            }
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const std::int32_t ix = cx + dx;
                if (ix < 0 || ix >= grid_nx_) {
                    continue;
                }
                const std::size_t c =
                    (static_cast<std::size_t>(iz) * static_cast<std::size_t>(grid_ny_) +
                     static_cast<std::size_t>(iy)) *
                        static_cast<std::size_t>(grid_nx_) +
                    static_cast<std::size_t>(ix);
                for (std::uint32_t k = cell_start_[c]; k < cell_start_[c + 1]; ++k) {
                    const SizeSource& s = sources_[items_[k]];
                    const double cand = s.h + beta_ * (point - s.x).norm();
                    if (cand < h) {
                        h = cand;
                        if (h <= h_min_) {
                            return h_min_;
                        }
                    }
                }
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
