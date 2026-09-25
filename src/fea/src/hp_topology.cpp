// SPDX-License-Identifier: BSD-3-Clause
#include "hp_topology.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace polymesh::fea::detail::hp {
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
constexpr std::array<std::array<int, 4>, 6> kHexFaceV{
    {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}}};
constexpr std::array<std::array<int, 2>, 6> kTetE{
    {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};

EdgeKey edge_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}
QuadKey quad_key(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    QuadKey k{a, b, c, d};
    std::sort(k.begin(), k.end());
    return k;
}
TriKey tri_key(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    TriKey k{a, b, c};
    std::sort(k.begin(), k.end());
    return k;
}

} // namespace

EdgeKey elem_edge(const HpElementDef& e, int local_edge) {
    const auto& tab = is_hex(e.type) ? kHexE[static_cast<std::size_t>(local_edge)]
                                     : kTetE[static_cast<std::size_t>(local_edge)];
    return edge_key(e.vertices[static_cast<std::size_t>(tab[0])],
                    e.vertices[static_cast<std::size_t>(tab[1])]);
}

std::pair<std::uint32_t, std::uint32_t> elem_edge_oriented(const HpElementDef& e,
                                                           int local_edge) {
    const auto& tab = is_hex(e.type) ? kHexE[static_cast<std::size_t>(local_edge)]
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

FaceOrient hex_face_orient(const HpElementDef& e, int local_face) {
    const auto& q = kHexFaceV[static_cast<std::size_t>(local_face)];
    // Local corners: 0=(-1,-1), 1=(+1,-1), 2=(+1,+1), 3=(-1,+1)
    const std::array<std::uint32_t, 4> id{e.vertices[static_cast<std::size_t>(q[0])],
                                          e.vertices[static_cast<std::size_t>(q[1])],
                                          e.vertices[static_cast<std::size_t>(q[2])],
                                          e.vertices[static_cast<std::size_t>(q[3])]};
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

void map_hex_face_mode(int i, int j, const FaceOrient& o, int& i_out, int& j_out,
                       double& sign) {
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

    // Local mode (1,0) = (lb-la)·la lb lc; global (1,0) = (lB-lA)·product and
    // (0,1) = (lC-lA)·product. Express lb-la (and lc-la) as α(lB-lA) + β(lC-lA)
    // by matching both linear forms at the face vertices: lb-la is (-1, +1, 0) at
    // (a, b, c), lB-lA the same pattern at (A, B, C).
    // role[i] = global role (0=A, 1=B, 2=C) of local vertex i.
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

    // On a tet face the map is a signed permutation of the two linear forms, so
    // (α,β) is one of {(±1,0),(0,±1)}: take the dominant component and its sign.
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

} // namespace polymesh::fea::detail::hp
