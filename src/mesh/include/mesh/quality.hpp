// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Tet quality metrics for mesher diagnostics (no fea dependency).

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <vector>

namespace polymesh::mesh {

struct TetQuality {
    double min_volume = 0.0;
    double max_volume = 0.0;
    /// Normalized volume/edge³ quality in (0,1]; 1 ≈ regular tet.
    double min_aspect = 0.0;
    double mean_aspect = 0.0;
    std::size_t n_sliver = 0;
};

std::vector<double> tet4_aspect_ratios(const std::vector<Eigen::Vector3d>& nodes,
                                       const std::vector<std::array<std::uint32_t, 4>>& tets);

TetQuality summarize_tet4_quality(const std::vector<Eigen::Vector3d>& nodes,
                                  const std::vector<std::array<std::uint32_t, 4>>& tets,
                                  double sliver_threshold = 0.05);

} // namespace polymesh::mesh
