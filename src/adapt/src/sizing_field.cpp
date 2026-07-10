// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/sizing_field.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace polymesh::adapt {
namespace {

double clamp_size(double h, double h_min, double h_max) { return std::clamp(h, h_min, h_max); }

double blend_to_max(double h_local, double h_max, double dist, double blend) {
    if (!(dist > 0.0) || !std::isfinite(dist)) {
        return h_local;
    }
    if (dist >= blend) {
        return h_max;
    }
    const double t = dist / blend;
    return h_local + t * (h_max - h_local);
}

} // namespace

FeatureSizing::FeatureSizing(double h_min, double h_max, double blend_distance,
                             DistanceFn distance_fn)
    : h_min_(h_min), h_max_(h_max), blend_(blend_distance), dist_(std::move(distance_fn)) {
    if (!(h_min_ > 0.0) || !(h_max_ >= h_min_) || !(blend_ > 0.0) || !dist_) {
        throw std::invalid_argument("FeatureSizing: need 0 < h_min <= h_max and "
                                    "blend_distance > 0 with a distance fn");
    }
}

double FeatureSizing::size_at(const Eigen::Vector3d& point) const {
    const double d = dist_(point);
    if (!(d > 0.0) || !std::isfinite(d)) {
        return h_min_;
    }
    if (d >= blend_) {
        return h_max_;
    }
    const double t = d / blend_;
    return h_min_ + t * (h_max_ - h_min_);
}

std::unique_ptr<SizingField> make_feature_sizing(double h_min, double h_max,
                                                 double blend_distance,
                                                 const geom::TriSurface& surface,
                                                 const std::vector<geom::SharpEdge>& edges) {
    // Capture surface/edges by value into the distance lambda so the field is self-contained.
    return std::make_unique<FeatureSizing>(
        h_min, h_max, blend_distance, [surface, edges](const Eigen::Vector3d& p) {
            return geom::distance_to_features(p, surface, edges);
        });
}

GeometrySizing::GeometrySizing(double h_min, double h_max, double blend_distance,
                               geom::TriSurface surface, std::vector<geom::SharpEdge> edges,
                               double curvature_fraction, double thickness_fraction)
    : h_min_(h_min), h_max_(h_max), blend_(blend_distance), curv_frac_(curvature_fraction),
      thick_frac_(thickness_fraction), surface_(std::move(surface)), edges_(std::move(edges)) {
    if (!(h_min_ > 0.0) || !(h_max_ >= h_min_) || !(blend_ > 0.0)) {
        throw std::invalid_argument(
            "GeometrySizing: need 0 < h_min <= h_max and blend_distance > 0");
    }
    if (!(curv_frac_ > 0.0) || !(thick_frac_ > 0.0)) {
        throw std::invalid_argument(
            "GeometrySizing: curvature/thickness fractions must be > 0");
    }
    surface_.validate();
    const auto curv = geom::estimate_vertex_curvature(surface_);
    const auto thick = geom::estimate_local_thickness(surface_);
    kappa_ = std::move(curv.kappa);
    thickness_ = std::move(thick.thickness);
}

double GeometrySizing::size_at(const Eigen::Vector3d& point) const {
    if (surface_.vertices.empty()) {
        return h_max_;
    }

    const auto idx = geom::nearest_vertex_index(surface_, point);
    const double d_surf = (surface_.vertices[idx] - point).norm();

    // Local geometric target from nearest vertex attributes.
    double h_local = h_max_;
    const double kappa = (idx < kappa_.size()) ? kappa_[idx] : 0.0;
    if (kappa > 1e-12) {
        // h ≈ c / κ  (κ ~ |H| ≈ 1/R ⇒ h ≈ c·R).
        h_local = std::min(h_local, clamp_size(curv_frac_ / kappa, h_min_, h_max_));
    }
    if (idx < thickness_.size() && geom::has_finite_thickness(thickness_[idx])) {
        h_local = std::min(h_local, clamp_size(thick_frac_ * thickness_[idx], h_min_, h_max_));
    }

    // Relax surface-based target toward h_max away from the wall.
    double h = blend_to_max(h_local, h_max_, d_surf, blend_);

    // Sharp-edge crease field (independent distance-to-edge blend).
    if (!edges_.empty()) {
        const double d_feat = geom::distance_to_features(point, surface_, edges_);
        double h_feat = h_max_;
        if (!(d_feat > 0.0) || !std::isfinite(d_feat)) {
            h_feat = h_min_;
        } else if (d_feat < blend_) {
            const double t = d_feat / blend_;
            h_feat = h_min_ + t * (h_max_ - h_min_);
        }
        h = std::min(h, h_feat);
    }

    return clamp_size(h, h_min_, h_max_);
}

std::unique_ptr<SizingField> make_geometry_sizing(double h_min, double h_max,
                                                  double blend_distance,
                                                  const geom::TriSurface& surface,
                                                  const std::vector<geom::SharpEdge>& edges,
                                                  double curvature_fraction,
                                                  double thickness_fraction) {
    return std::make_unique<GeometrySizing>(h_min, h_max, blend_distance, surface, edges,
                                            curvature_fraction, thickness_fraction);
}

} // namespace polymesh::adapt
