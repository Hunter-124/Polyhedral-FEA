// SPDX-License-Identifier: BSD-3-Clause
#include "fea/hp_assembly.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "hp_topology.hpp"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::fea {
namespace {

using namespace detail::hp;

Eigen::Matrix<double, Eigen::Dynamic, 3> vertex_coords(const HpModel& model,
                                                       const HpElementDef& e) {
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(static_cast<Eigen::Index>(e.vertices.size()),
                                               3);
    for (std::size_t v = 0; v < e.vertices.size(); ++v) {
        x.row(static_cast<Eigen::Index>(v)) = model.nodes[e.vertices[v]].transpose();
    }
    return x;
}

int max_order_for_type(ElementType t) { return is_hex(t) ? 6 : 4; }

} // namespace

HpSystem assemble_hp(const HpModel& model, const Material& material) {
    for (const auto& e : model.elements) {
        if (e.order < 1 || static_cast<int>(e.order) > max_order_for_type(e.type)) {
            throw FeaError("assemble_hp: order out of range for element type");
        }
        if (!is_hex(e.type) && !is_tet(e.type)) {
            throw FeaError("assemble_hp: only tet and hex supported");
        }
        if (is_hex(e.type) && e.vertices.size() != 8) {
            throw FeaError("assemble_hp: hex needs 8 vertices");
        }
        if (is_tet(e.type) && e.vertices.size() != 4) {
            throw FeaError("assemble_hp: tet needs 4 vertices");
        }
    }

    // Pass 1: minimum-rule order for every shared entity.
    constexpr int kInf = std::numeric_limits<int>::max();
    std::unordered_map<EdgeKey, int, EdgeKeyHash> edge_order;
    std::unordered_map<QuadKey, int, QuadKeyHash> quad_order;
    std::unordered_map<TriKey, int, TriKeyHash> tri_order;
    for (const auto& e : model.elements) {
        const int ne = is_hex(e.type) ? 12 : 6;
        for (int le = 0; le < ne; ++le) {
            auto& o = edge_order.try_emplace(elem_edge(e, le), kInf).first->second;
            o = std::min(o, static_cast<int>(e.order));
        }
        if (is_hex(e.type)) {
            for (int lf = 0; lf < 6; ++lf) {
                auto& o = quad_order.try_emplace(elem_quad(e, lf), kInf).first->second;
                o = std::min(o, static_cast<int>(e.order));
            }
        } else {
            for (int lf = 0; lf < 4; ++lf) {
                auto& o = tri_order.try_emplace(elem_tri(e, lf), kInf).first->second;
                o = std::min(o, static_cast<int>(e.order));
            }
        }
    }

    // Pass 2: assign global mode index blocks.
    HpSystem sys;
    const auto nverts = static_cast<Eigen::Index>(model.nodes.size());
    sys.mode_nodes.resize(static_cast<std::size_t>(nverts));
    for (Eigen::Index v = 0; v < nverts; ++v) {
        sys.mode_nodes[static_cast<std::size_t>(v)] = {static_cast<std::uint32_t>(v)};
    }
    Eigen::Index next = nverts;

    // Edge/face blocks are numbered by ascending key (EdgeKey pair, Quad/TriKey
    // arrays compare lexicographically), not in unordered_map bucket order: bucket
    // order is implementation-defined, and `next` hands out global mode blocks, so
    // it would permute the linear system (and its rounding) per standard library.
    std::vector<EdgeKey> edge_keys;
    edge_keys.reserve(edge_order.size());
    for (const auto& [key, pe] : edge_order) {
        edge_keys.push_back(key);
    }
    std::sort(edge_keys.begin(), edge_keys.end());

    // Edge blocks: pe-1 modes (orders 2..pe), global direction min->max id.
    std::unordered_map<EdgeKey, Eigen::Index, EdgeKeyHash> edge_base;
    for (const auto& key : edge_keys) {
        const int pe = edge_order[key];
        const int nm = n_edge_modes(pe);
        if (nm <= 0) {
            continue;
        }
        edge_base[key] = next;
        for (int s = 0; s < nm; ++s) {
            sys.mode_nodes.push_back({key.first, key.second});
        }
        next += nm;
    }

    std::vector<QuadKey> quad_keys;
    quad_keys.reserve(quad_order.size());
    for (const auto& [key, pf] : quad_order) {
        quad_keys.push_back(key);
    }
    std::sort(quad_keys.begin(), quad_keys.end());

    // Hex face blocks: (pf-1)^2 modes.
    std::unordered_map<QuadKey, Eigen::Index, QuadKeyHash> quad_base;
    std::unordered_map<QuadKey, int, QuadKeyHash> quad_pf;
    for (const auto& key : quad_keys) {
        const int pf = quad_order[key];
        const int nm = n_hex_face_modes(pf);
        if (nm <= 0) {
            continue;
        }
        quad_base[key] = next;
        quad_pf[key] = pf;
        for (int s = 0; s < nm; ++s) {
            sys.mode_nodes.push_back({key[0], key[1], key[2], key[3]});
        }
        next += nm;
    }

    std::vector<TriKey> tri_keys;
    tri_keys.reserve(tri_order.size());
    for (const auto& [key, pf] : tri_order) {
        tri_keys.push_back(key);
    }
    std::sort(tri_keys.begin(), tri_keys.end());

    // Tet face blocks.
    std::unordered_map<TriKey, Eigen::Index, TriKeyHash> tri_base;
    std::unordered_map<TriKey, int, TriKeyHash> tri_pf;
    for (const auto& key : tri_keys) {
        const int pf = tri_order[key];
        const int nm = n_tet_face_modes(pf);
        if (nm <= 0) {
            continue;
        }
        tri_base[key] = next;
        tri_pf[key] = pf;
        for (int s = 0; s < nm; ++s) {
            sys.mode_nodes.push_back({key[0], key[1], key[2]});
        }
        next += nm;
    }

    // Interior blocks (per element).
    std::vector<Eigen::Index> interior_base(model.elements.size(), -1);
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        const int nm = is_hex(e.type) ? n_hex_interior(e.order) : n_tet_interior(e.order);
        if (nm <= 0) {
            continue;
        }
        interior_base[ei] = next;
        for (int s = 0; s < nm; ++s) {
            sys.mode_nodes.push_back(e.vertices);
        }
        next += nm;
    }
    sys.n_modes = next;
    sys.ndof = 3 * next;

    // Pass 3: local -> global + signs, assemble.
    sys.local_to_global.resize(model.elements.size());
    sys.local_sign.resize(model.elements.size());
    std::vector<Eigen::Triplet<double>> triplets;
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        const auto modes = hp_modes(e.type, e.order);
        auto& g = sys.local_to_global[ei];
        auto& sg = sys.local_sign[ei];
        g.assign(modes.size(), -1);
        sg.assign(modes.size(), 1.0);

        for (std::size_t m = 0; m < modes.size(); ++m) {
            const auto& mode = modes[m];
            switch (mode.entity) {
            case HpMode::Entity::kVertex:
                g[m] = static_cast<Eigen::Index>(e.vertices[mode.entity_index]);
                break;
            case HpMode::Entity::kEdge: {
                const EdgeKey key = elem_edge(e, mode.entity_index);
                const auto it = edge_base.find(key);
                if (it == edge_base.end()) {
                    break; // suppressed (pe < 2)
                }
                const int pe = edge_order[key];
                const int k = mode.order;
                if (k > pe) {
                    break; // min rule
                }
                g[m] = it->second + edge_slot(k);
                // Sign: global direction is min->max id; local is table order.
                const auto [lo, hi] = elem_edge_oriented(e, mode.entity_index);
                const bool reversed = (lo != key.first); // key.first is min id
                // Tet edge kernels are functions of (λ_b - λ_a) and reverse as
                // (-1)^k when the local endpoint order opposes the global
                // min→max direction. Hex tensor-product edge modes are tied to
                // reference coordinates (φ_k(ξ)·hats), not the directed edge
                // parameter — endpoint order does not flip them (sign stays +1
                // for consistently numbered right-handed hexes).
                if (reversed && is_tet(e.type)) {
                    sg[m] = (k % 2 == 0) ? 1.0 : -1.0;
                }
                break;
            }
            case HpMode::Entity::kFace: {
                if (is_hex(e.type)) {
                    const QuadKey key = elem_quad(e, mode.entity_index);
                    const auto it = quad_base.find(key);
                    if (it == quad_base.end()) {
                        break;
                    }
                    const int pf = quad_pf[key];
                    const int i = mode.index0;
                    const int j = mode.index1;
                    if (i > pf || j > pf) {
                        break;
                    }
                    const FaceOrient o = hex_face_orient(e, mode.entity_index);
                    int ig = 0, jg = 0;
                    double sign = 1.0;
                    map_hex_face_mode(i, j, o, ig, jg, sign);
                    if (ig < 2 || jg < 2 || ig > pf || jg > pf) {
                        break;
                    }
                    g[m] = it->second + hex_face_slot(ig, jg, pf);
                    sg[m] = sign;
                } else {
                    const TriKey key = elem_tri(e, mode.entity_index);
                    const auto it = tri_base.find(key);
                    if (it == tri_base.end()) {
                        break;
                    }
                    const int pf = tri_pf[key];
                    const int n1 = mode.index0;
                    const int n2 = mode.index1;
                    if (n1 + n2 + 3 > pf) {
                        break;
                    }
                    const auto& tv = kTetF[mode.entity_index];
                    const std::array<std::uint32_t, 3> lids{
                        e.vertices[static_cast<std::size_t>(tv[0])],
                        e.vertices[static_cast<std::size_t>(tv[1])],
                        e.vertices[static_cast<std::size_t>(tv[2])]};
                    int n1g = 0, n2g = 0;
                    double sign = 1.0;
                    map_tet_face_mode(n1, n2, lids, n1g, n2g, sign);
                    if (n1g + n2g + 3 > pf) {
                        break;
                    }
                    g[m] = it->second + tet_face_slot(n1g, n2g);
                    sg[m] = sign;
                }
                break;
            }
            case HpMode::Entity::kInterior: {
                if (interior_base[ei] < 0) {
                    break;
                }
                if (is_hex(e.type)) {
                    // Interior ordered i,j,k each in 2..p (same as build_hex).
                    const int p = e.order;
                    const int i = mode.index0, j = mode.index1, k = mode.index2;
                    const int slot = ((i - 2) * (p - 1) + (j - 2)) * (p - 1) + (k - 2);
                    g[m] = interior_base[ei] + slot;
                } else {
                    // Tet interior slot: same enumeration as build_tet.
                    const int n1 = mode.index0, n2 = mode.index1, n3 = mode.index2;
                    int slot = 0;
                    bool found = false;
                    const int p = e.order;
                    for (int sum = 0; sum <= p - 4 && !found; ++sum) {
                        for (int a = 0; a <= sum && !found; ++a) {
                            for (int b = 0; a + b <= sum && !found; ++b) {
                                const int c = sum - a - b;
                                if (a == n1 && b == n2 && c == n3) {
                                    found = true;
                                    break;
                                }
                                ++slot;
                            }
                        }
                    }
                    if (found) {
                        g[m] = interior_base[ei] + slot;
                    }
                }
                break;
            }
            }
        }

        const Eigen::MatrixXd ke =
            hp_element_stiffness(vertex_coords(model, e), e.type, e.order, material);
        const auto nm = static_cast<Eigen::Index>(modes.size());
        for (Eigen::Index a = 0; a < nm; ++a) {
            if (g[static_cast<std::size_t>(a)] < 0) {
                continue;
            }
            for (Eigen::Index b = 0; b < nm; ++b) {
                if (g[static_cast<std::size_t>(b)] < 0) {
                    continue;
                }
                const Eigen::Index ga = g[static_cast<std::size_t>(a)];
                const Eigen::Index gb = g[static_cast<std::size_t>(b)];
                const double sab =
                    sg[static_cast<std::size_t>(a)] * sg[static_cast<std::size_t>(b)];
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        triplets.emplace_back(3 * ga + i, 3 * gb + j,
                                              sab * ke(3 * a + i, 3 * b + j));
                    }
                }
            }
        }
    }

    sys.k.resize(sys.ndof, sys.ndof);
    sys.k.setFromTriplets(triplets.begin(), triplets.end());
    return sys;
}

Eigen::VectorXd assemble_hp_body_load(const HpModel& model, const HpSystem& system,
                                      const BodyForce& body_force) {
    Eigen::VectorXd f = Eigen::VectorXd::Zero(system.ndof);
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        const auto x = vertex_coords(model, e);
        const ElementType geo = is_hex(e.type) ? ElementType::kHex8 : ElementType::kTet4;
        // Over-integrate manufactured body forces (smooth trig fields).
        const auto rule = is_hex(e.type) ? hex_rule(6) : tet_rule(8);
        const auto& g = system.local_to_global[ei];
        const auto& sg = system.local_sign[ei];
        for (const auto& qp : rule) {
            const auto gs = eval_shape(geo, qp.xi);
            const Eigen::Matrix3d jac = gs.dn.transpose() * x;
            const double det = jac.determinant();
            if (det <= 0.0) {
                throw FeaError("assemble_hp_body_load: non-positive Jacobian");
            }
            const Eigen::Vector3d point = x.transpose() * gs.n;
            const Eigen::Vector3d b = body_force(point);
            const auto field = hp_eval(e.type, e.order, qp.xi);
            for (Eigen::Index m = 0; m < field.n.size(); ++m) {
                const Eigen::Index gm = g[static_cast<std::size_t>(m)];
                if (gm < 0) {
                    continue;
                }
                const double s = sg[static_cast<std::size_t>(m)];
                f.segment<3>(3 * gm) += s * field.n(m) * b * (det * qp.weight);
            }
        }
    }
    return f;
}

Eigen::VectorXd solve_hp(const HpSystem& system, const Eigen::VectorXd& loads,
                         const std::map<Eigen::Index, double>& fixed) {
    const Eigen::Index n = system.ndof;
    std::vector<char> is_fixed(static_cast<std::size_t>(n), 0);
    Eigen::VectorXd u = Eigen::VectorXd::Zero(n);
    for (const auto& [dof, val] : fixed) {
        is_fixed[static_cast<std::size_t>(dof)] = 1;
        u(dof) = val;
    }
    std::vector<Eigen::Index> free_of(static_cast<std::size_t>(n), -1);
    std::vector<Eigen::Index> free_dofs;
    for (Eigen::Index i = 0; i < n; ++i) {
        if (!is_fixed[static_cast<std::size_t>(i)]) {
            free_of[static_cast<std::size_t>(i)] = static_cast<Eigen::Index>(free_dofs.size());
            free_dofs.push_back(i);
        }
    }
    const auto nf = static_cast<Eigen::Index>(free_dofs.size());
    if (nf == 0) {
        return u;
    }
    Eigen::VectorXd rhs(nf);
    for (Eigen::Index r = 0; r < nf; ++r) {
        rhs(r) = loads(free_dofs[static_cast<std::size_t>(r)]);
    }
    std::vector<Eigen::Triplet<double>> kff;
    for (Eigen::Index col = 0; col < system.k.outerSize(); ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(system.k, col); it; ++it) {
            const Eigen::Index i = it.row();
            const Eigen::Index j = it.col();
            const double v = it.value();
            const bool fi = is_fixed[static_cast<std::size_t>(i)];
            const bool fj = is_fixed[static_cast<std::size_t>(j)];
            if (!fi && !fj) {
                kff.emplace_back(free_of[static_cast<std::size_t>(i)],
                                 free_of[static_cast<std::size_t>(j)], v);
            } else if (!fi && fj) {
                rhs(free_of[static_cast<std::size_t>(i)]) -= v * u(j);
            }
        }
    }
    Eigen::SparseMatrix<double> a(nf, nf);
    a.setFromTriplets(kff.begin(), kff.end());
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
    solver.compute(a);
    if (solver.info() != Eigen::Success) {
        throw FeaError("solve_hp: factorization failed (under-constrained system?)");
    }
    const Eigen::VectorXd xf = solver.solve(rhs);
    if (solver.info() != Eigen::Success) {
        throw FeaError("solve_hp: solve failed");
    }
    for (Eigen::Index r = 0; r < nf; ++r) {
        u(free_dofs[static_cast<std::size_t>(r)]) = xf(r);
    }
    return u;
}

double hp_energy_error(const HpModel& model, const HpSystem& system, const Eigen::VectorXd& u,
                       const StrainField& exact_strain, const Material& material) {
    const auto d = material.d_matrix();
    double sum = 0.0;
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        const auto x = vertex_coords(model, e);
        const ElementType geo = is_hex(e.type) ? ElementType::kHex8 : ElementType::kTet4;
        const auto rule = is_hex(e.type) ? hex_rule(6) : tet_rule(8);
        const auto& g = system.local_to_global[ei];
        const auto& sg = system.local_sign[ei];
        const auto modes = hp_modes(e.type, e.order);
        const auto nm = static_cast<Eigen::Index>(modes.size());
        // Gather modal coefficients with orientation signs: u_local = s * u_global.
        Eigen::VectorXd ue = Eigen::VectorXd::Zero(3 * nm);
        for (Eigen::Index m = 0; m < nm; ++m) {
            const Eigen::Index gm = g[static_cast<std::size_t>(m)];
            if (gm >= 0) {
                ue.segment<3>(3 * m) = sg[static_cast<std::size_t>(m)] * u.segment<3>(3 * gm);
            }
        }
        for (const auto& qp : rule) {
            const auto gs = eval_shape(geo, qp.xi);
            const Eigen::Matrix3d jac = gs.dn.transpose() * x;
            const double det = jac.determinant();
            const Eigen::Matrix3d jac_inv = jac.inverse();
            const auto field = hp_eval(e.type, e.order, qp.xi);
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx =
                field.dn * jac_inv.transpose();
            Eigen::Matrix<double, 6, 1> eps_h = Eigen::Matrix<double, 6, 1>::Zero();
            for (Eigen::Index m = 0; m < nm; ++m) {
                const double dx = dndx(m, 0), dy = dndx(m, 1), dz = dndx(m, 2);
                const Eigen::Vector3d um = ue.segment<3>(3 * m);
                eps_h(0) += dx * um.x();
                eps_h(1) += dy * um.y();
                eps_h(2) += dz * um.z();
                eps_h(3) += dz * um.y() + dy * um.z();
                eps_h(4) += dz * um.x() + dx * um.z();
                eps_h(5) += dy * um.x() + dx * um.y();
            }
            const Eigen::Vector3d point = x.transpose() * gs.n;
            const Eigen::Matrix<double, 6, 1> diff = eps_h - exact_strain(point);
            sum += (diff.transpose() * d * diff)(0, 0) * det * qp.weight;
        }
    }
    return std::sqrt(std::max(0.0, sum));
}

} // namespace polymesh::fea
