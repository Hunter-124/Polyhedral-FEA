// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/loop.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace polymesh::adapt {

std::vector<Eigen::Vector3d> marked_centroids(
    std::span<const Eigen::Vector3d> element_centroids,
    const std::vector<double>& element_eta, double theta) {
    if (element_centroids.size() != element_eta.size()) {
        throw std::invalid_argument("marked_centroids: size mismatch");
    }
    const auto marked = dorfler_mark(element_eta, theta);
    std::vector<Eigen::Vector3d> seeds;
    seeds.reserve(marked.size());
    for (auto e : marked) {
        seeds.push_back(element_centroids[e]);
    }
    return seeds;
}

AdaptSuggestion suggest_uniform_refine(const std::vector<double>& element_eta,
                                       double h_current, double theta, double refine_factor,
                                       double h_min) {
    AdaptSuggestion s;
    s.n_marked = dorfler_mark(element_eta, theta).size();
    s.marked_fraction = element_eta.empty() ? 0.0
                                            : static_cast<double>(s.n_marked) /
                                                  static_cast<double>(element_eta.size());
    s.seed_band = 1.5 * h_current;
    if (s.n_marked == 0 || s.marked_fraction < 0.05) {
        s.h_next = h_current;
        return s;
    }
    s.h_next = h_current * refine_factor;
    if (h_min > 0.0) {
        s.h_next = std::max(s.h_next, h_min);
    }
    return s;
}

AdaptSuggestion suggest_refine(std::span<const Eigen::Vector3d> element_centroids,
                               const std::vector<double>& element_eta, double h_current,
                               double theta, double refine_factor, double h_min) {
    AdaptSuggestion s =
        suggest_uniform_refine(element_eta, h_current, theta, refine_factor, h_min);
    if (s.n_marked == 0 || element_centroids.empty()) {
        return s;
    }
    s.refine_seeds = marked_centroids(element_centroids, element_eta, theta);
    s.n_marked = s.refine_seeds.size();
    s.marked_fraction = static_cast<double>(s.n_marked) /
                        static_cast<double>(element_centroids.size());
    s.seed_band = 1.5 * h_current;
    return s;
}

} // namespace polymesh::adapt
