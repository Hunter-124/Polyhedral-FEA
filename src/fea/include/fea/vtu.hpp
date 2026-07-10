// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// VTK XML unstructured grid (.vtu) export for ParaView.
// Writes tet4 / hex8 / tet10 / hex20 connectivity with optional point data.

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <filesystem>
#include <string>
#include <vector>

namespace polymesh::fea {

struct VtuPointData {
    std::string name;
    /// One scalar per mesh node, or empty.
    std::vector<double> scalars;
    /// One 3-vector per mesh node, or empty. Size must be 3N if non-empty.
    Eigen::VectorXd vectors;
};

/// Write mesh (+ optional point data) to path. Throws FeaError on I/O failure.
void write_vtu(const std::filesystem::path& path, const NodalMesh& mesh,
               const std::vector<VtuPointData>& point_data = {});

} // namespace polymesh::fea
