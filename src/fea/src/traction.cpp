// SPDX-License-Identifier: BSD-3-Clause
#include "fea/traction.hpp"

#include "fea/boundary_faces.hpp"

#include <Eigen/Geometry> // cross()

#include <algorithm>
#include <array>
#include <format>
#include <map>
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

std::vector<FaceQp> face_rule(FaceType type) {
    if (type == FaceType::kTri3 || type == FaceType::kTri6) {
        // Duffy-collapsed Gauss on the unit triangle, exact to degree ~5:
        // x = s, y = t(1-s), jacobian (1-s), 4x4 points.
        static constexpr std::array<double, 4> x{-0.8611363115940526, -0.3399810435848563,
                                                 0.3399810435848563, 0.8611363115940526};
        static constexpr std::array<double, 4> w{0.3478548451374538, 0.6521451548625461,
                                                 0.6521451548625461, 0.3478548451374538};
        std::vector<FaceQp> rule;
        for (std::size_t i = 0; i < 4; ++i) {
            const double s = 0.5 * (x[i] + 1.0);
            for (std::size_t j = 0; j < 4; ++j) {
                const double t = 0.5 * (x[j] + 1.0);
                rule.push_back({s, t * (1.0 - s), 0.25 * w[i] * w[j] * (1.0 - s)});
            }
        }
        return rule;
    }
    // 3x3 Gauss on [-1,1]^2.
    static constexpr std::array<double, 3> x{-0.7745966692414834, 0.0, 0.7745966692414834};
    static constexpr std::array<double, 3> w{5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
    std::vector<FaceQp> rule;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            rule.push_back({x[i], x[j], w[i] * w[j]});
        }
    }
    return rule;
}

// Shared face-quadrature walk. `sink(face, shape, dS, point)` is called once
// per quadrature point, with dS the physical area weight.
template <class Sink>
void integrate_faces(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces, Sink&& sink) {
    const auto num_mesh_nodes = static_cast<std::uint32_t>(mesh.nodes.size());
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
        for (const auto& qp : face_rule(face.type)) {
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

} // namespace

Eigen::VectorXd assemble_traction_load(const NodalMesh& mesh,
                                       const std::vector<SurfaceFace>& faces,
                                       const Traction& traction) {
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    integrate_faces(mesh, faces,
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

double integrated_face_area(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces) {
    double area = 0.0;
    integrate_faces(mesh, faces,
                    [&](const SurfaceFace&, const FaceShape&, double dS,
                        const Eigen::Vector3d&) { area += dS; });
    return area;
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

ConsistentLoad consistent_face_load(const NodalMesh& mesh,
                                    const std::vector<SurfaceFace>& faces,
                                    const Eigen::Vector3d& total_force) {
    ConsistentLoad out;
    out.loads = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    out.area = integrated_face_area(mesh, faces);
    out.conservation_error = total_force.norm();
    if (!(out.area > 0.0)) {
        return out;
    }
    const Eigen::Vector3d t = total_force / out.area;
    out.loads = assemble_traction_load(mesh, faces, [&t](const Eigen::Vector3d&) { return t; });
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

} // namespace polymesh::fea
