// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Consistent surface-traction loads: f = integral of N^T t dS over boundary
// faces, with 2D shape functions on the face parameter domain.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace polymesh::fea {

/// Boundary face types matching the volume element zoo: tri3/tri6 bound
/// tets, quad4/quad8 bound hexes.
enum class FaceType : std::uint8_t { kTri3, kTri6, kQuad4, kQuad8 };

constexpr int face_num_nodes(FaceType type) {
    switch (type) {
    case FaceType::kTri3:
        return 3;
    case FaceType::kTri6:
        return 6;
    case FaceType::kQuad4:
        return 4;
    case FaceType::kQuad8:
        return 8;
    }
    return 0; // unreachable
}

/// A boundary face: corner nodes counter-clockwise (viewed from outside),
/// then mid-edge nodes for quadratic faces — tri6 edges (0,1),(1,2),(0,2);
/// quad8 edges (0,1),(1,2),(2,3),(3,0).
struct SurfaceFace {
    FaceType type = FaceType::kTri3;
    std::vector<std::uint32_t> nodes;
};
struct SurfaceSample {
    Eigen::Vector3d position = Eigen::Vector3d::Zero();
    std::array<std::uint32_t, 8> source_nodes{};
    std::array<double, 8> weights{};
    std::uint8_t count = 0;
};

struct SurfaceTessellation {
    std::vector<SurfaceSample> samples;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

/// Tessellate the actual isoparametric free surface, not its corner chords.
/// Samples retain nodal interpolation weights so result fields and deformation
/// use the same quadratic geometry solved by the volume elements.
SurfaceTessellation tessellate_boundary_surface(const NodalMesh& mesh,
                                                int subdivisions = 6);

/// Traction field t(x), N/m^2, evaluated at a physical surface point.
using Traction = std::function<Eigen::Vector3d(const Eigen::Vector3d&)>;

/// Consistent nodal load vector for a traction applied over `faces`,
/// size 3N, newtons. The traction vector is applied as given (its direction
/// does not depend on face orientation; only the area measure is used).
Eigen::VectorXd assemble_traction_load(const NodalMesh& mesh,
                                       const std::vector<SurfaceFace>& faces,
                                       const Traction& traction);

/// Free-surface faces of `mesh` as `SurfaceFace` records ready for
/// `assemble_traction_load`: tri3/quad4 corner topology, upgraded to
/// tri6/quad8 when the mesh is quadratic and every edge of the face carries a
/// mid-edge node — so quadratic meshes get the correct midside load split
/// instead of a corner-only lump.
std::vector<SurfaceFace> boundary_surface_faces(const NodalMesh& mesh);

/// Unit normal of a face from its corner loop winding (outward for the
/// windings produced by `boundary_surface_faces`); zero if degenerate.
Eigen::Vector3d surface_face_normal(const NodalMesh& mesh, const SurfaceFace& face);

/// Total area of `faces`, m^2, using the same surface quadrature as
/// `assemble_traction_load` — so a uniform traction of t over `faces` has
/// resultant exactly t * this area.
double integrated_face_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces);

/// Subset of `faces` whose every node lies in `nodes`: the face set implied by
/// a nodal selection (box, slab, or surface region). Order is preserved.
std::vector<SurfaceFace> faces_within(const std::vector<SurfaceFace>& faces,
                                      std::span<const std::uint32_t> nodes);

/// Result of energy-conjugate load application over a face set.
struct ConsistentLoad {
    Eigen::VectorXd loads;                     ///< 3N nodal load vector, N
    Eigen::Vector3d resultant{0.0, 0.0, 0.0};  ///< sum of nodal loads, N
    double area = 0.0;                         ///< integrated load area, m^2
    double conservation_error = 0.0;            ///< |resultant - requested|, N
};

/// Consistent (energy-conjugate) nodal loads for a uniform traction over
/// `faces` whose resultant is exactly `total_force` newtons: t =
/// total_force / area, f = integral of N^T t dS, then rescaled so the nodal
/// sum matches `total_force` to round-off. A face set of zero area yields a
/// zero load with `area == 0` so callers can report the degeneracy instead of
/// silently falling back to mesh-density-dependent even splitting.
ConsistentLoad consistent_face_load(const NodalMesh& mesh,
                                    const std::vector<SurfaceFace>& faces,
                                    const Eigen::Vector3d& total_force);

} // namespace polymesh::fea
