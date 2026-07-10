// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Face-based polyhedral mesh (ADR-0004): the mesh is a list of polygonal
// faces, each owned by one cell and optionally shared with a neighbour cell.
// Any convex or non-convex polyhedron is representable, and cell adjacency
// (needed for assembly and error-estimation patches) is a direct scan of
// interior faces.
//
// Units: coordinates in metres.

#include <Eigen/Core>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace polymesh::mesh {

using VertexId = std::uint32_t;
using FaceId = std::uint32_t;
using CellId = std::uint32_t;

/// Element shape family of a cell — drives which formulation fea uses
/// (isoparametric FEM for the standard zoo, VEM for kPolyhedron; ADR-0003).
enum class CellKind : std::uint8_t { kTet, kHex, kPrism, kPyramid, kPolyhedron };

/// A polygonal face: ordered vertex loop, owner cell, optional neighbour.
///
/// Orientation convention (OpenFOAM-style): the vertex loop is ordered so the
/// face normal points *out of the owner cell* (toward the neighbour, or out
/// of the domain for boundary faces).
struct Face {
    std::vector<VertexId> vertices;
    CellId owner = 0;
    /// Empty for boundary faces.
    std::optional<CellId> neighbour;
};

struct Cell {
    CellKind kind = CellKind::kPolyhedron;
    std::vector<FaceId> faces;
};

/// Structural validity violation; message identifies the offending entity.
class ValidityError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// The mesh: flat arrays of vertices, faces, cells.
struct PolyMesh {
    std::vector<Eigen::Vector3d> vertices;
    std::vector<Face> faces;
    std::vector<Cell> cells;

    /// Structural validity: index ranges, face/cell cross-references, minimum
    /// topology. Throws ValidityError on the first violation.
    void check_validity() const;

    /// Geometric validity (P2): manifold boundary edges appear once, interior
    /// face pairs match, tet cells have positive volume. Throws ValidityError.
    void check_geometry() const;
};

} // namespace polymesh::mesh
