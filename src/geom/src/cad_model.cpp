// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"

#include "geom/stl.hpp" // detail::weld

#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>

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
// TEMP PROFILING (removed before delivery).
struct ProjProf {
    long long calls = 0;
    long long extrema = 0;
    long long normals = 0;
    double t_total = 0.0;
    double t_extrema = 0.0;
    double t_normal = 0.0;
    ~ProjProf() {
        if (std::getenv("POLYMESH_PROJ_STATS") == nullptr || calls == 0) {
            return;
        }
        std::fprintf(stderr,
                     "[projprof] calls=%lld extrema=%lld normals=%lld ex/call=%.3f "
                     "t_total=%.3fs t_extrema=%.3fs t_normal=%.3fs us/call=%.2f "
                     "us/extrema=%.2f us/normal=%.2f\n",
                     calls, extrema, normals, double(extrema) / double(calls), t_total,
                     t_extrema, t_normal, 1e6 * t_total / double(calls),
                     1e6 * t_extrema / double(extrema), 1e6 * t_normal / double(normals));
    }
};
ProjProf g_pp;
using pclock = std::chrono::steady_clock;
double psecs(pclock::duration d) { return std::chrono::duration<double>(d).count(); }


/// Conservative axis-aligned bound of one BRep face. Culls faces that cannot
/// hold the closest point before paying for an exact extrema solve.
struct FaceBox {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
};

/// Exact lower bound on |p - q| over every q inside `b` (0 when p is inside).
double box_lower_bound(const Eigen::Vector3d& p, const FaceBox& b) {
    const Eigen::Vector3d d =
        (b.lo - p).cwiseMax(p - b.hi).cwiseMax(Eigen::Vector3d::Zero());
    return d.norm();
}

/// Outward-ish unit normal at 3D point `q` from a face's surface + orientation
/// (UV via surface project). Split from the face so the cached index can hand
/// over a pre-built surface handle and adaptor.
bool face_normal_at(const Handle(Geom_Surface)& surf, const BRepAdaptor_Surface& asurf,
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
    };

    std::vector<TopoDS_Face> faces;
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

    int flat(int i, int j, int k) const {
        return (k * ny + j) * nx + i;
    }

    void build(const TopoDS_Shape& shape) {
        faces.clear();
        boxes.clear();
        surfaces.clear();
        adaptors.clear();
        solvers.clear();
        unbounded.clear();
        bins.clear();
        seen.clear();
        epoch = 0;

        // Explorer order defines the face ids, so culled and unculled scans
        // break distance ties exactly like the original in-order scan.
        for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next()) {
            faces.push_back(TopoDS::Face(exp.Current()));
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
        const Eigen::Vector3d extent = (gmax - gmin).cwiseMax(Eigen::Vector3d::Constant(1e-12));
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
            if (std::find(unbounded.begin(), unbounded.end(),
                          static_cast<std::uint32_t>(f)) != unbounded.end()) {
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
        ++g_pp.normals;
        const auto t0 = pclock::now();
        const bool ok = face_normal_at(surfaces[f], *adaptors[f], faces[f].Orientation(), q, n_out);
        g_pp.t_normal += psecs(pclock::now() - t0);
        return ok;
    }

    /// Closest point on one trimmed face via the face's persistent extrema
    /// solver. `untrimmed` reports the degenerate path whose distance comes
    /// from the untrimmed surface and may therefore undercut the face's box.
    bool project_on_face(std::size_t f, const TopoDS_Vertex& vtx, gp_Pnt& closest, double& dist,
                         bool& untrimmed) const {
        BRepExtrema_DistShapeShape& dss = *solvers[f];
        ++g_pp.extrema;
        const auto t0 = pclock::now();
        dss.LoadS1(vtx);
        dss.Perform();
        g_pp.t_extrema += psecs(pclock::now() - t0);
        if (dss.IsDone() && dss.NbSolution() >= 1) {
            dist = static_cast<double>(dss.Value());
            closest = dss.PointOnShape2(1);
            untrimmed = false;
            return true;
        }
        // Fallback: infinite-surface project (ignores face trim). Only usable
        // with a real normal, matching the pre-index behaviour.
        if (surfaces[f].IsNull()) {
            return false;
        }
        GeomAPI_ProjectPointOnSurf proj(BRep_Tool::Pnt(vtx), surfaces[f]);
        if (proj.NbPoints() < 1) {
            return false;
        }
        gp_Vec n(0, 0, 0);
        if (!face_normal(f, proj.NearestPoint(), n)) {
            return false;
        }
        closest = proj.NearestPoint();
        dist = proj.LowerDistance();
        untrimmed = true;
        return true;
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
            if (!project_on_face(f, vtx, q, d, untrimmed)) {
                return;
            }
            out_untrimmed = out_untrimmed || untrimmed;
            if (d < w.dist || (d == w.dist && f < w.face)) {
                w.dist = d;
                w.point = q;
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

// TEMP A/B REFERENCE (removed before delivery): the pre-index implementation,
// byte-for-byte, reachable via POLYMESH_PROJ_BRUTE=1 so before/after can be
// measured in one binary under identical machine load.
bool project_on_face_brute(const TopoDS_Vertex& vtx, const TopoDS_Face& face, gp_Pnt& closest,
                           double& dist, gp_Vec& normal) {
    ++g_pp.extrema;
    const auto t0 = pclock::now();
    BRepExtrema_DistShapeShape dss(vtx, face);
    dss.Perform();
    g_pp.t_extrema += psecs(pclock::now() - t0);
    if (!dss.IsDone() || dss.NbSolution() < 1) {
        const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
        if (surf.IsNull()) {
            return false;
        }
        GeomAPI_ProjectPointOnSurf proj(BRep_Tool::Pnt(vtx), surf);
        if (proj.NbPoints() < 1) {
            return false;
        }
        closest = proj.NearestPoint();
        dist = proj.LowerDistance();
        return face_normal_at(face, closest, normal);
    }
    dist = static_cast<double>(dss.Value());
    closest = dss.PointOnShape2(1);
    ++g_pp.normals;
    const auto t1 = pclock::now();
    const bool ok = face_normal_at(face, closest, normal);
    g_pp.t_normal += psecs(pclock::now() - t1);
    if (!ok) {
        normal = gp_Vec(0, 0, 0);
    }
    return true;
}

bool proj_brute_enabled() {
    static const bool on = std::getenv("POLYMESH_PROJ_BRUTE") != nullptr;
    return on;
}

} // namespace

std::optional<ProjectResult> project_point_on_surface(const CadModel& model,
                                                      const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    ++g_pp.calls;
    struct ProfScope {
        pclock::time_point t0 = pclock::now();
        ~ProfScope() { g_pp.t_total += psecs(pclock::now() - t0); }
    } prof_scope;
    const BrepFaceIndex& index = face_index_for(model, *shape);

    BRep_Builder builder;
    TopoDS_Vertex vtx;
    builder.MakeVertex(vtx, gp_Pnt(p.x(), p.y(), p.z()), Precision::Confusion());

    double best_dist = std::numeric_limits<double>::infinity();
    gp_Pnt best_pt;
    gp_Vec best_n(0, 0, 0);
    bool found = false;

    // Per-face extrema (respects trim) — preferred over whole-shape when we
    // need a supporting face for the normal.
    if (proj_brute_enabled()) { // TEMP A/B
        for (TopExp_Explorer exp(*shape, TopAbs_FACE); exp.More(); exp.Next()) {
            const TopoDS_Face& face = TopoDS::Face(exp.Current());
            gp_Pnt closest;
            double dist = 0.0;
            gp_Vec n(0, 0, 0);
            if (!project_on_face_brute(vtx, face, closest, dist, n)) {
                continue;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_pt = closest;
                best_n = n;
                found = true;
            }
        }
    } else {
        bool untrimmed = false;
        BrepFaceIndex::Winner win = index.closest(vtx, p, /*cull=*/true, untrimmed);
        if (untrimmed) {
            // An untrimmed-surface distance can dip below the face's own box,
            // so the culled walk may have discarded the true winner: redo it.
            bool ignored = false;
            win = index.closest(vtx, p, /*cull=*/false, ignored);
        }
        if (win.face != BrepFaceIndex::kNoFace) {
            best_dist = win.dist;
            best_pt = win.point;
            // Normal only for the winner: the losing faces never needed one.
            if (!index.face_normal(win.face, best_pt, best_n)) {
                best_n = gp_Vec(0, 0, 0);
            }
            found = true;
        }
    }

    // Whole-shape fallback if face loop failed (degenerate / empty face map).
    if (!found) {
        BRepExtrema_DistShapeShape dss(vtx, *shape);
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
