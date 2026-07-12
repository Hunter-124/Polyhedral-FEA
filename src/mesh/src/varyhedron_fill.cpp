// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/varyhedron_fill.hpp"

#include "mesh/hybrid_fill.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

/// Collect free-boundary node indices from unpaired tet faces.
std::vector<std::uint32_t> boundary_nodes(const TetFillOutput& mesh) {
    std::unordered_set<std::uint64_t> face_hash;
    // Count oriented faces; unpaired are boundary.
    struct FaceKey {
        std::uint32_t a, b, c;
        bool operator==(const FaceKey& o) const {
            return a == o.a && b == o.b && c == o.c;
        }
    };
    struct FaceHash {
        std::size_t operator()(const FaceKey& f) const {
            return (static_cast<std::size_t>(f.a) * 73856093u) ^
                   (static_cast<std::size_t>(f.b) * 19349663u) ^
                   (static_cast<std::size_t>(f.c) * 83492791u);
        }
    };
    std::unordered_map<FaceKey, int, FaceHash> faces;
    auto sorted = [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        if (i > j) {
            std::swap(i, j);
        }
        if (j > k) {
            std::swap(j, k);
        }
        if (i > j) {
            std::swap(i, j);
        }
        return FaceKey{i, j, k};
    };
    const int quads[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : mesh.tets) {
        for (const auto& f : quads) {
            faces[sorted(t[static_cast<std::size_t>(f[0])], t[static_cast<std::size_t>(f[1])],
                         t[static_cast<std::size_t>(f[2])])]++;
        }
    }
    std::unordered_set<std::uint32_t> bset;
    for (const auto& [key, count] : faces) {
        if (count == 1) {
            bset.insert(key.a);
            bset.insert(key.b);
            bset.insert(key.c);
        }
    }
    return std::vector<std::uint32_t>(bset.begin(), bset.end());
}

} // namespace

VaryhedronFillOutput varyhedron_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers,
    std::span<const geom::SharpEdge> features, double feature_band,
    std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
    double curvature_turn_deg, const geom::CadTopology* topo) {

    VaryhedronFillOutput out;

    // --- Boundary edge seeds from CAD topology (packing constraint) ---
    std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
    if (topo != nullptr) {
        constexpr std::size_t kMaxEdgeSeeds = 512;
        const double min_sep = 0.75 * std::max(h, 1e-12);
        const double min_sep2 = min_sep * min_sep;
        for (const auto& e : topo->edges) {
            // Subsample CAD edge polyline into packing seeds.
            for (std::size_t i = 0; i < e.samples.size(); i += 1) {
                if (seeds.size() >= kMaxEdgeSeeds) {
                    break;
                }
                const auto& p = e.samples[i];
                bool far = true;
                for (const auto& q : seeds) {
                    if ((p - q).squaredNorm() < min_sep2) {
                        far = false;
                        break;
                    }
                }
                if (far) {
                    seeds.push_back(p);
                    ++out.n_edge_seeds;
                }
            }
            if (seeds.size() >= kMaxEdgeSeeds) {
                break;
            }
        }
        if (seed_band <= 0.0 && !seeds.empty()) {
            seed_band = 1.6 * h;
        }
    }

    // --- Scaffold: multi-level graded tet (dual-poly clustering is V6c) ---
    auto graded = graded_tet_fill_surface(surface, bbox_min, bbox_max, h, skin_layers, features,
                                          feature_band, seeds, seed_band, curvature_turn_deg);
    out.mesh = std::move(graded.mesh);
    out.h_coarse = graded.h_coarse;
    out.h_fine = graded.h_fine;
    out.n_tets = out.mesh.tets.size();

    // --- Edge-profile attraction: pull free nodes near CAD edges onto profile ---
    if (topo != nullptr && !topo->edges.empty()) {
        const auto bnodes = boundary_nodes(out.mesh);
        const double snap_band = 1.25 * std::max(h, 1e-12);
        for (std::uint32_t ni : bnodes) {
            if (ni >= out.mesh.nodes.size()) {
                continue;
            }
            auto& p = out.mesh.nodes[ni];
            const auto q = geom::closest_edge(*topo, p);
            if (!q || q->distance > snap_band) {
                continue;
            }
            // Blend toward CAD edge (keeps volume-ish; full project is aggressive).
            const double w = 1.0 - (q->distance / snap_band);
            p = p + w * (q->closest - p);
        }
        // Re-project lightly onto discrete surface so nodes stay on the solid.
        std::vector<std::uint32_t> bvec(bnodes.begin(), bnodes.end());
        for (std::uint32_t ni : bvec) {
            if (ni >= out.mesh.nodes.size()) {
                continue;
            }
            const auto cp = closest_on_surface(surface, out.mesh.nodes[ni]);
            out.mesh.nodes[ni] = cp.point;
        }

        // Metric: Hausdorff of a subsample of boundary nodes vs CAD edges.
        std::vector<Eigen::Vector3d> poly;
        poly.reserve(bnodes.size());
        for (std::uint32_t ni : bnodes) {
            if (ni < out.mesh.nodes.size()) {
                poly.push_back(out.mesh.nodes[ni]);
            }
        }
        out.edge_profile_hausdorff_max = geom::edge_profile_hausdorff(*topo, poly);
        double char_len = 0.0;
        for (const auto& e : topo->edges) {
            char_len = std::max(char_len, e.length);
        }
        if (char_len <= 0.0) {
            char_len = (bbox_max - bbox_min).norm();
        }
        out.edge_profile_rel =
            (char_len > 0.0) ? (out.edge_profile_hausdorff_max / char_len) : 0.0;
    }

    return out;
}

} // namespace polymesh::mesh
