// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vem.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace polymesh::fea {
namespace {

Eigen::Vector3d face_normal_area(const std::vector<Eigen::Vector3d>& coords,
                                 const std::vector<std::uint32_t>& face, double& area_out) {
    // Newell: robust for non-planar quads.
    Eigen::Vector3d n = Eigen::Vector3d::Zero();
    const auto m = face.size();
    for (std::size_t i = 0; i < m; ++i) {
        const auto& a = coords[face[i]];
        const auto& b = coords[face[(i + 1) % m]];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    n *= 0.5;
    area_out = n.norm();
    if (area_out <= 0.0) {
        return Eigen::Vector3d::Zero();
    }
    return n; // area-weighted normal (magnitude = area)
}

Eigen::MatrixXd b_from_grads(const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx) {
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

/// Columns = DOF vectors of the 12-dim linear space (RBM + constant strain)
/// evaluated at vertices. Size 3n × 12.
Eigen::MatrixXd linear_space_basis(const std::vector<Eigen::Vector3d>& coords) {
    const auto n = static_cast<Eigen::Index>(coords.size());
    Eigen::MatrixXd p(3 * n, 12);
    p.setZero();
    for (Eigen::Index i = 0; i < n; ++i) {
        p(3 * i + 0, 0) = 1.0;
        p(3 * i + 1, 1) = 1.0;
        p(3 * i + 2, 2) = 1.0;
        const auto& x = coords[static_cast<std::size_t>(i)];
        p(3 * i + 1, 3) = -x[2];
        p(3 * i + 2, 3) = x[1];
        p(3 * i + 0, 4) = x[2];
        p(3 * i + 2, 4) = -x[0];
        p(3 * i + 0, 5) = -x[1];
        p(3 * i + 1, 5) = x[0];
        p(3 * i + 0, 6) = x[0];
        p(3 * i + 1, 7) = x[1];
        p(3 * i + 2, 8) = x[2];
        p(3 * i + 1, 9) = x[2];
        p(3 * i + 2, 9) = x[1];
        p(3 * i + 0, 10) = x[2];
        p(3 * i + 2, 10) = x[0];
        p(3 * i + 0, 11) = x[1];
        p(3 * i + 1, 11) = x[0];
    }
    return p;
}

// ---- k=2 polynomial projector helpers (ADR-0017) ----

constexpr int kP2Mono = 10;         // dim P₂(ℝ³)
constexpr int kP2Vec = 3 * kP2Mono; // dim [P₂]³

/// Scaled monomials of total degree ≤ 2 at ξ = (x − x_c) / h.
Eigen::Matrix<double, kP2Mono, 1> p2_monomials(const Eigen::Vector3d& xi) {
    Eigen::Matrix<double, kP2Mono, 1> m;
    const double x = xi[0], y = xi[1], z = xi[2];
    m << 1.0, x, y, z, x * x, y * y, z * z, x * y, x * z, y * z;
    return m;
}

/// Gradients of scaled monomials w.r.t. physical x: ∇_x m = (1/h) ∇_ξ m.
Eigen::Matrix<double, kP2Mono, 3> p2_monomial_grads(const Eigen::Vector3d& xi, double h) {
    Eigen::Matrix<double, kP2Mono, 3> g = Eigen::Matrix<double, kP2Mono, 3>::Zero();
    const double x = xi[0], y = xi[1], z = xi[2];
    const double s = 1.0 / h;
    // 1
    // x, y, z
    g(1, 0) = s;
    g(2, 1) = s;
    g(3, 2) = s;
    // x², y², z²
    g(4, 0) = 2.0 * x * s;
    g(5, 1) = 2.0 * y * s;
    g(6, 2) = 2.0 * z * s;
    // xy, xz, yz
    g(7, 0) = y * s;
    g(7, 1) = x * s;
    g(8, 0) = z * s;
    g(8, 2) = x * s;
    g(9, 1) = z * s;
    g(9, 2) = y * s;
    return g;
}

/// Strain-displacement of the [P₂]³ basis (columns) at physical point x.
/// Basis layout: for mono a and component c, column j = 3*a + c is m_a e_c.
Eigen::Matrix<double, 6, kP2Vec> p2_strain_basis(const Eigen::Vector3d& xi, double h) {
    const auto gm = p2_monomial_grads(xi, h);
    Eigen::Matrix<double, 6, kP2Vec> b = Eigen::Matrix<double, 6, kP2Vec>::Zero();
    for (int a = 0; a < kP2Mono; ++a) {
        const double dx = gm(a, 0), dy = gm(a, 1), dz = gm(a, 2);
        // component 0 (u)
        b(0, 3 * a + 0) = dx;
        b(4, 3 * a + 0) = dz;
        b(5, 3 * a + 0) = dy;
        // component 1 (v)
        b(1, 3 * a + 1) = dy;
        b(3, 3 * a + 1) = dz;
        b(5, 3 * a + 1) = dx;
        // component 2 (w)
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 2) = dx;
    }
    return b;
}

/// DOF evaluation of [P₂]³ basis at all nodes: (3n) × 30.
Eigen::MatrixXd p2_dof_eval(const std::vector<Eigen::Vector3d>& coords,
                            const Eigen::Vector3d& xc, double h) {
    const auto n = static_cast<Eigen::Index>(coords.size());
    Eigen::MatrixXd d(3 * n, kP2Vec);
    d.setZero();
    for (Eigen::Index i = 0; i < n; ++i) {
        const Eigen::Vector3d xi = (coords[static_cast<std::size_t>(i)] - xc) / h;
        const auto m = p2_monomials(xi);
        for (int a = 0; a < kP2Mono; ++a) {
            d(3 * i + 0, 3 * a + 0) = m[a];
            d(3 * i + 1, 3 * a + 1) = m[a];
            d(3 * i + 2, 3 * a + 2) = m[a];
        }
    }
    return d;
}

struct TetFan {
    // Physical tet corners (4 vertices each) covering the polyhedron.
    std::vector<std::array<Eigen::Vector3d, 4>> tets;
    double volume = 0.0;
};

/// Fan tets from the volume centroid to triangulated faces (convex cells).
TetFan tet_fan(const std::vector<Eigen::Vector3d>& coords,
               const std::vector<std::vector<std::uint32_t>>& faces,
               const Eigen::Vector3d& centroid) {
    TetFan fan;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        const auto& a = coords[face[0]];
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            const auto& b = coords[face[i]];
            const auto& c = coords[face[i + 1]];
            // Oriented volume of tet (centroid, a, b, c).
            const double six_v = (a - centroid).dot((b - centroid).cross(c - centroid));
            if (std::abs(six_v) < 1e-30) {
                continue;
            }
            std::array<Eigen::Vector3d, 4> tet{centroid, a, b, c};
            if (six_v < 0.0) {
                std::swap(tet[2], tet[3]); // ensure positive orientation
            }
            fan.volume += std::abs(six_v) / 6.0;
            fan.tets.push_back(tet);
        }
    }
    return fan;
}

/// Integrate a 30×30 matrix-valued density over the tet fan with tet_rule(4).
template <typename Fun> Eigen::MatrixXd integrate_p2_matrix(const TetFan& fan, Fun&& density) {
    Eigen::MatrixXd acc = Eigen::MatrixXd::Zero(kP2Vec, kP2Vec);
    const auto rule = tet_rule(4);
    for (const auto& tet : fan.tets) {
        // Reference tet: L0+L1+L2+L3=1, Li>=0. Map xi (bary of first 3) → physical.
        // Our tet_rule uses {xi,eta,zeta >= 0, sum <= 1} with vertices
        // (0,0,0),(1,0,0),(0,1,0),(0,0,1) ↔ physical (v0,v1,v2,v3)?
        // Standard: x = v0 + xi*(v1-v0) + eta*(v2-v0) + zeta*(v3-v0),
        // but ref volume 1/6 matches tet (0,e1,e2,e3). Here v0=centroid.
        const Eigen::Vector3d& v0 = tet[0];
        const Eigen::Vector3d e1 = tet[1] - v0;
        const Eigen::Vector3d e2 = tet[2] - v0;
        const Eigen::Vector3d e3 = tet[3] - v0;
        const double det = e1.dot(e2.cross(e3)); // 6 * V_tet
        if (det <= 0.0) {
            continue;
        }
        for (const auto& qp : rule) {
            const Eigen::Vector3d x = v0 + qp.xi[0] * e1 + qp.xi[1] * e2 + qp.xi[2] * e3;
            // dV = det * w_ref, w_ref already integrates over ref volume 1/6,
            // so physical weight = det * qp.weight (det = 6 V ⇒ V * 6w).
            const Eigen::MatrixXd dens = density(x);
            acc.noalias() += dens * (det * qp.weight);
        }
    }
    return acc;
}

template <typename Fun> Eigen::VectorXd integrate_p2_vector(const TetFan& fan, Fun&& density) {
    Eigen::VectorXd acc = Eigen::VectorXd::Zero(kP2Vec);
    const auto rule = tet_rule(4);
    for (const auto& tet : fan.tets) {
        const Eigen::Vector3d& v0 = tet[0];
        const Eigen::Vector3d e1 = tet[1] - v0;
        const Eigen::Vector3d e2 = tet[2] - v0;
        const Eigen::Vector3d e3 = tet[3] - v0;
        const double det = e1.dot(e2.cross(e3));
        if (det <= 0.0) {
            continue;
        }
        for (const auto& qp : rule) {
            const Eigen::Vector3d x = v0 + qp.xi[0] * e1 + qp.xi[1] * e2 + qp.xi[2] * e3;
            const Eigen::VectorXd dens = density(x);
            acc.noalias() += dens * (det * qp.weight);
        }
    }
    return acc;
}

struct P2Projector {
    Eigen::Vector3d xc;
    double h = 1.0;
    Eigen::MatrixXd dof_eval; // 3n × 30
    Eigen::MatrixXd pi;       // 30 × 3n  (maps DOFs → coeffs)
    TetFan fan;
};

P2Projector make_p2_projector(const std::vector<Eigen::Vector3d>& coords,
                              const std::vector<std::vector<std::uint32_t>>& faces) {
    P2Projector p;
    p.xc = poly_centroid(coords, faces);
    const double vol = poly_volume(coords, faces);
    p.h = std::cbrt(std::max(vol, 1e-30));
    p.dof_eval = p2_dof_eval(coords, p.xc, p.h);
    // Π = (Dᵀ D)⁻¹ Dᵀ — exact on range(D) when nodal samples come from [P₂]³.
    const Eigen::MatrixXd dtd = p.dof_eval.transpose() * p.dof_eval;
    Eigen::LDLT<Eigen::MatrixXd> ldlt(dtd);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("vem k=2: singular P2 DOF Gram matrix");
    }
    p.pi = ldlt.solve(p.dof_eval.transpose());
    p.fan = tet_fan(coords, faces, p.xc);
    if (p.fan.tets.empty() || p.fan.volume <= 0.0) {
        throw FeaError("vem k=2: empty tet fan (non-convex or degenerate cell?)");
    }
    return p;
}

double char_length(const std::vector<Eigen::Vector3d>& coords,
                   const std::vector<std::vector<std::uint32_t>>& faces, double vol) {
    double area_sum = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        double area = 0.0;
        face_normal_area(coords, face, area);
        area_sum += area;
    }
    double h = std::sqrt(area_sum / (6.0 * vol + 1e-30));
    if (h <= 0.0) {
        h = std::cbrt(vol);
    }
    return h;
}

Eigen::MatrixXd vem_stiffness_k1(const std::vector<Eigen::Vector3d>& coords,
                                 const std::vector<std::vector<std::uint32_t>>& faces,
                                 const Material& material) {
    const auto n = coords.size();
    if (n < 4) {
        throw FeaError("vem_poly_stiffness: need at least 4 vertices");
    }
    const double vol = poly_volume(coords, faces);
    if (vol <= 0.0) {
        throw FeaError(std::format("vem_poly_stiffness: non-positive volume {:.3e}", vol));
    }

    Eigen::Matrix<double, Eigen::Dynamic, 3> grad(static_cast<Eigen::Index>(n), 3);
    grad.setZero();
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        double area = 0.0;
        const Eigen::Vector3d nA = face_normal_area(coords, face, area);
        if (area <= 0.0) {
            continue;
        }
        const double w = 1.0 / static_cast<double>(face.size());
        for (auto li : face) {
            grad.row(static_cast<Eigen::Index>(li)) += (w * nA).transpose();
        }
    }
    grad /= vol;
    const double h_char = char_length(coords, faces, vol);

    const auto b = b_from_grads(grad);
    const auto d = material.d_matrix();
    Eigen::MatrixXd k = vol * (b.transpose() * d * b);

    const Eigen::MatrixXd basis = linear_space_basis(coords);
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(basis);
    const Eigen::MatrixXd q =
        qr.householderQ() * Eigen::MatrixXd::Identity(basis.rows(), qr.rank());
    const Eigen::MatrixXd proj = q * q.transpose();
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(n);
    const Eigen::MatrixXd i_minus = Eigen::MatrixXd::Identity(ndof, ndof) - proj;
    const Eigen::MatrixXd stab = i_minus.transpose() * i_minus;
    // tau=1 was slightly over-stiff on clipped Voronoi cells (M5 SCF/SE lost to
    // tet hybrid). Minimal stab recovers compliance; residual stays ≪ 1e-6.
    const double tau = 0.08;
    const double stab_scale = tau * material.mu() * h_char;
    k.noalias() += stab_scale * stab;

    k = 0.5 * (k + k.transpose()).eval();
    return k;
}

/// True when the cell is an 8-vertex / 12-edge hex with 6 quad faces (serendipity k=2 path).
bool is_hex_serendipity_cell(const std::vector<Eigen::Vector3d>& coords,
                             const std::vector<std::vector<std::uint32_t>>& faces) {
    if (faces.size() != 6 || coords.size() != 20) {
        return false;
    }
    if (poly_vertex_count(faces) != 8 || poly_edges(faces).size() != 12) {
        return false;
    }
    for (const auto& f : faces) {
        if (f.size() != 4) {
            return false;
        }
    }
    return true;
}

/// Canonical hex20 edge list (matches fea/nodal_mesh.hpp and promote_to_quadratic).
constexpr std::array<std::array<int, 2>, 12> kHex20Edges{{{0, 1},
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

/// Reorder (verts | poly_edges mids) → canonical hex20 node coordinates.
std::vector<Eigen::Vector3d>
hex20_coords_canonical(const std::vector<Eigen::Vector3d>& coords,
                       const std::vector<std::vector<std::uint32_t>>& faces) {
    std::vector<Eigen::Vector3d> out(20);
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = coords[static_cast<std::size_t>(i)];
    }
    // Map endpoint-pair → mid coordinate from the input (verts + poly_edges order).
    const auto edges = poly_edges(faces);
    std::map<std::pair<std::uint32_t, std::uint32_t>, Eigen::Vector3d> mid_of;
    for (std::size_t e = 0; e < edges.size(); ++e) {
        mid_of[std::minmax(edges[e][0], edges[e][1])] = coords[8 + e];
    }
    for (std::size_t e = 0; e < kHex20Edges.size(); ++e) {
        const auto a = static_cast<std::uint32_t>(kHex20Edges[e][0]);
        const auto b = static_cast<std::uint32_t>(kHex20Edges[e][1]);
        const auto it = mid_of.find(std::minmax(a, b));
        if (it == mid_of.end()) {
            throw FeaError("vem k=2 hex: missing mid-edge for canonical hex20 edge");
        }
        out[8 + e] = it->second;
    }
    return out;
}

/// Isoparametric hex20 stiffness (serendipity FEM ≡ hex VEM k=2 path, ADR-0017).
Eigen::MatrixXd hex20_stiffness(const std::vector<Eigen::Vector3d>& coords20,
                                const Material& material) {
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(20, 3);
    for (int a = 0; a < 20; ++a) {
        x.row(a) = coords20[static_cast<std::size_t>(a)].transpose();
    }
    const auto d = material.d_matrix();
    Eigen::MatrixXd k = Eigen::MatrixXd::Zero(60, 60);
    for (const auto& qp : hex_rule(3)) {
        const auto shape = eval_shape(ElementType::kHex20, qp.xi);
        const Eigen::Matrix3d jac = shape.dn.transpose() * x;
        const double det = jac.determinant();
        if (det <= 0.0) {
            throw FeaError(std::format("vem k=2 hex20: non-positive Jacobian ({:.3e})", det));
        }
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = shape.dn * jac_inv.transpose();
        const auto b = b_from_grads(dndx);
        k.noalias() += b.transpose() * d * b * (det * qp.weight);
    }
    k = 0.5 * (k + k.transpose()).eval();
    return k;
}

/// Permutation of local DOFs from poly_edges mid order → hex20 mid order (3 dofs/node).
Eigen::VectorXi
hex20_dof_perm_from_poly(const std::vector<std::vector<std::uint32_t>>& faces) {
    // perm[hex20_local] = poly_local
    Eigen::VectorXi perm(20);
    for (int i = 0; i < 8; ++i) {
        perm[i] = i;
    }
    const auto edges = poly_edges(faces);
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> poly_mid_index;
    for (std::size_t e = 0; e < edges.size(); ++e) {
        poly_mid_index[std::minmax(edges[e][0], edges[e][1])] = static_cast<int>(8 + e);
    }
    for (std::size_t e = 0; e < kHex20Edges.size(); ++e) {
        const auto a = static_cast<std::uint32_t>(kHex20Edges[e][0]);
        const auto b = static_cast<std::uint32_t>(kHex20Edges[e][1]);
        perm[static_cast<Eigen::Index>(8 + e)] = poly_mid_index.at(std::minmax(a, b));
    }
    return perm;
}

Eigen::MatrixXd permute_stiffness_to_poly_order(const Eigen::MatrixXd& k_hex20,
                                                const Eigen::VectorXi& perm_nodes) {
    // k_poly(a,b) = k_hex20(hex_of(a), hex_of(b)); invert perm: poly = perm[hex]
    Eigen::VectorXi hex_of(20);
    for (int h = 0; h < 20; ++h) {
        hex_of[perm_nodes[h]] = h;
    }
    Eigen::MatrixXd k = Eigen::MatrixXd::Zero(60, 60);
    for (int a = 0; a < 20; ++a) {
        for (int b = 0; b < 20; ++b) {
            const int ha = hex_of[a];
            const int hb = hex_of[b];
            k.block<3, 3>(3 * a, 3 * b) = k_hex20.block<3, 3>(3 * ha, 3 * hb);
        }
    }
    return k;
}

Eigen::MatrixXd vem_stiffness_k2(const std::vector<Eigen::Vector3d>& coords,
                                 const std::vector<std::vector<std::uint32_t>>& faces,
                                 const Material& material) {
    const auto nv = poly_vertex_count(faces);
    const auto edges = poly_edges(faces);
    if (coords.size() != nv + edges.size()) {
        throw FeaError(std::format("vem k=2: expected {} nodes ({} verts + {} edges), got {}",
                                   nv + edges.size(), nv, edges.size(), coords.size()));
    }
    const double vol = poly_volume(coords, faces);
    if (vol <= 0.0) {
        throw FeaError(std::format("vem_poly_stiffness: non-positive volume {:.3e}", vol));
    }

    // Hex serendipity path (ROADMAP C4 / ADR-0017): coincides with isoparametric
    // hex20 so multi-element assembly, patch test, and MMS order-2 hold.
    if (is_hex_serendipity_cell(coords, faces)) {
        const auto c20 = hex20_coords_canonical(coords, faces);
        const auto k20 = hex20_stiffness(c20, material);
        const auto perm = hex20_dof_perm_from_poly(faces);
        return permute_stiffness_to_poly_order(k20, perm);
    }

    // General polyhedron: discrete [P₂]³ nodal projector + stabilization.
    // Exact on single-cell polynomial fields; multi-element face cancellation is
    // weaker than the hex-serendipity path (see ADR-0017).
    const P2Projector proj = make_p2_projector(coords, faces);
    const auto dmat = material.d_matrix();
    const Eigen::MatrixXd k_poly =
        integrate_p2_matrix(proj.fan, [&](const Eigen::Vector3d& x) {
            const Eigen::Vector3d xi = (x - proj.xc) / proj.h;
            const Eigen::Matrix<double, 6, kP2Vec> b = p2_strain_basis(xi, proj.h);
            const Eigen::Matrix<double, kP2Vec, 6> bt_d = b.transpose() * dmat;
            return Eigen::MatrixXd(bt_d * b);
        });

    const Eigen::MatrixXd k_cons = proj.pi.transpose() * k_poly;
    Eigen::MatrixXd k = k_cons * proj.pi;

    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(coords.size());
    const Eigen::MatrixXd nod_proj = proj.dof_eval * proj.pi;
    const Eigen::MatrixXd i_minus = Eigen::MatrixXd::Identity(ndof, ndof) - nod_proj;
    const Eigen::MatrixXd stab = i_minus.transpose() * i_minus;
    const double h_char = char_length(coords, faces, vol);
    const double tau = 1.0;
    k.noalias() += (tau * material.mu() * h_char) * stab;

    k = 0.5 * (k + k.transpose()).eval();
    return k;
}

} // namespace

std::vector<std::array<std::uint32_t, 2>>
poly_edges(const std::vector<std::vector<std::uint32_t>>& faces) {
    std::vector<std::array<std::uint32_t, 2>> edges;
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const auto& face : faces) {
        const auto m = face.size();
        if (m < 2) {
            continue;
        }
        for (std::size_t i = 0; i < m; ++i) {
            const auto a = face[i];
            const auto b = face[(i + 1) % m];
            const auto key = std::minmax(a, b);
            if (seen.insert(key).second) {
                edges.push_back({key.first, key.second});
            }
        }
    }
    return edges;
}

std::size_t poly_vertex_count(const std::vector<std::vector<std::uint32_t>>& faces) {
    std::uint32_t max_i = 0;
    bool any = false;
    for (const auto& face : faces) {
        for (auto i : face) {
            max_i = std::max(max_i, i);
            any = true;
        }
    }
    if (!any) {
        return 0;
    }
    return static_cast<std::size_t>(max_i) + 1;
}

int vem_infer_order(std::size_t num_nodes,
                    const std::vector<std::vector<std::uint32_t>>& faces) {
    const auto nv = poly_vertex_count(faces);
    if (num_nodes == nv) {
        return 1;
    }
    const auto ne = poly_edges(faces).size();
    if (num_nodes == nv + ne) {
        return 2;
    }
    throw FeaError(
        std::format("vem_infer_order: {} nodes, {} verts, {} edges — expected nv or nv+ne",
                    num_nodes, nv, ne));
}

double poly_volume(const std::vector<Eigen::Vector3d>& coords,
                   const std::vector<std::vector<std::uint32_t>>& faces) {
    double vol = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        double area = 0.0;
        const Eigen::Vector3d nA = face_normal_area(coords, face, area);
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        for (auto i : face) {
            c += coords[i];
        }
        c /= static_cast<double>(face.size());
        vol += c.dot(nA) / 3.0;
    }
    return vol;
}

Eigen::Vector3d poly_centroid(const std::vector<Eigen::Vector3d>& coords,
                              const std::vector<std::vector<std::uint32_t>>& faces) {
    // Volume-weighted centroid via divergence: for each tet of the fan from
    // an arbitrary origin, accumulate first moments. Use vertex mean as a
    // stable interior point for the fan.
    const auto nv = poly_vertex_count(faces);
    if (nv == 0) {
        return Eigen::Vector3d::Zero();
    }
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < nv; ++i) {
        mean += coords[i];
    }
    mean /= static_cast<double>(nv);

    Eigen::Vector3d moment = Eigen::Vector3d::Zero();
    double vol = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        const auto& a = coords[face[0]];
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            const auto& b = coords[face[i]];
            const auto& c = coords[face[i + 1]];
            // Tet (mean, a, b, c) — signed volume and centroid.
            const double six_v = (a - mean).dot((b - mean).cross(c - mean));
            const double v = six_v / 6.0;
            // Geometric centroid of tet vertices.
            const Eigen::Vector3d tc = 0.25 * (mean + a + b + c);
            moment += v * tc;
            vol += v;
        }
    }
    if (std::abs(vol) < 1e-30) {
        return mean;
    }
    return moment / vol;
}

Eigen::MatrixXd vem_poly_stiffness(const std::vector<Eigen::Vector3d>& coords,
                                   const std::vector<std::vector<std::uint32_t>>& faces,
                                   const Material& material, int order) {
    if (order == 1) {
        return vem_stiffness_k1(coords, faces, material);
    }
    if (order == 2) {
        return vem_stiffness_k2(coords, faces, material);
    }
    throw FeaError(std::format("vem_poly_stiffness: unsupported order {}", order));
}

Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material) {
    const int order = vem_infer_order(cell.nodes.size(), cell.faces);
    return vem_poly_stiffness(mesh, cell, material, order);
}

Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material, int order) {
    std::vector<Eigen::Vector3d> coords;
    coords.reserve(cell.nodes.size());
    for (auto id : cell.nodes) {
        if (id >= mesh.nodes.size()) {
            throw FeaError("vem_poly_stiffness: node out of range");
        }
        coords.push_back(mesh.nodes[id]);
    }
    return vem_poly_stiffness(coords, cell.faces, material, order);
}

PolyCell hex8_as_poly(const NodalElement& hex) {
    if (hex.nodes.size() != 8) {
        throw FeaError("hex8_as_poly: expected 8 nodes");
    }
    PolyCell c;
    c.nodes = hex.nodes;
    c.faces = {
        {0, 3, 2, 1}, // bottom -z
        {4, 5, 6, 7}, // top +z
        {0, 1, 5, 4}, // -y
        {2, 3, 7, 6}, // +y
        {0, 4, 7, 3}, // -x
        {1, 2, 6, 5}, // +x
    };
    return c;
}

PolyCell hex20_as_poly(const NodalElement& hex20) {
    if (hex20.nodes.size() != 20) {
        throw FeaError("hex20_as_poly: expected 20 nodes");
    }
    // Keep canonical hex20 node order (verts 0..7, mids 8..19 per nodal_mesh.hpp).
    // Stiffness maps this to poly_edges mid order only if needed; hex serendipity
    // path consumes hex20 order directly via reordering helpers.
    PolyCell c;
    c.nodes = hex20.nodes;
    c.faces = hex8_as_poly(NodalElement{ElementType::kHex8,
                                        {hex20.nodes.begin(), hex20.nodes.begin() + 8}})
                  .faces;
    // Append nothing — mids already present. Verify they match poly_edges geometry:
    // nodes must be (8 verts) + (12 mids). poly_edges order may differ from hex20;
    // vem_stiffness_k2 reorders via endpoint pairs.
    // Rebuild as verts + poly_edges-ordered mids so infer_order and general path agree.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mid_of;
    for (std::size_t e = 0; e < kHex20Edges.size(); ++e) {
        const auto a = static_cast<std::uint32_t>(kHex20Edges[e][0]);
        const auto b = static_cast<std::uint32_t>(kHex20Edges[e][1]);
        mid_of[std::minmax(a, b)] = hex20.nodes[8 + e];
    }
    c.nodes.assign(hex20.nodes.begin(), hex20.nodes.begin() + 8);
    for (const auto& e : poly_edges(c.faces)) {
        c.nodes.push_back(mid_of.at(std::minmax(e[0], e[1])));
    }
    return c;
}

PolyCell tet4_as_poly(const NodalElement& tet) {
    if (tet.nodes.size() != 4) {
        throw FeaError("tet4_as_poly: expected 4 nodes");
    }
    PolyCell c;
    c.nodes = tet.nodes;
    c.faces = {
        {0, 2, 1},
        {0, 1, 3},
        {0, 3, 2},
        {1, 2, 3},
    };
    return c;
}

Eigen::VectorXd
vem_body_load(const std::vector<Eigen::Vector3d>& coords,
              const std::vector<std::vector<std::uint32_t>>& faces,
              const std::function<Eigen::Vector3d(const Eigen::Vector3d&)>& body, int order) {
    const auto n = coords.size();
    Eigen::VectorXd f = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(n));
    const double vol = poly_volume(coords, faces);
    if (vol <= 0.0) {
        return f;
    }
    if (order == 1) {
        const Eigen::Vector3d c = poly_centroid(coords, faces);
        const Eigen::Vector3d b = body(c);
        const Eigen::Vector3d share = b * (vol / static_cast<double>(n));
        for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(n); ++i) {
            f.segment<3>(3 * i) = share;
        }
        return f;
    }
    if (order != 2) {
        throw FeaError(std::format("vem_body_load: unsupported order {}", order));
    }

    // Hex serendipity: consistent hex20 body load, then scatter to poly node order.
    if (is_hex_serendipity_cell(coords, faces)) {
        const auto c20 = hex20_coords_canonical(coords, faces);
        Eigen::Matrix<double, Eigen::Dynamic, 3> x(20, 3);
        for (int a = 0; a < 20; ++a) {
            x.row(a) = c20[static_cast<std::size_t>(a)].transpose();
        }
        Eigen::VectorXd f20 = Eigen::VectorXd::Zero(60);
        for (const auto& qp : hex_rule(4)) {
            const auto shape = eval_shape(ElementType::kHex20, qp.xi);
            const Eigen::Matrix3d jac = shape.dn.transpose() * x;
            const double det = jac.determinant();
            if (det <= 0.0) {
                throw FeaError("vem_body_load k=2 hex: non-positive Jacobian");
            }
            const Eigen::Vector3d point = x.transpose() * shape.n;
            const Eigen::Vector3d b = body(point);
            for (int a = 0; a < 20; ++a) {
                f20.segment<3>(3 * a) += shape.n[a] * b * (det * qp.weight);
            }
        }
        const auto perm = hex20_dof_perm_from_poly(faces); // perm[hex]=poly
        for (int h = 0; h < 20; ++h) {
            const int p = perm[h];
            f.segment<3>(3 * p) = f20.segment<3>(3 * h);
        }
        return f;
    }

    const P2Projector proj = make_p2_projector(coords, faces);
    const Eigen::VectorXd f_poly =
        integrate_p2_vector(proj.fan, [&](const Eigen::Vector3d& x) {
            const Eigen::Vector3d xi = (x - proj.xc) / proj.h;
            const auto m = p2_monomials(xi);
            const Eigen::Vector3d b = body(x);
            Eigen::VectorXd dens(kP2Vec);
            for (int a = 0; a < kP2Mono; ++a) {
                dens[3 * a + 0] = m[a] * b[0];
                dens[3 * a + 1] = m[a] * b[1];
                dens[3 * a + 2] = m[a] * b[2];
            }
            return dens;
        });
    f = proj.pi.transpose() * f_poly;
    return f;
}

double vem_energy_error_sq(
    const std::vector<Eigen::Vector3d>& coords,
    const std::vector<std::vector<std::uint32_t>>& faces, const Eigen::VectorXd& u_elem,
    const Material& material,
    const std::function<Eigen::Matrix<double, 6, 1>(const Eigen::Vector3d&)>& exact_strain,
    int order) {
    const auto dmat = material.d_matrix();
    if (order == 1) {
        // Constant projected strain (same B-bar as stiffness).
        const auto n = coords.size();
        const double vol = poly_volume(coords, faces);
        if (vol <= 0.0) {
            return 0.0;
        }
        Eigen::Matrix<double, Eigen::Dynamic, 3> grad(static_cast<Eigen::Index>(n), 3);
        grad.setZero();
        for (const auto& face : faces) {
            if (face.size() < 3) {
                continue;
            }
            double area = 0.0;
            const Eigen::Vector3d nA = face_normal_area(coords, face, area);
            if (area <= 0.0) {
                continue;
            }
            const double w = 1.0 / static_cast<double>(face.size());
            for (auto li : face) {
                grad.row(static_cast<Eigen::Index>(li)) += (w * nA).transpose();
            }
        }
        grad /= vol;
        const auto b = b_from_grads(grad);
        const Eigen::Matrix<double, 6, 1> eps_h = b * u_elem;
        const Eigen::Vector3d c = poly_centroid(coords, faces);
        const Eigen::Matrix<double, 6, 1> diff = eps_h - exact_strain(c);
        return diff.dot(dmat * diff) * vol;
    }
    if (order != 2) {
        throw FeaError(std::format("vem_energy_error_sq: unsupported order {}", order));
    }

    if (is_hex_serendipity_cell(coords, faces)) {
        const auto c20 = hex20_coords_canonical(coords, faces);
        const auto perm = hex20_dof_perm_from_poly(faces); // perm[hex]=poly
        Eigen::VectorXd u20(60);
        for (int h = 0; h < 20; ++h) {
            u20.segment<3>(3 * h) = u_elem.segment<3>(3 * perm[h]);
        }
        Eigen::Matrix<double, Eigen::Dynamic, 3> x(20, 3);
        for (int a = 0; a < 20; ++a) {
            x.row(a) = c20[static_cast<std::size_t>(a)].transpose();
        }
        double err = 0.0;
        for (const auto& qp : hex_rule(4)) {
            const auto shape = eval_shape(ElementType::kHex20, qp.xi);
            const Eigen::Matrix3d jac = shape.dn.transpose() * x;
            const double det = jac.determinant();
            if (det <= 0.0) {
                continue;
            }
            const Eigen::Matrix3d jac_inv = jac.inverse();
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx =
                shape.dn * jac_inv.transpose();
            Eigen::Matrix<double, 6, 1> eps_h = Eigen::Matrix<double, 6, 1>::Zero();
            for (int a = 0; a < 20; ++a) {
                const Eigen::Vector3d ua = u20.segment<3>(3 * a);
                eps_h[0] += dndx(a, 0) * ua[0];
                eps_h[1] += dndx(a, 1) * ua[1];
                eps_h[2] += dndx(a, 2) * ua[2];
                eps_h[3] += dndx(a, 2) * ua[1] + dndx(a, 1) * ua[2];
                eps_h[4] += dndx(a, 2) * ua[0] + dndx(a, 0) * ua[2];
                eps_h[5] += dndx(a, 1) * ua[0] + dndx(a, 0) * ua[1];
            }
            const Eigen::Vector3d point = x.transpose() * shape.n;
            const Eigen::Matrix<double, 6, 1> diff = eps_h - exact_strain(point);
            const Eigen::Matrix<double, 6, 1> d_diff = dmat * diff;
            err += diff.dot(d_diff) * (det * qp.weight);
        }
        return err;
    }

    const P2Projector proj = make_p2_projector(coords, faces);
    const Eigen::VectorXd coeffs = proj.pi * u_elem;
    double err = 0.0;
    const auto rule = tet_rule(4);
    for (const auto& tet : proj.fan.tets) {
        const Eigen::Vector3d& v0 = tet[0];
        const Eigen::Vector3d e1 = tet[1] - v0;
        const Eigen::Vector3d e2 = tet[2] - v0;
        const Eigen::Vector3d e3 = tet[3] - v0;
        const double det = e1.dot(e2.cross(e3));
        if (det <= 0.0) {
            continue;
        }
        for (const auto& qp : rule) {
            const Eigen::Vector3d x = v0 + qp.xi[0] * e1 + qp.xi[1] * e2 + qp.xi[2] * e3;
            const Eigen::Vector3d xi = (x - proj.xc) / proj.h;
            const Eigen::Matrix<double, 6, kP2Vec> b = p2_strain_basis(xi, proj.h);
            const Eigen::Matrix<double, 6, 1> eps_h = b * coeffs;
            const Eigen::Matrix<double, 6, 1> diff = eps_h - exact_strain(x);
            const Eigen::Matrix<double, 6, 1> d_diff = dmat * diff;
            err += diff.dot(d_diff) * (det * qp.weight);
        }
    }
    return err;
}

} // namespace polymesh::fea
