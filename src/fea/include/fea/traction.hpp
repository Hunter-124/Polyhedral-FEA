// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Consistent surface-traction loads: f = integral of N^T t dS over boundary
// faces, with 2D shape functions on the face parameter domain.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <functional>
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

/// Traction field t(x), N/m^2, evaluated at a physical surface point.
using Traction = std::function<Eigen::Vector3d(const Eigen::Vector3d&)>;

/// Consistent nodal load vector for a traction applied over `faces`,
/// size 3N, newtons. The traction vector is applied as given (its direction
/// does not depend on face orientation; only the area measure is used).
Eigen::VectorXd assemble_traction_load(const NodalMesh& mesh,
                                       const std::vector<SurfaceFace>& faces,
                                       const Traction& traction);

} // namespace polymesh::fea
