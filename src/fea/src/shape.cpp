// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/shape.hpp"

#include <array>

namespace polymesh::fea {
namespace {

// Tet edges in canonical order (nodal_mesh.hpp).
constexpr std::array<std::array<int, 2>, 6> kTetEdges{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};

// Hex corner signs (xi_i, eta_i, zeta_i) in canonical order.
constexpr std::array<std::array<double, 3>, 8> kHexCorners{{{-1, -1, -1},
                                                            {1, -1, -1},
                                                            {1, 1, -1},
                                                            {-1, 1, -1},
                                                            {-1, -1, 1},
                                                            {1, -1, 1},
                                                            {1, 1, 1},
                                                            {-1, 1, 1}}};

// Hex edges in canonical order: bottom ring, top ring, verticals.
constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
                                                        {1, 2},
                                                        {2, 3},
                                                        {3, 0},
                                                        {4, 5},
                                                        {5, 6},
                                                        {6, 7},
                                                        {7, 4},
                                                        {0, 4},
                                                        {1, 5},
                                                        {2, 6},
                                                        {3, 7}}};

ShapeEval eval_tet4(const Eigen::Vector3d& xi) {
    ShapeEval s;
    s.n.resize(4);
    s.dn.resize(4, 3);
    s.n << 1.0 - xi.sum(), xi[0], xi[1], xi[2];
    s.dn << -1, -1, -1, //
        1, 0, 0,        //
        0, 1, 0,        //
        0, 0, 1;
    return s;
}

ShapeEval eval_tet10(const Eigen::Vector3d& xi) {
    // Barycentric coordinates L and their constant gradients.
    const Eigen::Vector4d l(1.0 - xi.sum(), xi[0], xi[1], xi[2]);
    Eigen::Matrix<double, 4, 3> dl;
    dl << -1, -1, -1, //
        1, 0, 0,      //
        0, 1, 0,      //
        0, 0, 1;

    ShapeEval s;
    s.n.resize(10);
    s.dn.resize(10, 3);
    for (int i = 0; i < 4; ++i) {
        s.n[i] = l[i] * (2.0 * l[i] - 1.0);
        s.dn.row(i) = (4.0 * l[i] - 1.0) * dl.row(i);
    }
    for (int e = 0; e < 6; ++e) {
        const auto [a, b] = kTetEdges[static_cast<std::size_t>(e)];
        s.n[4 + e] = 4.0 * l[a] * l[b];
        s.dn.row(4 + e) = 4.0 * (l[a] * dl.row(b) + l[b] * dl.row(a));
    }
    return s;
}

ShapeEval eval_hex8(const Eigen::Vector3d& xi) {
    ShapeEval s;
    s.n.resize(8);
    s.dn.resize(8, 3);
    for (int i = 0; i < 8; ++i) {
        const auto& c = kHexCorners[static_cast<std::size_t>(i)];
        const double fx = 1.0 + c[0] * xi[0];
        const double fy = 1.0 + c[1] * xi[1];
        const double fz = 1.0 + c[2] * xi[2];
        s.n[i] = 0.125 * fx * fy * fz;
        s.dn(i, 0) = 0.125 * c[0] * fy * fz;
        s.dn(i, 1) = 0.125 * fx * c[1] * fz;
        s.dn(i, 2) = 0.125 * fx * fy * c[2];
    }
    return s;
}

ShapeEval eval_hex20(const Eigen::Vector3d& xi) {
    ShapeEval s;
    s.n.resize(20);
    s.dn.resize(20, 3);
    // Corners: N = 1/8 (1+xi xi_i)(1+eta eta_i)(1+zeta zeta_i)(xi xi_i + eta eta_i + zeta
    // zeta_i - 2)
    for (int i = 0; i < 8; ++i) {
        const auto& c = kHexCorners[static_cast<std::size_t>(i)];
        const double fx = 1.0 + c[0] * xi[0];
        const double fy = 1.0 + c[1] * xi[1];
        const double fz = 1.0 + c[2] * xi[2];
        const double r = c[0] * xi[0] + c[1] * xi[1] + c[2] * xi[2] - 2.0;
        s.n[i] = 0.125 * fx * fy * fz * r;
        s.dn(i, 0) = 0.125 * c[0] * fy * fz * (r + fx);
        s.dn(i, 1) = 0.125 * c[1] * fx * fz * (r + fy);
        s.dn(i, 2) = 0.125 * c[2] * fx * fy * (r + fz);
    }
    // Mid-edge nodes: the coordinate along the edge is zero at the node;
    // N = 1/4 (1 - t^2) * (1 + s s_i)(1 + q q_i) for the two non-edge axes.
    for (int e = 0; e < 12; ++e) {
        const auto [a, b] = kHexEdges[static_cast<std::size_t>(e)];
        const auto& ca = kHexCorners[static_cast<std::size_t>(a)];
        const auto& cb = kHexCorners[static_cast<std::size_t>(b)];
        // Edge axis = the one where the corner signs differ.
        int axis = 0;
        for (int d = 0; d < 3; ++d) {
            if (ca[static_cast<std::size_t>(d)] != cb[static_cast<std::size_t>(d)]) {
                axis = d;
            }
        }
        const int s1 = (axis + 1) % 3;
        const int s2 = (axis + 2) % 3;
        const double sign1 = ca[static_cast<std::size_t>(s1)];
        const double sign2 = ca[static_cast<std::size_t>(s2)];
        const double t = xi[axis];
        const double f1 = 1.0 + sign1 * xi[s1];
        const double f2 = 1.0 + sign2 * xi[s2];
        const int i = 8 + e;
        s.n[i] = 0.25 * (1.0 - t * t) * f1 * f2;
        s.dn(i, axis) = -0.5 * t * f1 * f2;
        s.dn(i, s1) = 0.25 * (1.0 - t * t) * sign1 * f2;
        s.dn(i, s2) = 0.25 * (1.0 - t * t) * f1 * sign2;
    }
    return s;
}

} // namespace

ShapeEval eval_shape(ElementType type, const Eigen::Vector3d& xi) {
    switch (type) {
    case ElementType::kTet4:
        return eval_tet4(xi);
    case ElementType::kTet10:
        return eval_tet10(xi);
    case ElementType::kHex8:
        return eval_hex8(xi);
    case ElementType::kHex20:
        return eval_hex20(xi);
    }
    throw FeaError("eval_shape: unknown element type");
}

std::vector<Eigen::Vector3d> reference_nodes(ElementType type) {
    const std::vector<Eigen::Vector3d> tet_corners{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<Eigen::Vector3d> nodes;
    switch (type) {
    case ElementType::kTet4:
        return tet_corners;
    case ElementType::kTet10:
        nodes = tet_corners;
        for (const auto& edge : kTetEdges) {
            nodes.push_back(0.5 * (tet_corners[static_cast<std::size_t>(edge[0])] +
                                   tet_corners[static_cast<std::size_t>(edge[1])]));
        }
        return nodes;
    case ElementType::kHex20:
    case ElementType::kHex8:
        for (const auto& c : kHexCorners) {
            nodes.emplace_back(c[0], c[1], c[2]);
        }
        if (type == ElementType::kHex20) {
            for (const auto& edge : kHexEdges) {
                nodes.push_back(0.5 * (nodes[static_cast<std::size_t>(edge[0])] +
                                       nodes[static_cast<std::size_t>(edge[1])]));
            }
        }
        return nodes;
    }
    throw FeaError("reference_nodes: unknown element type");
}

} // namespace polymesh::fea
