// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_topology.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#endif

namespace polymesh::geom {
namespace {

constexpr double kPi = 3.14159265358979323846;
/// Default: edges with dihedral within this of flat (π) are kSmooth.
constexpr double kSharpFromFlatRad = 25.0 * kPi / 180.0;
constexpr double kEps = 1e-15;

double dist2(const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
    return (a - b).squaredNorm();
}

/// Closest point on segment ab to p; returns (point, t in [0,1], dist2).
void closest_on_segment(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                        const Eigen::Vector3d& p, Eigen::Vector3d& out, double& t,
                        double& d2) {
    const Eigen::Vector3d ab = b - a;
    const double ab2 = ab.squaredNorm();
    if (ab2 <= 0.0) {
        out = a;
        t = 0.0;
        d2 = dist2(p, a);
        return;
    }
    t = (p - a).dot(ab) / ab2;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    out = a + t * ab;
    d2 = dist2(p, out);
}

bool edge_passes_filter(const CadEdge& e, bool sharp_only) {
    if (!sharp_only) {
        return true;
    }
    return e.feature == CadEdgeFeature::kSharp;
}

/// Menger curvature κ = 4·area / (ab·bc·ca) at sample index i (clamped interior).
double menger_kappa(const std::vector<Eigen::Vector3d>& samples, std::size_t i) {
    if (samples.size() < 3) {
        return 0.0;
    }
    // Clamp index to an interior sample when possible.
    std::size_t j = i;
    if (j == 0) {
        j = 1;
    }
    if (j + 1 >= samples.size()) {
        j = samples.size() - 2;
    }
    if (j == 0 || j + 1 >= samples.size()) {
        return 0.0;
    }
    const Eigen::Vector3d ab_v = samples[j] - samples[j - 1];
    const Eigen::Vector3d bc_v = samples[j + 1] - samples[j];
    const Eigen::Vector3d ca_v = samples[j - 1] - samples[j + 1];
    const double ab = ab_v.norm();
    const double bc = bc_v.norm();
    const double ca = ca_v.norm();
    if (ab < kEps || bc < kEps || ca < kEps) {
        return 0.0;
    }
    const double area = 0.5 * ab_v.cross(bc_v).norm();
    return 4.0 * area / (ab * bc * ca);
}

std::optional<ClosestEdgeQuery> closest_edge_impl(const CadTopology& topo,
                                                  const Eigen::Vector3d& p,
                                                  bool sharp_only) {
    if (topo.edges.empty()) {
        return std::nullopt;
    }
    ClosestEdgeQuery best;
    best.distance = std::numeric_limits<double>::infinity();
    bool found = false;
    for (const CadEdge& e : topo.edges) {
        if (!edge_passes_filter(e, sharp_only)) {
            continue;
        }
        if (e.samples.size() < 2) {
            continue;
        }
        double acc_len = 0.0;
        const double inv_L = (e.length > 0.0) ? (1.0 / e.length) : 0.0;
        for (std::size_t i = 1; i < e.samples.size(); ++i) {
            Eigen::Vector3d q;
            double t_seg = 0.0;
            double d2 = 0.0;
            closest_on_segment(e.samples[i - 1], e.samples[i], p, q, t_seg, d2);
            const double d = std::sqrt(d2);
            if (d < best.distance) {
                best.distance = d;
                best.closest = q;
                best.edge_id = e.id;
                const double seg_len = (e.samples[i] - e.samples[i - 1]).norm();
                best.t = (acc_len + t_seg * seg_len) * inv_L;
                found = true;
            }
            acc_len += (e.samples[i] - e.samples[i - 1]).norm();
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return best;
}

#ifdef POLYMESH_WITH_OCC

/// Outward-ish face normal at the 2d midpoint of edge-on-face pcurve.
bool face_normal_at_edge_mid(const TopoDS_Face& face, const TopoDS_Edge& edge,
                             gp_Vec& normal_out) {
    Standard_Real u0 = 0.0;
    Standard_Real u1 = 0.0;
    const Handle(Geom2d_Curve) c2d = BRep_Tool::CurveOnSurface(edge, face, u0, u1);
    if (c2d.IsNull()) {
        return false;
    }
    const Standard_Real um = 0.5 * (u0 + u1);
    const gp_Pnt2d uv = c2d->Value(um);
    BRepGProp_Face prop(face);
    gp_Pnt pnt;
    gp_Vec n;
    // BRepGProp_Face::Normal already respects face orientation.
    prop.Normal(uv.X(), uv.Y(), pnt, n);
    if (n.Magnitude() < 1e-14) {
        return false;
    }
    n.Normalize();
    normal_out = n;
    return true;
}

/// Interior-style dihedral from two outward normals: π = flat, 0 = knife edge.
double dihedral_from_normals(const gp_Vec& n1, const gp_Vec& n2) {
    double c = n1.Dot(n2);
    if (c > 1.0) {
        c = 1.0;
    } else if (c < -1.0) {
        c = -1.0;
    }
    // angle between normals θ; coplanar same-side → θ=0 → dihedral π
    return kPi - std::acos(c);
}

void classify_edges(const TopoDS_Shape& shape,
                    const TopTools_IndexedMapOfShape& emap,
                    const std::map<int, std::uint32_t>& emap_to_id,
                    CadTopology& topo) {
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);

    for (int i = 1; i <= emap.Extent(); ++i) {
        const auto id_it = emap_to_id.find(i);
        if (id_it == emap_to_id.end()) {
            continue; // degenerated edge skipped during extract
        }
        CadEdge& ce = topo.edges[id_it->second];
        const TopoDS_Edge& e = TopoDS::Edge(emap(i));

        if (!edge_faces.Contains(e)) {
            ce.feature = CadEdgeFeature::kSharp;
            ce.dihedral_rad = 0.0;
            continue;
        }
        const TopTools_ListOfShape& flist = edge_faces.FindFromKey(e);

        // Collect ancestor faces (may list same face twice for a seam).
        std::vector<TopoDS_Face> faces;
        faces.reserve(static_cast<std::size_t>(flist.Extent()));
        for (TopTools_ListIteratorOfListOfShape it(flist); it.More(); it.Next()) {
            faces.push_back(TopoDS::Face(it.Value()));
        }

        if (faces.empty()) {
            ce.feature = CadEdgeFeature::kSharp;
            ce.dihedral_rad = 0.0;
            continue;
        }

        // Seam: same topological face on both sides of the edge.
        if (faces.size() >= 2 && faces[0].IsSame(faces[1])) {
            ce.feature = CadEdgeFeature::kSeam;
            ce.dihedral_rad = 0.0;
            continue;
        }
        // Also treat a single listed face that uses the edge twice (closed
        // periodic) as a seam when the edge is only on one face index but
        // appears twice in the face's wire — MapShapesAndAncestors usually
        // lists the face twice; if only once, leave as open boundary.

        if (faces.size() == 1) {
            // Open-shell boundary → protect as sharp.
            ce.feature = CadEdgeFeature::kSharp;
            ce.dihedral_rad = 0.0;
            continue;
        }

        // ≥2 distinct faces: dihedral from face normals at edge midpoint.
        const TopoDS_Face& f0 = faces[0];
        TopoDS_Face f1 = faces[1];
        for (std::size_t k = 1; k < faces.size(); ++k) {
            if (!faces[k].IsSame(f0)) {
                f1 = faces[k];
                break;
            }
        }
        if (f1.IsSame(f0)) {
            ce.feature = CadEdgeFeature::kSeam;
            ce.dihedral_rad = 0.0;
            continue;
        }

        gp_Vec n0;
        gp_Vec n1;
        if (!face_normal_at_edge_mid(f0, e, n0) ||
            !face_normal_at_edge_mid(f1, e, n1)) {
            // Fallback: treat as sharp if normals unavailable.
            ce.feature = CadEdgeFeature::kSharp;
            ce.dihedral_rad = 0.0;
            continue;
        }

        const double dih = dihedral_from_normals(n0, n1);
        ce.dihedral_rad = dih;
        // |dihedral − π| < 25° → nearly coplanar / G1-ish → smooth.
        const double from_flat = std::abs(dih - kPi);
        if (from_flat < kSharpFromFlatRad) {
            ce.feature = CadEdgeFeature::kSmooth;
        } else {
            ce.feature = CadEdgeFeature::kSharp;
        }
    }
}

#endif // POLYMESH_WITH_OCC

} // namespace

#ifdef POLYMESH_WITH_OCC

CadTopology extract_topology(const CadModel& model, int samples_per_edge) {
    if (model.empty() || model.shape_handle() == nullptr) {
        throw GeomError("extract_topology: empty CadModel");
    }
    if (samples_per_edge < 1) {
        samples_per_edge = 1;
    }

    const auto* shape = static_cast<const TopoDS_Shape*>(model.shape_handle());

    CadTopology topo;
    TopTools_IndexedMapOfShape vmap;
    TopTools_IndexedMapOfShape emap;
    TopTools_IndexedMapOfShape fmap;
    TopExp::MapShapes(*shape, TopAbs_VERTEX, vmap);
    TopExp::MapShapes(*shape, TopAbs_EDGE, emap);
    TopExp::MapShapes(*shape, TopAbs_FACE, fmap);

    topo.vertices.reserve(static_cast<std::size_t>(vmap.Extent()));
    for (int i = 1; i <= vmap.Extent(); ++i) {
        const TopoDS_Vertex& v = TopoDS::Vertex(vmap(i));
        const gp_Pnt p = BRep_Tool::Pnt(v);
        CadVertex cv;
        cv.id = static_cast<std::uint32_t>(i - 1);
        cv.position = Eigen::Vector3d(p.X(), p.Y(), p.Z());
        topo.vertices.push_back(cv);
    }

    // Map OCC vertex id (1-based map index) → our id.
    auto vertex_id_of = [&](const TopoDS_Vertex& v) -> std::uint32_t {
        const int idx = vmap.FindIndex(v);
        if (idx <= 0) {
            return 0;
        }
        return static_cast<std::uint32_t>(idx - 1);
    };

    topo.edges.reserve(static_cast<std::size_t>(emap.Extent()));
    for (int i = 1; i <= emap.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(emap(i));
        if (BRep_Tool::Degenerated(e)) {
            continue;
        }
        TopoDS_Vertex va, vb;
        TopExp::Vertices(e, va, vb);
        CadEdge ce;
        ce.id = static_cast<std::uint32_t>(topo.edges.size());
        ce.v0 = vertex_id_of(va);
        ce.v1 = vertex_id_of(vb);

        BRepAdaptor_Curve curve(e);
        const double u0 = curve.FirstParameter();
        const double u1 = curve.LastParameter();
        const int n_seg = samples_per_edge + 1; // interior samples + end
        ce.samples.reserve(static_cast<std::size_t>(n_seg + 1));
        for (int s = 0; s <= n_seg; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n_seg);
            const double u = u0 + t * (u1 - u0);
            const gp_Pnt p = curve.Value(u);
            ce.samples.emplace_back(p.X(), p.Y(), p.Z());
        }
        ce.length = 0.0;
        for (std::size_t k = 1; k < ce.samples.size(); ++k) {
            ce.length += (ce.samples[k] - ce.samples[k - 1]).norm();
        }
        topo.edges.push_back(std::move(ce));
    }

    // Face → edge membership via TopExp_Explorer on each face.
    std::map<int, std::uint32_t> emap_to_id; // OCC edge map index → CadEdge id
    {
        std::uint32_t eid = 0;
        for (int i = 1; i <= emap.Extent(); ++i) {
            const TopoDS_Edge& e = TopoDS::Edge(emap(i));
            if (BRep_Tool::Degenerated(e)) {
                continue;
            }
            emap_to_id[i] = eid++;
        }
    }

    topo.faces.reserve(static_cast<std::size_t>(fmap.Extent()));
    for (int i = 1; i <= fmap.Extent(); ++i) {
        const TopoDS_Face& f = TopoDS::Face(fmap(i));
        CadFace cf;
        cf.id = static_cast<std::uint32_t>(topo.faces.size());

        BRepAdaptor_Surface surf(f, Standard_True);
        switch (surf.GetType()) {
        case GeomAbs_Plane:
            cf.kind = CadSurfaceKind::kPlane;
            break;
        case GeomAbs_Cylinder:
            cf.kind = CadSurfaceKind::kCylinder;
            break;
        case GeomAbs_Sphere:
            cf.kind = CadSurfaceKind::kSphere;
            break;
        case GeomAbs_Cone:
            cf.kind = CadSurfaceKind::kCone;
            break;
        case GeomAbs_Torus:
            cf.kind = CadSurfaceKind::kTorus;
            break;
        default:
            cf.kind = CadSurfaceKind::kOther;
            break;
        }

        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        cf.area = props.Mass();

        for (TopExp_Explorer exp(f, TopAbs_EDGE); exp.More(); exp.Next()) {
            const TopoDS_Edge& e = TopoDS::Edge(exp.Current());
            if (BRep_Tool::Degenerated(e)) {
                continue;
            }
            const int idx = emap.FindIndex(e);
            auto it = emap_to_id.find(idx);
            if (it != emap_to_id.end()) {
                cf.edge_ids.push_back(it->second);
            }
        }
        topo.faces.push_back(std::move(cf));
    }

    // Sharp / smooth / seam classification after edges + faces exist.
    classify_edges(*shape, emap, emap_to_id, topo);

    return topo;
}

#else // !POLYMESH_WITH_OCC

CadTopology extract_topology(const CadModel& /*model*/, int /*samples_per_edge*/) {
    throw GeomError("OpenCASCADE not enabled");
}

#endif

std::optional<ClosestEdgeQuery> closest_edge(const CadTopology& topo,
                                             const Eigen::Vector3d& p) {
    return closest_edge_impl(topo, p, /*sharp_only=*/false);
}

std::optional<ClosestEdgeQuery> closest_edge(const CadTopology& topo,
                                             const Eigen::Vector3d& p,
                                             bool sharp_only) {
    return closest_edge_impl(topo, p, sharp_only);
}

double edge_profile_hausdorff(const CadTopology& topo,
                              const std::vector<Eigen::Vector3d>& polyline) {
    return edge_profile_hausdorff_filtered(topo, polyline, /*sharp_only=*/false);
}

double edge_profile_hausdorff_filtered(const CadTopology& topo,
                                       const std::vector<Eigen::Vector3d>& polyline,
                                       bool sharp_only) {
    if (polyline.empty() || topo.edges.empty()) {
        return 0.0;
    }
    double max_d = 0.0;
    bool any = false;
    for (const Eigen::Vector3d& p : polyline) {
        const auto q = closest_edge_impl(topo, p, sharp_only);
        if (q) {
            max_d = std::max(max_d, q->distance);
            any = true;
        }
    }
    return any ? max_d : 0.0;
}

ChordalEdgeMetrics chordal_edge_metrics(
    const CadTopology& topo,
    const std::vector<Eigen::Vector3d>& mesh_feature_polyline, double h,
    bool sharp_edges_only) {
    ChordalEdgeMetrics out;
    if (mesh_feature_polyline.size() < 2 || topo.edges.empty()) {
        return out;
    }
    const double h_safe = std::max(h, kEps);
    out.n_segments = static_cast<int>(mesh_feature_polyline.size() - 1);

    // Pointwise Hausdorff (all polyline vertices).
    out.hausdorff =
        edge_profile_hausdorff_filtered(topo, mesh_feature_polyline, sharp_edges_only);
    out.hausdorff_over_h = out.hausdorff / h_safe;

    // Per-segment midpoint residual + efficiency vs theoretical chordal sagitta.
    for (std::size_t i = 1; i < mesh_feature_polyline.size(); ++i) {
        const Eigen::Vector3d mid =
            0.5 * (mesh_feature_polyline[i - 1] + mesh_feature_polyline[i]);
        const auto q = closest_edge_impl(topo, mid, sharp_edges_only);
        if (!q) {
            continue;
        }
        out.max_chordal = std::max(out.max_chordal, q->distance);

        double kappa = 0.0;
        if (q->edge_id < topo.edges.size()) {
            const CadEdge& ce = topo.edges[q->edge_id];
            // Sample index near arc-length fraction t.
            std::size_t si = 0;
            if (ce.samples.size() >= 2) {
                si = static_cast<std::size_t>(
                    std::llround(q->t * static_cast<double>(ce.samples.size() - 1)));
                if (si >= ce.samples.size()) {
                    si = ce.samples.size() - 1;
                }
            }
            kappa = menger_kappa(ce.samples, si);
        }
        if (kappa > 1e-12) {
            const double theory = (h_safe * h_safe * kappa) / 8.0;
            const double denom = std::max(kEps, theory);
            const double eff = q->distance / denom;
            out.max_efficiency = std::max(out.max_efficiency, eff);
        }
    }
    return out;
}

CadEdgeClassCounts count_edge_features(const CadTopology& topo) {
    CadEdgeClassCounts c;
    for (const CadEdge& e : topo.edges) {
        switch (e.feature) {
        case CadEdgeFeature::kSharp:
            ++c.n_sharp;
            break;
        case CadEdgeFeature::kSmooth:
            ++c.n_smooth;
            break;
        case CadEdgeFeature::kSeam:
            ++c.n_seam;
            break;
        }
    }
    return c;
}

} // namespace polymesh::geom
