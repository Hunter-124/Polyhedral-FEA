// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Tet quality metrics for mesher diagnostics (no fea dependency).

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
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

/// Face-matching audit for a pure tet4 volume mesh (G0 / ADR-0018).
/// Every interior triangular face must appear on exactly two tets; unpaired
/// faces are free-surface boundary (count once) or hanging-node interfaces
/// (count once but centroid deep inside the solid).
struct FaceConformityStats {
    std::size_t n_tet_faces = 0;       // 4 × n_tets
    std::size_t n_unique_faces = 0;    // distinct oriented-agnostic faces
    std::size_t n_boundary_faces = 0;  // appear once
    std::size_t n_interior_faces = 0;  // appear exactly twice
    std::size_t n_nonconforming = 0;   // appear 3+ times (should be 0)
    /// count==1 faces whose centroid is farther than margin from the AABB —
    /// classic 2:1 hanging-node signature (G0).
    std::size_t n_hanging_faces = 0;
    /// True when n_nonconforming==0 and n_hanging_faces==0 (when geometry given).
    bool is_conforming = false;
};

std::vector<double> tet4_aspect_ratios(const std::vector<Eigen::Vector3d>& nodes,
                                       const std::vector<std::array<std::uint32_t, 4>>& tets);

TetQuality summarize_tet4_quality(const std::vector<Eigen::Vector3d>& nodes,
                                  const std::vector<std::array<std::uint32_t, 4>>& tets,
                                  double sliver_threshold = 0.05);

/// Count tet faces by sorted 3-node key. Topology only (no hanging detection).
FaceConformityStats tet4_face_conformity(const std::vector<std::array<std::uint32_t, 4>>& tets);

/// Topology + geometry: flags count==1 faces whose centroid is more than
/// `surface_margin` inside the axis-aligned box [bbox_min, bbox_max] as hanging.
FaceConformityStats tet4_face_conformity(const std::vector<Eigen::Vector3d>& nodes,
                                         const std::vector<std::array<std::uint32_t, 4>>& tets,
                                         const Eigen::Vector3d& bbox_min,
                                         const Eigen::Vector3d& bbox_max,
                                         double surface_margin);

} // namespace polymesh::mesh
