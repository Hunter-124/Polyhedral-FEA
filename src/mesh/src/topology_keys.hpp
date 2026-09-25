// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Canonical topology keys for tet-mesh face/edge census (mesh-private).
//
// Key identity is the contract: a triangle is its three node ids sorted
// ascending, an undirected edge is (min, max). Hashers come in two fixed
// variants because unordered-container iteration order is observable by some
// callers — never swap one caller's hasher for the other, and never edit the
// mixing (or the noexcept, which changes libstdc++ hash caching).

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace polymesh::mesh::detail {

/// Unoriented triangle: node ids sorted ascending.
using TriKey = std::array<std::uint32_t, 3>;

/// Undirected edge: (smaller id, larger id).
using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;

/// Local corner indices of the four triangular faces of a tet.
inline constexpr int kTetFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};

inline TriKey sorted_tri_key(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    TriKey f{{a, b, c}};
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    if (f[1] > f[2]) {
        std::swap(f[1], f[2]);
    }
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    return f;
}

inline EdgeKey sorted_edge_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

/// Prime-multiply XOR hash (spatial-hash primes 73856093 / 19349663 / 83492791).
struct TriKeyXorHash {
    std::size_t operator()(const TriKey& f) const {
        return (static_cast<std::size_t>(f[0]) * 73856093u) ^
               (static_cast<std::size_t>(f[1]) * 19349663u) ^
               (static_cast<std::size_t>(f[2]) * 83492791u);
    }
};

struct EdgeKeyXorHash {
    std::size_t operator()(const EdgeKey& e) const {
        return (static_cast<std::size_t>(e.first) * 73856093u) ^
               (static_cast<std::size_t>(e.second) * 19349663u);
    }
};

/// Golden-ratio combine hash (boost::hash_combine style) seeded by the first id.
struct TriKeyMixHash {
    std::size_t operator()(const TriKey& f) const noexcept {
        std::size_t h = f[0];
        h ^= static_cast<std::size_t>(f[1]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(f[2]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct EdgeKeyMixHash {
    std::size_t operator()(const EdgeKey& e) const noexcept {
        std::size_t x = static_cast<std::size_t>(e.first);
        x ^= static_cast<std::size_t>(e.second) + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
        return x;
    }
};

} // namespace polymesh::mesh::detail
