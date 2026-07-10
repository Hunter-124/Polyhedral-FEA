// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Discrete geometry indicators for a-priori sizing (ROADMAP C2).
//
// Curvature — dihedral / 1-ring mean-curvature proxy at vertices (1/m).
// Thin-wall — inward ray-cast thickness proxy on closed manifold meshes (m).
//
// Heuristic limits (documented for consumers):
// - Curvature is a discrete proxy, not principal curvatures from a CAD kernel.
//   Cylinders/fillets register (mean); pure Gaussian spikes (cones) also rise.
//   Mesh noise and skinny triangles inflate kappa; prefer watertight, moderate
//   aspect-ratio STL for stable fields.
// - Thickness assumes outward CCW normals and a closed (or nearly closed)
//   surface. Open shells, self-intersections, and non-manifold fans yield
//   +infinity at affected vertices. Rays skip triangles incident to the
//   source vertex; very coarse tessellation underestimates local walls.
// - Both are O(V·T) worst-case for thickness and O(E) for curvature — fine for
//   product STL sizes in this repo, not a medial-axis package.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace polymesh::geom {

/// Per-vertex mean-curvature magnitude proxy, units 1/m (SI).
/// Index-aligned with `surface.vertices`. Zero on perfectly flat 1-rings.
struct VertexCurvature {
    std::vector<double> kappa; // 1/m, always ≥ 0
};

/// Per-vertex local thickness proxy, metres. `+infinity` where no opposite
/// hit was found (open surface, outward ray only, or degenerate normal).
struct VertexThickness {
    std::vector<double> thickness; // m
};

/// Discrete mean-curvature proxy: for each interior edge, turning angle
/// θ = acos(n0·n1) contributes θ·‖e‖ to both endpoints; divide by 4·A_1ring
/// (Meyer-style scaling). Boundary edges contribute 0 turning.
VertexCurvature estimate_vertex_curvature(const TriSurface& surface);

/// Inward normal ray from each vertex; first hit distance = local thickness.
/// `eps_scale` multiplies a length scale from mean edge length for the inward
/// origin offset (default 1e-4). Hits with t smaller than that offset are ignored.
VertexThickness estimate_local_thickness(const TriSurface& surface, double eps_scale = 1e-4);

/// Index of nearest surface vertex to `p` (linear scan). Empty surface → 0.
std::uint32_t nearest_vertex_index(const TriSurface& surface, const Eigen::Vector3d& p);

/// Euclidean distance to nearest vertex (metres). Empty → +infinity.
double distance_to_surface_vertices(const TriSurface& surface, const Eigen::Vector3d& p);

/// Convenience: true when thickness is a usable finite positive value.
inline bool has_finite_thickness(double t) {
    return std::isfinite(t) && t > 0.0 && t < std::numeric_limits<double>::max() * 0.25;
}

} // namespace polymesh::geom
