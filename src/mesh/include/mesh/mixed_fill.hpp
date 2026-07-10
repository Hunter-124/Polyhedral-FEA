// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Multi-element hybrid lattice fill (SPEC zoo, ADR-0012 v2).
//
// Cartesian classification by distance-to-boundary (+ optional feature /
// curvature bands):
//   • bulk (deep)     → hex8
//   • skin / feature  → Kuhn 6× tet4 per cell (snap-friendly curved walls)
//
// Shared lattice faces use the same Kuhn diagonal convention on both sides
// when hex is assembled as Kuhn PL in a hybrid mesh (see fea/assembly.cpp),
// so constant-strain patch is exact with true multi-type elements.
// Optional surface snap on free-boundary lattice nodes.
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
    double h = 0.0;
    std::size_t n_hex = 0;
    std::size_t n_pyramid = 0;
    std::size_t n_tet = 0;
    double boundary_max_distance = 0.0;
    int skin_layers = 0;
    std::size_t n_feature_skin_cells = 0;
};

/// Hybrid zoo: hex bulk + Kuhn tet skin. `skin_layers` ≥ 1.
MixedFillOutput mixed_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h,
                                   int skin_layers = 2,
                                   std::span<const geom::SharpEdge> features = {},
                                   double feature_band = 0.0,
                                   std::span<const Eigen::Vector3d> curvature_seeds = {},
                                   double seed_band = 0.0, bool snap_boundary = true);

/// Optional: expand hex → 6 pyramids (legacy ADR-0013 path). Tets pass through.
MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill);

} // namespace polymesh::mesh
