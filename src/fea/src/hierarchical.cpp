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
    if (n <= 0) {
        return 1.0;
    }
    if (n == 1) {
        return x;
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

// dP_n/dx via (1-x^2) P_n' = n P_{n-1} - n x P_n, rearranged at |x|<1;
// at the endpoints use the known values P_n'(±1) = (±1)^{n+1} n(n+1)/2.
double legendre_p_deriv(int n, double x) {
    if (n <= 0) {
        return 0.0;
    }
    if (n == 1) {
        return 1.0;
    }
    if (std::abs(x) >= 1.0 - 1e-14) {
        const double s = (x > 0.0) ? 1.0 : -1.0;
        // P_n'(1) = n(n+1)/2, P_n'(-1) = (-1)^{n+1} n(n+1)/2.
        const double mag = 0.5 * static_cast<double>(n) * static_cast<double>(n + 1);
        return (n % 2 == 0) ? (s > 0 ? mag : -mag) : mag;
    }
    const double pn = legendre_p(n, x);
    const double pn1 = legendre_p(n - 1, x);
    return (static_cast<double>(n) / (1.0 - x * x)) * (pn1 - x * pn);
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

// Tet faces as vertex triples (outward not required for H1 bubbles).
constexpr std::array<std::array<int, 3>, 4> kTetF{
    {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}}};

// A single hex mode as its tensor indices plus the public descriptor.
struct HexRecipe {
    int i;
    int j;
    int k;
    HpMode mode;
};

std::vector<HexRecipe> build_hex(std::uint8_t order) {
    if (order < 1 || order > 6) {
        throw FeaError(std::format(
            "hierarchical hex: order {} unsupported (1..6; cap is the Gauss rule)", order));
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
            m.index0 = static_cast<std::uint8_t>(mo);
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
                m.index0 = static_cast<std::uint8_t>(m1);
                m.index1 = static_cast<std::uint8_t>(m2);
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
                m.index0 = static_cast<std::uint8_t>(i);
                m.index1 = static_cast<std::uint8_t>(j);
                m.index2 = static_cast<std::uint8_t>(k);
                out.push_back({i, j, k, m});
            }
        }
    }
    return out;
}

// Tet hierarchical recipes.
// Kind: vertex / edge / face / interior.
enum class TetKind : std::uint8_t { kVertex, kEdge, kFace, kInterior };

struct TetRecipe {
    TetKind kind = TetKind::kVertex;
    int a = 0; // vertex, or edge endpoint a / face vertex a
    int b = 0; // edge endpoint b / face vertex b
    int c = 0; // face vertex c
    int d = 0; // unused, or interior unused
    int n1 = 0; // edge order (>=2), or face multi-index n1>=0, or interior n1
    int n2 = 0; // face multi-index n2, or interior n2
    int n3 = 0; // interior n3
    HpMode mode;
};

// Face kernels up to order p: all (n1,n2) with n1>=0, n2>=0, n1+n2 <= p-3.
// Total count = (p-1)(p-2)/2. Ordering: n1 outer, n2 inner for n1+n2 = 0,1,...,p-3.
// Interior kernels: (n1,n2,n3) with n1,n2,n3>=0, n1+n2+n3 <= p-4.
// Total = (p-1)(p-2)(p-3)/6.

std::vector<TetRecipe> build_tet(std::uint8_t order) {
    if (order < 1 || order > 4) {
        throw FeaError(std::format(
            "hierarchical tet: order {} unsupported (1..4)", order));
    }
    const int p = order;
    std::vector<TetRecipe> out;
    for (int v = 0; v < 4; ++v) {
        HpMode m;
        m.entity = HpMode::Entity::kVertex;
        m.entity_index = static_cast<std::uint8_t>(v);
        m.order = 1;
        out.push_back({TetKind::kVertex, v, 0, 0, 0, 0, 0, 0, m});
    }
    // Edges: kernel order l = 2..p. N = 4 λ_a λ_b L_{l-2}(λ_b - λ_a).
    for (int e = 0; e < 6; ++e) {
        for (int l = 2; l <= p; ++l) {
            HpMode m;
            m.entity = HpMode::Entity::kEdge;
            m.entity_index = static_cast<std::uint8_t>(e);
            m.order = static_cast<std::uint8_t>(l);
            m.edge_odd = (l % 2 == 1);
            m.index0 = static_cast<std::uint8_t>(l);
            out.push_back({TetKind::kEdge, kTetE[static_cast<std::size_t>(e)][0],
                           kTetE[static_cast<std::size_t>(e)][1], 0, 0, l, 0, 0, m});
        }
    }
    // Faces: for each face, kernels with n1+n2 <= p-3, n1,n2 >= 0.
    // Mode order = n1 + n2 + 3 (the λ_a λ_b λ_c factor is degree 3).
    for (int f = 0; f < 4; ++f) {
        int face_slot = 0;
        for (int sum = 0; sum <= p - 3; ++sum) {
            for (int n1 = 0; n1 <= sum; ++n1) {
                const int n2 = sum - n1;
                HpMode m;
                m.entity = HpMode::Entity::kFace;
                m.entity_index = static_cast<std::uint8_t>(f);
                m.order = static_cast<std::uint8_t>(n1 + n2 + 3);
                m.index0 = static_cast<std::uint8_t>(n1);
                m.index1 = static_cast<std::uint8_t>(n2);
                m.index2 = static_cast<std::uint8_t>(face_slot); // slot within face
                out.push_back({TetKind::kFace, kTetF[static_cast<std::size_t>(f)][0],
                               kTetF[static_cast<std::size_t>(f)][1],
                               kTetF[static_cast<std::size_t>(f)][2], 0, n1, n2, face_slot, m});
                ++face_slot;
            }
        }
    }
    // Interior: n1+n2+n3 <= p-4.
    int int_slot = 0;
    for (int sum = 0; sum <= p - 4; ++sum) {
        for (int n1 = 0; n1 <= sum; ++n1) {
            for (int n2 = 0; n1 + n2 <= sum; ++n2) {
                const int n3 = sum - n1 - n2;
                HpMode m;
                m.entity = HpMode::Entity::kInterior;
                m.entity_index = 0;
                m.order = static_cast<std::uint8_t>(n1 + n2 + n3 + 4);
                m.index0 = static_cast<std::uint8_t>(n1);
                m.index1 = static_cast<std::uint8_t>(n2);
                m.index2 = static_cast<std::uint8_t>(n3);
                out.push_back({TetKind::kInterior, 0, 1, 2, 3, n1, n2, n3, m});
                ++int_slot;
            }
        }
    }
    (void)int_slot;
    return out;
}

// Evaluate one tet mode value + gradient in reference coordinates.
void eval_tet_mode(const TetRecipe& rec, const std::array<double, 4>& lam,
                   const std::array<Eigen::Vector3d, 4>& glam, double& val,
                   Eigen::Vector3d& grad) {
    if (rec.kind == TetKind::kVertex) {
        const auto v = static_cast<std::size_t>(rec.a);
        val = lam[v];
        grad = glam[v];
        return;
    }
    if (rec.kind == TetKind::kEdge) {
        const auto ia = static_cast<std::size_t>(rec.a);
        const auto ib = static_cast<std::size_t>(rec.b);
        const double la = lam[ia], lb = lam[ib];
        const int l = rec.n1; // order
        const int deg = l - 2;
        const double xi = lb - la;
        const double L = legendre_p(deg, xi);
        const double dL = legendre_p_deriv(deg, xi);
        // N = 4 la lb L(lb-la)
        val = 4.0 * la * lb * L;
        // dN = 4[(dla) lb L + la (dlb) L + la lb dL (dlb - dla)]
        grad = 4.0 * (lb * L * glam[ia] + la * L * glam[ib] +
                      la * lb * dL * (glam[ib] - glam[ia]));
        return;
    }
    if (rec.kind == TetKind::kFace) {
        const auto ia = static_cast<std::size_t>(rec.a);
        const auto ib = static_cast<std::size_t>(rec.b);
        const auto ic = static_cast<std::size_t>(rec.c);
        const double la = lam[ia], lb = lam[ib], lc = lam[ic];
        const int n1 = rec.n1, n2 = rec.n2;
        const double x1 = lb - la;
        const double x2 = lc - la;
        const double L1 = legendre_p(n1, x1);
        const double L2 = legendre_p(n2, x2);
        const double dL1 = legendre_p_deriv(n1, x1);
        const double dL2 = legendre_p_deriv(n2, x2);
        // N = la lb lc L_n1(lb-la) L_n2(lc-la)
        val = la * lb * lc * L1 * L2;
        // Product rule over la,lb,lc and the two Legendre arguments.
        grad = (lb * lc * L1 * L2) * glam[ia] + (la * lc * L1 * L2) * glam[ib] +
               (la * lb * L1 * L2) * glam[ic] +
               (la * lb * lc * dL1 * L2) * (glam[ib] - glam[ia]) +
               (la * lb * lc * L1 * dL2) * (glam[ic] - glam[ia]);
        return;
    }
    // Interior: N = l0 l1 l2 l3 L_n1(l1-l0) L_n2(l2-l0) L_n3(l3-l0)
    const double l0 = lam[0], l1 = lam[1], l2 = lam[2], l3 = lam[3];
    const int n1 = rec.n1, n2 = rec.n2, n3 = rec.n3;
    const double L1 = legendre_p(n1, l1 - l0);
    const double L2 = legendre_p(n2, l2 - l0);
    const double L3 = legendre_p(n3, l3 - l0);
    const double dL1 = legendre_p_deriv(n1, l1 - l0);
    const double dL2 = legendre_p_deriv(n2, l2 - l0);
    const double dL3 = legendre_p_deriv(n3, l3 - l0);
    val = l0 * l1 * l2 * l3 * L1 * L2 * L3;
    const double base = L1 * L2 * L3;
    grad = (l1 * l2 * l3 * base) * glam[0] + (l0 * l2 * l3 * base) * glam[1] +
           (l0 * l1 * l3 * base) * glam[2] + (l0 * l1 * l2 * base) * glam[3] +
           (l0 * l1 * l2 * l3 * dL1 * L2 * L3) * (glam[1] - glam[0]) +
           (l0 * l1 * l2 * l3 * L1 * dL2 * L3) * (glam[2] - glam[0]) +
           (l0 * l1 * l2 * l3 * L1 * L2 * dL3) * (glam[3] - glam[0]);
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
            double val = 0.0;
            Eigen::Vector3d g = Eigen::Vector3d::Zero();
            eval_tet_mode(recipes[static_cast<std::size_t>(r)], lam, glam, val, g);
            out.n(r) = val;
            out.dn.row(r) = g.transpose();
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
    // Stiffness integrand degree ~ 2(p-1) on affine cells; n >= p Gauss points
    // (hex) / Duffy degree 2p (tet) is enough, with a small overkill margin.
    std::vector<QuadraturePoint> rule;
    if (is_hex) {
        rule = hex_rule(std::min(6, std::max(2, static_cast<int>(order) + 1)));
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
