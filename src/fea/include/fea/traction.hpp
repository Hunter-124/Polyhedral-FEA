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
SurfaceTessellation tessellate_boundary_surface(const NodalMesh& mesh, int subdivisions = 6);

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

/// Ascending, de-duplicated ids of every node carried by `faces`: the boundary
/// node set of the mesh those faces came from.
std::vector<std::uint32_t> boundary_face_nodes(const std::vector<SurfaceFace>& faces);

/// Result of energy-conjugate load application over a face set.
struct ConsistentLoad {
    Eigen::VectorXd loads;                    ///< 3N nodal load vector, N
    Eigen::Vector3d resultant{0.0, 0.0, 0.0}; ///< sum of nodal loads, N
    double area = 0.0;                        ///< integrated load area, m^2
    double conservation_error = 0.0;          ///< |resultant - requested|, N
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

/// An axis-aligned box a surface load is confined to, as `--load-box` names it.
struct LoadRegion {
    Eigen::Vector3d lo;
    Eigen::Vector3d hi;
};

/// Faces whose node bounding box overlaps `region`: the candidate set for the
/// region integrators below. Deliberately a superset — a face that only grazes
/// the box contributes nothing once its quadrature is clipped, so admitting it
/// costs a few shape-function evaluations and never a wrong load, whereas the
/// `faces_within` rule of "every node inside" drops exactly the straddling
/// faces the region cuts through.
std::vector<SurfaceFace> faces_touching(const NodalMesh& mesh,
                                        const std::vector<SurfaceFace>& faces,
                                        const LoadRegion& region);

/// Boundary nodes inside `region` — what a `--fix-box` / `--load-box` selection
/// means. Half-open coordinates are allowed: pass an infinite bound to express a
/// slab.
///
/// A box selection names a region of the boundary SURFACE, so a fixture applied
/// through one constrains surface nodes; it is not a volume of material to
/// freeze. Constraining every interior node inside the box embeds a rigid
/// inclusion, and an element whose nodes all fall inside it then has identically
/// zero strain, hence identically zero stress. The union of those elements ends
/// on a one-element staircase whose height varies with the local tiling, so the
/// zero-stress region has a ragged boundary that is a property of the mesh
/// rather than of the problem. Measured on the showcase parts before this rule
/// existed, as the fully-constrained fraction of all elements: cylinder 30.7%
/// (49,660 of 161,976), sphere 6.7%, icecream_cone 6.1%, plate_hole 2.9%,
/// cantilever 1.7% — visible in every gallery render as a jagged flat-coloured
/// blob against the clamp. Restricted to the boundary, four of the five drop to
/// zero fully-constrained elements and the stress field is continuous into the
/// clamp. The cone keeps 8 of 86,512 (0.009%): its foot is a 6 mm-radius disc and
/// the corner between that disc and the wall above it is thinner than one
/// element, so those elements have all ten nodes on the boundary surface and are
/// strain-free by geometry rather than by selection. Any fixture patch enclosing
/// a region thinner than an element does that, and resolution is the only fix.
std::vector<std::uint32_t> boundary_nodes_within(const NodalMesh& mesh,
                                                 const std::vector<SurfaceFace>& faces,
                                                 const LoadRegion& region);

/// Area of the part of `faces` that lies inside `region`, m^2. Faces wholly
/// inside contribute exactly what `integrated_face_area` gives them; faces the
/// region cuts contribute their clipped part.
double integrated_region_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                              const LoadRegion& region);

/// `consistent_face_load` restricted to the part of `faces` inside `region`.
///
/// A box selection names a REGION of the boundary surface, not a set of faces.
/// Accepting whole faces stops the loaded patch on a staircase of element edges
/// instead of on the box plane, which makes the applied traction a function of
/// the tiling: on the showcase sphere at h = 8 mm with `--load-box z >= 40 mm`
/// the accepted-face patch edge wandered by 1.06 mm rms and 4.63 mm
/// peak-to-peak in z (0.28 h and 1.20 h) and under-covered the cap by 6.2% of
/// its area. Clipping the quadrature to the region removes that dependence: the
/// patch ends on the plane, and the resultant is still exactly `total_force`.
ConsistentLoad consistent_region_load(const NodalMesh& mesh,
                                      const std::vector<SurfaceFace>& faces,
                                      const LoadRegion& region,
                                      const Eigen::Vector3d& total_force);

} // namespace polymesh::fea
