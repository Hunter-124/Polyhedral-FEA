// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_topology.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <utility>

#ifdef POLYMESH_WITH_OCC

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#endif

namespace polymesh::geom {
namespace {

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

    return topo;
}

#else // !POLYMESH_WITH_OCC

CadTopology extract_topology(const CadModel& /*model*/, int /*samples_per_edge*/) {
    throw GeomError("OpenCASCADE not enabled");
}

#endif

std::optional<ClosestEdgeQuery> closest_edge(const CadTopology& topo,
                                             const Eigen::Vector3d& p) {
    if (topo.edges.empty()) {
        return std::nullopt;
    }
    ClosestEdgeQuery best;
    best.distance = std::numeric_limits<double>::infinity();
    bool found = false;
    for (const CadEdge& e : topo.edges) {
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

double edge_profile_hausdorff(const CadTopology& topo,
                              const std::vector<Eigen::Vector3d>& polyline) {
    if (polyline.empty() || topo.edges.empty()) {
        return 0.0;
    }
    double max_d = 0.0;
    for (const Eigen::Vector3d& p : polyline) {
        const auto q = closest_edge(topo, p);
        if (q) {
            max_d = std::max(max_d, q->distance);
        }
    }
    return max_d;
}

} // namespace polymesh::geom
