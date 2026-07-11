// SPDX-License-Identifier: BSD-3-Clause
#include "fea/hierarchical.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace polymesh::fea {
namespace {

// Legendre polynomial P_n(x) on [-1, 1] by the three-term recurrence.
double legendre_p(int n, double x) {
    if (n == 0) {
        return 1.0;
    }
    double p_prev = 1.0; // P_0
    double p_cur = x;    // P_1
    for (int k = 1; k < n; ++k) {
        const double p_next =
            (static_cast<double>(2 * k + 1) * x * p_cur - static_cast<double>(k) * p_prev) /
            static_cast<double>(k + 1);
        p_prev = p_cur;
        p_cur = p_next;
    }
    return p_cur;
}

} // namespace

namespace lobatto {

double value(int mode, double xi) {
    if (mode == 0) {
        return 0.5 * (1.0 - xi);
    }
    if (mode == 1) {
        return 0.5 * (1.0 + xi);
    }
    // Bubble phi_k = (P_k - P_{k-2}) / sqrt(2(2k-1)); vanishes at xi = +/-1.
    return (legendre_p(mode, xi) - legendre_p(mode - 2, xi)) /
           std::sqrt(2.0 * (2.0 * mode - 1.0));
}

double deriv(int mode, double xi) {
    if (mode == 0) {
        return -0.5;
    }
    if (mode == 1) {
        return 0.5;
    }
    // phi_k'(xi) = sqrt((2k-1)/2) * P_{k-1}(xi)  (from P_k' - P_{k-2}' = (2k-1)P_{k-1}).
    return std::sqrt((2.0 * mode - 1.0) / 2.0) * legendre_p(mode - 1, xi);
}

} // namespace lobatto

namespace {

// Canonical hex vertex sign-indices (0 -> the -1 side, 1 -> the +1 side),
// matching nodal_mesh.hpp v0..v7.
constexpr std::array<std::array<int, 3>, 8> kHexV{{{0, 0, 0},
                                                   {1, 0, 0},
                                                   {1, 1, 0},
                                                   {0, 1, 0},
                                                   {0, 0, 1},
                                                   {1, 0, 1},
                                                   {1, 1, 1},
                                                   {0, 1, 1}}};

// Hex edges in hex20 order (nodal_mesh.hpp).
constexpr std::array<std::array<int, 2>, 12> kHexE{{{0, 1},
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

// Hex faces: fixed axis, fixed sign-index, and the two varying axes.
struct HexFace {
    int fixed_axis;
    int fixed_val;
    int vary0;
    int vary1;
};
constexpr std::array<HexFace, 6> kHexF{{{2, 0, 0, 1},
                                        {2, 1, 0, 1},
                                        {1, 0, 0, 2},
                                        {1, 1, 0, 2},
                                        {0, 0, 1, 2},
                                        {0, 1, 1, 2}}};

// Tet edges in tet10 order (nodal_mesh.hpp).
constexpr std::array<std::array<int, 2>, 6> kTetE{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};

// A single hex mode as its tensor indices plus the public descriptor.
struct HexRecipe {
    int i;
    int j;
    int k;
    HpMode mode;
};

std::vector<HexRecipe> build_hex(std::uint8_t order) {
    if (order < 1 || order > 4) {
        throw FeaError(std::format(
            "hierarchical hex: order {} unsupported (1..4; cap is the Gauss rule)", order));
    }
    const int p = order;
    std::vector<HexRecipe> out;
    // Vertices.
    for (int v = 0; v < 8; ++v) {
        HpMode m;
        m.entity = HpMode::Entity::kVertex;
        m.entity_index = static_cast<std::uint8_t>(v);
        m.order = 1;
        out.push_back({kHexV[static_cast<std::size_t>(v)][0], kHexV[static_cast<std::size_t>(v)][1],
                       kHexV[static_cast<std::size_t>(v)][2], m});
    }
    // Edges: one axis varies (order 2..p), the other two fixed at the shared
    // vertex sign.
    for (int e = 0; e < 12; ++e) {
        const auto& va = kHexV[static_cast<std::size_t>(kHexE[static_cast<std::size_t>(e)][0])];
        const auto& vb = kHexV[static_cast<std::size_t>(kHexE[static_cast<std::size_t>(e)][1])];
        int axis = 0;
        for (int d = 0; d < 3; ++d) {
            if (va[static_cast<std::size_t>(d)] != vb[static_cast<std::size_t>(d)]) {
                axis = d;
            }
        }
        for (int mo = 2; mo <= p; ++mo) {
            std::array<int, 3> idx{va[0], va[1], va[2]};
            idx[static_cast<std::size_t>(axis)] = mo;
            HpMode m;
            m.entity = HpMode::Entity::kEdge;
            m.entity_index = static_cast<std::uint8_t>(e);
            m.order = static_cast<std::uint8_t>(mo);
            m.edge_odd = (mo % 2 == 1);
            out.push_back({idx[0], idx[1], idx[2], m});
        }
    }
    // Faces: two axes vary, one fixed.
    for (int f = 0; f < 6; ++f) {
        const auto& face = kHexF[static_cast<std::size_t>(f)];
        for (int m1 = 2; m1 <= p; ++m1) {
            for (int m2 = 2; m2 <= p; ++m2) {
                std::array<int, 3> idx{0, 0, 0};
                idx[static_cast<std::size_t>(face.fixed_axis)] = face.fixed_val;
                idx[static_cast<std::size_t>(face.vary0)] = m1;
                idx[static_cast<std::size_t>(face.vary1)] = m2;
                HpMode m;
                m.entity = HpMode::Entity::kFace;
                m.entity_index = static_cast<std::uint8_t>(f);
                m.order = static_cast<std::uint8_t>(std::max(m1, m2));
                out.push_back({idx[0], idx[1], idx[2], m});
            }
        }
    }
    // Interior.
    for (int i = 2; i <= p; ++i) {
        for (int j = 2; j <= p; ++j) {
            for (int k = 2; k <= p; ++k) {
                HpMode m;
                m.entity = HpMode::Entity::kInterior;
                m.entity_index = 0;
                m.order = static_cast<std::uint8_t>(std::max({i, j, k}));
                out.push_back({i, j, k, m});
            }
        }
    }
    return out;
}

// A tet mode: vertex v (edge_a<0) or quadratic edge bubble on (edge_a, edge_b).
struct TetRecipe {
    int edge_a; // -1 for a vertex mode
    int edge_b;
    int vertex; // valid when edge_a < 0
    HpMode mode;
};

std::vector<TetRecipe> build_tet(std::uint8_t order) {
    if (order < 1 || order > 2) {
        throw FeaError(std::format(
            "hierarchical tet: order {} unsupported (1..2; k>=3 kernels are the next increment)",
            order));
    }
    std::vector<TetRecipe> out;
    for (int v = 0; v < 4; ++v) {
        HpMode m;
        m.entity = HpMode::Entity::kVertex;
        m.entity_index = static_cast<std::uint8_t>(v);
        m.order = 1;
        out.push_back({-1, -1, v, m});
    }
    if (order >= 2) {
        for (int e = 0; e < 6; ++e) {
            HpMode m;
            m.entity = HpMode::Entity::kEdge;
            m.entity_index = static_cast<std::uint8_t>(e);
            m.order = 2;
            // phi_2 is even, so the quadratic edge bubble does not flip.
            out.push_back({kTetE[static_cast<std::size_t>(e)][0],
                           kTetE[static_cast<std::size_t>(e)][1], -1, m});
        }
    }
    return out;
}

// Strain-displacement matrix (6 x 3n), Voigt order (xx,yy,zz,yz,xz,xy) with
// engineering shears, from physical mode gradients (n x 3).
Eigen::MatrixXd b_matrix(const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx) {
    const Eigen::Index n = dndx.rows();
    Eigen::MatrixXd b = Eigen::MatrixXd::Zero(6, 3 * n);
    for (Eigen::Index a = 0; a < n; ++a) {
        const double dx = dndx(a, 0), dy = dndx(a, 1), dz = dndx(a, 2);
        b(0, 3 * a + 0) = dx;
        b(1, 3 * a + 1) = dy;
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 1) = dz;
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 0) = dz;
        b(4, 3 * a + 2) = dx;
        b(5, 3 * a + 0) = dy;
        b(5, 3 * a + 1) = dx;
    }
    return b;
}

} // namespace

std::vector<HpMode> hp_modes(ElementType type, std::uint8_t order) {
    std::vector<HpMode> modes;
    if (type == ElementType::kHex8 || type == ElementType::kHex20) {
        for (const auto& r : build_hex(order)) {
            modes.push_back(r.mode);
        }
    } else if (type == ElementType::kTet4 || type == ElementType::kTet10) {
        for (const auto& r : build_tet(order)) {
            modes.push_back(r.mode);
        }
    } else {
        throw FeaError("hierarchical basis: only tet and hex are supported");
    }
    return modes;
}

std::size_t hp_num_modes(ElementType type, std::uint8_t order) {
    return hp_modes(type, order).size();
}

HpShape hp_eval(ElementType type, std::uint8_t order, const Eigen::Vector3d& xi) {
    HpShape out;
    if (type == ElementType::kHex8 || type == ElementType::kHex20) {
        const auto recipes = build_hex(order);
        const auto n = static_cast<Eigen::Index>(recipes.size());
        out.n.resize(n);
        out.dn.resize(n, 3);
        for (Eigen::Index r = 0; r < n; ++r) {
            const auto& rec = recipes[static_cast<std::size_t>(r)];
            const double vi = lobatto::value(rec.i, xi.x());
            const double vj = lobatto::value(rec.j, xi.y());
            const double vk = lobatto::value(rec.k, xi.z());
            const double di = lobatto::deriv(rec.i, xi.x());
            const double dj = lobatto::deriv(rec.j, xi.y());
            const double dk = lobatto::deriv(rec.k, xi.z());
            out.n(r) = vi * vj * vk;
            out.dn(r, 0) = di * vj * vk;
            out.dn(r, 1) = vi * dj * vk;
            out.dn(r, 2) = vi * vj * dk;
        }
        return out;
    }
    if (type == ElementType::kTet4 || type == ElementType::kTet10) {
        const auto recipes = build_tet(order);
        const std::array<double, 4> lam{1.0 - xi.x() - xi.y() - xi.z(), xi.x(), xi.y(), xi.z()};
        const std::array<Eigen::Vector3d, 4> glam{Eigen::Vector3d(-1, -1, -1),
                                                   Eigen::Vector3d(1, 0, 0),
                                                   Eigen::Vector3d(0, 1, 0),
                                                   Eigen::Vector3d(0, 0, 1)};
        const auto n = static_cast<Eigen::Index>(recipes.size());
        out.n.resize(n);
        out.dn.resize(n, 3);
        for (Eigen::Index r = 0; r < n; ++r) {
            const auto& rec = recipes[static_cast<std::size_t>(r)];
            if (rec.edge_a < 0) {
                const auto v = static_cast<std::size_t>(rec.vertex);
                out.n(r) = lam[v];
                out.dn.row(r) = glam[v].transpose();
            } else {
                const auto a = static_cast<std::size_t>(rec.edge_a);
                const auto b = static_cast<std::size_t>(rec.edge_b);
                // Quadratic edge bubble 4*lam_a*lam_b (peaks at 1 at the midpoint).
                out.n(r) = 4.0 * lam[a] * lam[b];
                out.dn.row(r) = (4.0 * (lam[b] * glam[a] + lam[a] * glam[b])).transpose();
            }
        }
        return out;
    }
    throw FeaError("hierarchical basis: only tet and hex are supported");
}

Eigen::MatrixXd hp_element_stiffness(const Eigen::Matrix<double, Eigen::Dynamic, 3>& vertex_coords,
                                     ElementType type, std::uint8_t order,
                                     const Material& material) {
    const bool is_hex = (type == ElementType::kHex8 || type == ElementType::kHex20);
    const bool is_tet = (type == ElementType::kTet4 || type == ElementType::kTet10);
    if (!is_hex && !is_tet) {
        throw FeaError("hierarchical basis: only tet and hex are supported");
    }
    const ElementType geo_type = is_hex ? ElementType::kHex8 : ElementType::kTet4;
    const Eigen::Index expected_v = is_hex ? 8 : 4;
    if (vertex_coords.rows() != expected_v) {
        throw FeaError("hp_element_stiffness: vertex_coords row count does not match element type");
    }

    // Subparametric geometry: straight-sided map from the p=1 vertex functions.
    std::vector<QuadraturePoint> rule;
    if (is_hex) {
        rule = hex_rule(std::min(5, static_cast<int>(order) + 1));
    } else {
        rule = tet_rule(std::max(2, 2 * static_cast<int>(order)));
    }

    const auto d = material.d_matrix();
    const auto nmodes = static_cast<Eigen::Index>(hp_num_modes(type, order));
    Eigen::MatrixXd k = Eigen::MatrixXd::Zero(3 * nmodes, 3 * nmodes);
    for (const auto& qp : rule) {
        const auto geo = eval_shape(geo_type, qp.xi);
        const Eigen::Matrix3d jac = geo.dn.transpose() * vertex_coords;
        const double det = jac.determinant();
        if (det <= 0.0) {
            throw FeaError(std::format(
                "hp_element_stiffness: non-positive Jacobian ({:.3e})", det));
        }
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const auto field = hp_eval(type, order, qp.xi);
        const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = field.dn * jac_inv.transpose();
        const auto b = b_matrix(dndx);
        k.noalias() += b.transpose() * d * b * (det * qp.weight);
    }
    return k;
}

} // namespace polymesh::fea
