// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Adaptive remesh suggestions from ZZ indicators (P5).
// Uniform h shrink for all meshers; Dörfler centroids as local refine seeds
// for graded Cartesian fill.

#include "adapt/error.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::adapt {

struct AdaptSuggestion {
    /// Suggested uniform h for the next mesh (metres).
    double h_next = 0.0;
    /// Fraction of elements marked.
    double marked_fraction = 0.0;
    std::size_t n_marked = 0;
    /// Centroids of Dörfler-marked elements (metres) for local re-meshing.
    std::vector<Eigen::Vector3d> refine_seeds;
    /// Suggested ball radius around each seed for graded fine blocks (metres).
    double seed_band = 0.0;
};

/// Centroids of Dörfler-marked elements. `element_centroids` must align with
/// `element_eta` (one entry per mesh element).
std::vector<Eigen::Vector3d> marked_centroids(
    std::span<const Eigen::Vector3d> element_centroids,
    const std::vector<double>& element_eta, double theta = 0.3);

/// From current element η and current h, produce refined uniform size.
/// Marked fraction / counts come from Dörfler; seeds are empty.
AdaptSuggestion suggest_uniform_refine(const std::vector<double>& element_eta,
                                       double h_current, double theta = 0.3,
                                       double refine_factor = 0.7, double h_min = 0.0);

/// Uniform h suggestion plus Dörfler seed locations for local graded remesh.
AdaptSuggestion suggest_refine(std::span<const Eigen::Vector3d> element_centroids,
                               const std::vector<double>& element_eta, double h_current,
                               double theta = 0.3, double refine_factor = 0.7,
                               double h_min = 0.0);

} // namespace polymesh::adapt
