// SPDX-License-Identifier: BSD-3-Clause
#include "fea/boundary_faces.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace polymesh::fea {
namespace {

// Canonical key: sorted node ids, pad unused slots with max uint32.
using FaceKey = std::array<std::uint32_t, 4>;

FaceKey make_key(const std::uint32_t* ids, int n) {
    // Manual sort (3 or 4 nodes) — avoids -Warray-bounds on partial std::sort.
    FaceKey k{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    if (n == 3) {
        k[0] = ids[0];
        k[1] = ids[1];
        k[2] = ids[2];
        if (k[0] > k[1]) {
            std::swap(k[0], k[1]);
        }
        if (k[1] > k[2]) {
            std::swap(k[1], k[2]);
        }
        if (k[0] > k[1]) {
            std::swap(k[0], k[1]);
        }
    } else if (n == 4) {
        k[0] = ids[0];
        k[1] = ids[1];
        k[2] = ids[2];
        k[3] = ids[3];
        // Network sort for 4 elements.
        if (k[0] > k[1]) {
            std::swap(k[0], k[1]);
        }
        if (k[2] > k[3]) {
            std::swap(k[2], k[3]);
        }
        if (k[0] > k[2]) {
            std::swap(k[0], k[2]);
        }
        if (k[1] > k[3]) {
            std::swap(k[1], k[3]);
        }
        if (k[1] > k[2]) {
            std::swap(k[1], k[2]);
        }
    }
    return k;
}

struct FaceRecord {
    std::array<std::uint32_t, 4> verts{}; // draw order; tris use verts[3]=verts[2]
    int n = 0;
    int count = 0;
};

void add_face(std::map<FaceKey, FaceRecord>& faces, const std::uint32_t* ids, int n) {
    if (n < 3 || n > 4) {
        return;
    }
    // Drop collapsed faces (duplicate corners).
    if (n == 3 && (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])) {
        return;
    }
    if (n == 4 &&
        (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[3] || ids[3] == ids[0])) {
        // Still allow planar-ish quads; only reject if any consecutive pair matches.
        // (Skip full uniqueness check — lattice quads are fine.)
    }
    const FaceKey key = make_key(ids, n);
    auto [it, fresh] = faces.try_emplace(key);
    if (fresh) {
        it->second.n = n;
        for (int i = 0; i < n; ++i) {
            it->second.verts[static_cast<std::size_t>(i)] = ids[i];
        }
        if (n == 3) {
            it->second.verts[3] = ids[2]; // degenerate quad for renderers
        }
        it->second.count = 1;
    } else {
        ++it->second.count;
    }
}

void emit_element_faces(std::map<FaceKey, FaceRecord>& faces, const NodalElement& el) {
    const auto& n = el.nodes;
    if (n.empty()) {
        return;
    }
    switch (el.type) {
    case ElementType::kTet4:
    case ElementType::kTet10: {
        // Corner faces only (mid-edge nodes ignored for skin topology).
        if (n.size() < 4) {
            return;
        }
        const std::uint32_t c[4] = {n[0], n[1], n[2], n[3]};
        // Orientation: right-hand so outward for positive-volume tet.
        const std::uint32_t f[4][3] = {
            {c[0], c[2], c[1]}, // opposite 3
            {c[0], c[1], c[3]}, // opposite 2
            {c[0], c[3], c[2]}, // opposite 1
            {c[1], c[2], c[3]}, // opposite 0
        };
        for (const auto& tri : f) {
            add_face(faces, tri, 3);
        }
        break;
    }
    case ElementType::kHex8:
    case ElementType::kHex20: {
        if (n.size() < 8) {
            return;
        }
        const std::uint32_t c[8] = {n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]};
        const std::uint32_t f[6][4] = {
            {c[0], c[3], c[2], c[1]}, // bottom -z
            {c[4], c[5], c[6], c[7]}, // top +z
            {c[0], c[1], c[5], c[4]}, // -y
            {c[1], c[2], c[6], c[5]}, // +x
            {c[2], c[3], c[7], c[6]}, // +y
            {c[3], c[0], c[4], c[7]}, // -x
        };
        for (const auto& q : f) {
            add_face(faces, q, 4);
        }
        break;
    }
    case ElementType::kPrism6: {
        if (n.size() < 6) {
            return;
        }
        const std::uint32_t c[6] = {n[0], n[1], n[2], n[3], n[4], n[5]};
        const std::uint32_t t0[3] = {c[0], c[2], c[1]};
        const std::uint32_t t1[3] = {c[3], c[4], c[5]};
        add_face(faces, t0, 3);
        add_face(faces, t1, 3);
        const std::uint32_t q[3][4] = {
            {c[0], c[1], c[4], c[3]},
            {c[1], c[2], c[5], c[4]},
            {c[2], c[0], c[3], c[5]},
        };
        for (const auto& face : q) {
            add_face(faces, face, 4);
        }
        break;
    }
    case ElementType::kPyramid5: {
        if (n.size() < 5) {
            return;
        }
        const std::uint32_t c[5] = {n[0], n[1], n[2], n[3], n[4]};
        const std::uint32_t base[4] = {c[0], c[1], c[2], c[3]};
        add_face(faces, base, 4);
        const std::uint32_t t[4][3] = {
            {c[0], c[1], c[4]},
            {c[1], c[2], c[4]},
            {c[2], c[3], c[4]},
            {c[3], c[0], c[4]},
        };
        for (const auto& tri : t) {
            add_face(faces, tri, 3);
        }
        break;
    }
    case ElementType::kPolyVem: {
        for (const auto& face : el.faces) {
            if (face.size() == 3) {
                const std::uint32_t t[3] = {n[face[0]], n[face[1]], n[face[2]]};
                add_face(faces, t, 3);
            } else if (face.size() == 4) {
                const std::uint32_t q[4] = {n[face[0]], n[face[1]], n[face[2]], n[face[3]]};
                add_face(faces, q, 4);
            } else if (face.size() > 4) {
                // Fan triangulation of general polygon → exterior tris.
                for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                    const std::uint32_t t[3] = {n[face[0]], n[face[i]], n[face[i + 1]]};
                    add_face(faces, t, 3);
                }
            }
        }
        break;
    }
    }
}

} // namespace

std::vector<std::array<std::uint32_t, 4>> extract_boundary_faces(const NodalMesh& mesh) {
    std::map<FaceKey, FaceRecord> faces;
    for (const auto& el : mesh.elements) {
        emit_element_faces(faces, el);
    }
    std::vector<std::array<std::uint32_t, 4>> out;
    out.reserve(faces.size() / 2 + 8);
    for (const auto& [key, rec] : faces) {
        (void)key;
        if (rec.count == 1) {
            out.push_back(rec.verts);
        }
    }
    return out;
}

namespace {

// Append every face of `el` as a polygon loop of GLOBAL node ids. Poly faces
// stay polygons; FEM element faces use the same corner-face tables as
// emit_element_faces (mid-edge nodes ignored for skin topology).
void collect_element_loops(const NodalElement& el,
                           std::vector<std::vector<std::uint32_t>>& loops) {
    const auto& n = el.nodes;
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        loops.push_back({a, b, c, d});
    };
    auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        loops.push_back({a, b, c});
    };
    switch (el.type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        if (n.size() < 4) return;
        tri(n[0], n[2], n[1]);
        tri(n[0], n[1], n[3]);
        tri(n[0], n[3], n[2]);
        tri(n[1], n[2], n[3]);
        break;
    case ElementType::kHex8:
    case ElementType::kHex20:
        if (n.size() < 8) return;
        quad(n[0], n[3], n[2], n[1]);
        quad(n[4], n[5], n[6], n[7]);
        quad(n[0], n[1], n[5], n[4]);
        quad(n[1], n[2], n[6], n[5]);
        quad(n[2], n[3], n[7], n[6]);
        quad(n[3], n[0], n[4], n[7]);
        break;
    case ElementType::kPrism6:
        if (n.size() < 6) return;
        tri(n[0], n[2], n[1]);
        tri(n[3], n[4], n[5]);
        quad(n[0], n[1], n[4], n[3]);
        quad(n[1], n[2], n[5], n[4]);
        quad(n[2], n[0], n[3], n[5]);
        break;
    case ElementType::kPyramid5:
        if (n.size() < 5) return;
        quad(n[0], n[1], n[2], n[3]);
        tri(n[0], n[1], n[4]);
        tri(n[1], n[2], n[4]);
        tri(n[2], n[3], n[4]);
        tri(n[3], n[0], n[4]);
        break;
    case ElementType::kPolyVem:
        for (const auto& f : el.faces) {
            if (f.size() < 3) continue;
            std::vector<std::uint32_t> loop;
            loop.reserve(f.size());
            for (auto li : f) {
                if (li < n.size()) loop.push_back(n[li]);
            }
            if (loop.size() >= 3) loops.push_back(std::move(loop));
        }
        break;
    }
}

// Canonical key over a loop's node set (sorted, dedup) for owner counting.
std::vector<std::uint32_t> loop_key(const std::vector<std::uint32_t>& loop) {
    std::vector<std::uint32_t> k = loop;
    std::sort(k.begin(), k.end());
    k.erase(std::unique(k.begin(), k.end()), k.end());
    return k;
}

} // namespace

std::vector<std::vector<std::uint32_t>> extract_boundary_polys(const NodalMesh& mesh) {
    struct Rec {
        std::vector<std::uint32_t> loop;
        int count = 0;
    };
    std::map<std::vector<std::uint32_t>, Rec> faces;
    std::vector<std::vector<std::uint32_t>> loops;
    for (const auto& el : mesh.elements) {
        loops.clear();
        collect_element_loops(el, loops);
        for (auto& lp : loops) {
            auto key = loop_key(lp);
            if (key.size() < 3) continue;
            auto [it, fresh] = faces.try_emplace(std::move(key));
            if (fresh) {
                it->second.loop = std::move(lp);
                it->second.count = 1;
            } else {
                ++it->second.count;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> out;
    out.reserve(faces.size() / 2 + 8);
    for (auto& [key, rec] : faces) {
        (void)key;
        if (rec.count == 1) {
            out.push_back(std::move(rec.loop));
        }
    }
    return out;
}

} // namespace polymesh::fea
