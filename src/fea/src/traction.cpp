// SPDX-License-Identifier: BSD-3-Clause
#include "fea/traction.hpp"

#include "fea/boundary_faces.hpp"

#include <Eigen/Geometry> // cross()

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::fea {
namespace {

struct FaceShape {
    Eigen::VectorXd n;
    Eigen::Matrix<double, Eigen::Dynamic, 2> dn; // dN/d(u,v)
};

// Quad corner signs in canonical order.
constexpr std::array<std::array<double, 2>, 4> kQuadCorners{
    {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
constexpr std::array<std::array<int, 2>, 4> kQuadEdges{{{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
constexpr std::array<std::array<int, 2>, 3> kTriEdges{{{0, 1}, {1, 2}, {0, 2}}};

FaceShape eval_tri(FaceType type, double u, double v) {
    const Eigen::Vector3d l(1.0 - u - v, u, v);
    Eigen::Matrix<double, 3, 2> dl;
    dl << -1, -1, //
        1, 0,     //
        0, 1;
    FaceShape s;
    if (type == FaceType::kTri3) {
        s.n = l;
        s.dn = dl;
        return s;
    }
    s.n.resize(6);
    s.dn.resize(6, 2);
    for (int i = 0; i < 3; ++i) {
        s.n[i] = l[i] * (2.0 * l[i] - 1.0);
        s.dn.row(i) = (4.0 * l[i] - 1.0) * dl.row(i);
    }
    for (int e = 0; e < 3; ++e) {
        const auto [a, b] = kTriEdges[static_cast<std::size_t>(e)];
        s.n[3 + e] = 4.0 * l[a] * l[b];
        s.dn.row(3 + e) = 4.0 * (l[a] * dl.row(b) + l[b] * dl.row(a));
    }
    return s;
}

FaceShape eval_quad(FaceType type, double u, double v) {
    FaceShape s;
    if (type == FaceType::kQuad4) {
        s.n.resize(4);
        s.dn.resize(4, 2);
        for (int i = 0; i < 4; ++i) {
            const auto& c = kQuadCorners[static_cast<std::size_t>(i)];
            s.n[i] = 0.25 * (1.0 + c[0] * u) * (1.0 + c[1] * v);
            s.dn(i, 0) = 0.25 * c[0] * (1.0 + c[1] * v);
            s.dn(i, 1) = 0.25 * (1.0 + c[0] * u) * c[1];
        }
        return s;
    }
    // quad8 serendipity.
    s.n.resize(8);
    s.dn.resize(8, 2);
    for (int i = 0; i < 4; ++i) {
        const auto& c = kQuadCorners[static_cast<std::size_t>(i)];
        const double fu = 1.0 + c[0] * u;
        const double fv = 1.0 + c[1] * v;
        const double r = c[0] * u + c[1] * v - 1.0;
        s.n[i] = 0.25 * fu * fv * r;
        s.dn(i, 0) = 0.25 * c[0] * fv * (r + fu);
        s.dn(i, 1) = 0.25 * c[1] * fu * (r + fv);
    }
    for (int e = 0; e < 4; ++e) {
        const auto [a, b] = kQuadEdges[static_cast<std::size_t>(e)];
        const auto& ca = kQuadCorners[static_cast<std::size_t>(a)];
        const auto& cb = kQuadCorners[static_cast<std::size_t>(b)];
        const int axis = ca[0] != cb[0] ? 0 : 1; // coordinate that varies along the edge
        const int other = 1 - axis;
        const double sign = ca[static_cast<std::size_t>(other)];
        const double t = axis == 0 ? u : v;
        const double q = axis == 0 ? v : u;
        const int i = 4 + e;
        s.n[i] = 0.5 * (1.0 - t * t) * (1.0 + sign * q);
        s.dn(i, axis) = -t * (1.0 + sign * q);
        s.dn(i, other) = 0.5 * (1.0 - t * t) * sign;
    }
    return s;
}

FaceShape eval_face_shape(FaceType type, double u, double v) {
    if (type == FaceType::kTri3 || type == FaceType::kTri6) {
        return eval_tri(type, u, v);
    }
    return eval_quad(type, u, v);
}

struct FaceQp {
    double u, v, weight;
};

const std::vector<FaceQp>& face_rule(FaceType type) {
    // Duffy-collapsed Gauss on the unit triangle, exact to degree ~5:
    // x = s, y = t(1-s), jacobian (1-s), 4x4 points. Weights sum to 1/2, the
    // area of the unit triangle, so they are a parameter-domain measure.
    static const std::vector<FaceQp> tri = [] {
        static constexpr std::array<double, 4> x{-0.8611363115940526, -0.3399810435848563,
                                                 0.3399810435848563, 0.8611363115940526};
        static constexpr std::array<double, 4> w{0.3478548451374538, 0.6521451548625461,
                                                 0.6521451548625461, 0.3478548451374538};
        std::vector<FaceQp> rule;
        rule.reserve(16);
        for (std::size_t i = 0; i < 4; ++i) {
            const double s = 0.5 * (x[i] + 1.0);
            for (std::size_t j = 0; j < 4; ++j) {
                const double t = 0.5 * (x[j] + 1.0);
                rule.push_back({s, t * (1.0 - s), 0.25 * w[i] * w[j] * (1.0 - s)});
            }
        }
        return rule;
    }();
    // 3x3 Gauss on [-1,1]^2; weights sum to 4, that domain's area.
    static const std::vector<FaceQp> quad = [] {
        static constexpr std::array<double, 3> x{-0.7745966692414834, 0.0,
                                                 0.7745966692414834};
        static constexpr std::array<double, 3> w{5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
        std::vector<FaceQp> rule;
        rule.reserve(9);
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                rule.push_back({x[i], x[j], w[i] * w[j]});
            }
        }
        return rule;
    }();
    return type == FaceType::kTri3 || type == FaceType::kTri6 ? tri : quad;
}

// --------------------------------------------------------------------------
// Load-region clipping
//
// A box selection names a REGION of the boundary surface. Approximating it by
// whole faces stops the loaded patch on a staircase of element edges, so the
// applied traction becomes a function of the tiling rather than of the region.
// Clipping the face quadrature to the region instead is what makes the load
// mesh-independent; see `consistent_region_load` for the measured motivation.
// --------------------------------------------------------------------------

// One reference triangle of a face's parameter domain.
struct RefTriangle {
    std::array<double, 2> a, b, c;
};

// The parameter domain as triangles: the unit triangle for tri3/tri6, the two
// halves of [-1,1]^2 for quad4/quad8. Their areas sum to the measure the
// matching `face_rule` weights sum to, so a triangle rule mapped onto them
// carries the same parameter-domain measure the unclipped rule does.
std::span<const RefTriangle> reference_triangles(FaceType type) {
    static constexpr std::array<RefTriangle, 1> kTri{{{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}}}};
    static constexpr std::array<RefTriangle, 2> kQuad{
        {{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}},
         {{-1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}}};
    if (type == FaceType::kTri3 || type == FaceType::kTri6) {
        return kTri;
    }
    return kQuad;
}

// Uniform subdivision of each reference triangle into 4^kClipLevels pieces. A
// cut is located by interpolating the box plane's value linearly along a
// sub-triangle edge, so its error is the surface's deviation from that chord:
// the face's own curvature deviation divided by 4^kClipLevels. Measured on the
// showcase sphere's tet10 skin (h = 8 mm, --load-box z >= 40 mm, 1312 candidate
// faces) the clipped patch area in m^2 converges as level 0 3.137614e-3, 1
// 3.140484e-3, 2 3.141320e-3, 3 3.141527e-3, 4 3.141575e-3, 5 3.141588e-3 —
// relative to level 5 that is -1.3e-3, -3.5e-4, -8.5e-5, -1.9e-5, -3.9e-6, a
// clean factor of four per level, and level 5 sits 1.5e-6 under the analytic cap
// area 2*pi*R*(R - 40 mm) = 3.1415927e-3. Level 3 spreads that 1.9e-5 of area
// over a 0.188 m patch edge, i.e. 3.2e-7 m or 1.8e-6 of the bounding-box
// diagonal of cut-position error: 55 times inside the 1e-4-of-bbox surface
// fidelity bar the mesher itself is held to, for 64 sub-triangles paid only on
// the faces the region actually cuts. The whole-face rule this replaces put the
// same patch at 2.948e-3 m^2, 6.2% short.
constexpr int kClipLevels = 3;
constexpr int kClipSpan = 1 << kClipLevels;
// Barycentric lattice nodes per reference triangle, and over a whole domain.
constexpr int kLatticeNodes = (kClipSpan + 1) * (kClipSpan + 2) / 2;
constexpr int kMaxLatticeNodes = 2 * kLatticeNodes;

// Index of lattice node (i, j) with i + j <= kClipSpan, rows laid out by j.
constexpr int lattice_index(int i, int j) {
    return j * (kClipSpan + 1) - (j * (j - 1)) / 2 + i;
}

// A parameter-domain polygon vertex: where it sits in (u, v) and the physical
// point the enclosing sub-triangle's affine map puts there.
struct ClipVertex {
    double u = 0.0;
    double v = 0.0;
    Eigen::Vector3d x = Eigen::Vector3d::Zero();
};

// Clipping a triangle by the six planes of a box adds at most one vertex per
// plane, so nine is the true maximum; twelve keeps it on the stack with slack.
struct ClipPoly {
    std::array<ClipVertex, 12> v{};
    int n = 0;
};

// Sutherland-Hodgman: keep the part of `in` with sign*(x[axis] - bound) >= 0.
// The physical point is interpolated affinely together with (u, v), so a vertex
// created on this plane sits exactly on it and the next plane's sign test is
// consistent — the reason the clip cannot produce a sliver from round-off.
void clip_half_space(const ClipPoly& in, ClipPoly& out, int axis, double bound, double sign) {
    out.n = 0;
    for (int i = 0; i < in.n; ++i) {
        const ClipVertex& a = in.v[static_cast<std::size_t>(i)];
        const ClipVertex& b = in.v[static_cast<std::size_t>((i + 1) % in.n)];
        const double da = sign * (a.x[axis] - bound);
        const double db = sign * (b.x[axis] - bound);
        if (da >= 0.0) {
            out.v[static_cast<std::size_t>(out.n++)] = a;
        }
        if ((da > 0.0 && db < 0.0) || (da < 0.0 && db > 0.0)) {
            const double t = da / (da - db);
            ClipVertex m;
            m.u = a.u + t * (b.u - a.u);
            m.v = a.v + t * (b.v - a.v);
            m.x = a.x + t * (b.x - a.x);
            out.v[static_cast<std::size_t>(out.n++)] = m;
        }
    }
}

// Fan-triangulate a clipped parameter polygon and append the unit-triangle rule
// mapped onto each piece. A triangle of parameter area A needs the weights
// scaled by A / (1/2) = |2A|, which is the raw cross product.
void emit_polygon_rule(const ClipPoly& poly, std::vector<FaceQp>& out) {
    const auto& tri_rule = face_rule(FaceType::kTri3);
    for (int i = 1; i + 1 < poly.n; ++i) {
        const ClipVertex& p0 = poly.v[0];
        const ClipVertex& p1 = poly.v[static_cast<std::size_t>(i)];
        const ClipVertex& p2 = poly.v[static_cast<std::size_t>(i + 1)];
        const double cross =
            (p1.u - p0.u) * (p2.v - p0.v) - (p2.u - p0.u) * (p1.v - p0.v);
        const double scale = std::abs(cross);
        if (!(scale > 0.0)) {
            continue;
        }
        for (const auto& q : tri_rule) {
            const double b1 = q.u;
            const double b2 = q.v;
            const double b0 = 1.0 - b1 - b2;
            out.push_back({b0 * p0.u + b1 * p1.u + b2 * p2.u,
                           b0 * p0.v + b1 * p1.v + b2 * p2.v, q.weight * scale});
        }
    }
}

// Quadrature for the part of one face inside `region`, in the face's own
// parameter domain. Returns the plain `face_rule` when the whole face is inside
// — so a region whose boundary misses every face integrates bit-for-bit what
// `consistent_face_load` integrated — nullptr when the face is wholly outside,
// and `scratch` otherwise.
const std::vector<FaceQp>* region_face_rule(FaceType type,
                                            const Eigen::Matrix<double, Eigen::Dynamic, 3>& x,
                                            const LoadRegion& region,
                                            std::vector<FaceQp>& scratch) {
    const auto tris = reference_triangles(type);
    // Sample the actual isoparametric surface on the barycentric lattice once.
    // Every sub-triangle reuses three of these samples, and the same samples
    // decide whether the face meets the region boundary at all.
    std::array<ClipVertex, kMaxLatticeNodes> lat{};
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const RefTriangle& tri = tris[t];
        const auto base = static_cast<int>(t) * kLatticeNodes;
        for (int j = 0; j <= kClipSpan; ++j) {
            for (int i = 0; i + j <= kClipSpan; ++i) {
                const double s = static_cast<double>(i) / kClipSpan;
                const double r = static_cast<double>(j) / kClipSpan;
                ClipVertex& p = lat[static_cast<std::size_t>(base + lattice_index(i, j))];
                p.u = tri.a[0] + s * (tri.b[0] - tri.a[0]) + r * (tri.c[0] - tri.a[0]);
                p.v = tri.a[1] + s * (tri.b[1] - tri.a[1]) + r * (tri.c[1] - tri.a[1]);
                p.x = x.transpose() * eval_face_shape(type, p.u, p.v).n;
            }
        }
    }
    const auto n_lat = static_cast<std::size_t>(tris.size()) * kLatticeNodes;
    bool straddles = false;
    for (int axis = 0; axis < 3; ++axis) {
        for (const double sign : {1.0, -1.0}) {
            const double bound = sign > 0.0 ? region.lo[axis] : region.hi[axis];
            std::size_t inside = 0;
            for (std::size_t k = 0; k < n_lat; ++k) {
                inside += sign * (lat[k].x[axis] - bound) >= 0.0 ? 1u : 0u;
            }
            if (inside == 0) {
                return nullptr; // outside one supporting half-space of a convex box
            }
            straddles = straddles || inside < n_lat;
        }
    }
    if (!straddles) {
        return &face_rule(type);
    }
    scratch.clear();
    ClipPoly a;
    ClipPoly b;
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const auto base = static_cast<int>(t) * kLatticeNodes;
        for (int j = 0; j < kClipSpan; ++j) {
            for (int i = 0; i + j < kClipSpan; ++i) {
                // The upward sub-triangle, then the downward one that completes
                // the rhombus when there is room for it.
                for (int which = 0; which < 2; ++which) {
                    if (which == 1 && i + j + 1 >= kClipSpan) {
                        break;
                    }
                    a.n = 3;
                    if (which == 0) {
                        a.v[0] = lat[static_cast<std::size_t>(base + lattice_index(i, j))];
                        a.v[1] = lat[static_cast<std::size_t>(base + lattice_index(i + 1, j))];
                        a.v[2] = lat[static_cast<std::size_t>(base + lattice_index(i, j + 1))];
                    } else {
                        a.v[0] = lat[static_cast<std::size_t>(base + lattice_index(i + 1, j))];
                        a.v[1] =
                            lat[static_cast<std::size_t>(base + lattice_index(i + 1, j + 1))];
                        a.v[2] = lat[static_cast<std::size_t>(base + lattice_index(i, j + 1))];
                    }
                    for (int axis = 0; axis < 3 && a.n > 2; ++axis) {
                        clip_half_space(a, b, axis, region.lo[axis], 1.0);
                        clip_half_space(b, a, axis, region.hi[axis], -1.0);
                    }
                    if (a.n > 2) {
                        emit_polygon_rule(a, scratch);
                    }
                }
            }
        }
    }
    return &scratch;
}

// Shared face-quadrature walk. `sink(face, shape, dS, point)` is called once
// per quadrature point, with dS the physical area weight. A non-null `region`
// clips each face's quadrature to that box.
template <class Sink>
void integrate_faces(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                     const LoadRegion* region, Sink&& sink) {
    const auto num_mesh_nodes = static_cast<std::uint32_t>(mesh.nodes.size());
    std::vector<FaceQp> scratch;
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const auto& face = faces[fi];
        const auto expected = static_cast<std::size_t>(face_num_nodes(face.type));
        if (face.nodes.size() != expected) {
            throw FeaError(std::format("traction face {} has {} nodes, expected {}", fi,
                                       face.nodes.size(), expected));
        }
        Eigen::Matrix<double, Eigen::Dynamic, 3> x(face.nodes.size(), 3);
        for (std::size_t a = 0; a < face.nodes.size(); ++a) {
            if (face.nodes[a] >= num_mesh_nodes) {
                throw FeaError(
                    std::format("traction face {} references out-of-range node", fi));
            }
            x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[face.nodes[a]].transpose();
        }
        const std::vector<FaceQp>* rule =
            region != nullptr ? region_face_rule(face.type, x, *region, scratch)
                              : &face_rule(face.type);
        if (rule == nullptr) {
            continue;
        }
        for (const auto& qp : *rule) {
            const auto shape = eval_face_shape(face.type, qp.u, qp.v);
            const Eigen::Vector3d du = (shape.dn.col(0).transpose() * x).transpose();
            const Eigen::Vector3d dv = (shape.dn.col(1).transpose() * x).transpose();
            const double dS = du.cross(dv).norm() * qp.weight;
            const Eigen::Vector3d point = x.transpose() * shape.n;
            sink(face, shape, dS, point);
        }
    }
}

// Mid-edge node of every quadratic element edge, keyed by its two corners.
// Mirrors the canonical edge order p_elevate appends mid-nodes in.
std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
quadratic_edge_mids(const NodalMesh& mesh) {
    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5},
         {2, 6}, {3, 7}}};
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mids;
    for (const auto& el : mesh.elements) {
        const bool tet10 = el.type == ElementType::kTet10;
        const bool hex20 = el.type == ElementType::kHex20;
        if (!tet10 && !hex20) {
            continue;
        }
        const std::size_t n_corner = tet10 ? 4 : 8;
        const std::size_t n_mid = tet10 ? 6 : 12;
        if (el.nodes.size() < n_corner + n_mid) {
            continue;
        }
        for (std::size_t m = 0; m < n_mid; ++m) {
            const auto& e = tet10 ? kTetEdges[m] : kHexEdges[m];
            const auto a = el.nodes[static_cast<std::size_t>(e[0])];
            const auto b = el.nodes[static_cast<std::size_t>(e[1])];
            mids[std::minmax(a, b)] = el.nodes[n_corner + m];
        }
    }
    return mids;
}

// Nodal load vector for `traction` over `faces`, restricted to `region` when it
// is non-null.
Eigen::VectorXd assemble(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                         const LoadRegion* region, const Traction& traction) {
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    integrate_faces(mesh, faces, region,
                    [&](const SurfaceFace& face, const FaceShape& shape, double dS,
                        const Eigen::Vector3d& point) {
                        const Eigen::Vector3d t = traction(point);
                        for (std::size_t a = 0; a < face.nodes.size(); ++a) {
                            f.segment<3>(3 * static_cast<Eigen::Index>(face.nodes[a])) +=
                                shape.n[static_cast<Eigen::Index>(a)] * t * dS;
                        }
                    });
    return f;
}

double integrated_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                       const LoadRegion* region) {
    double area = 0.0;
    integrate_faces(mesh, faces, region,
                    [&](const SurfaceFace&, const FaceShape&, double dS,
                        const Eigen::Vector3d&) { area += dS; });
    return area;
}

// Uniform traction over `faces` (clipped to `region` when non-null) whose
// resultant is exactly `total_force`.
ConsistentLoad consistent_load(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                               const LoadRegion* region, const Eigen::Vector3d& total_force) {
    ConsistentLoad out;
    out.loads = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    out.area = integrated_area(mesh, faces, region);
    out.conservation_error = total_force.norm();
    if (!(out.area > 0.0)) {
        return out;
    }
    const Eigen::Vector3d t = total_force / out.area;
    out.loads = assemble(mesh, faces, region, [&t](const Eigen::Vector3d&) { return t; });
    // The quadrature is exact for the (bi)linear/quadratic partition of unity,
    // so the nodal sum already equals t*area; rescale only to kill round-off
    // accumulated over many faces, keeping total force conserved exactly.
    Eigen::Vector3d sum = Eigen::Vector3d::Zero();
    for (Eigen::Index i = 0; i + 2 < out.loads.size(); i += 3) {
        sum += out.loads.segment<3>(i);
    }
    const double s2 = sum.squaredNorm();
    if (s2 > 0.0) {
        out.loads *= total_force.dot(sum) / s2;
        sum = Eigen::Vector3d::Zero();
        for (Eigen::Index i = 0; i + 2 < out.loads.size(); i += 3) {
            sum += out.loads.segment<3>(i);
        }
    }
    out.resultant = sum;
    out.conservation_error = (sum - total_force).norm();
    return out;
}

} // namespace

Eigen::VectorXd assemble_traction_load(const NodalMesh& mesh,
                                       const std::vector<SurfaceFace>& faces,
                                       const Traction& traction) {
    return assemble(mesh, faces, nullptr, traction);
}

double integrated_face_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces) {
    return integrated_area(mesh, faces, nullptr);
}

double integrated_region_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                              const LoadRegion& region) {
    return integrated_area(mesh, faces, &region);
}

std::vector<SurfaceFace> boundary_surface_faces(const NodalMesh& mesh) {
    const auto quads = extract_boundary_faces(mesh);
    const auto mids = quadratic_edge_mids(mesh);
    std::vector<SurfaceFace> out;
    out.reserve(quads.size());
    // Contract order: tri6 edges (0,1),(1,2),(0,2); quad8 edges (0,1),(1,2),(2,3),(3,0).
    static constexpr std::array<std::array<int, 2>, 3> kTriFaceEdges{{{0, 1}, {1, 2}, {0, 2}}};
    static constexpr std::array<std::array<int, 2>, 4> kQuadFaceEdges{
        {{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
    for (const auto& q : quads) {
        SurfaceFace f;
        const bool tri = q[2] == q[3];
        f.type = tri ? FaceType::kTri3 : FaceType::kQuad4;
        f.nodes.assign(q.begin(), q.begin() + (tri ? 3 : 4));
        if (!mids.empty()) {
            const std::size_t n_edges = tri ? 3 : 4;
            std::vector<std::uint32_t> mid_nodes;
            mid_nodes.reserve(n_edges);
            for (std::size_t e = 0; e < n_edges; ++e) {
                const auto& ep = tri ? kTriFaceEdges[e] : kQuadFaceEdges[e];
                const auto it = mids.find(std::minmax(f.nodes[static_cast<std::size_t>(ep[0])],
                                                      f.nodes[static_cast<std::size_t>(ep[1])]));
                if (it == mids.end()) {
                    mid_nodes.clear();
                    break;
                }
                mid_nodes.push_back(it->second);
            }
            if (mid_nodes.size() == n_edges) {
                f.type = tri ? FaceType::kTri6 : FaceType::kQuad8;
                f.nodes.insert(f.nodes.end(), mid_nodes.begin(), mid_nodes.end());
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

SurfaceTessellation tessellate_boundary_surface(const NodalMesh& mesh,
                                                int subdivisions) {
    subdivisions = std::clamp(subdivisions, 1, 16);
    SurfaceTessellation out;
    const auto faces = boundary_surface_faces(mesh);
    const auto sample = [&](const SurfaceFace& face, double u, double v) {
        const auto shape = eval_face_shape(face.type, u, v);
        SurfaceSample value;
        value.count = static_cast<std::uint8_t>(face.nodes.size());
        for (std::size_t i = 0; i < face.nodes.size(); ++i) {
            value.source_nodes[i] = face.nodes[i];
            value.weights[i] = shape.n[static_cast<Eigen::Index>(i)];
            value.position += value.weights[i] * mesh.nodes[face.nodes[i]];
        }
        out.samples.push_back(value);
        return static_cast<std::uint32_t>(out.samples.size() - 1);
    };

    for (const auto& face : faces) {
        const bool quadratic =
            face.type == FaceType::kTri6 || face.type == FaceType::kQuad8;
        const int n = quadratic ? subdivisions : 1;
        if (face.type == FaceType::kTri3 || face.type == FaceType::kTri6) {
            std::vector<std::vector<std::uint32_t>> grid(
                static_cast<std::size_t>(n + 1));
            for (int i = 0; i <= n; ++i) {
                auto& row = grid[static_cast<std::size_t>(i)];
                row.reserve(static_cast<std::size_t>(n - i + 1));
                for (int j = 0; j <= n - i; ++j) {
                    row.push_back(sample(face, static_cast<double>(i) / n,
                                         static_cast<double>(j) / n));
                }
            }
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n - i; ++j) {
                    const auto a = grid[static_cast<std::size_t>(i)]
                                      [static_cast<std::size_t>(j)];
                    const auto b = grid[static_cast<std::size_t>(i + 1)]
                                      [static_cast<std::size_t>(j)];
                    const auto c = grid[static_cast<std::size_t>(i)]
                                      [static_cast<std::size_t>(j + 1)];
                    out.triangles.push_back({a, b, c});
                    if (j + 1 < n - i) {
                        const auto d = grid[static_cast<std::size_t>(i + 1)]
                                          [static_cast<std::size_t>(j + 1)];
                        out.triangles.push_back({b, d, c});
                    }
                }
            }
            continue;
        }

        std::vector<std::uint32_t> grid;
        grid.reserve(static_cast<std::size_t>((n + 1) * (n + 1)));
        for (int i = 0; i <= n; ++i) {
            const double u = -1.0 + 2.0 * static_cast<double>(i) / n;
            for (int j = 0; j <= n; ++j) {
                const double v = -1.0 + 2.0 * static_cast<double>(j) / n;
                grid.push_back(sample(face, u, v));
            }
        }
        const auto at = [&](int i, int j) {
            return grid[static_cast<std::size_t>(i * (n + 1) + j)];
        };
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const auto a = at(i, j);
                const auto b = at(i + 1, j);
                const auto c = at(i, j + 1);
                const auto d = at(i + 1, j + 1);
                out.triangles.push_back({a, b, d});
                out.triangles.push_back({a, d, c});
            }
        }
    }
    return out;
}

Eigen::Vector3d surface_face_normal(const NodalMesh& mesh, const SurfaceFace& face) {
    if (face.nodes.size() < 3) {
        return Eigen::Vector3d::Zero();
    }
    const std::size_t n_corner = (face.type == FaceType::kQuad4 || face.type == FaceType::kQuad8)
                                     ? 4u
                                     : 3u;
    // Newell's method over the corner loop: robust for non-planar quads.
    Eigen::Vector3d n = Eigen::Vector3d::Zero();
    for (std::size_t a = 0; a < n_corner; ++a) {
        const Eigen::Vector3d& p = mesh.nodes[face.nodes[a]];
        const Eigen::Vector3d& q = mesh.nodes[face.nodes[(a + 1) % n_corner]];
        n += p.cross(q);
    }
    const double len = n.norm();
    return len > 0.0 ? Eigen::Vector3d(n / len) : Eigen::Vector3d::Zero();
}

std::vector<SurfaceFace> faces_within(const std::vector<SurfaceFace>& faces,
                                      std::span<const std::uint32_t> nodes) {
    std::vector<std::uint32_t> sorted(nodes.begin(), nodes.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<SurfaceFace> out;
    for (const auto& f : faces) {
        const bool all_in = std::all_of(f.nodes.begin(), f.nodes.end(), [&](std::uint32_t n) {
            return std::binary_search(sorted.begin(), sorted.end(), n);
        });
        if (all_in) {
            out.push_back(f);
        }
    }
    return out;
}

std::vector<SurfaceFace> faces_touching(const NodalMesh& mesh,
                                        const std::vector<SurfaceFace>& faces,
                                        const LoadRegion& region) {
    std::vector<SurfaceFace> out;
    for (const auto& f : faces) {
        Eigen::Vector3d lo = Eigen::Vector3d::Constant(
            std::numeric_limits<double>::infinity());
        Eigen::Vector3d hi = -lo;
        for (const auto n : f.nodes) {
            const Eigen::Vector3d& p = mesh.nodes[n];
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        if ((lo.array() <= region.hi.array()).all() &&
            (hi.array() >= region.lo.array()).all()) {
            out.push_back(f);
        }
    }
    return out;
}

ConsistentLoad consistent_face_load(const NodalMesh& mesh,
                                    const std::vector<SurfaceFace>& faces,
                                    const Eigen::Vector3d& total_force) {
    return consistent_load(mesh, faces, nullptr, total_force);
}

ConsistentLoad consistent_region_load(const NodalMesh& mesh,
                                      const std::vector<SurfaceFace>& faces,
                                      const LoadRegion& region,
                                      const Eigen::Vector3d& total_force) {
    return consistent_load(mesh, faces, &region, total_force);
}

} // namespace polymesh::fea
