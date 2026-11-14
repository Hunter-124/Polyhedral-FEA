// SPDX-License-Identifier: BSD-3-Clause
// Exact closest-point queries on the retained BRep (project_point_on_*).
#include "geom/cad_model.hpp"

#include <cstdint>
#include <optional>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepClass_FaceExplorer.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepExtrema_ExtPC.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Extrema_ExtPS.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <OSD_ThreadPool.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "uv_state_grid.hpp"

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

/// Outward-ish unit normal at the lowest-distance solution of a finished
/// projection onto a face's surface.
bool normal_at_projection(const GeomAPI_ProjectPointOnSurf& proj,
                          const BRepAdaptor_Surface& asurf, TopAbs_Orientation orientation,
                          gp_Vec& n_out) {
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

/// Outward-ish unit normal at 3D point `q` from a face's surface + orientation
/// (UV via surface project).
bool face_normal_at(const Handle(Geom_Surface) & surf, const BRepAdaptor_Surface& asurf,
                    TopAbs_Orientation orientation, const gp_Pnt& q, gp_Vec& n_out) {
    if (surf.IsNull()) {
        return false;
    }
    GeomAPI_ProjectPointOnSurf proj(q, surf);
    return normal_at_projection(proj, asurf, orientation, n_out);
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

// ---------------------------------------------------------------------------
// Cached replica of BRepExtrema_DistShapeShape(vertex, face | edge).
//
// A fresh solve pays for an adaptor, UV bounds, an Extrema grid and a
// BRepClass_FaceClassifier walk of every pcurve on every query (OCCT 7.9
// BRepExtrema_ExtPF / DistanceSS). The probes below build those once per face
// or edge and replay the solver's algorithm on top of them: the same sub-shape
// maps and order, the same Extrema setup, the same box pruning, the same
// eps merge of the solution list (including the round-robin task layout of
// DistanceMapMap, which fixes which tied solution comes first), and the same
// BRepClass classification, run on a per-face explorer that keeps its UV
// bounds instead of a fresh one per call.
// ---------------------------------------------------------------------------

/// BRepExtrema_DistShapeShape's default myEps.
constexpr double kDistEps = Precision::Confusion();

struct DistSolution {
    double dist = 0.0;
    gp_Pnt point;
    TopoDS_Shape support;
};

/// myDistRef plus the shape-2 solution list, for the whole solve or one task.
struct DistState {
    double ref = 0.0;
    std::vector<DistSolution> sols;

    /// Merge one result the way every functor and map merge does. `take_ref`
    /// is DistanceVertVert's merge, which adopts a within-eps distance even
    /// when it is the larger one.
    void merge(double d, const std::vector<DistSolution>& more, bool take_ref = false) {
        if (d < ref - kDistEps) {
            sols = more;
            ref = d;
        } else if (std::abs(d - ref) < kDistEps) {
            sols.insert(sols.end(), more.begin(), more.end());
            if (take_ref || ref > d) {
                ref = d;
            }
        }
    }
};

/// DistanceSS's TRI_SOLUTION: true when `p` is not already in `sols`.
bool new_solution_point(const std::vector<DistSolution>& sols, const gp_Pnt& p) {
    return std::none_of(sols.begin(), sols.end(), [&](const DistSolution& s) {
        return s.point.Distance(p) <= Precision::Confusion();
    });
}

/// The query point as DistShapeShape's shape 1.
struct VertexQuery {
    TopoDS_Vertex vtx;
    gp_Pnt p;
    Bnd_Box box; ///< BRepBndLib box of the vertex (myBV1)
};

VertexQuery make_vertex_query(const Eigen::Vector3d& q) {
    VertexQuery query;
    query.p = gp_Pnt(q.x(), q.y(), q.z());
    BRep_Builder builder;
    builder.MakeVertex(query.vtx, query.p, Precision::Confusion());
    BRepBndLib::Add(query.vtx, query.box);
    return query;
}

/// One edge as a shape-2 sub-shape: a persistent BRepExtrema_ExtPC plus what
/// DistanceSS's vertex-edge branch reads per solution.
struct EdgeProbe {
    struct End {
        double param = 0.0;
        gp_Pnt point;
        double tol = 0.0;
    };

    TopoDS_Edge edge;
    bool degenerated = false;
    Bnd_Box box; ///< BRepBndLib box (myBE2)
    BRepExtrema_ExtPC ext;
    std::vector<End> ends; ///< TopoDS_Iterator order, as isOnBoundary walks them
    bool ends_cached = false;

    explicit EdgeProbe(const TopoDS_Edge& e) : edge(e) {
        BRepBndLib::Add(edge, box);
        degenerated = BRep_Tool::Degenerated(edge);
        if (degenerated) {
            return;
        }
        ext.Initialize(edge);
        try {
            for (TopoDS_Iterator it(edge); it.More(); it.Next()) {
                const TopoDS_Vertex& v = TopoDS::Vertex(it.Value());
                ends.push_back({BRep_Tool::Parameter(v, edge), BRep_Tool::Pnt(v),
                                BRep_Tool::Tolerance(v)});
            }
            ends_cached = true;
        } catch (const Standard_Failure&) {
            ends.clear(); // a vertex without a parameter: look it up live, as OCCT does
        }
    }

    /// DistanceSS::Perform(vertex, edge) against reference `dst_ref`.
    /// Returns false when it records nothing (myModif false).
    bool solve(const VertexQuery& q, double dst_ref, double& value,
               std::vector<DistSolution>& seq) {
        seq.clear();
        if (q.box.Distance(box) - dst_ref > kDistEps || degenerated) {
            return false;
        }
        ext.Perform(q.vtx);
        const int n = ext.IsDone() ? ext.NbExt() : 0;
        if (n <= 0) {
            return false;
        }
        double dmin = ext.SquareDistance(1);
        for (int i = 2; i <= n; ++i) {
            dmin = std::min(dmin, ext.SquareDistance(i));
        }
        dmin = std::sqrt(dmin);
        if (!(dmin < dst_ref - kDistEps || std::abs(dmin - dst_ref) < kDistEps)) {
            return false;
        }
        value = dst_ref;
        for (int i = 1; i <= n; ++i) {
            if (!(std::abs(dmin - std::sqrt(ext.SquareDistance(i))) < kDistEps)) {
                continue;
            }
            const gp_Pnt pt = ext.Point(i);
            if (!new_solution_point(seq, pt) || on_end_vertex(pt, ext.Parameter(i))) {
                continue;
            }
            value = std::min(value, dmin);
            seq.push_back({dmin, pt, edge});
        }
        return !seq.empty();
    }

    /// DistanceSS's isOnBoundary with Precision::PConfusion().
    bool on_end_vertex(const gp_Pnt& pt, double t) const {
        if (!ends_cached) {
            for (TopoDS_Iterator it(edge); it.More(); it.Next()) {
                const TopoDS_Vertex& v = TopoDS::Vertex(it.Value());
                if (std::abs(BRep_Tool::Parameter(v, edge) - t) < Precision::PConfusion() &&
                    BRep_Tool::Pnt(v).Distance(pt) < BRep_Tool::Tolerance(v)) {
                    return true;
                }
            }
            return false;
        }
        return std::any_of(ends.begin(), ends.end(), [&](const End& end) {
            return std::abs(end.param - t) < Precision::PConfusion() &&
                   end.point.Distance(pt) < end.tol;
        });
    }
};

/// The face BRepClass walks for `face`. A planar face's edges often store no
/// pcurve, and BRep_Tool::CurveOnSurface then re-projects the 3D curve onto the
/// plane on every call. This copy (shared geometry, own topology) stores exactly
/// those projections with the ranges CurveOnPlane reports, so the classifier
/// reads the same 2D curves without rebuilding them. The model's shape is never
/// touched; any failure falls back to `face` itself.
TopoDS_Face classifier_face(const TopoDS_Face& face) {
    try {
        bool any_missing = false;
        for (TopExp_Explorer it(face, TopAbs_EDGE); it.More() && !any_missing; it.Next()) {
            Standard_Real f = 0.0, l = 0.0;
            Standard_Boolean stored = Standard_True;
            const Handle(Geom2d_Curve) pc =
                BRep_Tool::CurveOnSurface(TopoDS::Edge(it.Current()), face, f, l, &stored);
            any_missing = !stored && !pc.IsNull();
        }
        if (!any_missing) {
            return face;
        }
        BRepBuilderAPI_Copy copier(face, /*copyGeom=*/Standard_False,
                                   /*copyMesh=*/Standard_False);
        const TopoDS_Face copy = TopoDS::Face(copier.Shape());
        BRep_Builder builder;
        for (TopExp_Explorer it(copy, TopAbs_EDGE); it.More(); it.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
            Standard_Real f = 0.0, l = 0.0;
            Standard_Boolean stored = Standard_True;
            const Handle(Geom2d_Curve) pc =
                BRep_Tool::CurveOnSurface(edge, copy, f, l, &stored);
            if (!stored && !pc.IsNull()) {
                builder.UpdateEdge(edge, pc, copy, BRep_Tool::Tolerance(edge));
                builder.Range(edge, copy, f, l);
            }
        }
        return copy;
    } catch (const Standard_Failure&) {
        return face;
    }
}

/// The face part of a face shape 2: BRepExtrema_ExtPF's surface extrema and
/// BRepClass_FaceClassifier's explorer, set up once.
struct FaceProbe {
    TopoDS_Face face;
    Bnd_Box box; ///< BRepBndLib box (myBF2)
    Handle(BRepAdaptor_Surface) surf;
    Extrema_ExtPS ext;
    bool ext_ready = false;
    double tol = 0.0;
    TopoDS_Face classified; ///< the face the explorer walks
    BRepClass_FaceExplorer explorer;
    BRepClass_FaceClassifier classifier;
    detail::UvStateGrid grid;
    int n_classified = 0;

    explicit FaceProbe(const TopoDS_Face& f)
        : face(f), classified(classifier_face(f)), explorer(classified) {
        BRepBndLib::Add(face, box);
        tol = BRep_Tool::Tolerance(face);
        // BRepExtrema_ExtPF::Initialize, flag MINMAX and algo Grad being
        // DistShapeShape's defaults.
        surf = new BRepAdaptor_Surface(face, Standard_False);
        if (surf->GetType() == GeomAbs_OtherSurface) {
            return;
        }
        const double t = std::min(tol, Precision::Confusion());
        const double tol_u = std::max(surf->UResolution(t), Precision::PConfusion());
        const double tol_v = std::max(surf->VResolution(t), Precision::PConfusion());
        Standard_Real u1 = 0.0, u2 = 0.0, v1 = 0.0, v2 = 0.0;
        BRepTools::UVBounds(face, u1, u2, v1, v2);
        ext.SetFlag(Extrema_ExtFlag_MINMAX);
        ext.SetAlgo(Extrema_ExtAlgo_Grad);
        ext.Initialize(*surf, u1, u2, v1, v2, tol_u, tol_v);
        ext_ready = true;
    }

    /// BRepClass_FaceClassifier::Perform(face, uv, tol) with its defaults
    /// (gap check 0.1, no edge boxes), on the kept explorer.
    TopAbs_State classify_exact(const gp_Pnt2d& uv) {
        explorer.SetMaxTolerance(0.1);
        explorer.SetUseBndBox(Standard_False);
        classifier.BRepClass_FClassifier::Perform(explorer, uv, tol);
        return classifier.State();
    }

    /// classify_exact, answered from the UV state grid away from the
    /// boundary (see UvStateGrid). The grid is built once the face has been
    /// classified a few times, so faces queried rarely never pay for it.
    TopAbs_State classify(const gp_Pnt2d& uv) {
        if (n_classified < kGridAfter && ++n_classified == kGridAfter) {
            grid.build(classified, tol);
        }
        return grid.classify(uv.X(), uv.Y(),
                             [&](const gp_Pnt2d& p) { return classify_exact(p); });
    }
    static constexpr int kGridAfter = 8;

    /// DistanceSS::Perform(vertex, face) against reference `dst_ref`.
    bool solve(const VertexQuery& q, double dst_ref, double& value,
               std::vector<DistSolution>& seq) {
        seq.clear();
        if (q.box.Distance(box) - dst_ref > kDistEps || !ext_ready) {
            return false;
        }
        ext.Perform(q.p);
        if (!ext.IsDone()) {
            return false;
        }
        // ExtPF keeps IN/ON extrema; DistanceSS then accepts only IN ones.
        // The classifier input is identical both times, so classify once.
        struct Kept {
            double sq;
            gp_Pnt point;
            bool inside;
        };
        std::vector<Kept> kept;
        for (int i = 1; i <= ext.NbExt(); ++i) {
            Standard_Real u = 0.0, v = 0.0;
            ext.Point(i).Parameter(u, v);
            const TopAbs_State state = classify(gp_Pnt2d(u, v));
            if (state == TopAbs_IN || state == TopAbs_ON) {
                kept.push_back(
                    {ext.SquareDistance(i), ext.Point(i).Value(), state == TopAbs_IN});
            }
        }
        if (kept.empty()) {
            return false;
        }
        double dmin = kept.front().sq;
        for (const Kept& k : kept) {
            dmin = std::min(dmin, k.sq);
        }
        dmin = std::sqrt(dmin);
        if (!(dmin < dst_ref - kDistEps || std::abs(dmin - dst_ref) < kDistEps)) {
            return false;
        }
        value = dst_ref;
        for (const Kept& k : kept) {
            if (std::abs(dmin - std::sqrt(k.sq)) < kDistEps &&
                new_solution_point(seq, k.point) && k.inside) {
                value = std::min(value, dmin);
                seq.push_back({dmin, k.point, face});
            }
        }
        return !seq.empty();
    }
};

/// Shape 2 of a vertex-to-shape solve: its vertex map, its edge map (as probes
/// shared across the index) and at most one face.
struct Shape2Probe {
    std::vector<gp_Pnt> vertex_points;
    std::vector<TopoDS_Vertex> vertices;
    std::vector<EdgeProbe*> edges;
    FaceProbe* face = nullptr;
};

/// BRepExtrema_DistShapeShape(vertex, shape 2).Perform(): Value() and the
/// first shape-2 solution. `n_threads` is the default pool size the solver
/// sizes its (sequentially executed) task bands by.
bool distance_to_shape(const VertexQuery& q, const Shape2Probe& s, int n_threads,
                       double& value, DistSolution& first) {
    DistState g;
    g.ref = s.vertices.empty() ? 1.e30 : q.p.Distance(s.vertex_points.front());

    // DistanceVertVert: shape 1 has one vertex, so band 0 does all the work
    // and the other n_threads - 1 bands are empty.
    {
        const double start = g.ref;
        DistState band{start, {}};
        for (std::size_t i = 0; i < s.vertices.size(); ++i) {
            const double d = q.p.Distance(s.vertex_points[i]);
            band.merge(d, {DistSolution{d, s.vertex_points[i], s.vertices[i]}});
        }
        g.merge(band.ref, band.sols, /*take_ref=*/true);
        for (int t = 1; t < n_threads; ++t) {
            g.merge(start, {}, /*take_ref=*/true);
        }
    }

    // DistanceMapMap over (vertex, edges) then (vertex, face): pairs within
    // eps of the box test, stable-sorted by box distance, dealt round-robin
    // to min(pairs, threads) tasks that each start from the same reference.
    struct Pair {
        double box_dist;
        EdgeProbe* edge;
        FaceProbe* face;
    };
    std::vector<Pair> pairs;
    std::vector<DistSolution> seq;
    const auto map_map = [&]() {
        if (pairs.empty()) {
            return;
        }
        std::stable_sort(pairs.begin(), pairs.end(),
                         [](const Pair& a, const Pair& b) { return a.box_dist < b.box_dist; });
        const std::size_t n_tasks =
            std::min(pairs.size(), static_cast<std::size_t>(std::max(1, n_threads)));
        const double start = g.ref;
        std::vector<DistState> tasks(n_tasks, DistState{start, {}});
        for (std::size_t t = 0; t < n_tasks; ++t) {
            DistState& task = tasks[t];
            for (std::size_t i = t; i < pairs.size(); i += n_tasks) {
                if (pairs[i].box_dist > task.ref + kDistEps) {
                    break;
                }
                double v = 0.0;
                const bool done = pairs[i].edge != nullptr
                                      ? pairs[i].edge->solve(q, task.ref, v, seq)
                                      : pairs[i].face->solve(q, task.ref, v, seq);
                if (done) {
                    task.merge(v, seq);
                }
            }
        }
        for (const DistState& task : tasks) {
            g.merge(task.ref, task.sols);
        }
    };
    const auto keep_pair = [&](const Bnd_Box& box) {
        return !q.box.IsVoid() && !box.IsVoid() && q.box.Distance(box) - g.ref < kDistEps;
    };
    for (EdgeProbe* e : s.edges) {
        if (keep_pair(e->box)) {
            pairs.push_back({q.box.Distance(e->box), e, nullptr});
        }
    }
    map_map();
    pairs.clear();
    if (s.face != nullptr && keep_pair(s.face->box)) {
        pairs.push_back({q.box.Distance(s.face->box), nullptr, s.face});
    }
    map_map();

    // The solver's final sweep, including its skip after each removal.
    for (std::size_t i = 0; i < g.sols.size(); ++i) {
        if (g.sols[i].dist > g.ref + kDistEps) {
            g.sols.erase(g.sols.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    if (g.sols.empty()) {
        return false;
    }
    value = g.ref;
    first = g.sols.front();
    return true;
}

/// Uniform grid hash over BRep face AABBs, same layout as the triangle grid in
/// mesh/surface_project.cpp (origin / cell / nx,ny,nz plus per-cell bins of
/// member ids). Faces whose box cannot beat the running best are never solved,
/// and each face or edge keeps a lazily built Shape2Probe so its extrema setup
/// and classifier are reused across queries.
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
    /// Lazily built solve caches. Edge probes are indexed by edge-map index
    /// (slot 0 unused) and shared by both faces of an edge; `unique_ptr`
    /// keeps the Extrema objects, which point into their adaptors, in place.
    mutable std::vector<std::unique_ptr<EdgeProbe>> edge_probes;
    mutable std::vector<std::unique_ptr<FaceProbe>> face_probes;
    mutable std::vector<std::unique_ptr<Shape2Probe>> face_shapes;
    mutable std::vector<std::unique_ptr<Shape2Probe>> edge_shapes; ///< by CadEdge id
    int n_threads = 1;
    /// One surface projector per face, initialised on first use. Constructing a
    /// GeomAPI_ProjectPointOnSurf per query re-samples the surface's whole
    /// extrema grid; Init once + Perform per query reuses it with identical
    /// solutions (same adaptor, bounds, tolerance and algorithm).
    mutable std::vector<std::unique_ptr<GeomAPI_ProjectPointOnSurf>> projectors;
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
        edge_probes.clear();
        face_probes.clear();
        face_shapes.clear();
        edge_shapes.clear();
        projectors.clear();
        edge_boxes.clear();
        edge_box_valid.clear();
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
        edge_probes.resize(static_cast<std::size_t>(edge_map.Extent() + 1));
        edge_shapes.resize(edges.size());
        n_threads = OSD_ThreadPool::DefaultPool()->NbThreads();
        const std::size_t nf = faces.size();
        if (nf == 0) {
            return;
        }
        boxes.resize(nf);
        surfaces.resize(nf);
        adaptors.resize(nf);
        face_probes.resize(nf);
        face_shapes.resize(nf);
        projectors.resize(nf);
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
        const Handle(Geom_Surface)& surf = surfaces[f];
        if (surf.IsNull()) {
            return false;
        }
        std::unique_ptr<GeomAPI_ProjectPointOnSurf>& proj = projectors[f];
        if (!proj) {
            Standard_Real u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
            surf->Bounds(u0, u1, v0, v1);
            proj = std::make_unique<GeomAPI_ProjectPointOnSurf>();
            proj->Init(surf, u0, u1, v0, v1, Precision::Confusion());
        }
        proj->Perform(q);
        return normal_at_projection(*proj, *adaptors[f], faces[f].Orientation(), n_out);
    }

    EdgeProbe* edge_probe(const TopoDS_Edge& edge) const {
        const Standard_Integer i = edge_map.FindIndex(edge);
        if (i <= 0) { // not a sub-shape of the indexed shape: give it its own slot
            edge_probes.push_back(std::make_unique<EdgeProbe>(edge));
            return edge_probes.back().get();
        }
        std::unique_ptr<EdgeProbe>& slot = edge_probes[static_cast<std::size_t>(i)];
        if (!slot) {
            slot = std::make_unique<EdgeProbe>(edge);
        }
        return slot.get();
    }

    /// Vertex and edge maps of `shape` in TopExp::MapShapes order, as
    /// BRepExtrema_DistShapeShape::LoadS2 decomposes it.
    void decompose(const TopoDS_Shape& shape, Shape2Probe& s) const {
        TopTools_IndexedMapOfShape vmap;
        TopTools_IndexedMapOfShape emap;
        TopExp::MapShapes(shape, TopAbs_VERTEX, vmap);
        TopExp::MapShapes(shape, TopAbs_EDGE, emap);
        for (Standard_Integer i = 1; i <= vmap.Extent(); ++i) {
            s.vertices.push_back(TopoDS::Vertex(vmap(i)));
            s.vertex_points.push_back(BRep_Tool::Pnt(s.vertices.back()));
        }
        for (Standard_Integer i = 1; i <= emap.Extent(); ++i) {
            s.edges.push_back(edge_probe(TopoDS::Edge(emap(i))));
        }
    }

    const Shape2Probe& face_shape(std::size_t f) const {
        std::unique_ptr<Shape2Probe>& slot = face_shapes[f];
        if (!slot) {
            slot = std::make_unique<Shape2Probe>();
            decompose(faces[f], *slot);
            face_probes[f] = std::make_unique<FaceProbe>(faces[f]);
            slot->face = face_probes[f].get();
        }
        return *slot;
    }

    const Shape2Probe& edge_shape(std::size_t e) const {
        std::unique_ptr<Shape2Probe>& slot = edge_shapes[e];
        if (!slot) {
            slot = std::make_unique<Shape2Probe>();
            decompose(edges[e], *slot);
        }
        return *slot;
    }

    /// Closest point on one trimmed face, exactly as a vertex-to-face
    /// BRepExtrema_DistShapeShape reports it. Failure is reported instead of
    /// falling back to the untrimmed underlying surface.
    bool project_on_face(std::size_t f, const VertexQuery& q, gp_Pnt& closest, double& dist,
                         TopoDS_Shape* support = nullptr) const {
        if (f >= faces.size()) {
            return false;
        }
        DistSolution first;
        if (!distance_to_shape(q, face_shape(f), n_threads, dist, first)) {
            return false;
        }
        closest = first.point;
        if (support != nullptr) {
            *support = first.support;
        }
        return std::isfinite(dist);
    }

    /// Conservative boxes of `edges`, built on first use: each holds the exact
    /// 3D curve (BndLib bounds a conic exactly and a Bezier/BSpline by its
    /// poles) enlarged by the edge tolerance, plus the edge's vertices
    /// enlarged by theirs, so every point a vertex-to-edge solve can return is
    /// inside. An edge whose curve BndLib only samples gets no box.
    mutable std::vector<FaceBox> edge_boxes;
    mutable std::vector<char> edge_box_valid;

    double edge_lower_bound(std::size_t e, const Eigen::Vector3d& p) const {
        if (e >= edges.size()) {
            return 0.0;
        }
        if (edge_boxes.size() != edges.size()) {
            edge_boxes.assign(edges.size(), FaceBox{});
            edge_box_valid.assign(edges.size(), 0);
            for (std::size_t i = 0; i < edges.size(); ++i) {
                try {
                    if (!BRep_Tool::IsGeometric(edges[i])) {
                        continue;
                    }
                    const BRepAdaptor_Curve curve(edges[i]);
                    switch (curve.GetType()) {
                    case GeomAbs_Line:
                    case GeomAbs_Circle:
                    case GeomAbs_Ellipse:
                    case GeomAbs_Hyperbola:
                    case GeomAbs_Parabola:
                    case GeomAbs_BezierCurve:
                    case GeomAbs_BSplineCurve:
                        break;
                    default:
                        continue;
                    }
                    Bnd_Box box;
                    BRepBndLib::Add(edges[i], box, /*useTriangulation=*/Standard_False);
                    double vertex_tol = 0.0;
                    for (TopoDS_Iterator it(edges[i]); it.More(); it.Next()) {
                        const TopoDS_Vertex& v = TopoDS::Vertex(it.Value());
                        box.Add(BRep_Tool::Pnt(v));
                        vertex_tol = std::max(vertex_tol, BRep_Tool::Tolerance(v));
                    }
                    if (box.IsVoid() || box.IsOpen()) {
                        continue;
                    }
                    box.Enlarge(vertex_tol);
                    double x0 = 0.0, y0 = 0.0, z0 = 0.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
                    box.Get(x0, y0, z0, x1, y1, z1);
                    edge_boxes[i].lo = Eigen::Vector3d(x0, y0, z0);
                    edge_boxes[i].hi = Eigen::Vector3d(x1, y1, z1);
                    edge_box_valid[i] = 1;
                } catch (const Standard_Failure&) {
                    edge_box_valid[i] = 0;
                }
            }
        }
        if (edge_box_valid[e] == 0) {
            return 0.0;
        }
        const double d = box_lower_bound(p, edge_boxes[e]);
        // Leave a rounding margin so a solve's own arithmetic can never land
        // under the bound.
        return std::isfinite(d) ? std::max(0.0, d * (1.0 - 1e-9) - 1e-12) : 0.0;
    }

    /// Closest point on one edge, as a vertex-to-edge solve reports it.
    bool project_on_edge(std::size_t e, const VertexQuery& q, gp_Pnt& closest,
                         double& dist) const {
        if (e >= edges.size()) {
            return false;
        }
        DistSolution first;
        if (!distance_to_shape(q, edge_shape(e), n_threads, dist, first)) {
            return false;
        }
        closest = first.point;
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

    /// Exact closest face for `p`: the grid is walked outward, each shell's
    /// faces are solved nearest-box first, and boxed-out faces are skipped.
    /// With no grid (every face unbounded) every face is solved. The winner is
    /// order-independent, so every path agrees bit for bit.
    Winner closest(const VertexQuery& vq, const Eigen::Vector3d& p) const {
        Winner w;

        const auto consider = [&](std::size_t f) {
            gp_Pnt q;
            double d = 0.0;
            TopoDS_Shape support;
            if (!project_on_face(f, vq, q, d, &support)) {
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
        std::vector<std::pair<double, std::uint32_t>> shell; // (box lower bound, face)

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
            shell.clear();
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
                            const double lb = box_lower_bound(p, boxes[f]);
                            // `w.dist` only shrinks, so a boxed-out face stays out.
                            if (lb <= w.dist) {
                                shell.emplace_back(lb, f);
                            }
                        }
                    }
                }
            }
            // Nearest box first lets the first exact solve cull the rest.
            std::sort(shell.begin(), shell.end());
            for (const auto& [lb, f] : shell) {
                if (lb > w.dist) {
                    break;
                }
                consider(f);
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

    const VertexQuery q = make_vertex_query(p);

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
            if (!project_on_face_brute(q.vtx, index.faces[f], closest, dist, n, support)) {
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
        const BrepFaceIndex::Winner win = index.closest(q, p);
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
        BRepExtrema_DistShapeShape dss(q.vtx, *shape);
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
    const VertexQuery q = make_vertex_query(p);
    gp_Pnt closest;
    double distance = 0.0;
    if (!index.project_on_face(face_id, q, closest, distance)) {
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
    const VertexQuery q = make_vertex_query(p);
    gp_Pnt closest;
    double distance = 0.0;
    if (proj_brute_enabled()) {
        BRepExtrema_DistShapeShape dss(q.vtx, index.edges[edge_id]);
        if (!dss.IsDone() || dss.NbSolution() < 1) {
            return std::nullopt;
        }
        closest = dss.PointOnShape2(1);
        distance = static_cast<double>(dss.Value());
    } else if (!index.project_on_edge(edge_id, q, closest, distance)) {
        return std::nullopt;
    }
    if (!std::isfinite(distance)) {
        return std::nullopt;
    }
    return make_project_result(p, closest, distance, gp_Vec(0, 0, 0), CadSupportKind::kEdge,
                               edge_id, kInvalidCadSupportId);
}

double edge_distance_lower_bound(const CadModel& model, std::uint32_t edge_id,
                                 const Eigen::Vector3d& p) {
    if (model.empty() || model.shape_handle() == nullptr || proj_brute_enabled()) {
        return 0.0;
    }
    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());
    return face_index_for(model, *shape).edge_lower_bound(edge_id, p);
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

double edge_distance_lower_bound(const CadModel& /*model*/, std::uint32_t /*edge_id*/,
                                 const Eigen::Vector3d& /*p*/) {
    return 0.0;
}

std::optional<ProjectResult> project_point_on_vertex(const CadModel& /*model*/,
                                                     std::uint32_t /*vertex_id*/,
                                                     const Eigen::Vector3d& /*p*/) {
    return std::nullopt;
}

#endif // POLYMESH_WITH_OCC

} // namespace polymesh::geom
