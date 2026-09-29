// SPDX-License-Identifier: BSD-3-Clause
// Exact closest-point queries on the retained BRep (project_point_on_*).
#include "geom/cad_model.hpp"

#include <cstdint>
#include <optional>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <TopAbs.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#endif // POLYMESH_WITH_OCC

namespace polymesh::geom {

#ifdef POLYMESH_WITH_OCC

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

/// Uniform grid hash over BRep face AABBs, same layout as the triangle grid in
/// mesh/surface_project.cpp (origin / cell / nx,ny,nz plus per-cell bins of
/// member ids). Faces whose box cannot beat the running best are never solved,
/// and each face keeps a persistent extrema solver with the face pre-loaded as
/// shape 2 so OCC's cached decomposition is reused across queries.
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
                         double& dist, TopoDS_Shape* support = nullptr) const {
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

    /// Exact closest face for `p`: the grid is walked outward and boxed-out
    /// faces are skipped. With no grid (every face unbounded) every face is
    /// solved. The winner is order-independent, so both paths agree bit for bit.
    Winner closest(const TopoDS_Vertex& vtx, const Eigen::Vector3d& p) const {
        Winner w;

        const auto consider = [&](std::size_t f) {
            gp_Pnt q;
            double d = 0.0;
            TopoDS_Shape support;
            if (!project_on_face(f, vtx, q, d, &support)) {
                return;
            }
            if (d < w.dist || (d == w.dist && f < w.face)) {
                w.dist = d;
                w.point = q;
                w.support = support;
                w.face = f;
            }
        };

        if (bins.empty()) {
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

/// Uncached O(faces) reference used when POLYMESH_PROJ_BRUTE is set.
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
    // POLYMESH_PROJ_BRUTE (any value, read once): bypass the face index; A/B oracle for it.
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
        const BrepFaceIndex::Winner win = index.closest(vtx, p);
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
    if (!index.project_on_face(face_id, vtx, closest, distance)) {
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

} // namespace polymesh::geom
