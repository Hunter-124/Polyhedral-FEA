// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vtu.hpp"

#include "mesh/quality.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <iomanip>
#include <vector>

namespace polymesh::fea {
namespace {

constexpr int kVtkWedge = 13;
constexpr int kVtkConvexPointSet = 41;
constexpr int kVtkPolyhedron = 42;

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
        return kVtkWedge;
    case ElementType::kPyramid5:
        return 14; // VTK_PYRAMID
    case ElementType::kPolyVem:
        return kVtkPolyhedron; // carries a faces/faceoffsets stream
    }
    return 0;
}

/// vtkWedge winds the base triangle so its right-hand normal points AWAY from
/// the opposite triangle. PolyMesh's kPrism6 puts base (0,1,2) at ζ = −1, whose
/// right-hand normal points at the top face (3,4,5) — the opposite convention,
/// which made every exported prism inside-out (negative signed cell volume,
/// inverted surface normals in every viewer). Reversing the winding of both
/// triangles is export-only: the mesher's emission order and therefore FE
/// assembly and `reference_nodes(kPrism6)` are untouched.
constexpr std::array<std::size_t, 6> kPrism6ToVtkWedge{0, 2, 1, 3, 5, 4};

/// Face loops of a polyhedral cell with at least 3 distinct corners.
std::vector<const std::vector<std::uint32_t>*> usable_faces(const NodalElement& e) {
    std::vector<const std::vector<std::uint32_t>*> out;
    out.reserve(e.faces.size());
    for (const auto& f : e.faces) {
        if (f.size() >= 3) {
            out.push_back(&f);
        }
    }
    return out;
}

/// Local node indices the face stream references, in first-appearance order.
/// k=2 VEM cells keep mid-edge nodes in `nodes` that no face loop mentions
/// (loops index the vertex block only) and VTK_POLYHEDRON has no quadratic
/// variant, so the exported cell is the linear hull of the same geometry.
std::vector<std::uint32_t>
poly_face_locals(const NodalElement& e,
                 const std::vector<const std::vector<std::uint32_t>*>& faces) {
    std::vector<char> seen(e.nodes.size(), 0);
    std::vector<std::uint32_t> out;
    out.reserve(e.nodes.size());
    for (const auto* f : faces) {
        for (const auto li : *f) {
            if (!seen[li]) {
                seen[li] = 1;
                out.push_back(li);
            }
        }
    }
    return out;
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
    // Float64 ASCII must carry enough significant digits to round-trip node
    // coordinates. The stream default (6) can flatten valid boundary cells on
    // export and make independent signed-volume checks report zero.
    out << std::setprecision(17);

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

    // Built up-front: a polyhedral cell's connectivity length is not
    // nodes.size(), and its faces go into a separate VTK face stream.
    std::vector<std::uint32_t> connectivity;
    std::vector<std::int32_t> offsets;
    std::vector<int> types;
    std::vector<std::int64_t> face_stream;
    std::vector<std::int64_t> face_offsets; // one per cell, −1 when not polyhedral
    connectivity.reserve(8 * n_cells);
    offsets.reserve(n_cells);
    types.reserve(n_cells);
    face_offsets.reserve(n_cells);
    bool any_polyhedron = false;

    for (const auto& e : mesh.elements) {
        int type = vtk_cell_type(e.type);
        std::int64_t face_offset = -1;
        if (e.type == ElementType::kPolyVem) {
            const auto faces = usable_faces(e);
            if (faces.size() >= 4) {
                // [numFaces, (numPointsInFace, global point ids...) × numFaces]
                face_stream.push_back(static_cast<std::int64_t>(faces.size()));
                for (const auto* f : faces) {
                    face_stream.push_back(static_cast<std::int64_t>(f->size()));
                    for (const auto li : *f) {
                        face_stream.push_back(static_cast<std::int64_t>(e.nodes[li]));
                    }
                }
                face_offset = static_cast<std::int64_t>(face_stream.size());
                any_polyhedron = true;
                for (const auto li : poly_face_locals(e, faces)) {
                    connectivity.push_back(e.nodes[li]);
                }
            } else {
                // No closed face stream to describe: fall back to a point set so
                // the cell still renders as its convex hull.
                type = kVtkConvexPointSet;
                connectivity.insert(connectivity.end(), e.nodes.begin(), e.nodes.end());
            }
        } else if (e.type == ElementType::kPrism6 && e.nodes.size() == 6) {
            for (const auto k : kPrism6ToVtkWedge) {
                connectivity.push_back(e.nodes[k]);
            }
        } else {
            connectivity.insert(connectivity.end(), e.nodes.begin(), e.nodes.end());
        }
        offsets.push_back(static_cast<std::int32_t>(connectivity.size()));
        types.push_back(type);
        face_offsets.push_back(face_offset);
    }

    out << "<Cells>\n"
           "<DataArray type=\"Int32\" Name=\"connectivity\" format=\"ascii\">\n";
    std::size_t written = 0;
    for (std::size_t c = 0; c < n_cells; ++c) {
        const auto end = static_cast<std::size_t>(offsets[c]);
        for (; written < end; ++written) {
            out << connectivity[written] << (written + 1 == end ? '\n' : ' ');
        }
    }
    out << "</DataArray>\n"
           "<DataArray type=\"Int32\" Name=\"offsets\" format=\"ascii\">\n";
    for (const auto o : offsets) {
        out << o << '\n';
    }
    out << "</DataArray>\n"
           "<DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
    for (const auto t : types) {
        out << t << '\n';
    }
    out << "</DataArray>\n";
    if (any_polyhedron) {
        out << "<DataArray type=\"Int64\" Name=\"faces\" format=\"ascii\">\n";
        for (const auto v : face_stream) {
            out << v << '\n';
        }
        out << "</DataArray>\n"
               "<DataArray type=\"Int64\" Name=\"faceoffsets\" format=\"ascii\">\n";
        for (const auto v : face_offsets) {
            out << v << '\n';
        }
        out << "</DataArray>\n";
    }
    out << "</Cells>\n";

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
