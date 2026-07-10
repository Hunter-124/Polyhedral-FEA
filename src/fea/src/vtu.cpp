// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vtu.hpp"

#include "mesh/quality.hpp"

#include <array>
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
    case ElementType::kPyramid5:
        return 14; // VTK_PYRAMID
    case ElementType::kPolyVem:
        // VTK_POLYHEDRON = 42 needs face stream; export as VTK_CONVEX_POINT_SET=41
        return 41;
    }
    return 0;
}

} // namespace

std::vector<double> tet4_cell_quality(const NodalMesh& mesh) {
    std::vector<double> out(mesh.elements.size(), 0.0);
    std::vector<std::array<std::uint32_t, 4>> tets;
    std::vector<std::size_t> map;
    tets.reserve(mesh.elements.size());
    map.reserve(mesh.elements.size());
    for (std::size_t i = 0; i < mesh.elements.size(); ++i) {
        const auto& e = mesh.elements[i];
        if (e.type == ElementType::kTet4 && e.nodes.size() == 4) {
            tets.push_back({e.nodes[0], e.nodes[1], e.nodes[2], e.nodes[3]});
            map.push_back(i);
        }
    }
    const auto aspects = mesh::tet4_aspect_ratios(mesh.nodes, tets);
    for (std::size_t k = 0; k < aspects.size(); ++k) {
        out[map[k]] = aspects[k];
    }
    return out;
}

void write_vtu(const std::filesystem::path& path, const NodalMesh& mesh,
               const std::vector<VtuPointData>& point_data,
               const std::vector<VtuCellData>& cell_data) {
    mesh.check_validity();
    std::ofstream out(path);
    if (!out) {
        throw FeaError(std::format("write_vtu: cannot open {}", path.string()));
    }

    const auto n_pts = mesh.nodes.size();
    const auto n_cells = mesh.elements.size();

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

    if (!cell_data.empty()) {
        out << "<CellData>\n";
        for (const auto& cd : cell_data) {
            if (cd.scalars.empty()) {
                continue;
            }
            if (cd.scalars.size() != n_cells) {
                throw FeaError("write_vtu: cell scalar array size mismatch");
            }
            out << "<DataArray type=\"Float64\" Name=\"" << cd.name
                << "\" format=\"ascii\">\n";
            for (double v : cd.scalars) {
                out << v << '\n';
            }
            out << "</DataArray>\n";
        }
        out << "</CellData>\n";
    }

    out << "</Piece>\n</UnstructuredGrid>\n</VTKFile>\n";
}

} // namespace polymesh::fea
