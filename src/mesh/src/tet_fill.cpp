// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/tet_fill.hpp"

#include "mesh/cell_validity.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <span>
#include <unordered_map>

namespace polymesh::mesh {
namespace {

double tet_signed_volume_impl(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                              const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

} // namespace

double tet_signed_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                         const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return tet_signed_volume_impl(a, b, c, d);
}

void check_tet_fill_geometry(const TetFillOutput& out, double min_volume) {
    for (std::size_t e = 0; e < out.tets.size(); ++e) {
        const auto& n = out.tets[e];
        for (const auto idx : n) {
            if (idx >= out.nodes.size()) {
                throw ValidityError(
                    std::format("check_tet_fill_geometry: tet {} bad node index", e));
            }
            if (!out.nodes[idx].allFinite()) {
                throw ValidityError(
                    std::format("check_tet_fill_geometry: tet {} non-finite node", e));
            }
        }
        const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                out.nodes[n[2]], out.nodes[n[3]]);
        if (v <= min_volume) {
            throw ValidityError(std::format(
                "check_tet_fill_geometry: tet {} non-positive volume {:.3e}", e, v));
        }
    }
}

namespace {

// Free faces of an all-tet mesh with their owning tet.
struct FreeTetFace {
    std::array<std::uint32_t, 3> nodes;
    std::uint32_t owner;
};

std::vector<FreeTetFace> free_tet_faces(std::span<const std::array<std::uint32_t, 4>> tets) {
    static constexpr int kTF[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    std::map<std::array<std::uint32_t, 3>, std::pair<int, std::uint32_t>> census;
    for (std::size_t ti = 0; ti < tets.size(); ++ti) {
        for (const auto& f : kTF) {
            std::array<std::uint32_t, 3> key{{tets[ti][static_cast<std::size_t>(f[0])],
                                              tets[ti][static_cast<std::size_t>(f[1])],
                                              tets[ti][static_cast<std::size_t>(f[2])]}};
            std::sort(key.begin(), key.end());
            auto& slot = census[key];
            ++slot.first;
            slot.second = static_cast<std::uint32_t>(ti);
        }
    }
    std::vector<FreeTetFace> free;
    for (const auto& [key, slot] : census) {
        if (slot.first == 1) {
            free.push_back({key, slot.second});
        }
    }
    return free;
}

// Spatial hash of tet bounding boxes on a cubic grid of pitch `cell`.
class TetGrid {
public:
    TetGrid(std::span<const Eigen::Vector3d> nodes,
            std::span<const std::array<std::uint32_t, 4>> tets, double cell)
        : nodes_(nodes), tets_(tets), cell_(cell) {
        for (std::size_t ti = 0; ti < tets.size(); ++ti) {
            Eigen::Vector3d lo = nodes[tets[ti][0]];
            Eigen::Vector3d hi = lo;
            for (int k = 1; k < 4; ++k) {
                lo = lo.cwiseMin(nodes[tets[ti][static_cast<std::size_t>(k)]]);
                hi = hi.cwiseMax(nodes[tets[ti][static_cast<std::size_t>(k)]]);
            }
            const auto a = key_of(lo);
            const auto b = key_of(hi);
            for (long long i = a[0]; i <= b[0]; ++i) {
                for (long long j = a[1]; j <= b[1]; ++j) {
                    for (long long k = a[2]; k <= b[2]; ++k) {
                        buckets_[pack(i, j, k)].push_back(static_cast<std::uint32_t>(ti));
                    }
                }
            }
        }
    }

    /// Index of a tet strictly containing `p` that is not `owner` and shares
    /// no node with `face` — the burying tet. SIZE_MAX when none.
    std::size_t buried_in(const Eigen::Vector3d& p, const std::array<std::uint32_t, 3>& face,
                          std::uint32_t owner) const {
        const auto k = key_of(p);
        const auto it = buckets_.find(pack(k[0], k[1], k[2]));
        if (it == buckets_.end()) {
            return SIZE_MAX;
        }
        for (const auto ti : it->second) {
            if (ti == owner) {
                continue;
            }
            const auto& t = tets_[ti];
            if (std::find(t.begin(), t.end(), face[0]) != t.end() ||
                std::find(t.begin(), t.end(), face[1]) != t.end() ||
                std::find(t.begin(), t.end(), face[2]) != t.end()) {
                continue;
            }
            if (strictly_inside(p, t)) {
                return ti;
            }
        }
        return SIZE_MAX;
    }

private:
    std::array<long long, 3> key_of(const Eigen::Vector3d& p) const {
        return {static_cast<long long>(std::floor(p.x() / cell_)),
                static_cast<long long>(std::floor(p.y() / cell_)),
                static_cast<long long>(std::floor(p.z() / cell_))};
    }
    static std::uint64_t pack(long long i, long long j, long long k) {
        const auto u = [](long long v) {
            return static_cast<std::uint64_t>(v + (1LL << 20)) & ((1ULL << 21) - 1);
        };
        return (u(i) << 42) | (u(j) << 21) | u(k);
    }
    bool strictly_inside(const Eigen::Vector3d& p, const std::array<std::uint32_t, 4>& t) const {
        const Eigen::Vector3d& a = nodes_[t[0]];
        const double v = tet_signed_volume_impl(a, nodes_[t[1]], nodes_[t[2]], nodes_[t[3]]);
        if (!(v > 0.0)) {
            return false;
        }
        // Strictly interior: every barycentric coordinate above a relative
        // floor. A centroid ON a shared face (healthy adjacency) has one
        // coordinate ~0 and MUST NOT count as buried.
        const double tol = 1e-6;
        const double l1 = tet_signed_volume_impl(a, p, nodes_[t[2]], nodes_[t[3]]) / v;
        if (l1 < tol) {
            return false;
        }
        const double l2 = tet_signed_volume_impl(a, nodes_[t[1]], p, nodes_[t[3]]) / v;
        if (l2 < tol) {
            return false;
        }
        const double l3 = tet_signed_volume_impl(a, nodes_[t[1]], nodes_[t[2]], p) / v;
        if (l3 < tol) {
            return false;
        }
        return 1.0 - l1 - l2 - l3 > tol;
    }

    std::span<const Eigen::Vector3d> nodes_;
    std::span<const std::array<std::uint32_t, 4>> tets_;
    double cell_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets_;
};

// (buried free face index, index of the tet burying it)
std::vector<std::pair<std::size_t, std::size_t>>
buried_face_ids(std::span<const Eigen::Vector3d> nodes, const std::vector<FreeTetFace>& free,
                const TetGrid& grid) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t fi = 0; fi < free.size(); ++fi) {
        const auto& f = free[fi];
        const Eigen::Vector3d c =
            (nodes[f.nodes[0]] + nodes[f.nodes[1]] + nodes[f.nodes[2]]) / 3.0;
        if (const auto ti = grid.buried_in(c, f.nodes, f.owner); ti != SIZE_MAX) {
            out.push_back({fi, ti});
        }
    }
    return out;
}

} // namespace

BuriedFaceStats count_buried_free_tet_faces(std::span<const Eigen::Vector3d> nodes,
                                            std::span<const std::array<std::uint32_t, 4>> tets,
                                            double h) {
    BuriedFaceStats stats;
    if (tets.empty() || !(h > 0.0)) {
        return stats;
    }
    const auto free = free_tet_faces(tets);
    stats.n_free_faces = free.size();
    const TetGrid grid(nodes, tets, h);
    stats.n_buried = buried_face_ids(nodes, free, grid).size();
    return stats;
}

std::vector<std::uint32_t>
buried_free_tet_face_owners(std::span<const Eigen::Vector3d> nodes,
                            std::span<const std::array<std::uint32_t, 4>> tets, double h) {
    std::vector<std::uint32_t> owners;
    if (tets.empty() || !(h > 0.0)) {
        return owners;
    }
    const auto free = free_tet_faces(tets);
    const TetGrid grid(nodes, tets, h);
    for (const auto& [fi, buryer] : buried_face_ids(nodes, free, grid)) {
        owners.push_back(free[fi].owner);
    }
    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    return owners;
}

std::size_t pull_buried_free_faces(std::vector<Eigen::Vector3d>& nodes,
                                   std::span<const std::array<std::uint32_t, 4>> tets,
                                   double h, int max_iters) {
    if (tets.empty() || !(h > 0.0)) {
        return 0;
    }
    const auto free = free_tet_faces(tets);
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> star;
    for (std::size_t ti = 0; ti < tets.size(); ++ti) {
        for (const auto ni : tets[ti]) {
            star[ni].push_back(static_cast<std::uint32_t>(ti));
        }
    }
    const auto star_ok = [&](std::uint32_t ni) {
        for (const auto ti : star[ni]) {
            const auto& t = tets[ti];
            if (tet_signed_volume_impl(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]) <=
                0.0) {
                return false;
            }
        }
        return true;
    };
    for (int iter = 0; iter < max_iters; ++iter) {
        const TetGrid grid(nodes, tets, h);
        const auto buried = buried_face_ids(nodes, free, grid);
        if (buried.empty()) {
            return 0;
        }
        bool any_moved = false;
        for (const auto& [fi, buryer] : buried) {
            for (const auto ni : free[fi].nodes) {
                Eigen::Vector3d target = Eigen::Vector3d::Zero();
                std::size_t n_used = 0;
                for (const auto ti : star[ni]) {
                    for (const auto o : tets[ti]) {
                        target += nodes[o];
                        ++n_used;
                    }
                }
                if (n_used == 0) {
                    continue;
                }
                target /= static_cast<double>(n_used);
                const Eigen::Vector3d saved = nodes[ni];
                double frac = 0.5;
                bool moved = false;
                for (int cut = 0; cut < 4; ++cut) {
                    nodes[ni] = saved + frac * (target - saved);
                    if (star_ok(ni)) {
                        moved = true;
                        break;
                    }
                    frac *= 0.5;
                }
                if (!moved) {
                    nodes[ni] = saved;
                } else {
                    any_moved = true;
                }
            }
        }
        if (!any_moved) {
            break;
        }
    }
    const TetGrid grid(nodes, tets, h);
    return buried_face_ids(nodes, free, grid).size();
}

TetFillOutput tet_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h, bool snap_boundary) {
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h);
    const auto inside = classify_cells_inside(surface, grid);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;

    TetFillOutput out;
    out.h = grid.max_edge();
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[grid.index(i, j, k)];
    };

    // Corner numbering matches hex8 convention.
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[grid.index(i, j, k)]) {
                    continue;
                }
                const std::array<std::uint32_t, 8> c{{
                    node_at(i, j, k),
                    node_at(i + 1, j, k),
                    node_at(i + 1, j + 1, k),
                    node_at(i, j + 1, k),
                    node_at(i, j, k + 1),
                    node_at(i + 1, j, k + 1),
                    node_at(i + 1, j + 1, k + 1),
                    node_at(i, j + 1, k + 1),
                }};

                static constexpr std::array<std::array<int, 4>, 6> tets{{
                    {{0, 1, 2, 6}},
                    {{0, 2, 3, 6}},
                    {{0, 1, 5, 6}},
                    {{0, 3, 7, 6}},
                    {{0, 4, 5, 6}},
                    {{0, 4, 7, 6}},
                }};
                for (const auto& t : tets) {
                    std::array<std::uint32_t, 4> n{{c[static_cast<std::size_t>(t[0])],
                                                    c[static_cast<std::size_t>(t[1])],
                                                    c[static_cast<std::size_t>(t[2])],
                                                    c[static_cast<std::size_t>(t[3])]}};
                    const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                            out.nodes[n[2]], out.nodes[n[3]]);
                    if (v < 0.0) {
                        std::swap(n[1], n[2]);
                    } else if (v == 0.0) {
                        continue;
                    }
                    out.tets.push_back(n);
                }

                struct FaceDef {
                    int di, dj, dk;
                    std::array<std::array<int, 3>, 4> corners;
                };
                const std::array<FaceDef, 6> faces{{
                    {-1, 0, 0, {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}}},
                    {1, 0, 0, {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}}},
                    {0, -1, 0, {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}}},
                    {0, 1, 0, {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}}},
                    {0, 0, -1, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}},
                    {0, 0, 1, {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}}},
                }};
                for (const auto& f : faces) {
                    if (is_inside(i + f.di, j + f.dj, k + f.dk)) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int q = 0; q < 4; ++q) {
                        const auto& corner = f.corners[static_cast<std::size_t>(q)];
                        quad[static_cast<std::size_t>(q)] =
                            node_at(i + corner[0], j + corner[1], k + corner[2]);
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }

    if (out.tets.empty()) {
        throw ValidityError("tet_fill_surface: no interior cells (empty or open surface?)");
    }

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const double h_snap = out.h;
        const double vol_eps = 1e-14 * h_snap * h_snap * h_snap;
        const auto tet_is_bad = [&](const std::array<std::uint32_t, 4>& n) {
            const Eigen::Vector3d& a = out.nodes[n[0]];
            const Eigen::Vector3d& b = out.nodes[n[1]];
            const Eigen::Vector3d& c = out.nodes[n[2]];
            const Eigen::Vector3d& d = out.nodes[n[3]];
            // vol_eps alone is a machine-degeneracy test (~1e-14·h³, thirteen
            // orders under a healthy tet), so the snap was free to flatten skin
            // tets into slivers. Add the shape floor the unsnap line-search was
            // supposed to defend.
            return !(tet_signed_volume_impl(a, b, c, d) > vol_eps &&
                     validity::tet_shape_quality(a, b, c, d) >= validity::kCellShapeFloor);
        };
        // Node -> incident tets, so the snap can line-search ONE node against
        // its own star instead of rescanning the whole mesh.
        //
        // Without this callback `snap_boundary_nodes` takes its compatibility
        // path: a 0.75/0.5/0.25 ladder, and if none of the three fractions is
        // valid the node retreats ALL THE WAY to its raw Cartesian lattice
        // site. On a bore that is a spike. Measured on plate_hole at h=3 mm:
        // 30 near-bore boundary nodes off the exact CAD by up to 1.99 mm --
        // 0.67 h, a fifth of the bore radius -- and the mesher printed
        // `snap max|d|=0.002 m` while shipping it. With the callback the same
        // node keeps the largest fraction of its projection that stays valid.
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> incident;
        incident.reserve(out.nodes.size());
        for (std::size_t ti = 0; ti < out.tets.size(); ++ti) {
            for (const auto ni : out.tets[ti]) {
                incident[ni].push_back(ti);
            }
        }
        // Multi-pass snap ≤0.75 h with Jacobian safety (shared helper; ADR-0015/B3).
        snap_boundary_nodes(
            surface, out.nodes, bnodes, h_snap,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto& n : out.tets) {
                    if (tet_is_bad(n)) {
                        offenders.insert(n.begin(), n.end());
                    }
                }
            },
            /*max_move_frac=*/0.75, /*passes=*/4, /*feature_edges=*/{},
            /*repair_interior=*/{},
            /*node_offends=*/
            [&](std::uint32_t ni) {
                const auto it = incident.find(ni);
                if (it == incident.end()) {
                    return false;
                }
                for (const auto ti : it->second) {
                    if (tet_is_bad(out.tets[ti])) {
                        return true;
                    }
                }
                return false;
            });

        for (auto& n : out.tets) {
            const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                    out.nodes[n[2]], out.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
        check_tet_fill_geometry(out, vol_eps);
        return out;
    }

    check_tet_fill_geometry(out);
    return out;
}

} // namespace polymesh::mesh
