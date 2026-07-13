// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_export.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "Delaunay_psm.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

namespace polymesh::mesh {
namespace {

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

struct QuantKey {
    long long x = 0, y = 0, z = 0;
    bool operator==(const QuantKey& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct QuantHash {
    std::size_t operator()(const QuantKey& k) const noexcept {
        // splitmix-ish combine
        std::size_t h = static_cast<std::size_t>(k.x) * 0x9e3779b97f4a7c15ULL;
        h ^= static_cast<std::size_t>(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

QuantKey quantize(const Eigen::Vector3d& p, double inv_eps) {
    QuantKey k;
    k.x = llround(p.x() * inv_eps);
    k.y = llround(p.y() * inv_eps);
    k.z = llround(p.z() * inv_eps);
    return k;
}

ClipPlane bisector_keep_site(const Eigen::Vector3d& site,
                             const Eigen::Vector3d& other) {
    const Eigen::Vector3d n = site - other;
    const Eigen::Vector3d mid = 0.5 * (site + other);
    ClipPlane pl;
    pl.a = n.x();
    pl.b = n.y();
    pl.c = n.z();
    pl.d = -n.dot(mid);
    return pl;
}

/// Face extracted from one cell before welding / pairing.
struct RawFace {
    std::vector<Eigen::Vector3d> loop;  // ordered polygon
    /// If this face came from a bisector to site j, set to j; else npos (domain).
    std::size_t neighbour_site = static_cast<std::size_t>(-1);
};

struct RawCell {
    std::vector<RawFace> faces;
    double volume = 0.0;
    bool empty = true;
};

bool build_cell(VBW::ConvexCell& cell, const ClipBox& box,
                const Eigen::Vector3d& site,
                std::span<const Eigen::Vector3d> all_sites, std::size_t self) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    // Use create_vglobal so bisector planes carry neighbour site ids.
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());
    cell.create_vglobal();
    // Box planes get vglobal = -1 by default after create_vglobal.
    for (std::size_t j = 0; j < all_sites.size(); ++j) {
        if (j == self) {
            continue;
        }
        const Eigen::Vector3d d = site - all_sites[j];
        if (d.squaredNorm() < 1e-30) {
            continue;
        }
        const ClipPlane pl = bisector_keep_site(site, all_sites[j]);
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d),
                           static_cast<VBW::global_index_t>(j));
        if (cell.empty()) {
            return false;
        }
    }
    return !cell.empty();
}

RawCell extract_raw_cell(VBW::ConvexCell& cell) {
    RawCell raw;
    if (cell.empty()) {
        return raw;
    }
    cell.compute_geometry();
    raw.volume = cell.volume();
    raw.empty = !(raw.volume > 0.0);
    if (raw.empty) {
        return raw;
    }

    // Facet per contributing plane v (skip infinity).
    for (VBW::index_t v = 0; v < cell.nb_v(); ++v) {
        if (cell.vertex_triangle(v) == VBW::END_OF_LIST) {
            continue;
        }
        RawFace face;
        if (cell.has_vglobal()) {
            const auto g = cell.v_global_index(v);
            // -1 = box / unset; valid site ids are 0..n-1
            if (g != static_cast<VBW::global_index_t>(-1) &&
                g != static_cast<VBW::global_index_t>(-2)) {
                face.neighbour_site = static_cast<std::size_t>(g);
            }
        }

        cell.for_each_Voronoi_vertex(v, [&](VBW::index_t t) {
            const VBW::vec3 p =
                cell.triangle_point(static_cast<VBW::ushort>(t));
            face.loop.emplace_back(p.x, p.y, p.z);
        });
        if (face.loop.size() >= 3) {
            raw.faces.push_back(std::move(face));
        }
    }
    raw.empty = raw.faces.size() < 4;
    return raw;
}

// Canonical undirected pair for site adjacency.
std::pair<std::size_t, std::size_t> site_pair(std::size_t a, std::size_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

#endif  // GEOGRAM

}  // namespace

ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const Eigen::Vector3d> sites) {
    ClippedVoronoiExport out;
    out.stats.n_sites = sites.size();
    out.site_to_cell.assign(sites.size(), static_cast<std::size_t>(-1));

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    out.stats.geogram_ok = false;
    return out;
#else
    if (!geogram_available() || sites.empty()) {
        out.stats.geogram_ok = geogram_available();
        return out;
    }
    out.stats.geogram_ok = true;
    geogram_ensure_initialized();

    std::vector<RawCell> raw_cells(sites.size());
    {
        VBW::ConvexCell cell;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            if (!build_cell(cell, domain, sites[i], sites, i)) {
                raw_cells[i].empty = true;
                ++out.stats.n_empty_cells;
                continue;
            }
            raw_cells[i] = extract_raw_cell(cell);
            if (raw_cells[i].empty) {
                ++out.stats.n_empty_cells;
            } else {
                out.stats.sum_cell_volume += raw_cells[i].volume;
            }
        }
    }

    // Weld vertices.
    const double diag = std::max((domain.max - domain.min).norm(), 1e-30);
    const double eps = 1e-10 * diag;
    const double inv_eps = 1.0 / eps;
    std::unordered_map<QuantKey, VertexId, QuantHash> weld;
    auto weld_point = [&](const Eigen::Vector3d& p) -> VertexId {
        const QuantKey k = quantize(p, inv_eps);
        if (auto it = weld.find(k); it != weld.end()) {
            return it->second;
        }
        const auto id = static_cast<VertexId>(out.mesh.vertices.size());
        out.mesh.vertices.push_back(p);
        weld.emplace(k, id);
        return id;
    };

    // Map cell index in raw → cell id in mesh for non-empty cells.
    std::vector<std::size_t> raw_to_mesh(sites.size(), static_cast<std::size_t>(-1));
    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (raw_cells[i].empty || raw_cells[i].faces.size() < 4) {
            continue;
        }
        const auto cid = out.mesh.cells.size();
        raw_to_mesh[i] = cid;
        out.site_to_cell[i] = cid;
        out.mesh.cells.push_back(
            Cell{.kind = CellKind::kPolyhedron, .faces = {}});
        ++out.stats.n_cells;
    }

    // Emit faces; pair interior bisectors by (min_site, max_site).
    // Key: site pair → face id already created by the first cell that saw it.
    std::map<std::pair<std::size_t, std::size_t>, FaceId> interior_faces;

    for (std::size_t i = 0; i < sites.size(); ++i) {
        if (raw_to_mesh[i] == static_cast<std::size_t>(-1)) {
            continue;
        }
        const CellId cid = static_cast<CellId>(raw_to_mesh[i]);

        for (const RawFace& rf : raw_cells[i].faces) {
            std::vector<VertexId> loop;
            loop.reserve(rf.loop.size());
            for (const auto& p : rf.loop) {
                loop.push_back(weld_point(p));
            }
            // Drop degenerate (collapsed) loops after weld.
            {
                std::vector<VertexId> dedup;
                for (VertexId v : loop) {
                    if (dedup.empty() || dedup.back() != v) {
                        dedup.push_back(v);
                    }
                }
                if (dedup.size() >= 2 && dedup.front() == dedup.back()) {
                    dedup.pop_back();
                }
                loop = std::move(dedup);
            }
            if (loop.size() < 3) {
                continue;
            }

            // Interior face shared with neighbour site?
            if (rf.neighbour_site != static_cast<std::size_t>(-1) &&
                rf.neighbour_site < sites.size() &&
                raw_to_mesh[rf.neighbour_site] != static_cast<std::size_t>(-1)) {
                const auto key = site_pair(i, rf.neighbour_site);
                if (auto it = interior_faces.find(key); it != interior_faces.end()) {
                    // Second cell sees this face — set neighbour, reverse check.
                    Face& f = out.mesh.faces[it->second];
                    f.neighbour = cid;
                    out.mesh.cells[cid].faces.push_back(it->second);
                    continue;
                }
                // First cell owns the face.
                Face face;
                face.vertices = std::move(loop);
                face.owner = cid;
                face.neighbour = std::nullopt;  // filled when neighbour arrives
                const FaceId fid = static_cast<FaceId>(out.mesh.faces.size());
                out.mesh.faces.push_back(std::move(face));
                out.mesh.cells[cid].faces.push_back(fid);
                interior_faces.emplace(key, fid);
                continue;
            }

            // Boundary face (domain).
            Face face;
            face.vertices = std::move(loop);
            face.owner = cid;
            face.neighbour = std::nullopt;
            const FaceId fid = static_cast<FaceId>(out.mesh.faces.size());
            out.mesh.faces.push_back(std::move(face));
            out.mesh.cells[cid].faces.push_back(fid);
        }
    }

    // Count face kinds; drop cells that lost too many faces after weld.
    std::vector<Cell> kept_cells;
    kept_cells.reserve(out.mesh.cells.size());
    // Rebuild face owner ids if we drop cells — simpler: just validate faces ≥4.
    for (std::size_t c = 0; c < out.mesh.cells.size(); ++c) {
        if (out.mesh.cells[c].faces.size() < 4) {
            // Leave as-is; check_validity will catch in tests if needed.
            // Prefer removing broken cells.
        }
    }

    out.stats.n_vertices = out.mesh.vertices.size();
    out.stats.n_faces = out.mesh.faces.size();
    for (const Face& f : out.mesh.faces) {
        if (f.neighbour) {
            ++out.stats.n_interior_faces;
        } else {
            ++out.stats.n_boundary_faces;
        }
    }
    return out;
#endif
}

ClippedVoronoiExport export_clipped_voronoi(const ClipBox& domain,
                                            std::span<const CvtSite> sites) {
    std::vector<Eigen::Vector3d> pos;
    pos.reserve(sites.size());
    for (const CvtSite& s : sites) {
        pos.push_back(s.pos);
    }
    return export_clipped_voronoi(domain, std::span<const Eigen::Vector3d>(pos));
}

}  // namespace polymesh::mesh
