// SPDX-License-Identifier: BSD-3-Clause
#include "fea/hp_assembly.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

#include <Eigen/Dense>
#include <Eigen/SparseCholesky>

#include <algorithm>
#include <array>
#include <cmath>
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
// Hex face corners in local (s,t) order: origin (s,t)=(-1,-1), +s, +s+t, +t.
// Matches hierarchical kHexF vary0/vary1 convention.
constexpr std::array<std::array<int, 4>, 6> kHexFaceV{{{0, 1, 2, 3},
                                                       {4, 5, 6, 7},
                                                       {0, 1, 5, 4},
                                                       {3, 2, 6, 7},
                                                       {0, 3, 7, 4},
                                                       {1, 2, 6, 5}}};
constexpr std::array<std::array<int, 2>, 6> kTetE{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
constexpr std::array<std::array<int, 3>, 4> kTetF{
    {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}}};

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
using QuadKey = std::array<std::uint32_t, 4>;
QuadKey quad_key(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    QuadKey k{a, b, c, d};
    std::sort(k.begin(), k.end());
    return k;
}
using TriKey = std::array<std::uint32_t, 3>;
TriKey tri_key(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    TriKey k{a, b, c};
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

EdgeKey elem_edge(const HpElementDef& e, int local_edge) {
    const auto& tab =
        is_hex(e.type) ? kHexE[static_cast<std::size_t>(local_edge)]
                       : kTetE[static_cast<std::size_t>(local_edge)];
    return edge_key(e.vertices[static_cast<std::size_t>(tab[0])],
                    e.vertices[static_cast<std::size_t>(tab[1])]);
}

// Local edge endpoints as (start, end) in the element's edge table order.
std::pair<std::uint32_t, std::uint32_t> elem_edge_oriented(const HpElementDef& e, int local_edge) {
    const auto& tab =
        is_hex(e.type) ? kHexE[static_cast<std::size_t>(local_edge)]
                       : kTetE[static_cast<std::size_t>(local_edge)];
    return {e.vertices[static_cast<std::size_t>(tab[0])],
            e.vertices[static_cast<std::size_t>(tab[1])]};
}

QuadKey elem_quad(const HpElementDef& e, int local_face) {
    const auto& q = kHexFaceV[static_cast<std::size_t>(local_face)];
    return quad_key(e.vertices[static_cast<std::size_t>(q[0])],
                    e.vertices[static_cast<std::size_t>(q[1])],
                    e.vertices[static_cast<std::size_t>(q[2])],
                    e.vertices[static_cast<std::size_t>(q[3])]);
}

TriKey elem_tri(const HpElementDef& e, int local_face) {
    const auto& t = kTetF[static_cast<std::size_t>(local_face)];
    return tri_key(e.vertices[static_cast<std::size_t>(t[0])],
                   e.vertices[static_cast<std::size_t>(t[1])],
                   e.vertices[static_cast<std::size_t>(t[2])]);
}

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const {
        return (static_cast<std::size_t>(k.first) << 32) ^ k.second;
    }
};
struct QuadKeyHash {
    std::size_t operator()(const QuadKey& k) const {
        std::size_t h = 1469598103934665603ULL;
        for (auto v : k) {
            h = (h ^ v) * 1099511628211ULL;
        }
        return h;
    }
};
struct TriKeyHash {
    std::size_t operator()(const TriKey& k) const {
        std::size_t h = 1469598103934665603ULL;
        for (auto v : k) {
            h = (h ^ v) * 1099511628211ULL;
        }
        return h;
    }
};

// Number of edge modes at entity order pe (>=0; 0 if pe < 2).
int n_edge_modes(int pe) {
    return pe >= 2 ? pe - 1 : 0;
}
// Hex face modes at entity order pf: (pf-1)^2 tensor bubbles (indices 2..pf).
int n_hex_face_modes(int pf) {
    return pf >= 2 ? (pf - 1) * (pf - 1) : 0;
}
// Tet face modes at entity order pf: (pf-1)(pf-2)/2.
int n_tet_face_modes(int pf) {
    return pf >= 3 ? (pf - 1) * (pf - 2) / 2 : 0;
}
// Hex interior modes at element order p: (p-1)^3.
int n_hex_interior(int p) {
    return p >= 2 ? (p - 1) * (p - 1) * (p - 1) : 0;
}
// Tet interior modes at element order p: (p-1)(p-2)(p-3)/6.
int n_tet_interior(int p) {
    return p >= 4 ? (p - 1) * (p - 2) * (p - 3) / 6 : 0;
}

// Slot of edge mode of polynomial order k (k=2..pe) within the edge block.
int edge_slot(int k) {
    return k - 2;
}
// Slot of hex face mode (i,j) with i,j in 2..pf within the face block.
int hex_face_slot(int i, int j, int pf) {
    return (i - 2) * (pf - 1) + (j - 2);
}

// Dihedral orientation of a hex face: local (s,t) vs global (S,T).
// Global frame: origin = min-id vertex; +S toward the adjacent corner with
// smaller id; +T toward the other adjacent corner.
// Then s = sign0 * (swap ? T : S), t = sign1 * (swap ? S : T).
struct FaceOrient {
    bool swap = false;
    int sign0 = 1; // ±1
    int sign1 = 1; // ±1
};

FaceOrient hex_face_orient(const HpElementDef& e, int local_face) {
    const auto& q = kHexFaceV[static_cast<std::size_t>(local_face)];
    // Local corners: 0=(-1,-1), 1=(+1,-1), 2=(+1,+1), 3=(-1,+1)
    const std::array<std::uint32_t, 4> id{
        e.vertices[static_cast<std::size_t>(q[0])], e.vertices[static_cast<std::size_t>(q[1])],
        e.vertices[static_cast<std::size_t>(q[2])], e.vertices[static_cast<std::size_t>(q[3])]};
    const std::array<double, 4> s{-1, 1, 1, -1};
    const std::array<double, 4> t{-1, -1, 1, 1};
    // Neighbour pairs on the face cycle: 0-1, 1-2, 2-3, 3-0.
    const std::array<std::array<int, 2>, 4> nbr{{{{1, 3}}, {{0, 2}}, {{1, 3}}, {{0, 2}}}};

    int go = 0;
    for (int i = 1; i < 4; ++i) {
        if (id[static_cast<std::size_t>(i)] < id[static_cast<std::size_t>(go)]) {
            go = i;
        }
    }
    const int n0 = nbr[static_cast<std::size_t>(go)][0];
    const int n1 = nbr[static_cast<std::size_t>(go)][1];
    int s_idx = n0;
    int t_idx = n1;
    if (id[static_cast<std::size_t>(n1)] < id[static_cast<std::size_t>(n0)]) {
        s_idx = n1;
        t_idx = n0;
    }

    // Global S runs along local axis from go to s_idx; T from go to t_idx.
    // S = cS * local_coord, where cS is the local coord of s_idx on that axis.
    const bool s_along_s = (std::abs(s[static_cast<std::size_t>(s_idx)] -
                                     s[static_cast<std::size_t>(go)]) > 1e-12);
    const bool t_along_s = (std::abs(s[static_cast<std::size_t>(t_idx)] -
                                     s[static_cast<std::size_t>(go)]) > 1e-12);

    FaceOrient o;
    // We need local (s,t) in terms of global (S,T):
    // If S along local s: S = s_idx_s * s  (since at s_idx, S=+1 and s = s_idx_s)
    //   => s = s_idx_s * S
    // If S along local t: S = s_idx_t * t => t = s_idx_t * S
    if (s_along_s && !t_along_s) {
        // S || s, T || t  (no swap of axes)
        o.swap = false;
        o.sign0 = static_cast<int>(s[static_cast<std::size_t>(s_idx)]); // s = sign0 * S
        o.sign1 = static_cast<int>(t[static_cast<std::size_t>(t_idx)]); // t = sign1 * T
    } else if (!s_along_s && t_along_s) {
        // S || t, T || s  (swap)
        o.swap = true;
        o.sign0 = static_cast<int>(t[static_cast<std::size_t>(s_idx)]); // s = sign0 * T? wait
        // S = t_S * t => t = t_S * S, with t_S = t[s_idx]
        // T = s_T * s => s = s_T * T, with s_T = s[t_idx]
        o.sign0 = static_cast<int>(s[static_cast<std::size_t>(t_idx)]); // s = sign0 * T
        o.sign1 = static_cast<int>(t[static_cast<std::size_t>(s_idx)]); // t = sign1 * S
    } else {
        // Degenerate (should not happen on a non-collapsed quad).
        o = {};
    }
    return o;
}

// Map local hex face mode (i,j) through FaceOrient to global (i',j') and sign.
// phi_i(s) phi_j(t) with s = e0 * U, t = e1 * V where {U,V} is {S,T} possibly swapped.
void map_hex_face_mode(int i, int j, const FaceOrient& o, int& i_out, int& j_out, double& sign) {
    // s = sign0 * (swap ? T : S), t = sign1 * (swap ? S : T)
    // phi_i(s) = phi_i(sign0 * X) = (sign0 < 0 ? (-1)^i : 1) phi_i(X)
    const double si = (o.sign0 < 0 && (i % 2 == 1)) ? -1.0 : 1.0;
    const double sj = (o.sign1 < 0 && (j % 2 == 1)) ? -1.0 : 1.0;
    sign = si * sj;
    if (!o.swap) {
        // X=S, Y=T => global mode (i,j)
        i_out = i;
        j_out = j;
    } else {
        // s = sign0*T, t = sign1*S => phi_i(T)*phi_j(S) = global (j,i)
        i_out = j;
        j_out = i;
    }
}

// Tet face orientation for multi-index modes: global origin = min-id vertex
// among the three; local (a,b,c) from kTetF. For the invariant product
// λa λb λc (n1=n2=0) sign is always +1. Higher kernels re-express Legendre
// args relative to the global origin.
//
// We store face modes in a canonical order keyed by multi-index (n1,n2) with
// n1+n2 = 0,1,... using the *global* origin as vertex A and the two remaining
// vertices sorted by id as B, C. Local mode (n1,n2) relative to local (a,b,c)
// is matched by permuting arguments.
//
// For p<=3 only (n1,n2)=(0,0) exists — always identity. For p=4 the three
// modes are (0,0), (1,0), (0,1). Mapping under vertex permutation:
//   N_{n1,n2} = la lb lc L_n1(lb-la) L_n2(lc-la)
// When global uses (A,B,C) = permutation of (a,b,c), we re-index.

struct TetFaceMap {
    // For each local slot, global slot and sign (size = n_tet_face_modes(pf)).
    // Built on demand for the face's pf.
};

// Slot order for tet face: same as build_tet — sum = 0..pf-3, n1 = 0..sum, n2 = sum-n1.
int tet_face_slot(int n1, int n2) {
    // sum = n1+n2; slot = sum*(sum+1)/2 + n1
    const int sum = n1 + n2;
    return sum * (sum + 1) / 2 + n1;
}

// Map local tet face multi-index through global origin rule.
// Returns global (n1',n2') and sign. For kernels that are not pure monomials
// in (lb-la, lc-la) this is approximate; for our Legendre-on-diffs basis the
// only p<=4 modes are degree <=1 in the args, which transform cleanly.
void map_tet_face_mode(int n1, int n2, const std::array<std::uint32_t, 3>& local_ids,
                       int& n1_out, int& n2_out, double& sign) {
    // local vertices a,b,c with ids local_ids[0,1,2]
    // Global: A = min id, then B,C = remaining sorted by id.
    int ord[3] = {0, 1, 2};
    std::sort(ord, ord + 3, [&](int i, int j) {
        return local_ids[static_cast<std::size_t>(i)] < local_ids[static_cast<std::size_t>(j)];
    });
    // ord[0] is global A index in local {a,b,c}; ord[1]=B, ord[2]=C.
    // Local N = la lb lc L_n1(lb-la) L_n2(lc-la)
    // We need coefficients of L_p(lB-lA) L_q(lC-lA) in the global frame.
    //
    // Only (0,0), (1,0), (0,1) appear at p<=4:
    // (0,0): invariant, sign +1, maps to (0,0)
    // (1,0): proportional to (lb-la); express in (lB-lA, lC-lA)
    // (0,1): proportional to (lc-la)
    if (n1 == 0 && n2 == 0) {
        n1_out = 0;
        n2_out = 0;
        sign = 1.0;
        return;
    }

    // la,lb,lc as barycentric; differences:
    // On the face lA+lB+lC=1. Linear forms:
    // lb - la, lc - la in terms of which local index is A,B,C.
    // Let da = lb-la, db_form wait.
    // Evaluate: local mode (1,0) = (lb-la) * (la lb lc)
    // global (1,0) = (lB-lA) * product, global (0,1) = (lC-lA) * product.
    //
    // Express lb-la as α (lB-lA) + β (lC-lA).
    // Using the three points as basis for linear functions on the face.
    // At vertices: la=(1,0,0) in (la,lb,lc), etc.
    // lb-la at a: 0-1=-1; at b: 1-0=1; at c: 0-0=0
    // lB-lA at A: -1; at B: +1; at C: 0  (same pattern relative to A,B,C)
    //
    // If local a maps to global A (ord[0]==0), b->B (ord[1]==1), c->C: identity.
    // Generally: vertex local i has global role r where ord[r]==i... 
    // role_of_local[i] = position of i in ord.
    int role[3];
    for (int r = 0; r < 3; ++r) {
        role[ord[r]] = r; // role 0=A, 1=B, 2=C
    }
    // lb - la as function: value at local verts (a,b,c) = (-1, +1, 0)
    // lB - lA values at global (A,B,C) = (-1, +1, 0), so at local verts:
    //   at local i: value of (lB-lA) = (role[i]==0 ? -1 : role[i]==1 ? +1 : 0)
    auto diff_BA_at = [&](int local_i) {
        if (role[local_i] == 0) {
            return -1.0; // A
        }
        if (role[local_i] == 1) {
            return 1.0; // B
        }
        return 0.0; // C
    };
    auto diff_CA_at = [&](int local_i) {
        if (role[local_i] == 0) {
            return -1.0;
        }
        if (role[local_i] == 2) {
            return 1.0;
        }
        return 0.0;
    };
    // lb-la at local (a,b,c) = (-1,1,0). Find α,β: lb-la = α(lB-lA)+β(lC-lA)
    // Match at three vertices (over-determined but consistent on face).
    // At a: -1 = α diff_BA(a) + β diff_CA(a)
    // At b: +1 = α diff_BA(b) + β diff_CA(b)
    // Solve 2x2 from verts a,b.
    const double m00 = diff_BA_at(0), m01 = diff_CA_at(0);
    const double m10 = diff_BA_at(1), m11 = diff_CA_at(1);
    const double det = m00 * m11 - m01 * m10;
    double alpha_b = 0.0, beta_b = 0.0; // lb-la coeffs
    double alpha_c = 0.0, beta_c = 0.0; // lc-la coeffs
    if (std::abs(det) < 1e-14) {
        // Fall back to identity if degenerate.
        n1_out = n1;
        n2_out = n2;
        sign = 1.0;
        return;
    }
    // [m00 m01; m10 m11] [α;β] = [-1; 1]  (lb-la at verts a,b)
    alpha_b = (-1.0 * m11 - m01 * 1.0) / det;
    beta_b = (m00 * 1.0 - m10 * (-1.0)) / det;
    // rhs for lc-la: (0-1, 0-0, 1-0) at (a,b,c) = (-1, 0, 1)
    // use verts a,c:
    const double c00 = diff_BA_at(0), c01 = diff_CA_at(0);
    const double c10 = diff_BA_at(2), c11 = diff_CA_at(2);
    const double detc = c00 * c11 - c01 * c10;
    if (std::abs(detc) < 1e-14) {
        n1_out = n1;
        n2_out = n2;
        sign = 1.0;
        return;
    }
    // [c00 c01; c10 c11] [α;β] = [-1; 1]
    alpha_c = (-1.0 * c11 - c01 * 1.0) / detc;
    beta_c = (c00 * 1.0 - c10 * (-1.0)) / detc;

    // Local mode (n1,n2) for degree-1: only one of n1,n2 is 1.
    // (1,0): ~ (lb-la) = α_b (lB-lA) + β_b (lC-lA) => mix of global (1,0) and (0,1)
    // At p=4 we need a pure mapping for DOF numbering — if the transform mixes
    // modes, scatter must use a 2x2 block. For simplicity at p<=4: if the
    // result is a single global mode with sign, use it; otherwise use the
    // dominant component (should be pure ± for our axis-aligned barycentric).
    //
    // On a tet face the map is always a signed permutation of the two linear
    // forms, so (α,β) is one of {(±1,0),(0,±1)}.
    if (n1 == 1 && n2 == 0) {
        if (std::abs(alpha_b) > 0.5) {
            n1_out = 1;
            n2_out = 0;
            sign = alpha_b > 0 ? 1.0 : -1.0;
        } else {
            n1_out = 0;
            n2_out = 1;
            sign = beta_b > 0 ? 1.0 : -1.0;
        }
        return;
    }
    if (n1 == 0 && n2 == 1) {
        if (std::abs(alpha_c) > 0.5) {
            n1_out = 1;
            n2_out = 0;
            sign = alpha_c > 0 ? 1.0 : -1.0;
        } else {
            n1_out = 0;
            n2_out = 1;
            sign = beta_c > 0 ? 1.0 : -1.0;
        }
        return;
    }
    // Higher multi-indices not needed for p<=4.
    n1_out = n1;
    n2_out = n2;
    sign = 1.0;
}

int max_order_for_type(ElementType t) {
    return is_hex(t) ? 6 : 4;
}

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

    // Edge blocks: pe-1 modes (orders 2..pe), global direction min->max id.
    std::unordered_map<EdgeKey, Eigen::Index, EdgeKeyHash> edge_base;
    for (const auto& [key, pe] : edge_order) {
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

    // Hex face blocks: (pf-1)^2 modes.
    std::unordered_map<QuadKey, Eigen::Index, QuadKeyHash> quad_base;
    std::unordered_map<QuadKey, int, QuadKeyHash> quad_pf;
    for (const auto& [key, pf] : quad_order) {
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

    // Tet face blocks.
    std::unordered_map<TriKey, Eigen::Index, TriKeyHash> tri_base;
    std::unordered_map<TriKey, int, TriKeyHash> tri_pf;
    for (const auto& [key, pf] : tri_order) {
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
                    const int slot =
                        ((i - 2) * (p - 1) + (j - 2)) * (p - 1) + (k - 2);
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
                const double sab = sg[static_cast<std::size_t>(a)] * sg[static_cast<std::size_t>(b)];
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
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = field.dn * jac_inv.transpose();
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
