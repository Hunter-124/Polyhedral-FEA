// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: boundary skin at h/2, deep interior at h (conforming).
//
// True hex-core + tet-skin needs pyramid (or mortar) transitions — pure
// hex/tet face pairs are non-conforming (quad vs two tris). Until pyramids
// land, "hybrid" means *graded all-tet*: Cartesian lattice at spacing h,
// cells on the free surface (and `skin_layers` of neighbors) are refined
// into 2×2×2 Kuhn tets at h/2; deeper cells use one Kuhn split at h.
// That is conforming, deterministic, and feature-friendly.

#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

namespace polymesh::mesh {

struct GradedTetFillOutput {
    TetFillOutput mesh; // nodes + tets + boundary quads at the fine lattice
    double h_coarse = 0.0;
    double h_fine = 0.0;
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0;
    int skin_layers = 0;
};

/// `skin_layers` ≥ 1. Coarse spacing is `h`; fine is `h/2` in the skin.
GradedTetFillOutput graded_tet_fill_surface(const geom::TriSurface& surface,
                                            const Eigen::Vector3d& bbox_min,
                                            const Eigen::Vector3d& bbox_max, double h,
                                            int skin_layers = 2);

} // namespace polymesh::mesh
