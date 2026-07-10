// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vtu.hpp"

#include <cstdio>
#include <format>
#include <fstream>

namespace polymesh::fea {
namespace {

int vtk_cell_type(ElementType t) {
    switch (t) {
    case ElementType::kTet4:
        return 10;
    case ElementType::kHex8:
        return 12;
    case ElementType::kTet10:
        return 24;
    case ElementType::kHex20:
        return 25;
    case ElementType::kPrism6:
        return 13; // VTK_WEDGE
    case ElementType::kPolyVem:
        // VTK_POLYHEDRON = 42 needs face stream; export as VTK_CONVEX_POINT_SET=41
        return 41;
    }
    return 0;
}

} // namespace

void write_vtu(const std::filesystem::path& path, const NodalMesh& mesh,
               const std::vector<VtuPointData>& point_data) {
    mesh.check_validity();
    std::ofstream out(path);
    if (!out) {
        throw FeaError(std::format("write_vtu: cannot open {}", path.string()));
    }

    const auto n_pts = mesh.nodes.size();
    const auto n_cells = mesh.elements.size();
    std::size_t connectivity_len = 0;
    for (const auto& e : mesh.elements) {
        connectivity_len += e.nodes.size();
    }

    out << R"(<?xml version="1.0"?>
<VTKFile type="UnstructuredGrid" version="0.1" byte_order="LittleEndian">
<UnstructuredGrid>
<Piece NumberOfPoints=")"
        << n_pts << R"(" NumberOfCells=")" << n_cells << "\">\n";

    out << "<Points>\n"
           "<DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"ascii\">\n";
    for (const auto& p : mesh.nodes) {
        out << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    }
    out << "</DataArray>\n</Points>\n";

    out << "<Cells>\n"
           "<DataArray type=\"Int32\" Name=\"connectivity\" format=\"ascii\">\n";
    for (const auto& e : mesh.elements) {
        for (std::size_t i = 0; i < e.nodes.size(); ++i) {
            out << e.nodes[i] << (i + 1 == e.nodes.size() ? '\n' : ' ');
        }
    }
    out << "</DataArray>\n"
           "<DataArray type=\"Int32\" Name=\"offsets\" format=\"ascii\">\n";
    int off = 0;
    for (const auto& e : mesh.elements) {
        off += static_cast<int>(e.nodes.size());
        out << off << '\n';
    }
    out << "</DataArray>\n"
           "<DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
    for (const auto& e : mesh.elements) {
        out << vtk_cell_type(e.type) << '\n';
    }
    out << "</DataArray>\n</Cells>\n";

    if (!point_data.empty()) {
        out << "<PointData>\n";
        for (const auto& pd : point_data) {
            if (!pd.scalars.empty()) {
                if (pd.scalars.size() != n_pts) {
                    throw FeaError("write_vtu: scalar array size mismatch");
                }
                out << "<DataArray type=\"Float64\" Name=\"" << pd.name
                    << "\" format=\"ascii\">\n";
                for (double v : pd.scalars) {
                    out << v << '\n';
                }
                out << "</DataArray>\n";
            }
            if (pd.vectors.size() != 0) {
                if (static_cast<std::size_t>(pd.vectors.size()) != 3 * n_pts) {
                    throw FeaError("write_vtu: vector array size mismatch");
                }
                out << "<DataArray type=\"Float64\" Name=\"" << pd.name
                    << "\" NumberOfComponents=\"3\" format=\"ascii\">\n";
                for (Eigen::Index i = 0; i < pd.vectors.size(); i += 3) {
                    out << pd.vectors[i] << ' ' << pd.vectors[i + 1] << ' '
                        << pd.vectors[i + 2] << '\n';
                }
                out << "</DataArray>\n";
            }
        }
        out << "</PointData>\n";
    }

    out << "</Piece>\n</UnstructuredGrid>\n</VTKFile>\n";
    (void)connectivity_len;
}

} // namespace polymesh::fea
