// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/traction.hpp"

#include <Eigen/Geometry> // cross()

#include <array>
#include <format>

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

} // namespace

Eigen::VectorXd assemble_traction_load(const NodalMesh& mesh,
                                       const std::vector<SurfaceFace>& faces,
                                       const Traction& traction) {
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
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
            const double area = du.cross(dv).norm();
            const Eigen::Vector3d point = x.transpose() * shape.n;
            const Eigen::Vector3d t = traction(point);
            for (std::size_t a = 0; a < face.nodes.size(); ++a) {
                f.segment<3>(3 * static_cast<Eigen::Index>(face.nodes[a])) +=
                    shape.n[static_cast<Eigen::Index>(a)] * t * (area * qp.weight);
            }
        }
    }
    return f;
}

} // namespace polymesh::fea
