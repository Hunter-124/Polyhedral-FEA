// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"

#include "geom/stl.hpp" // detail::weld

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <STEPControl_Reader.hxx>
#include <TopAbs.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>

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
            const double cx = ay * bz - az * by, cy = az * bx - ax * bz,
                         cz = ax * by - ay * bx;
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

bool CadModel::empty() const noexcept { return !impl_ || impl_->shape.IsNull(); }

bool CadModel::has_brep() const noexcept { return !empty(); }

double CadModel::bbox_diagonal() const noexcept { return (bbox_max_ - bbox_min_).norm(); }

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

/// Conservative axis-aligned bound of one BRep face. Culls faces that cannot
/// hold the closest point before paying for an exact extrema solve.
struct FaceBox {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
};

/// Exact lower bound on |p - q| over every q inside `b` (0 when p is inside).
double box_lower_bound(const Eigen::Vector3d& p, const FaceBox& b) {
    const Eigen::Vector3d d = (b.lo - p).cwiseMax(p - b.hi).cwiseMax(Eigen::Vector3d::Zero());
    return d.norm();
}

/// Outward-ish unit normal at 3D point `q` from a face's surface + orientation
/// (UV via surface project). Split from the face so the cached index can hand
/// over a pre-built surface handle and adaptor.
bool face_normal_at(const Handle(Geom_Surface) & surf, const BRepAdaptor_Surface& asurf,
                    TopAbs_Orientation orientation, const gp_Pnt& q, gp_Vec& n_out) {
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
    gp_Pnt pnt;
    gp_Vec d1u, d1v;
    asurf.D1(u, v, pnt, d1u, d1v);
    gp_Vec n = d1u.Crossed(d1v);
    if (n.SquareMagnitude() <= Precision::SquareConfusion()) {
        return false;
    }
    n.Normalize();
    if (orientation == TopAbs_REVERSED) {
        n.Reverse();
    }
    n_out = n;
    return true;
}

/// Uncached variant for the whole-shape fallback, whose support face is not
/// addressed through the index.
bool face_normal_at(const TopoDS_Face& face, const gp_Pnt& q, gp_Vec& n_out) {
    const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull()) {
        return false;
    }
    const BRepAdaptor_Surface asurf(face, Standard_True);
    return face_normal_at(surf, asurf, face.Orientation(), q, n_out);
}

/// Uniform grid hash over BRep face AABBs — same index convention as the
/// triangle grid in mesh/surface_project.cpp (origin / cell / nx,ny,nz plus
/// per-cell bins of member ids), applied to the same closest-point problem on
/// a B-rep instead of a triangle soup. Two costs the brute-force scan paid per
/// query are hoisted here: (1) faces whose box cannot beat the running best are
/// never solved, and (2) each face owns a persistent extrema solver with the
/// face pre-loaded as shape 2 — OCC caches a loaded shape's decomposition and
/// sub-shape bounding boxes, so re-loading only the query vertex is ~5x cheaper
/// than rebuilding the solver per query.
struct BrepFaceIndex {
    static constexpr std::size_t kNoFace = static_cast<std::size_t>(-1);

    /// Lexicographic minimum of (distance, face id) — identical to scanning
    /// faces in explorer order and keeping the first strict improvement.
    struct Winner {
        std::size_t face = kNoFace;
        double dist = std::numeric_limits<double>::infinity();
        gp_Pnt point;
        TopoDS_Shape support;
    };

    std::vector<TopoDS_Face> faces;
    TopTools_IndexedMapOfShape face_map;
    TopTools_IndexedMapOfShape edge_map;
    TopTools_IndexedMapOfShape vertex_map;
    std::vector<TopoDS_Edge> edges;
    std::vector<TopoDS_Vertex> vertices;
    /// OCC edge-map index → nondegenerate CadEdge::id, or invalid.
    std::vector<std::uint32_t> edge_ids;
    std::vector<FaceBox> boxes;
    std::vector<Handle(Geom_Surface)> surfaces;
    std::vector<Handle(BRepAdaptor_Surface)> adaptors;
    /// One solver per face; `unique_ptr` keeps the cached OCC maps put.
    std::vector<std::unique_ptr<BRepExtrema_DistShapeShape>> solvers;
    /// Faces with no usable AABB — always solved, never culled.
    std::vector<std::uint32_t> unbounded;

    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector3d cell = Eigen::Vector3d::Ones();
    int nx = 1, ny = 1, nz = 1;
    double min_cell = 1.0;
    std::vector<std::vector<std::uint32_t>> bins;
    mutable std::vector<std::uint32_t> seen; ///< per-query dedupe stamps
    mutable std::uint32_t epoch = 0;

    int flat(int i, int j, int k) const { return (k * ny + j) * nx + i; }

    void build(const TopoDS_Shape& shape) {
        faces.clear();
        face_map.Clear();
        edge_map.Clear();
        vertex_map.Clear();
        edges.clear();
        vertices.clear();
        edge_ids.clear();
        boxes.clear();
        surfaces.clear();
        adaptors.clear();
        solvers.clear();
        unbounded.clear();
        bins.clear();
        seen.clear();
        epoch = 0;

        // Stable zero-based ids mirror CadTopology's TopExp::MapShapes order.
        TopExp::MapShapes(shape, TopAbs_FACE, face_map);
        TopExp::MapShapes(shape, TopAbs_EDGE, edge_map);
        TopExp::MapShapes(shape, TopAbs_VERTEX, vertex_map);
        faces.reserve(static_cast<std::size_t>(face_map.Extent()));
        for (Standard_Integer i = 1; i <= face_map.Extent(); ++i) {
            faces.push_back(TopoDS::Face(face_map(i)));
        }
        edge_ids.assign(static_cast<std::size_t>(edge_map.Extent() + 1), kInvalidCadSupportId);
        for (Standard_Integer i = 1; i <= edge_map.Extent(); ++i) {
            const TopoDS_Edge& edge = TopoDS::Edge(edge_map(i));
            if (BRep_Tool::Degenerated(edge)) {
                continue;
            }
            edge_ids[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(edges.size());
            edges.push_back(edge);
        }
        vertices.reserve(static_cast<std::size_t>(vertex_map.Extent()));
        for (Standard_Integer i = 1; i <= vertex_map.Extent(); ++i) {
            vertices.push_back(TopoDS::Vertex(vertex_map(i)));
        }
        const std::size_t nf = faces.size();
        if (nf == 0) {
            return;
        }
        boxes.resize(nf);
        surfaces.resize(nf);
        adaptors.resize(nf);
        solvers.resize(nf);
        seen.assign(nf, 0);

        constexpr double kInf = std::numeric_limits<double>::infinity();
        Eigen::Vector3d gmin = Eigen::Vector3d::Constant(kInf);
        Eigen::Vector3d gmax = Eigen::Vector3d::Constant(-kInf);
        for (std::size_t f = 0; f < nf; ++f) {
            // BRepBndLib enlarges by triangulation deflection + face tolerance,
            // so the box is a superset of the trimmed face either way.
            Bnd_Box bb;
            BRepBndLib::Add(faces[f], bb);
            if (bb.IsVoid()) {
                unbounded.push_back(static_cast<std::uint32_t>(f));
            } else {
                double x0 = 0.0, y0 = 0.0, z0 = 0.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
                bb.Get(x0, y0, z0, x1, y1, z1);
                boxes[f].lo = Eigen::Vector3d(x0, y0, z0);
                boxes[f].hi = Eigen::Vector3d(x1, y1, z1);
                gmin = gmin.cwiseMin(boxes[f].lo);
                gmax = gmax.cwiseMax(boxes[f].hi);
            }
            surfaces[f] = BRep_Tool::Surface(faces[f]);
            adaptors[f] = new BRepAdaptor_Surface(faces[f], Standard_True);
            solvers[f] = std::make_unique<BRepExtrema_DistShapeShape>();
            solvers[f]->LoadS2(faces[f]);
        }
        if (!(gmin.array() <= gmax.array()).all()) {
            return; // every face unbounded: cull-free scans only
        }

        // Pad so boundary queries land inside the hash and a hair-tight OCC
        // bound can never cull the true closest face.
        const Eigen::Vector3d extent =
            (gmax - gmin).cwiseMax(Eigen::Vector3d::Constant(1e-12));
        const double pad = 1e-6 * extent.norm() + 1e-12;
        gmin.array() -= pad;
        gmax.array() += pad;
        for (auto& b : boxes) {
            b.lo.array() -= pad;
            b.hi.array() += pad;
        }

        // Target ~2 faces per bin, same as the triangle grid. Real B-reps run
        // from 1 face (sphere) to 10^4 (imported assemblies).
        const double nf_d = static_cast<double>(std::max<std::size_t>(1, nf / 2));
        const int res = std::clamp(static_cast<int>(std::cbrt(nf_d)), 2, 64);
        nx = ny = nz = res;
        origin = gmin;
        cell = (gmax - gmin).cwiseQuotient(Eigen::Vector3d(nx, ny, nz));
        cell = cell.cwiseMax(Eigen::Vector3d::Constant(1e-30));
        min_cell = cell.minCoeff();
        bins.assign(static_cast<std::size_t>(nx * ny * nz), {});

        for (std::size_t f = 0; f < nf; ++f) {
            if (std::find(unbounded.begin(), unbounded.end(), static_cast<std::uint32_t>(f)) !=
                unbounded.end()) {
                continue;
            }
            const Eigen::Vector3d lomin = (boxes[f].lo - origin).cwiseQuotient(cell);
            const Eigen::Vector3d lomax = (boxes[f].hi - origin).cwiseQuotient(cell);
            const int i0 = std::clamp(static_cast<int>(std::floor(lomin[0])), 0, nx - 1);
            const int j0 = std::clamp(static_cast<int>(std::floor(lomin[1])), 0, ny - 1);
            const int k0 = std::clamp(static_cast<int>(std::floor(lomin[2])), 0, nz - 1);
            const int i1 = std::clamp(static_cast<int>(std::floor(lomax[0])), 0, nx - 1);
            const int j1 = std::clamp(static_cast<int>(std::floor(lomax[1])), 0, ny - 1);
            const int k1 = std::clamp(static_cast<int>(std::floor(lomax[2])), 0, nz - 1);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        bins[static_cast<std::size_t>(flat(i, j, k))].push_back(
                            static_cast<std::uint32_t>(f));
                    }
                }
            }
        }
    }

    bool face_normal(std::size_t f, const gp_Pnt& q, gp_Vec& n_out) const {
        return face_normal_at(surfaces[f], *adaptors[f], faces[f].Orientation(), q, n_out);
    }

    /// Closest point on one trimmed face via the face's persistent extrema
    /// solver. Failure is reported instead of falling back to the untrimmed
    /// underlying surface.
    bool project_on_face(std::size_t f, const TopoDS_Vertex& vtx, gp_Pnt& closest,
                         double& dist, bool& untrimmed,
                         TopoDS_Shape* support = nullptr) const {
        untrimmed = false;
        if (f >= solvers.size()) {
            return false;
        }
        BRepExtrema_DistShapeShape& dss = *solvers[f];
        dss.LoadS1(vtx);
        dss.Perform();
        if (!dss.IsDone() || dss.NbSolution() < 1) {
            return false;
        }
        dist = static_cast<double>(dss.Value());
        closest = dss.PointOnShape2(1);
        if (support != nullptr) {
            *support = dss.SupportOnShape2(1);
        }
        return std::isfinite(dist);
    }

    std::pair<CadSupportKind, std::uint32_t>
    stable_support(const TopoDS_Shape& support) const {
        if (support.IsNull()) {
            return {CadSupportKind::kUnknown, kInvalidCadSupportId};
        }
        if (support.ShapeType() == TopAbs_VERTEX) {
            const Standard_Integer i = vertex_map.FindIndex(support);
            if (i > 0) {
                return {CadSupportKind::kVertex, static_cast<std::uint32_t>(i - 1)};
            }
        } else if (support.ShapeType() == TopAbs_EDGE) {
            const Standard_Integer i = edge_map.FindIndex(support);
            if (i > 0 && static_cast<std::size_t>(i) < edge_ids.size() &&
                edge_ids[static_cast<std::size_t>(i)] != kInvalidCadSupportId) {
                return {CadSupportKind::kEdge, edge_ids[static_cast<std::size_t>(i)]};
            }
        } else if (support.ShapeType() == TopAbs_FACE) {
            const Standard_Integer i = face_map.FindIndex(support);
            if (i > 0) {
                return {CadSupportKind::kFace, static_cast<std::uint32_t>(i - 1)};
            }
        }
        return {CadSupportKind::kUnknown, kInvalidCadSupportId};
    }

    /// Exact closest face for `p`. With `cull` the grid is walked outward and
    /// boxed-out faces are skipped; without it every face is solved. The winner
    /// is order-independent, so both paths agree bit for bit.
    Winner closest(const TopoDS_Vertex& vtx, const Eigen::Vector3d& p, bool cull,
                   bool& out_untrimmed) const {
        Winner w;
        out_untrimmed = false;

        const auto consider = [&](std::size_t f) {
            gp_Pnt q;
            double d = 0.0;
            bool untrimmed = false;
            TopoDS_Shape support;
            if (!project_on_face(f, vtx, q, d, untrimmed, &support)) {
                return;
            }
            out_untrimmed = out_untrimmed || untrimmed;
            if (d < w.dist || (d == w.dist && f < w.face)) {
                w.dist = d;
                w.point = q;
                w.support = support;
                w.face = f;
            }
        };

        if (!cull || bins.empty()) {
            for (std::size_t f = 0; f < faces.size(); ++f) {
                consider(f);
            }
            return w;
        }

        for (std::uint32_t f : unbounded) {
            consider(f);
        }

        if (++epoch == 0) { // wrapped: stale stamps would alias
            std::fill(seen.begin(), seen.end(), 0);
            epoch = 1;
        }
        const Eigen::Vector3d local = (p - origin).cwiseQuotient(cell);
        const int ic = std::clamp(static_cast<int>(std::floor(local[0])), 0, nx - 1);
        const int jc = std::clamp(static_cast<int>(std::floor(local[1])), 0, ny - 1);
        const int kc = std::clamp(static_cast<int>(std::floor(local[2])), 0, nz - 1);
        const int max_r = std::max({nx, ny, nz});

        for (int r = 0; r <= max_r; ++r) {
            // Any point of an unvisited cell at shell radius r is at least
            // (r-1)*min_cell from p (p sits somewhere inside the centre cell),
            // so beyond that nothing can beat — or tie — the running best.
            if (r > 0 && static_cast<double>(r - 1) * min_cell > w.dist) {
                break;
            }
            const int i0 = std::max(0, ic - r), i1 = std::min(nx - 1, ic + r);
            const int j0 = std::max(0, jc - r), j1 = std::min(ny - 1, jc + r);
            const int k0 = std::max(0, kc - r), k1 = std::min(nz - 1, kc + r);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        // Only the shell at radius r (the inner cube is done).
                        if (r > 0 && i != i0 && i != i1 && j != j0 && j != j1 && k != k0 &&
                            k != k1) {
                            continue;
                        }
                        for (std::uint32_t f : bins[static_cast<std::size_t>(flat(i, j, k))]) {
                            if (seen[f] == epoch) {
                                continue;
                            }
                            seen[f] = epoch;
                            // `w.dist` only shrinks, so a boxed-out face stays out.
                            if (box_lower_bound(p, boxes[f]) > w.dist) {
                                continue;
                            }
                            consider(f);
                        }
                    }
                }
            }
        }
        return w;
    }
};

/// Thread-local cache: rebuild when the shape identity or its bounds change
/// (mirrors the triangle-grid cache in mesh/surface_project.cpp).
const BrepFaceIndex& face_index_for(const CadModel& model, const TopoDS_Shape& shape) {
    thread_local const TopoDS_Shape* cached_ptr = nullptr;
    thread_local Eigen::Vector3d cached_lo = Eigen::Vector3d::Zero();
    thread_local Eigen::Vector3d cached_hi = Eigen::Vector3d::Zero();
    thread_local BrepFaceIndex cached;
    if (cached_ptr != &shape || (cached_lo.array() != model.bbox_min().array()).any() ||
        (cached_hi.array() != model.bbox_max().array()).any()) {
        cached.build(shape);
        cached_ptr = &shape;
        cached_lo = model.bbox_min();
        cached_hi = model.bbox_max();
    }
    return cached;
}

/// Brute-force reference for verifying the spatial index against the original
/// O(queries * faces) algorithm. Set POLYMESH_PROJ_BRUTE=1 before process
/// startup to disable culling and compare output bytes/quality deterministically.
bool project_on_face_brute(const TopoDS_Vertex& vtx, const TopoDS_Face& face, gp_Pnt& closest,
                           double& dist, gp_Vec& normal, TopoDS_Shape& support) {
    BRepExtrema_DistShapeShape dss(vtx, face);
    dss.Perform();
    if (!dss.IsDone() || dss.NbSolution() < 1) {
        return false;
    }
    dist = static_cast<double>(dss.Value());
    closest = dss.PointOnShape2(1);
    support = dss.SupportOnShape2(1);
    const bool ok = face_normal_at(face, closest, normal);
    if (!ok) {
        normal = gp_Vec(0, 0, 0);
    }
    return std::isfinite(dist);
}

bool proj_brute_enabled() {
    static const bool on = std::getenv("POLYMESH_PROJ_BRUTE") != nullptr;
    return on;
}

ProjectResult make_project_result(const Eigen::Vector3d& query, const gp_Pnt& point,
                                  double distance, gp_Vec normal, CadSupportKind support_kind,
                                  std::uint32_t support_id, std::uint32_t face_id) {
    ProjectResult r;
    r.point = Eigen::Vector3d(point.X(), point.Y(), point.Z());
    if (normal.SquareMagnitude() > Precision::SquareConfusion()) {
        normal.Normalize();
        r.normal = Eigen::Vector3d(normal.X(), normal.Y(), normal.Z());
    } else {
        const Eigen::Vector3d d = r.point - query;
        const double len = d.norm();
        if (len > 1e-15) {
            r.normal = d / len;
        }
    }
    r.support_kind = support_kind;
    r.support_id = support_id;
    r.face_id = face_id;
    r.distance = distance;
    return r;
}

} // namespace

BRepInspection inspect_brep(const CadModel& model) {
    BRepInspection out;
    if (model.empty() || model.shape_handle() == nullptr) {
        return out;
    }

    const auto& shape = *static_cast<const TopoDS_Shape*>(model.shape_handle());
    out.available = true;
    out.valid = BRepCheck_Analyzer(shape).IsValid();

    TopTools_IndexedMapOfShape solids;
    TopTools_IndexedMapOfShape shells;
    TopTools_IndexedMapOfShape faces;
    TopTools_IndexedMapOfShape edges;
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_SHELL, shells);
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    TopExp::MapShapes(shape, TopAbs_VERTEX, vertices);
    out.solid_count = static_cast<std::size_t>(solids.Extent());
    out.shell_count = static_cast<std::size_t>(shells.Extent());
    out.face_count = static_cast<std::size_t>(faces.Extent());
    out.edge_count = static_cast<std::size_t>(edges.Extent());
    out.vertex_count = static_cast<std::size_t>(vertices.Extent());

    for (Standard_Integer i = 1; i <= shells.Extent(); ++i) {
        if (BRep_Tool::IsClosed(TopoDS::Shell(shells(i)))) {
            ++out.closed_shell_count;
        }
    }
    out.closed = out.shell_count > 0 && out.closed_shell_count == out.shell_count;

    GProp_GProps volume_props;
    BRepGProp::VolumeProperties(shape, volume_props);
    out.volume = std::abs(static_cast<double>(volume_props.Mass()));

    GProp_GProps surface_props;
    BRepGProp::SurfaceProperties(shape, surface_props);
    out.surface_area = std::abs(static_cast<double>(surface_props.Mass()));
    return out;
}

BRepSurfaceSamples sample_brep_surface(const CadModel& model, std::size_t max_samples) {
    BRepSurfaceSamples result;
    if (model.empty() || model.shape_handle() == nullptr) {
        return result;
    }
    const auto& shape = *static_cast<const TopoDS_Shape*>(model.shape_handle());
    TopTools_IndexedMapOfShape mapped_faces;
    TopExp::MapShapes(shape, TopAbs_FACE, mapped_faces);
    const std::size_t face_count = static_cast<std::size_t>(mapped_faces.Extent());
    result.face_count = face_count;
    if (face_count == 0) {
        return result;
    }
    if (max_samples < face_count) {
        throw GeomError(
            std::format("sample_brep_surface: max_samples={} cannot cover {} BRep faces",
                        max_samples, face_count));
    }

    // Give every face one point, then distribute the remaining budget by exact
    // surface area. Stable fractional-remainder ordering makes the allocation
    // deterministic while approximating an area-weighted surface distribution.
    std::vector<std::size_t> quotas(face_count, 1);
    std::vector<double> areas(face_count, 0.0);
    double total_area = 0.0;
    for (std::size_t i = 0; i < face_count; ++i) {
        GProp_GProps properties;
        BRepGProp::SurfaceProperties(TopoDS::Face(mapped_faces(static_cast<int>(i + 1))),
                                     properties);
        const double area = std::abs(static_cast<double>(properties.Mass()));
        if (std::isfinite(area) && area > 0.0) {
            areas[i] = area;
            total_area += area;
        }
    }
    const std::size_t remaining = max_samples - face_count;
    if (remaining > 0 && total_area > 0.0 && std::isfinite(total_area)) {
        std::vector<double> fractions(face_count, 0.0);
        std::size_t allocated = 0;
        for (std::size_t i = 0; i < face_count; ++i) {
            const long double exact =
                static_cast<long double>(remaining) *
                (static_cast<long double>(areas[i]) / static_cast<long double>(total_area));
            const std::size_t available = remaining - allocated;
            const long double floored = std::floor(exact);
            const std::size_t whole =
                !std::isfinite(exact) || floored >= static_cast<long double>(available)
                    ? available
                    : static_cast<std::size_t>(std::max(0.0L, floored));
            quotas[i] += whole;
            allocated += whole;
            fractions[i] = std::isfinite(exact) ? static_cast<double>(exact - floored) : 0.0;
        }
        std::vector<std::size_t> order(face_count);
        for (std::size_t i = 0; i < face_count; ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&fractions](std::size_t a, std::size_t b) {
                             return fractions[a] > fractions[b];
                         });
        for (std::size_t i = 0; i < remaining - allocated; ++i) {
            ++quotas[order[i % face_count]];
        }
    } else if (remaining > 0) {
        for (std::size_t i = 0; i < face_count; ++i) {
            quotas[i] += remaining / face_count;
            if (i < remaining % face_count) {
                ++quotas[i];
            }
        }
    }

    std::vector<Eigen::Vector3d>& samples = result.points;
    samples.reserve(max_samples);
    for (std::size_t i = 0; i < face_count; ++i) {
        const TopoDS_Face face =
            TopoDS::Face(mapped_faces(static_cast<Standard_Integer>(i + 1)));
        const std::size_t quota = quotas[i];
        const std::size_t before = samples.size();

        Standard_Real u_min = 0.0;
        Standard_Real u_max = 0.0;
        Standard_Real v_min = 0.0;
        Standard_Real v_max = 0.0;
        BRepTools::UVBounds(face, u_min, u_max, v_min, v_max);
        const bool finite_bounds = std::isfinite(static_cast<double>(u_min)) &&
                                   std::isfinite(static_cast<double>(u_max)) &&
                                   std::isfinite(static_cast<double>(v_min)) &&
                                   std::isfinite(static_cast<double>(v_max)) &&
                                   u_max > u_min && v_max > v_min;
        if (finite_bounds) {
            // A ceil(2*sqrt(quota)) grid attempts at most 9*quota points.
            // Cell centres avoid over-counting coincident face boundaries.
            const std::size_t grid_side = static_cast<std::size_t>(
                std::ceil(2.0 * std::sqrt(static_cast<double>(quota))));
            BRepAdaptor_Surface surface(face, Standard_True);
            for (std::size_t v = 0; v < grid_side && samples.size() - before < quota; ++v) {
                const double fv =
                    (static_cast<double>(v) + 0.5) / static_cast<double>(grid_side);
                const Standard_Real param_v =
                    v_min + static_cast<Standard_Real>(fv) * (v_max - v_min);
                for (std::size_t u = 0; u < grid_side && samples.size() - before < quota;
                     ++u) {
                    const double fu =
                        (static_cast<double>(u) + 0.5) / static_cast<double>(grid_side);
                    const Standard_Real param_u =
                        u_min + static_cast<Standard_Real>(fu) * (u_max - u_min);
                    BRepClass_FaceClassifier classifier(face, gp_Pnt2d(param_u, param_v),
                                                        Precision::Confusion());
                    ++result.uv_attempt_count;
                    const TopAbs_State state = classifier.State();
                    if (state != TopAbs_IN && state != TopAbs_ON) {
                        continue;
                    }
                    const gp_Pnt point = surface.Value(param_u, param_v);
                    if (std::isfinite(static_cast<double>(point.X())) &&
                        std::isfinite(static_cast<double>(point.Y())) &&
                        std::isfinite(static_cast<double>(point.Z()))) {
                        samples.emplace_back(point.X(), point.Y(), point.Z());
                    }
                }
            }
        }

        if (samples.size() == before) {
            // Thin/degenerate trims can miss every bounded cell centre. A BRep
            // vertex remains an exact point on the trimmed face.
            TopExp_Explorer vertices(face, TopAbs_VERTEX);
            bool found_finite_vertex = false;
            while (vertices.More()) {
                const gp_Pnt point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
                if (std::isfinite(static_cast<double>(point.X())) &&
                    std::isfinite(static_cast<double>(point.Y())) &&
                    std::isfinite(static_cast<double>(point.Z()))) {
                    samples.emplace_back(point.X(), point.Y(), point.Z());
                    found_finite_vertex = true;
                    ++result.fallback_vertex_count;
                    break;
                }
                vertices.Next();
            }
            if (!found_finite_vertex) {
                throw GeomError(std::format(
                    "sample_brep_surface: face {} yielded no finite bounded exact sample", i));
            }
        }
    }
    return result;
}

std::optional<ProjectResult> project_point_on_surface(const CadModel& model,
                                                      const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    const BrepFaceIndex& index = face_index_for(model, *shape);

    BRep_Builder builder;
    TopoDS_Vertex vtx;
    builder.MakeVertex(vtx, gp_Pnt(p.x(), p.y(), p.z()), Precision::Confusion());

    double best_dist = std::numeric_limits<double>::infinity();
    gp_Pnt best_pt;
    gp_Vec best_n(0, 0, 0);
    TopoDS_Shape best_support;
    std::uint32_t best_face = kInvalidCadSupportId;
    bool found = false;

    // Per-face extrema respects wires and supplies a stable supporting face.
    if (proj_brute_enabled()) {
        for (std::size_t f = 0; f < index.faces.size(); ++f) {
            gp_Pnt closest;
            double dist = 0.0;
            gp_Vec n(0, 0, 0);
            TopoDS_Shape support;
            if (!project_on_face_brute(vtx, index.faces[f], closest, dist, n, support)) {
                continue;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_pt = closest;
                best_n = n;
                best_support = support;
                best_face = static_cast<std::uint32_t>(f);
                found = true;
            }
        }
    } else {
        bool untrimmed = false;
        const BrepFaceIndex::Winner win = index.closest(vtx, p, /*cull=*/true, untrimmed);
        if (win.face != BrepFaceIndex::kNoFace) {
            best_dist = win.dist;
            best_pt = win.point;
            best_support = win.support;
            best_face = static_cast<std::uint32_t>(win.face);
            if (!index.face_normal(win.face, best_pt, best_n)) {
                best_n = gp_Vec(0, 0, 0);
            }
            found = true;
        }
    }

    // Whole-shape fallback if the face map is empty or every trimmed solve
    // failed. It is still exact, but cannot always name an owning face.
    if (!found) {
        BRepExtrema_DistShapeShape dss(vtx, *shape);
        dss.Perform();
        if (!dss.IsDone() || dss.NbSolution() < 1) {
            return std::nullopt;
        }
        best_dist = static_cast<double>(dss.Value());
        best_pt = dss.PointOnShape2(1);
        best_support = dss.SupportOnShape2(1);
        if (best_support.ShapeType() == TopAbs_FACE) {
            (void)face_normal_at(TopoDS::Face(best_support), best_pt, best_n);
        }
        found = true;
    }
    if (!found || !std::isfinite(best_dist)) {
        return std::nullopt;
    }

    auto [kind, id] = index.stable_support(best_support);
    if (kind == CadSupportKind::kUnknown && best_face != kInvalidCadSupportId) {
        kind = CadSupportKind::kFace;
        id = best_face;
    }
    return make_project_result(p, best_pt, best_dist, best_n, kind, id, best_face);
}

std::optional<ProjectResult>
project_point_on_face(const CadModel& model, std::uint32_t face_id, const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    const BrepFaceIndex& index = face_index_for(model, *shape);
    if (face_id >= index.faces.size()) {
        return std::nullopt;
    }
    BRep_Builder builder;
    TopoDS_Vertex vtx;
    builder.MakeVertex(vtx, gp_Pnt(p.x(), p.y(), p.z()), Precision::Confusion());
    gp_Pnt closest;
    double distance = 0.0;
    bool untrimmed = false;
    if (!index.project_on_face(face_id, vtx, closest, distance, untrimmed)) {
        return std::nullopt;
    }
    gp_Vec normal(0, 0, 0);
    (void)index.face_normal(face_id, closest, normal);
    return make_project_result(p, closest, distance, normal, CadSupportKind::kFace, face_id,
                               face_id);
}

std::optional<ProjectResult>
project_point_on_edge(const CadModel& model, std::uint32_t edge_id, const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    const BrepFaceIndex& index = face_index_for(model, *shape);
    if (edge_id >= index.edges.size()) {
        return std::nullopt;
    }
    BRep_Builder builder;
    TopoDS_Vertex vtx;
    builder.MakeVertex(vtx, gp_Pnt(p.x(), p.y(), p.z()), Precision::Confusion());
    BRepExtrema_DistShapeShape dss(vtx, index.edges[edge_id]);
    dss.Perform();
    if (!dss.IsDone() || dss.NbSolution() < 1 ||
        !std::isfinite(static_cast<double>(dss.Value()))) {
        return std::nullopt;
    }
    return make_project_result(p, dss.PointOnShape2(1), static_cast<double>(dss.Value()),
                               gp_Vec(0, 0, 0), CadSupportKind::kEdge, edge_id,
                               kInvalidCadSupportId);
}

std::optional<ProjectResult> project_point_on_vertex(const CadModel& model,
                                                     std::uint32_t vertex_id,
                                                     const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    const BrepFaceIndex& index = face_index_for(model, *shape);
    if (vertex_id >= index.vertices.size()) {
        return std::nullopt;
    }
    const gp_Pnt point = BRep_Tool::Pnt(index.vertices[vertex_id]);
    return make_project_result(p, point, gp_Pnt(p.x(), p.y(), p.z()).Distance(point),
                               gp_Vec(0, 0, 0), CadSupportKind::kVertex, vertex_id,
                               kInvalidCadSupportId);
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

double CadModel::bbox_diagonal() const noexcept { return (bbox_max_ - bbox_min_).norm(); }

TriSurface CadModel::tessellate(double /*deflection*/, double /*angular_deflection*/) const {
    throw GeomError("OpenCASCADE not enabled");
}

const void* CadModel::shape_handle() const noexcept { return nullptr; }

void CadModel::compute_bbox() {}

BRepInspection inspect_brep(const CadModel& /*model*/) { return {}; }

BRepSurfaceSamples sample_brep_surface(const CadModel& /*model*/,
                                       std::size_t /*max_samples*/) {
    return {};
}

std::optional<ProjectResult> project_point_on_surface(const CadModel& /*model*/,
                                                      const Eigen::Vector3d& /*p*/) {
    // Stub without OCC: no BRep oracle (STL-only builds keep surface snap only).
    return std::nullopt;
}

std::optional<ProjectResult> project_point_on_face(const CadModel& /*model*/,
                                                   std::uint32_t /*face_id*/,
                                                   const Eigen::Vector3d& /*p*/) {
    return std::nullopt;
}

std::optional<ProjectResult> project_point_on_edge(const CadModel& /*model*/,
                                                   std::uint32_t /*edge_id*/,
                                                   const Eigen::Vector3d& /*p*/) {
    return std::nullopt;
}

std::optional<ProjectResult> project_point_on_vertex(const CadModel& /*model*/,
                                                     std::uint32_t /*vertex_id*/,
                                                     const Eigen::Vector3d& /*p*/) {
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
    throw GeomError(
        std::format("load_cad: unsupported extension '{}' (use .step/.stp/.brep)", ext));
}

} // namespace polymesh::geom
