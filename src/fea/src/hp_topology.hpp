// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to hp_assembly.cpp / hp_topology.cpp: shared-entity keys, per-entity
// mode-block layout, and the orientation maps that make both sides of a shared
// edge or face agree (conventions in fea/hp_assembly.hpp, ADR-0019).

#include "fea/hp_assembly.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace polymesh::fea::detail::hp {

/// Local vertex triple of each tet face, matching hierarchical.cpp.
inline constexpr std::array<std::array<int, 3>, 4> kTetF{
    {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}}};

inline bool is_hex(ElementType t) {
    return t == ElementType::kHex8 || t == ElementType::kHex20;
}
inline bool is_tet(ElementType t) {
    return t == ElementType::kTet4 || t == ElementType::kTet10;
}

using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;
using QuadKey = std::array<std::uint32_t, 4>;
using TriKey = std::array<std::uint32_t, 3>;
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

/// Sorted global vertex ids of a local edge (as a pair), hex face, or tet face.
EdgeKey elem_edge(const HpElementDef& e, int local_edge);
QuadKey elem_quad(const HpElementDef& e, int local_face);
TriKey elem_tri(const HpElementDef& e, int local_face);
// Local edge endpoints as (start, end) in the element's edge table order.
std::pair<std::uint32_t, std::uint32_t> elem_edge_oriented(const HpElementDef& e,
                                                           int local_edge);

// Number of edge modes at entity order pe (>=0; 0 if pe < 2).
inline int n_edge_modes(int pe) { return pe >= 2 ? pe - 1 : 0; }
// Hex face modes at entity order pf: (pf-1)^2 tensor bubbles (indices 2..pf).
inline int n_hex_face_modes(int pf) { return pf >= 2 ? (pf - 1) * (pf - 1) : 0; }
// Tet face modes at entity order pf: (pf-1)(pf-2)/2.
inline int n_tet_face_modes(int pf) { return pf >= 3 ? (pf - 1) * (pf - 2) / 2 : 0; }
// Hex interior modes at element order p: (p-1)^3.
inline int n_hex_interior(int p) { return p >= 2 ? (p - 1) * (p - 1) * (p - 1) : 0; }
// Tet interior modes at element order p: (p-1)(p-2)(p-3)/6.
inline int n_tet_interior(int p) { return p >= 4 ? (p - 1) * (p - 2) * (p - 3) / 6 : 0; }

// Slot of edge mode of polynomial order k (k=2..pe) within the edge block.
inline int edge_slot(int k) { return k - 2; }
// Slot of hex face mode (i,j) with i,j in 2..pf within the face block.
inline int hex_face_slot(int i, int j, int pf) { return (i - 2) * (pf - 1) + (j - 2); }

// Slot order for tet face: same as build_tet — sum = 0..pf-3, n1 = 0..sum, n2 = sum-n1.
inline int tet_face_slot(int n1, int n2) {
    // sum = n1+n2; slot = sum*(sum+1)/2 + n1
    const int sum = n1 + n2;
    return sum * (sum + 1) / 2 + n1;
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

FaceOrient hex_face_orient(const HpElementDef& e, int local_face);

// Map local hex face mode (i,j) through FaceOrient to global (i',j') and sign.
// phi_i(s) phi_j(t) with s = e0 * U, t = e1 * V where {U,V} is {S,T} possibly swapped.
void map_hex_face_mode(int i, int j, const FaceOrient& o, int& i_out, int& j_out,
                       double& sign);

/// Tet face multi-index (n1,n2) under the min-vertex origin rule: global A is the
/// min-id vertex, B and C the other two by ascending id; local modes are
/// N_{n1,n2} = la lb lc L_n1(lb-la) L_n2(lc-la) over kTetF's (a,b,c). Returns
/// global (n1',n2') and sign. Exact for p <= 4, where only (0,0), (1,0), (0,1)
/// exist and transform as signed permutations; higher indices pass through.
void map_tet_face_mode(int n1, int n2, const std::array<std::uint32_t, 3>& local_ids,
                       int& n1_out, int& n2_out, double& sign);

} // namespace polymesh::fea::detail::hp
