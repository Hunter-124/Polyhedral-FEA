// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// VTK XML unstructured grid (.vtu) export for ParaView.
// Writes tet4 / hex8 / tet10 / hex20 connectivity with optional point/cell data.
// Node coordinates are metres (SI). Point/cell array units are caller-defined;
// product path uses: displacement (m), von_mises (Pa), quality (dimensionless).

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <filesystem>
#include <string>
#include <vector>

namespace polymesh::fea {

struct VtuPointData {
    std::string name;
    /// One scalar per mesh node, or empty (units depend on field, e.g. Pa or m).
    std::vector<double> scalars;
    /// One 3-vector per mesh node, or empty. Size must be 3N if non-empty
    /// (e.g. displacement in metres).
    Eigen::VectorXd vectors;
};

struct VtuCellData {
    std::string name;
    /// One scalar per mesh cell (element). Size must equal mesh.elements.size().
    std::vector<double> scalars;
};

/// Write mesh (+ optional point/cell data) to path. Throws FeaError on I/O failure.
/// Mesh node coordinates are metres. Existing callers that pass only point_data
/// keep working (cell_data defaults empty).
void write_vtu(const std::filesystem::path& path, const NodalMesh& mesh,
               const std::vector<VtuPointData>& point_data = {},
               const std::vector<VtuCellData>& cell_data = {});

/// Per-element tet4 aspect quality in (0,1] (regular ≈ 1). Non-tet4 cells are 0.
std::vector<double> tet4_cell_quality(const NodalMesh& mesh);

} // namespace polymesh::fea
