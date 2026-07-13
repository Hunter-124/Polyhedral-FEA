// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/varyhedron_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

/// Collect free-boundary node indices from unpaired tet faces.
std::vector<std::uint32_t> boundary_nodes(const TetFillOutput& mesh) {
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

/// Deterministic unit-interval hash for lattice jitter (no RNG dependency).
double hash01(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<double>(x) * (1.0 / 4294967295.0);
}

double hash_signed(int i, int j, int k, int axis) {
    const std::uint32_t key = static_cast<std::uint32_t>(i * 73856093) ^
                              static_cast<std::uint32_t>(j * 19349663) ^
                              static_cast<std::uint32_t>(k * 83492791) ^
                              static_cast<std::uint32_t>(axis * 2654435761u);
    return 2.0 * hash01(key) - 1.0; // [-1, 1]
}

bool far_enough(const Eigen::Vector3d& p, const std::vector<Eigen::Vector3d>& pts,
                double min_sep2) {
    for (const auto& q : pts) {
        if ((p - q).squaredNorm() < min_sep2) {
            return false;
        }
    }
    return true;
}

/// Shimada-style bubble relax: movable volume seeds repel from fixed edge
/// anchors and from each other. Keeps volume seeds inside the bbox collar.
void bubble_relax_volume(std::vector<Eigen::Vector3d>& volume, const std::vector<Eigen::Vector3d>& edge,
                         double radius, int iterations, const Eigen::Vector3d& bbox_min,
                         const Eigen::Vector3d& bbox_max) {
    if (volume.empty() || iterations <= 0 || !(radius > 0.0)) {
        return;
    }
    const double min_d = 2.0 * radius;
    const double min_d2 = min_d * min_d;
    const Eigen::Vector3d lo = bbox_min + Eigen::Vector3d::Constant(0.5 * radius);
    const Eigen::Vector3d hi = bbox_max - Eigen::Vector3d::Constant(0.5 * radius);
    for (int it = 0; it < iterations; ++it) {
        std::vector<Eigen::Vector3d> forces(volume.size(), Eigen::Vector3d::Zero());
        // Fixed edge anchors repel volume seeds (edge protect wins).
        for (std::size_t i = 0; i < volume.size(); ++i) {
            for (const auto& e : edge) {
                const Eigen::Vector3d d = volume[i] - e;
                const double d2 = d.squaredNorm();
                if (d2 < 1e-24) {
                    forces[i] += Eigen::Vector3d(1e-3, -1e-3, 5e-4);
                    continue;
                }
                if (d2 >= min_d2) {
                    continue;
                }
                const double dist = std::sqrt(d2);
                const double overlap = (min_d - dist) / min_d;
                forces[i] += (0.45 * overlap) * (d / dist);
            }
        }
        // Volume–volume mutual repulsion.
        for (std::size_t i = 0; i < volume.size(); ++i) {
            for (std::size_t j = i + 1; j < volume.size(); ++j) {
                const Eigen::Vector3d d = volume[i] - volume[j];
                const double d2 = d.squaredNorm();
                if (d2 < 1e-24) {
                    forces[i] += Eigen::Vector3d(1e-3, 1e-3, -1e-3);
                    forces[j] -= Eigen::Vector3d(1e-3, 1e-3, -1e-3);
                    continue;
                }
                if (d2 >= min_d2) {
                    continue;
                }
                const double dist = std::sqrt(d2);
                const double overlap = (min_d - dist) / min_d;
                const Eigen::Vector3d push = (0.35 * overlap) * (d / dist);
                forces[i] += push;
                forces[j] -= push;
            }
        }
        for (std::size_t i = 0; i < volume.size(); ++i) {
            Eigen::Vector3d p = volume[i] + forces[i] * radius;
            p = p.cwiseMax(lo).cwiseMin(hi);
            volume[i] = p;
        }
    }
}

double pack_fill_fraction(const std::vector<Eigen::Vector3d>& edge,
                          const std::vector<Eigen::Vector3d>& volume, double radius,
                          double domain_vol) {
    if (!(domain_vol > 0.0) || !(radius > 0.0)) {
        return 0.0;
    }
    const double ball = (4.0 / 3.0) * 3.14159265358979323846 * radius * radius * radius;
    // Cheap proxy: raw sum / domain (overlaps ignored → may exceed 1; clamp).
    const double frac = ball * static_cast<double>(edge.size() + volume.size()) / domain_vol;
    return std::max(0.0, std::min(1.0, frac));
}

} // namespace

VaryhedronFillOutput varyhedron_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers,
    std::span<const geom::SharpEdge> features, double feature_band,
    std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
    double curvature_turn_deg, const geom::CadTopology* topo) {

    VaryhedronFillOutput out;

    // --- Boundary edge seeds from CAD topology (protecting balls) ---
    // ADR-0023 / M3: protect ONLY sharp (G0) edges. OCC seams and smooth
    // dihedrals must not get fixed sites — that is the staircasing root cause.
    // Prefer short sharp edges first (holes/fillets need denser protect).
    std::vector<Eigen::Vector3d> edge_seeds;
    std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
    if (topo != nullptr) {
        constexpr std::size_t kMaxEdgeSeeds = 768;
        const double min_sep = 0.55 * std::max(h, 1e-12);
        const double min_sep2 = min_sep * min_sep;
        const auto counts = geom::count_edge_features(*topo);
        out.n_sharp_edges = static_cast<std::size_t>(counts.n_sharp);
        out.n_smooth_edges = static_cast<std::size_t>(counts.n_smooth);
        out.n_seam_edges = static_cast<std::size_t>(counts.n_seam);
        std::vector<const geom::CadEdge*> edges;
        edges.reserve(topo->edges.size());
        for (const auto& e : topo->edges) {
            if (e.feature != geom::CadEdgeFeature::kSharp) {
                continue;
            }
            edges.push_back(&e);
        }
        std::sort(edges.begin(), edges.end(),
                  [](const geom::CadEdge* a, const geom::CadEdge* b) {
                      return a->length < b->length;
                  });
        for (const geom::CadEdge* ep : edges) {
            const auto& e = *ep;
            if (e.samples.size() < 2) {
                continue;
            }
            // Target spacing along edge ≈ 0.5 h (finer on short edges).
            const double target = std::max(0.35 * h, std::min(0.75 * h, e.length / 6.0));
            double acc = 0.0;
            Eigen::Vector3d prev = e.samples.front();
            auto try_add = [&](const Eigen::Vector3d& p) {
                if (edge_seeds.size() >= kMaxEdgeSeeds) {
                    return;
                }
                if (!far_enough(p, edge_seeds, min_sep2) || !far_enough(p, seeds, min_sep2)) {
                    return;
                }
                edge_seeds.push_back(p);
                seeds.push_back(p);
                ++out.n_edge_seeds;
            };
            try_add(prev);
            for (std::size_t i = 1; i < e.samples.size(); ++i) {
                const Eigen::Vector3d& p = e.samples[i];
                acc += (p - prev).norm();
                if (acc >= target) {
                    try_add(p);
                    acc = 0.0;
                }
                prev = p;
            }
            try_add(e.samples.back());
            if (edge_seeds.size() >= kMaxEdgeSeeds) {
                break;
            }
        }
    }

    // --- Interior packing seeds (V6c packing engine; dual deferred to V11) ---
    // Jittered lattice on interior cells of a coarse classify grid, then bubble
    // relax so volume bubbles clear CAD edge protecting balls.
    {
        constexpr std::size_t kMaxVolumeSeeds = 512;
        constexpr int kRelaxIters = 6;
        const double hh = std::max(h, 1e-12);
        const double bubble_r = 0.5 * hh;
        const double vol_sep = 0.85 * hh;
        const double vol_sep2 = vol_sep * vol_sep;
        const double edge_clear2 = (1.05 * hh) * (1.05 * hh);

        // Coarse lattice for inside classification (cheap; auto-coarsens).
        const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, std::max(hh, 1e-9));
        const auto inside = classify_cells_inside(surface, grid);
        const int nx = grid.nx, ny = grid.ny, nz = grid.nz;

        // Face-boundary hop distance so we leave a collar for edge protect.
        std::vector<int> dist(inside.size(), -1);
        std::vector<std::array<int, 3>> q;
        q.reserve(static_cast<std::size_t>(nx + ny + nz) * 4);
        const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
        const int face_nbr[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                    {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    if (!inside[idx(i, j, k)]) {
                        continue;
                    }
                    bool boundary = false;
                    for (const auto& o : face_nbr) {
                        const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                        if (ni < 0 || ni >= nx || nj < 0 || nj >= ny || nk < 0 || nk >= nz ||
                            !inside[idx(ni, nj, nk)]) {
                            boundary = true;
                            break;
                        }
                    }
                    if (boundary) {
                        dist[idx(i, j, k)] = 0;
                        q.push_back({i, j, k});
                    }
                }
            }
        }
        for (std::size_t qi = 0; qi < q.size(); ++qi) {
            const auto c = q[qi];
            const int d0 = dist[idx(c[0], c[1], c[2])];
            for (const auto& o : face_nbr) {
                const int ni = c[0] + o[0], nj = c[1] + o[1], nk = c[2] + o[2];
                if (ni < 0 || ni >= nx || nj < 0 || nj >= ny || nk < 0 || nk >= nz) {
                    continue;
                }
                if (!inside[idx(ni, nj, nk)]) {
                    continue;
                }
                auto& dn = dist[idx(ni, nj, nk)];
                if (dn < 0 || dn > d0 + 1) {
                    dn = d0 + 1;
                    q.push_back({ni, nj, nk});
                }
            }
        }

        std::vector<Eigen::Vector3d> volume;
        volume.reserve(std::min(kMaxVolumeSeeds, static_cast<std::size_t>(nx * ny * nz)));
        // Subsample every other cell when the grid is dense to stay under cap.
        const int stride = (nx * ny * nz > static_cast<int>(kMaxVolumeSeeds) * 4) ? 2 : 1;
        for (int k = 0; k < nz; k += stride) {
            for (int j = 0; j < ny; j += stride) {
                for (int i = 0; i < nx; i += stride) {
                    if (volume.size() >= kMaxVolumeSeeds) {
                        break;
                    }
                    const auto id = idx(i, j, k);
                    if (!inside[id] || dist[id] < 1) {
                        // Skip exterior and one-cell boundary collar.
                        continue;
                    }
                    Eigen::Vector3d p = grid.cell_center(i, j, k);
                    // Deterministic jitter (~15% of spacing) so packing is not on a
                    // rigid lattice (Shimada-style seed engine).
                    p[0] += 0.15 * grid.cell[0] * hash_signed(i, j, k, 0);
                    p[1] += 0.15 * grid.cell[1] * hash_signed(i, j, k, 1);
                    p[2] += 0.15 * grid.cell[2] * hash_signed(i, j, k, 2);
                    if (!far_enough(p, edge_seeds, edge_clear2)) {
                        continue;
                    }
                    if (!far_enough(p, volume, vol_sep2)) {
                        continue;
                    }
                    if (!far_enough(p, seeds, vol_sep2)) {
                        continue;
                    }
                    volume.push_back(p);
                }
            }
        }

        bubble_relax_volume(volume, edge_seeds, bubble_r, kRelaxIters, bbox_min, bbox_max);

        // Drop any seeds that drifted into edge protect after relax.
        std::vector<Eigen::Vector3d> kept;
        kept.reserve(volume.size());
        for (const auto& p : volume) {
            if (!far_enough(p, edge_seeds, edge_clear2 * 0.85 * 0.85)) {
                continue;
            }
            if (!far_enough(p, kept, vol_sep2 * 0.7 * 0.7)) {
                continue;
            }
            kept.push_back(p);
        }
        out.n_volume_seeds = kept.size();
        out.n_pack_relax_iters = kRelaxIters;
        const Eigen::Vector3d extent = bbox_max - bbox_min;
        const double domain_vol =
            std::max(0.0, extent[0]) * std::max(0.0, extent[1]) * std::max(0.0, extent[2]);
        out.pack_fill_frac = pack_fill_fraction(edge_seeds, kept, bubble_r, domain_vol);

        for (const auto& p : kept) {
            seeds.push_back(p);
        }
    }

    if (seed_band <= 0.0 && !seeds.empty()) {
        // Slightly wider band so edge protect + volume packing mark L2 cells.
        seed_band = 1.8 * std::max(h, 1e-12);
    }

    // --- Scaffold: multi-level graded tet (dual-poly clustering → V11) ---
    auto graded = graded_tet_fill_surface(surface, bbox_min, bbox_max, h, skin_layers, features,
                                          feature_band, seeds, seed_band, curvature_turn_deg);
    out.mesh = std::move(graded.mesh);
    out.h_coarse = graded.h_coarse;
    out.h_fine = graded.h_fine;
    out.n_tets = out.mesh.tets.size();

    // --- Edge-profile attraction: soft pull toward CAD edges, Jacobian-safe ---
    if (topo != nullptr && !topo->edges.empty()) {
        const auto bnodes = boundary_nodes(out.mesh);
        const double snap_band = 0.75 * std::max(h, 1e-12);
        // Save pre-snap positions for offender revert.
        std::vector<Eigen::Vector3d> saved = out.mesh.nodes;
        for (std::uint32_t ni : bnodes) {
            if (ni >= out.mesh.nodes.size()) {
                continue;
            }
            auto& p = out.mesh.nodes[ni];
            // Snap only toward sharp protected features (never seams/smooth).
            const auto q = geom::closest_edge(*topo, p, /*sharp_only=*/true);
            if (!q || q->distance > snap_band) {
                continue;
            }
            // Mild blend only (aggressive full project inverts boundary tets).
            const double w = 0.35 * (1.0 - (q->distance / snap_band));
            p = p + w * (q->closest - p);
        }

        // Revert any tet with non-positive volume after the blend.
        auto tet_vol6 = [&](const std::array<std::uint32_t, 4>& t) {
            const Eigen::Vector3d a = out.mesh.nodes[t[0]];
            const Eigen::Vector3d b = out.mesh.nodes[t[1]];
            const Eigen::Vector3d c = out.mesh.nodes[t[2]];
            const Eigen::Vector3d d = out.mesh.nodes[t[3]];
            const Eigen::Vector3d ab = b - a;
            const Eigen::Vector3d ac = c - a;
            const Eigen::Vector3d ad = d - a;
            return ab.dot(ac.cross(ad));
        };
        std::unordered_set<std::uint32_t> offenders;
        for (const auto& t : out.mesh.tets) {
            if (tet_vol6(t) <= 0.0) {
                offenders.insert(t[0]);
                offenders.insert(t[1]);
                offenders.insert(t[2]);
                offenders.insert(t[3]);
            }
        }
        for (std::uint32_t ni : offenders) {
            if (ni < out.mesh.nodes.size()) {
                out.mesh.nodes[ni] = saved[ni];
            }
        }

        // Metric: boundary nodes vs sharp CAD edges only (protected features).
        std::vector<Eigen::Vector3d> poly;
        poly.reserve(bnodes.size());
        for (std::uint32_t ni : bnodes) {
            if (ni < out.mesh.nodes.size()) {
                poly.push_back(out.mesh.nodes[ni]);
            }
        }
        out.edge_profile_hausdorff_max =
            geom::edge_profile_hausdorff_filtered(*topo, poly, /*sharp_only=*/true);
        const auto chord = geom::chordal_edge_metrics(*topo, poly, h, /*sharp_edges_only=*/true);
        out.edge_chordal_efficiency_max = chord.max_efficiency;
        out.edge_hausdorff_over_h = chord.hausdorff_over_h;
        double char_len = 0.0;
        for (const auto& e : topo->edges) {
            if (e.feature == geom::CadEdgeFeature::kSharp) {
                char_len = std::max(char_len, e.length);
            }
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
