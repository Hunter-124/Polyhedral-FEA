// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Multi-element hybrid lattice fill (SPEC zoo, ADR-0012 v3 / H2).
//
// Cartesian classification by distance-to-boundary (+ optional feature /
// curvature bands):
//   • bulk (deep)          → hex8 at h
//   • feature / seed fine  → 2×2×2 hex8 at h/2 (true size adaptivity)
//   • 2:1 interface        → pyramid5 transition (mid-face nodes, no hanging)
//   • plain free-surface   → pyramid5 skin at h (when no geo drivers)
//
// Product FE path expands remaining hex → pyramids (ADR-0013) so the solve
// mesh is all-pyramid with matching face diagonals (constant-strain exact).
// Free-surface bases stay quads (no Kuhn diagonals on the silhouette).
// NOT Delaunay / CAD-fitted (ADR-0015).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::mesh {

enum class MixedCellKind : std::uint8_t { kHex8 = 0, kPyramid5 = 1, kTet4 = 2 };

struct MixedCell {
    MixedCellKind kind = MixedCellKind::kHex8;
    std::array<std::uint32_t, 8> nodes{};
    std::uint8_t n_nodes = 8;
};

struct MixedFillOutput {
    std::vector<Eigen::Vector3d> nodes; // metres
    std::vector<MixedCell> cells;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0;       // bulk cell edge (metres)
    double h_fine = 0.0;  // fine cell edge when 2:1 active (≈ h/2), else = h
    std::size_t n_hex = 0;
    std::size_t n_pyramid = 0;
    std::size_t n_tet = 0;
    double boundary_max_distance = 0.0;
    int skin_layers = 0;
    std::size_t n_feature_skin_cells = 0;
    std::size_t n_fine_cells = 0;       // coarse cells refined to 2×2×2
    std::size_t n_transition_cells = 0; // 2:1 interface pyramid cells
};

/// Hybrid zoo: hex bulk + optional 2:1 feature fine + pyramid skin/transition.
/// `skin_layers` ≥ 1 (used only when no feature/seed drivers).
MixedFillOutput mixed_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h,
                                   int skin_layers = 2,
                                   std::span<const geom::SharpEdge> features = {},
                                   double feature_band = 0.0,
                                   std::span<const Eigen::Vector3d> curvature_seeds = {},
                                   double seed_band = 0.0, bool snap_boundary = true);

/// Expand every hex8 → 6 pyramid5 (centroid apex). Pyramids/tets pass through.
/// Product FE path for hybrid / hexpyr (constant-strain exact).
MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill);

} // namespace polymesh::mesh
