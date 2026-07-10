// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/error.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace polymesh::adapt {

std::vector<std::size_t> dorfler_mark(const std::vector<double>& element_eta, double theta) {
    if (!(theta > 0.0 && theta <= 1.0)) {
        throw std::invalid_argument("dorfler_mark: theta must be in (0,1]");
    }
    const auto n = element_eta.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return element_eta[a] > element_eta[b]; });
    double total = 0.0;
    for (double e : element_eta) {
        total += e * e;
    }
    if (total <= 0.0) {
        return {};
    }
    const double target = theta * total;
    double acc = 0.0;
    std::vector<std::size_t> marked;
    for (auto i : order) {
        marked.push_back(i);
        acc += element_eta[i] * element_eta[i];
        if (acc >= target) {
            break;
        }
    }
    return marked;
}

double FeatureGradedSizing::size_at(const Eigen::Vector3d& point) const {
    const double d = geom::distance_to_features(point, *surface_, edges_);
    if (!std::isfinite(d)) {
        return h_max_;
    }
    return std::clamp(h_min_ + alpha_ * d, h_min_, h_max_);
}

} // namespace polymesh::adapt
