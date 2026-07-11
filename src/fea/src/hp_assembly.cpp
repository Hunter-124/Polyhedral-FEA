// SPDX-License-Identifier: BSD-3-Clause
#include "fea/hp_assembly.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace polymesh::fea {
namespace {

// Local vertex indices of each edge / face, matching hierarchical.cpp so that
// an HpMode's entity_index selects the right element vertices.
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
constexpr std::array<std::array<int, 4>, 6> kHexFaceV{{{0, 1, 2, 3},
                                                       {4, 5, 6, 7},
                                                       {0, 1, 5, 4},
                                                       {3, 2, 6, 7},
                                                       {0, 3, 7, 4},
                                                       {1, 2, 6, 5}}};
constexpr std::array<std::array<int, 2>, 6> kTetE{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};

bool is_hex(ElementType t) {
    return t == ElementType::kHex8 || t == ElementType::kHex20;
}
bool is_tet(ElementType t) {
    return t == ElementType::kTet4 || t == ElementType::kTet10;
}

using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;
EdgeKey edge_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}
using FaceKey = std::array<std::uint32_t, 4>;
FaceKey face_key(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    FaceKey k{a, b, c, d};
    std::sort(k.begin(), k.end());
    return k;
}

Eigen::Matrix<double, Eigen::Dynamic, 3> vertex_coords(const HpModel& model,
                                                       const HpElementDef& e) {
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(static_cast<Eigen::Index>(e.vertices.size()), 3);
    for (std::size_t v = 0; v < e.vertices.size(); ++v) {
        x.row(static_cast<Eigen::Index>(v)) = model.nodes[e.vertices[v]].transpose();
    }
    return x;
}

// Element-local edge/face -> global node ids for the entity the mode sits on.
EdgeKey elem_edge(const HpElementDef& e, int local_edge) {
    const auto& tab =
        is_hex(e.type) ? kHexE[static_cast<std::size_t>(local_edge)]
                       : kTetE[static_cast<std::size_t>(local_edge)];
    return edge_key(e.vertices[static_cast<std::size_t>(tab[0])],
                    e.vertices[static_cast<std::size_t>(tab[1])]);
}
FaceKey elem_face(const HpElementDef& e, int local_face) {
    const auto& q = kHexFaceV[static_cast<std::size_t>(local_face)];
    return face_key(e.vertices[static_cast<std::size_t>(q[0])],
                    e.vertices[static_cast<std::size_t>(q[1])],
                    e.vertices[static_cast<std::size_t>(q[2])],
                    e.vertices[static_cast<std::size_t>(q[3])]);
}

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const {
        return (static_cast<std::size_t>(k.first) << 32) ^ k.second;
    }
};
struct FaceKeyHash {
    std::size_t operator()(const FaceKey& k) const {
        std::size_t h = 1469598103934665603ULL;
        for (auto v : k) {
            h = (h ^ v) * 1099511628211ULL;
        }
        return h;
    }
};

} // namespace

HpSystem assemble_hp(const HpModel& model, const Material& material) {
    for (const auto& e : model.elements) {
        if (e.order < 1 || e.order > 2) {
            throw FeaError("assemble_hp: only order 1..2 supported in this increment");
        }
        if (!is_hex(e.type) && !is_tet(e.type)) {
            throw FeaError("assemble_hp: only tet and hex supported");
        }
    }

    // Pass 1: minimum-rule order for every shared entity.
    constexpr int kInf = std::numeric_limits<int>::max();
    std::unordered_map<EdgeKey, int, EdgeKeyHash> edge_order;
    std::unordered_map<FaceKey, int, FaceKeyHash> face_order;
    for (const auto& e : model.elements) {
        const int ne = is_hex(e.type) ? 12 : 6;
        for (int le = 0; le < ne; ++le) {
            auto& o = edge_order.try_emplace(elem_edge(e, le), kInf).first->second;
            o = std::min(o, static_cast<int>(e.order));
        }
        if (is_hex(e.type)) {
            for (int lf = 0; lf < 6; ++lf) {
                auto& o = face_order.try_emplace(elem_face(e, lf), kInf).first->second;
                o = std::min(o, static_cast<int>(e.order));
            }
        }
    }

    // Pass 2: assign global mode indices. Vertices first (mode == node id), then
    // edge / face / interior blocks (one mode each at order 2).
    HpSystem sys;
    const auto nverts = static_cast<Eigen::Index>(model.nodes.size());
    sys.mode_nodes.resize(static_cast<std::size_t>(nverts));
    for (Eigen::Index v = 0; v < nverts; ++v) {
        sys.mode_nodes[static_cast<std::size_t>(v)] = {static_cast<std::uint32_t>(v)};
    }
    Eigen::Index next = nverts;
    std::unordered_map<EdgeKey, Eigen::Index, EdgeKeyHash> edge_mode;
    for (const auto& [key, order] : edge_order) {
        if (order >= 2) {
            edge_mode[key] = next++;
            sys.mode_nodes.push_back({key.first, key.second});
        }
    }
    std::unordered_map<FaceKey, Eigen::Index, FaceKeyHash> face_mode;
    for (const auto& [key, order] : face_order) {
        if (order >= 2) {
            face_mode[key] = next++;
            sys.mode_nodes.push_back({key[0], key[1], key[2], key[3]});
        }
    }
    std::vector<Eigen::Index> interior_mode(model.elements.size(), -1);
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        if (is_hex(e.type) && e.order >= 2) {
            interior_mode[ei] = next++;
            sys.mode_nodes.push_back(e.vertices);
        }
    }
    sys.n_modes = next;
    sys.ndof = 3 * next;

    // Pass 3: per-element local mode -> global mode, honouring the min rule.
    sys.local_to_global.resize(model.elements.size());
    std::vector<Eigen::Triplet<double>> triplets;
    for (std::size_t ei = 0; ei < model.elements.size(); ++ei) {
        const auto& e = model.elements[ei];
        const auto modes = hp_modes(e.type, e.order);
        auto& g = sys.local_to_global[ei];
        g.assign(modes.size(), -1);
        for (std::size_t m = 0; m < modes.size(); ++m) {
            const auto& mode = modes[m];
            switch (mode.entity) {
            case HpMode::Entity::kVertex:
                g[m] = static_cast<Eigen::Index>(e.vertices[mode.entity_index]);
                break;
            case HpMode::Entity::kEdge: {
                const auto it = edge_mode.find(elem_edge(e, mode.entity_index));
                g[m] = (it == edge_mode.end()) ? -1 : it->second; // suppressed by min rule
                break;
            }
            case HpMode::Entity::kFace: {
                const auto it = face_mode.find(elem_face(e, mode.entity_index));
                g[m] = (it == face_mode.end()) ? -1 : it->second;
                break;
            }
            case HpMode::Entity::kInterior:
                g[m] = interior_mode[ei];
                break;
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
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        triplets.emplace_back(3 * ga + i, 3 * gb + j, ke(3 * a + i, 3 * b + j));
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
        // Over-integrate: manufactured body forces are typically higher-degree
        // than the stiffness integrand.
        const auto rule = is_hex(e.type) ? hex_rule(5) : tet_rule(6);
        const auto& g = system.local_to_global[ei];
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
                f.segment<3>(3 * gm) += field.n(m) * b * (det * qp.weight);
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
    // Map free DOFs to a compact index.
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
    // rhs = loads_f - K_fc u_c, and K_ff, from the triplets of K.
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
        const auto rule = is_hex(e.type) ? hex_rule(5) : tet_rule(6);
        const auto& g = system.local_to_global[ei];
        const auto modes = hp_modes(e.type, e.order);
        const auto nm = static_cast<Eigen::Index>(modes.size());
        // Gather this element's modal coefficients (suppressed modes -> 0).
        Eigen::VectorXd ue = Eigen::VectorXd::Zero(3 * nm);
        for (Eigen::Index m = 0; m < nm; ++m) {
            const Eigen::Index gm = g[static_cast<std::size_t>(m)];
            if (gm >= 0) {
                ue.segment<3>(3 * m) = u.segment<3>(3 * gm);
            }
        }
        for (const auto& qp : rule) {
            const auto gs = eval_shape(geo, qp.xi);
            const Eigen::Matrix3d jac = gs.dn.transpose() * x;
            const double det = jac.determinant();
            const Eigen::Matrix3d jac_inv = jac.inverse();
            const auto field = hp_eval(e.type, e.order, qp.xi);
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = field.dn * jac_inv.transpose();
            // Strain from modal coefficients (Voigt engineering).
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
