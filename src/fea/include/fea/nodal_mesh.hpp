// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Classic nodal-connectivity mesh consumed by the reference solver (Phase P1).
//
// This is deliberately distinct from mesh::PolyMesh (the face-based structure
// the P2+ mesher produces): P1 solves on externally generated meshes with
// standard element connectivity, and this baseline gets frozen at GATE 1 as
// the benchmark comparator.
//
// Units: node coordinates in metres.

#include <Eigen/Core>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace polymesh::fea {

class FeaError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class ElementType : std::uint8_t { kTet4, kTet10, kHex8, kHex20 };

/// Number of nodes for an element type.
constexpr int element_num_nodes(ElementType type) {
    switch (type) {
    case ElementType::kTet4:
        return 4;
    case ElementType::kTet10:
        return 10;
    case ElementType::kHex8:
        return 8;
    case ElementType::kHex20:
        return 20;
    }
    return 0; // unreachable
}

/// Canonical node ordering (documented once, used everywhere):
/// - Tet: v0..v3 with reference positions (0,0,0),(1,0,0),(0,1,0),(0,0,1).
///   Tet10 adds mid-edge nodes 4..9 on edges (0,1),(1,2),(0,2),(0,3),(1,3),(2,3).
/// - Hex: v0..v7 counter-clockwise bottom then top:
///   (-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1).
///   Hex20 adds mid-edge nodes 8..19 on bottom edges (0,1),(1,2),(2,3),(3,0),
///   top edges (4,5),(5,6),(6,7),(7,4), then vertical edges (0,4),(1,5),(2,6),(3,7).
struct NodalElement {
    ElementType type = ElementType::kTet4;
    std::vector<std::uint32_t> nodes;
};

struct NodalMesh {
    std::vector<Eigen::Vector3d> nodes;
    std::vector<NodalElement> elements;

    /// Index ranges and per-element node counts. Throws FeaError.
    void check_validity() const;
};

} // namespace polymesh::fea
