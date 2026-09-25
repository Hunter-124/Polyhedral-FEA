// SPDX-License-Identifier: BSD-3-Clause
#include "fea/boundary_faces.hpp"

#include "boundary_loops.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace polymesh::fea {
namespace {

using detail::boundary::Edge;
using detail::boundary::edge;
using detail::boundary::Loop;
using detail::boundary::resolve_boundary_loops;
using detail::boundary::triangulate_loop;

/// Mid-edge node of every quadratic element edge, keyed by its corner pair.
/// Node orders are the canonical ones documented on `NodalElement`.
std::map<Edge, std::uint32_t> quadratic_edge_mids(const NodalMesh& mesh) {
    static constexpr std::array<std::array<std::size_t, 2>, 6> kTet10Edges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    static constexpr std::array<std::array<std::size_t, 2>, 12> kHex20Edges{{{0, 1},
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
    std::map<Edge, std::uint32_t> mids;
    for (const auto& el : mesh.elements) {
        if (el.type == ElementType::kTet10 && el.nodes.size() >= 10) {
            for (std::size_t e = 0; e < kTet10Edges.size(); ++e) {
                mids.emplace(edge(el.nodes[kTet10Edges[e][0]], el.nodes[kTet10Edges[e][1]]),
                             el.nodes[4 + e]);
            }
        } else if (el.type == ElementType::kHex20 && el.nodes.size() >= 20) {
            for (std::size_t e = 0; e < kHex20Edges.size(); ++e) {
                mids.emplace(edge(el.nodes[kHex20Edges[e][0]], el.nodes[kHex20Edges[e][1]]),
                             el.nodes[8 + e]);
            }
        }
    }
    return mids;
}

/// Split one boundary loop through its mid-edge nodes, so a curved face draws
/// curved. Emits only nodes that already exist and already sit on the exact
/// B-rep (`pipeline::project_quadratic_boundary_mids` put them there), so no
/// interpolated vertex is invented and every sub-facet still carries real
/// nodal data for result colouring.
///
///   triangle a-b-c   -> 4 triangles through (ab, bc, ca)
///   quad a-b-c-d     -> 4 corner triangles + the central quad (ab, bc, cd, da)
///
/// Returns false when the loop is not a quadratic face, leaving it untouched.
bool subdivide_curved_loop(const Loop& loop, const std::map<Edge, std::uint32_t>& mids,
                           std::vector<Loop>& out) {
    if (loop.size() != 3 && loop.size() != 4) {
        return false;
    }
    std::array<std::uint32_t, 4> mid{};
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto found = mids.find(edge(loop[i], loop[(i + 1) % loop.size()]));
        if (found == mids.end()) {
            // A mixed-p mesh has linear cells next to quadratic ones. Refining
            // only some edges of a face would tear it, so leave the face whole.
            return false;
        }
        mid[i] = found->second;
    }
    if (loop.size() == 3) {
        out.push_back({loop[0], mid[0], mid[2]});
        out.push_back({mid[0], loop[1], mid[1]});
        out.push_back({mid[2], mid[1], loop[2]});
        out.push_back({mid[0], mid[1], mid[2]});
        return true;
    }
    out.push_back({loop[0], mid[0], mid[3]});
    out.push_back({mid[0], loop[1], mid[1]});
    out.push_back({mid[1], loop[2], mid[2]});
    out.push_back({mid[2], loop[3], mid[3]});
    out.push_back({mid[0], mid[1], mid[2], mid[3]});
    return true;
}

} // namespace

std::vector<std::array<std::uint32_t, 4>> extract_boundary_faces(const NodalMesh& mesh) {
    std::vector<std::array<std::uint32_t, 4>> boundary;
    for (const Loop& loop : resolve_boundary_loops(mesh)) {
        // A 3- or 4-node loop IS the face: keep it even when degenerate. A
        // zero-area face integrates to zero (`traction.cpp` guards the zero-length
        // normal); dropping it would punch a hole in the shell.
        if (loop.size() == 3) {
            boundary.push_back({loop[0], loop[1], loop[2], loop[2]});
        } else if (loop.size() == 4) {
            boundary.push_back({loop[0], loop[1], loop[2], loop[3]});
        } else {
            std::vector<std::array<std::uint32_t, 3>> triangles;
            if (!triangulate_loop(mesh, loop, triangles)) {
                // Star and ear clipping both failed, which means the polygon is
                // degenerate rather than merely awkward. Fan it anyway: a
                // degenerate fan is zero-area and harmless, a missing facet is
                // a hole.
                triangles.clear();
                for (std::size_t k = 1; k + 1 < loop.size(); ++k) {
                    triangles.push_back({loop[0], loop[k], loop[k + 1]});
                }
            }
            for (const auto& triangle : triangles) {
                boundary.push_back({triangle[0], triangle[1], triangle[2], triangle[2]});
            }
        }
    }
    return boundary;
}

std::vector<std::vector<std::uint32_t>> extract_boundary_polys(const NodalMesh& mesh) {
    auto loops = resolve_boundary_loops(mesh);
    // Split quadratic faces through the mid-edge nodes they already own, so a
    // curved rim draws curved rather than as the straight corner chord.
    const auto mids = quadratic_edge_mids(mesh);
    if (mids.empty()) {
        return loops;
    }
    std::vector<Loop> curved;
    curved.reserve(loops.size() * 4);
    for (const auto& loop : loops) {
        if (!subdivide_curved_loop(loop, mids, curved)) {
            curved.push_back(loop);
        }
    }
    return curved;
}

} // namespace polymesh::fea
