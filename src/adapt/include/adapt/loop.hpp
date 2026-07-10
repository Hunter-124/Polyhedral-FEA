// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// One-pass adaptive sizing update from ZZ indicators (P5 scaffold).
// Marks elements, shrinks local size targets, returns a uniform mesh size
// suggestion for the next tet_fill (full local remesh comes later).

#include "adapt/error.hpp"

#include <cstddef>
#include <vector>

namespace polymesh::adapt {

struct AdaptSuggestion {
    /// Suggested uniform h for the next mesh (metres).
    double h_next = 0.0;
    /// Fraction of elements marked.
    double marked_fraction = 0.0;
    std::size_t n_marked = 0;
};

/// From current element η and current h, produce a refined uniform size.
/// marked elements drive h_next = h_current * refine_factor (default 0.7).
AdaptSuggestion suggest_uniform_refine(const std::vector<double>& element_eta,
                                       double h_current, double theta = 0.3,
                                       double refine_factor = 0.7, double h_min = 0.0);

} // namespace polymesh::adapt
