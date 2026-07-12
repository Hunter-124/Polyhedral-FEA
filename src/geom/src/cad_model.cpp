// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"

#include "geom/stl.hpp" // detail::weld

#include <cctype>
#include <format>
#include <utility>

#ifdef POLYMESH_WITH_OCC

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
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
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <fstream>

#endif // POLYMESH_WITH_OCC

namespace polymesh::geom {

#ifdef POLYMESH_WITH_OCC

namespace {

constexpr double kDeflectionFraction = 1e-3;
constexpr double kMinDeflection = 1e-6;
constexpr double kAngularDeflection = 0.5; // radians

Soup triangulate_shape(const TopoDS_Shape& shape, double deflection) {
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

void fill_bbox(const TopoDS_Shape& shape, Eigen::Vector3d& lo, Eigen::Vector3d& hi) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) {
        lo = Eigen::Vector3d::Zero();
        hi = Eigen::Vector3d::Zero();
        return;
    }
    Standard_Real xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    lo = Eigen::Vector3d(static_cast<double>(xmin), static_cast<double>(ymin),
                         static_cast<double>(zmin));
    hi = Eigen::Vector3d(static_cast<double>(xmax), static_cast<double>(ymax),
                         static_cast<double>(zmax));
}

} // namespace

struct CadModel::Impl {
    TopoDS_Shape shape;
};

CadModel CadModel::load_step(const std::filesystem::path& path) {
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

    CadModel model;
    model.impl_ = std::make_shared<Impl>();
    model.impl_->shape = shape;
    model.name_ = path.filename().string();
    fill_bbox(shape, model.bbox_min_, model.bbox_max_);
    return model;
}

CadModel CadModel::load_brep(const std::filesystem::path& path) {
    const std::string path_str = path.string();
    TopoDS_Shape shape;
    BRep_Builder builder;
    if (!BRepTools::Read(shape, path_str.c_str(), builder)) {
        throw GeomError(std::format("cannot read BREP file '{}'", path_str));
    }
    if (shape.IsNull()) {
        throw GeomError(std::format("BREP file '{}' produced an empty shape", path_str));
    }
    CadModel model;
    model.impl_ = std::make_shared<Impl>();
    model.impl_->shape = std::move(shape);
    model.name_ = path.filename().string();
    fill_bbox(model.impl_->shape, model.bbox_min_, model.bbox_max_);
    return model;
}

bool CadModel::empty() const noexcept {
    return !impl_ || impl_->shape.IsNull();
}

bool CadModel::has_brep() const noexcept { return !empty(); }

double CadModel::bbox_diagonal() const noexcept {
    return (bbox_max_ - bbox_min_).norm();
}

TriSurface CadModel::tessellate(double deflection) const {
    if (empty()) {
        throw GeomError("CadModel::tessellate: empty model");
    }
    double defl = deflection;
    if (defl <= 0.0) {
        const double diag = bbox_diagonal();
        defl = std::max(kMinDeflection, kDeflectionFraction * (diag > 0.0 ? diag : 1.0));
    }
    const Soup soup = triangulate_shape(impl_->shape, defl);
    if (soup.empty()) {
        throw GeomError("CadModel::tessellate: triangulation produced no triangles");
    }
    TriSurface surface = detail::weld(soup);
    surface.validate();
    return surface;
}

const void* CadModel::shape_handle() const noexcept {
    if (empty()) {
        return nullptr;
    }
    return static_cast<const void*>(&impl_->shape);
}

void CadModel::compute_bbox() {
    if (empty()) {
        bbox_min_.setZero();
        bbox_max_.setZero();
        return;
    }
    fill_bbox(impl_->shape, bbox_min_, bbox_max_);
}

#else // !POLYMESH_WITH_OCC

struct CadModel::Impl {};

CadModel CadModel::load_step(const std::filesystem::path& path) {
    (void)path;
    throw GeomError("OpenCASCADE not enabled");
}

CadModel CadModel::load_brep(const std::filesystem::path& path) {
    (void)path;
    throw GeomError("OpenCASCADE not enabled");
}

bool CadModel::empty() const noexcept { return true; }

bool CadModel::has_brep() const noexcept { return false; }

double CadModel::bbox_diagonal() const noexcept {
    return (bbox_max_ - bbox_min_).norm();
}

TriSurface CadModel::tessellate(double /*deflection*/) const {
    throw GeomError("OpenCASCADE not enabled");
}

const void* CadModel::shape_handle() const noexcept { return nullptr; }

void CadModel::compute_bbox() {}

#endif // POLYMESH_WITH_OCC

CadModel load_cad(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    std::string lower;
    lower.reserve(ext.size());
    for (char c : ext) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == ".step" || lower == ".stp") {
        return CadModel::load_step(path);
    }
    if (lower == ".brep" || lower == ".brp") {
        return CadModel::load_brep(path);
    }
    throw GeomError(std::format("load_cad: unsupported extension '{}' (use .step/.stp/.brep)",
                                ext));
}

} // namespace polymesh::geom
