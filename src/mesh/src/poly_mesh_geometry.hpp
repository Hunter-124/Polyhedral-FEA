// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Geometric admission primitives behind PolyMesh::check_geometry and
// PolyMesh::triangulate_boundary_incident_faces (mesh-private). Callers choose
// tolerances from the mesh/face scale; 2D tests run on the face's projection
// that drops its dominant area-normal axis.

#include "mesh/poly_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <vector>

namespace polymesh::mesh::detail {

struct FaceGeometry {
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    Eigen::Vector3d area = Eigen::Vector3d::Zero();
    Eigen::Vector3d min = Eigen::Vector3d::Zero();
    Eigen::Vector3d max = Eigen::Vector3d::Zero();
    double diameter = 0.0;
    int drop_axis = 2;
};

/// Centroid, area vector, bbox, diameter and projection axis of a face loop.
FaceGeometry face_geometry(const PolyMesh& mesh, const Face& face);

/// No two non-adjacent edges of the projected loop intersect.
bool polygon_is_simple(const PolyMesh& mesh, const Face& face, const FaceGeometry& geometry);

/// Ear-clipping triangulation (a triangle is returned as-is); empty when the
/// loop is not simple or no non-degenerate ear decomposition exists.
std::vector<std::array<VertexId, 3>> triangulate_simple_polygon(const PolyMesh& mesh,
                                                                const Face& face);

/// Segment a-b crosses the interior of `face`, away from the segment ends and
/// the face boundary.
bool segment_hits_face_interior(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                                const PolyMesh& mesh, const Face& face,
                                const FaceGeometry& geometry, double linear_tol);

/// Two coplanar faces share positive area.
bool coplanar_polygons_overlap(const PolyMesh& mesh, const Face& a, const FaceGeometry& ga,
                               const Face& b, const FaceGeometry& gb, double linear_tol);

/// `point` lies strictly inside `cell` (boundary contact excluded).
bool point_in_cell_strict(const Eigen::Vector3d& point, const PolyMesh& mesh, const Cell& cell,
                          CellId cell_id, const std::vector<FaceGeometry>& geometry,
                          double linear_tol);

} // namespace polymesh::mesh::detail
