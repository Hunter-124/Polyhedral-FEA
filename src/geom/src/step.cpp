// SPDX-License-Identifier: BSD-3-Clause
#include "geom/step.hpp"

#include "geom/stl.hpp" // detail::weld

#include <format>

#ifdef POLYMESH_WITH_OCC

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <utility>

#endif // POLYMESH_WITH_OCC

namespace polymesh::geom {

#ifdef POLYMESH_WITH_OCC

namespace {

/// Linear deflection as a fraction of the shape bounding-box diagonal.
constexpr double kDeflectionFraction = 1e-3;
constexpr double kMinDeflection = 1e-6;    // metres
constexpr double kAngularDeflection = 0.5; // radians

double bounding_diagonal(const TopoDS_Shape& shape) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        return 0.0;
    }
    Standard_Real xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double dx = static_cast<double>(xmax - xmin);
    const double dy = static_cast<double>(ymax - ymin);
    const double dz = static_cast<double>(zmax - zmin);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Soup triangulate_shape(const TopoDS_Shape& shape) {
    const double diag = bounding_diagonal(shape);
    const double deflection =
        std::max(kMinDeflection, kDeflectionFraction * (diag > 0.0 ? diag : 1.0));

    BRepMesh_IncrementalMesh mesher(shape, deflection, Standard_False, kAngularDeflection,
                                    Standard_True);
    (void)mesher;

    Soup soup;
    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face& face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull() || tri->NbTriangles() == 0) {
            continue;
        }
        const gp_Trsf trsf = loc.Transformation();
        const bool reverse = face.Orientation() == TopAbs_REVERSED;
        const Standard_Integer ntri = tri->NbTriangles();
        for (Standard_Integer i = 1; i <= ntri; ++i) {
            Standard_Integer n1 = 0, n2 = 0, n3 = 0;
            tri->Triangle(i).Get(n1, n2, n3);
            if (reverse) {
                std::swap(n2, n3);
            }
            const gp_Pnt p1 = tri->Node(n1).Transformed(trsf);
            const gp_Pnt p2 = tri->Node(n2).Transformed(trsf);
            const gp_Pnt p3 = tri->Node(n3).Transformed(trsf);
            soup.push_back({static_cast<double>(p1.X()), static_cast<double>(p1.Y()),
                            static_cast<double>(p1.Z()), static_cast<double>(p2.X()),
                            static_cast<double>(p2.Y()), static_cast<double>(p2.Z()),
                            static_cast<double>(p3.X()), static_cast<double>(p3.Y()),
                            static_cast<double>(p3.Z())});
        }
    }
    return soup;
}

} // namespace

TriSurface load_step(const std::filesystem::path& path) {
    const std::string path_str = path.string();
    STEPControl_Reader reader;
    const IFSelect_ReturnStatus status = reader.ReadFile(path_str.c_str());
    if (status != IFSelect_RetDone) {
        throw GeomError(std::format("cannot read STEP file '{}'", path_str));
    }
    if (reader.NbRootsForTransfer() == 0) {
        throw GeomError(std::format("STEP file '{}' has no transferable roots", path_str));
    }
    const Standard_Integer ntrans = reader.TransferRoots();
    if (ntrans == 0) {
        throw GeomError(std::format("STEP file '{}': failed to transfer geometry", path_str));
    }
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull()) {
        throw GeomError(std::format("STEP file '{}' produced an empty shape", path_str));
    }

    const Soup soup = triangulate_shape(shape);
    if (soup.empty()) {
        throw GeomError(
            std::format("STEP file '{}': triangulation produced no triangles", path_str));
    }
    TriSurface surface = detail::weld(soup);
    surface.validate();
    return surface;
}

#else // !POLYMESH_WITH_OCC

TriSurface load_step(const std::filesystem::path& path) {
    (void)path;
    throw GeomError("OpenCASCADE not enabled");
}

#endif // POLYMESH_WITH_OCC

} // namespace polymesh::geom
