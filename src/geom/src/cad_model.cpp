// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"

#include "geom/stl.hpp" // detail::weld

#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <STEPControl_Reader.hxx>
#include <TopAbs.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <fstream>

#endif // POLYMESH_WITH_OCC

namespace polymesh::geom {

#ifdef POLYMESH_WITH_OCC

namespace {

// Linear sag tolerance as a fraction of the bbox diagonal. Tight enough that a
// curved wall (pipe/fillet) is represented with sub-percent chord deviation.
constexpr double kDeflectionFraction = 5e-4;
constexpr double kMinDeflection = 1e-6;
// Angular deflection (radians) between adjacent facet normals on a curved face.
// 0.2 rad ≈ 11.5° → a cylinder gets ~30 facets around, killing the coarse
// ~28° (0.5 rad) faceting on imported pipes. Curved-face fidelity comes first.
constexpr double kAngularDeflection = 0.2;

Soup triangulate_shape(const TopoDS_Shape& shape, double deflection,
                       double angular_deflection) {
    BRepMesh_IncrementalMesh mesher(shape, deflection, Standard_False, angular_deflection,
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
            // Skip zero-area facets (OCC tessellation can emit them on spheres).
            const double ax = p2.X() - p1.X(), ay = p2.Y() - p1.Y(), az = p2.Z() - p1.Z();
            const double bx = p3.X() - p1.X(), by = p3.Y() - p1.Y(), bz = p3.Z() - p1.Z();
            const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
            const double area2 = cx * cx + cy * cy + cz * cz;
            if (area2 < 1e-30) {
                continue;
            }
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

TriSurface CadModel::tessellate(double deflection, double angular_deflection) const {
    if (empty()) {
        throw GeomError("CadModel::tessellate: empty model");
    }
    double defl = deflection;
    if (defl <= 0.0) {
        const double diag = bbox_diagonal();
        defl = std::max(kMinDeflection, kDeflectionFraction * (diag > 0.0 ? diag : 1.0));
    }
    const double ang = angular_deflection > 0.0 ? angular_deflection : kAngularDeflection;
    const Soup soup = triangulate_shape(impl_->shape, defl, ang);
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

namespace {

/// Outward-ish unit normal on `face` at 3D point `q` (UV via surface project).
bool face_normal_at(const TopoDS_Face& face, const gp_Pnt& q, gp_Vec& n_out) {
    const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull()) {
        return false;
    }
    GeomAPI_ProjectPointOnSurf proj(q, surf);
    if (proj.NbPoints() < 1) {
        return false;
    }
    Standard_Real u = 0.0;
    Standard_Real v = 0.0;
    proj.LowerDistanceParameters(u, v);
    BRepAdaptor_Surface asurf(face, Standard_True);
    gp_Pnt pnt;
    gp_Vec d1u, d1v;
    asurf.D1(u, v, pnt, d1u, d1v);
    gp_Vec n = d1u.Crossed(d1v);
    if (n.SquareMagnitude() <= Precision::SquareConfusion()) {
        return false;
    }
    n.Normalize();
    if (face.Orientation() == TopAbs_REVERSED) {
        n.Reverse();
    }
    n_out = n;
    return true;
}

/// Closest point on a single face (trimmed) via BRepExtrema.
bool project_on_face(const TopoDS_Vertex& vtx, const TopoDS_Face& face, gp_Pnt& closest,
                     double& dist, gp_Vec& normal) {
    BRepExtrema_DistShapeShape dss(vtx, face);
    dss.Perform();
    if (!dss.IsDone() || dss.NbSolution() < 1) {
        // Fallback: infinite-surface project (ignores face trim).
        const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
        if (surf.IsNull()) {
            return false;
        }
        const gp_Pnt q = BRep_Tool::Pnt(vtx);
        GeomAPI_ProjectPointOnSurf proj(q, surf);
        if (proj.NbPoints() < 1) {
            return false;
        }
        closest = proj.NearestPoint();
        dist = proj.LowerDistance();
        return face_normal_at(face, closest, normal);
    }
    dist = static_cast<double>(dss.Value());
    closest = dss.PointOnShape2(1);
    if (!face_normal_at(face, closest, normal)) {
        normal = gp_Vec(0, 0, 0);
    }
    return true;
}

} // namespace

std::optional<ProjectResult> project_point_on_surface(const CadModel& model,
                                                      const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());

    BRep_Builder builder;
    TopoDS_Vertex vtx;
    builder.MakeVertex(vtx, gp_Pnt(p.x(), p.y(), p.z()), Precision::Confusion());

    double best_dist = std::numeric_limits<double>::infinity();
    gp_Pnt best_pt;
    gp_Vec best_n(0, 0, 0);
    bool found = false;

    // Per-face extrema (respects trim) — preferred over whole-shape when we
    // need a supporting face for the normal.
    for (TopExp_Explorer exp(*shape, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face& face = TopoDS::Face(exp.Current());
        gp_Pnt closest;
        double dist = 0.0;
        gp_Vec n(0, 0, 0);
        if (!project_on_face(vtx, face, closest, dist, n)) {
            continue;
        }
        if (dist < best_dist) {
            best_dist = dist;
            best_pt = closest;
            best_n = n;
            found = true;
        }
    }

    // Whole-shape fallback if face loop failed (degenerate / empty face map).
    if (!found) {
        BRepExtrema_DistShapeShape dss(vtx, *shape);
        dss.Perform();
        if (!dss.IsDone() || dss.NbSolution() < 1) {
            return std::nullopt;
        }
        best_dist = static_cast<double>(dss.Value());
        best_pt = dss.PointOnShape2(1);
        const TopoDS_Shape support = dss.SupportOnShape2(1);
        if (support.ShapeType() == TopAbs_FACE) {
            (void)face_normal_at(TopoDS::Face(support), best_pt, best_n);
        }
        found = true;
    }

    if (!found || !std::isfinite(best_dist)) {
        return std::nullopt;
    }

    ProjectResult r;
    r.point = Eigen::Vector3d(best_pt.X(), best_pt.Y(), best_pt.Z());
    if (best_n.SquareMagnitude() > Precision::SquareConfusion()) {
        best_n.Normalize();
        r.normal = Eigen::Vector3d(best_n.X(), best_n.Y(), best_n.Z());
    } else {
        // Fallback geometric normal from query → projected point.
        const Eigen::Vector3d d = r.point - p;
        const double len = d.norm();
        if (len > 1e-15) {
            r.normal = d / len;
        }
    }
    r.distance = best_dist;
    return r;
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

std::optional<ProjectResult> project_point_on_surface(const CadModel& /*model*/,
                                                      const Eigen::Vector3d& /*p*/) {
    // Stub without OCC: no BRep oracle (STL-only builds keep surface snap only).
    return std::nullopt;
}

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
