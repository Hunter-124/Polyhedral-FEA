// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/sizing_field.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace polymesh::adapt {

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

} // namespace polymesh::adapt
