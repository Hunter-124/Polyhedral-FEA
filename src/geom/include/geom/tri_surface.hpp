// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Geometry kernel interface for PolyMesh.
//
// Responsibilities of the geom module:
// - Load surface geometry (STL now; STEP/B-rep behind POLYMESH_WITH_OCC,
//   see ADR-0001).
// - Discrete feature analysis: sharp edges via dihedral angle, curvature
//   estimation, thin-wall / proximity detection (Phase P3).
//
// Units: SI throughout — coordinates in metres.

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace polymesh::geom {

/// Any failure loading or validating geometry.
class GeomError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// An indexed triangle surface, the common discrete-geometry currency.
///
/// Invariants (enforced by validate()):
/// - all vertex indices in range,
/// - no degenerate (zero-area) triangles.
struct TriSurface {
    /// Vertex coordinates in metres.
    std::vector<Eigen::Vector3d> vertices;
    /// Counter-clockwise (outward normal) vertex index triples.
    std::vector<std::array<std::uint32_t, 3>> triangles;

    /// Checks index validity and rejects degenerate triangles.
    /// Throws GeomError on the first violation.
    void validate() const;
};

} // namespace polymesh::geom
