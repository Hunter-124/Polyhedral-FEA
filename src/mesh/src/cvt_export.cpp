// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_export.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
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

/// Plane through triangle that keeps `hint` (site) on the ≥0 side.
bool plane_keep_hint(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                     const Eigen::Vector3d& c, const Eigen::Vector3d& hint,
                     double min_area, ClipPlane& out) {
    const Eigen::Vector3d e1 = b - a;
    const Eigen::Vector3d e2 = c - a;
    Eigen::Vector3d n = e1.cross(e2);
    const double area2 = n.norm();
    if (!(area2 > min_area)) {
        return false;
    }
    n /= area2;
    // ConvexCell keep: n·x + d ≥ 0 with d = -n·p.
    // Flip if hint is currently on the negative side.
    double d = -n.dot(a);
    if (n.dot(hint) + d < 0.0) {
        n = -n;
        d = -n.dot(a);
    }
    // Site must stay strictly inside (tolerance for coplanar).
    if (n.dot(hint) + d < -1e-14 * (1.0 + a.norm())) {
        return false;
    }
    out.a = n.x();
    out.b = n.y();
    out.c = n.z();
    out.d = d;
    return true;
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
                std::span<const Eigen::Vector3d> all_sites, std::size_t self,
                std::span<const ClipPlane> domain_planes,
                std::size_t& n_domain_clips) {
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

    // M5: *global* solid halfspaces (same planes for every site) so adjacent
    // cells share a consistent domain. Per-site local triangle subsets caused
    // gaps/overlaps and destroyed VEM energy. Domain faces: vglobal = -2.
    // Sites on the wrong side of any plane are dropped (not selectively
    // clipped) so every surviving cell uses the same plane set.
    if (!domain_planes.empty()) {
        constexpr VBW::global_index_t kDomainVGlobal =
            static_cast<VBW::global_index_t>(-2);
        for (const ClipPlane& pl : domain_planes) {
            const double sd =
                pl.a * site.x() + pl.b * site.y() + pl.c * site.z() + pl.d;
            if (sd < -1e-10) {
                return false;  // site outside solid halfspaces
            }
            cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d),
                               kDomainVGlobal);
            ++n_domain_clips;
            if (cell.empty()) {
                return false;
            }
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
            // -1 = box / unset; -2 = domain surface; valid site ids are 0..n-1
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

std::vector<ClipPlane> domain_planes_from_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& interior_hint,
    double min_area) {
    std::vector<ClipPlane> planes;
    planes.reserve(surface.triangles.size());
    for (const auto& tri : surface.triangles) {
        if (tri[0] >= surface.vertices.size() ||
            tri[1] >= surface.vertices.size() ||
            tri[2] >= surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d& a = surface.vertices[tri[0]];
        const Eigen::Vector3d& b = surface.vertices[tri[1]];
        const Eigen::Vector3d& c = surface.vertices[tri[2]];
        ClipPlane pl;
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
        if (!plane_keep_hint(a, b, c, interior_hint, min_area, pl)) {
            continue;
        }
#else
        const Eigen::Vector3d e1 = b - a;
        const Eigen::Vector3d e2 = c - a;
        Eigen::Vector3d n = e1.cross(e2);
        const double area2 = n.norm();
        if (!(area2 > min_area)) {
            continue;
        }
        n /= area2;
        double d = -n.dot(a);
        if (n.dot(interior_hint) + d < 0.0) {
            n = -n;
            d = -n.dot(a);
        }
        pl.a = n.x();
        pl.b = n.y();
        pl.c = n.z();
        pl.d = d;
#endif
        planes.push_back(pl);
    }
    return planes;
}

ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
    const DomainClipParams& domain_clip) {
    ClippedVoronoiExport out;
    out.stats.n_sites = sites.size();
    out.site_to_cell.assign(sites.size(), static_cast<std::size_t>(-1));

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    (void)domain_clip;
    out.stats.geogram_ok = false;
    return out;
#else
    if (!geogram_available() || sites.empty()) {
        out.stats.geogram_ok = geogram_available();
        return out;
    }
    out.stats.geogram_ok = true;
    geogram_ensure_initialized();

    // Build a *global* set of domain halfspaces from the surface.
    // Prefer TriSurface outward CCW → keep inward (-n_out). If a plane would
    // exclude a majority of sites (bad winding / non-manifold), flip it.
    std::vector<ClipPlane> domain_planes;
    if (domain_clip.surface && !domain_clip.surface->triangles.empty() &&
        !domain_clip.surface->vertices.empty() && !sites.empty()) {
        out.stats.domain_clip_used = true;
        const auto& surf = *domain_clip.surface;

        // Cap plane count for cost on dense tessellations.
        const std::size_t n_tri = surf.triangles.size();
        const std::size_t max_planes = 8000;
        const std::size_t stride =
            std::max<std::size_t>(1, (n_tri + max_planes - 1) / max_planes);

        domain_planes.reserve(std::min(n_tri, max_planes) + 8);
        const std::size_t n_sites = sites.size();
        for (std::size_t ti = 0; ti < n_tri; ti += stride) {
            const auto& tri = surf.triangles[ti];
            if (tri[0] >= surf.vertices.size() || tri[1] >= surf.vertices.size() ||
                tri[2] >= surf.vertices.size()) {
                continue;
            }
            const Eigen::Vector3d& a = surf.vertices[tri[0]];
            const Eigen::Vector3d& b = surf.vertices[tri[1]];
            const Eigen::Vector3d& c = surf.vertices[tri[2]];
            Eigen::Vector3d n_out = (b - a).cross(c - a);
            const double area2 = n_out.norm();
            if (!(area2 > 1e-30)) {
                continue;
            }
            n_out /= area2;
            // Inward keep halfspace from outward triangle normal.
            ClipPlane pl = ClipPlane::from_point_normal(a, -n_out);
            std::size_t n_in = 0;
            for (const auto& s : sites) {
                if (pl.a * s.x() + pl.b * s.y() + pl.c * s.z() + pl.d >= -1e-12) {
                    ++n_in;
                }
            }
            if (n_in * 2 < n_sites) {
                // Flip: triangle winding disagreed with site cloud.
                pl.a = -pl.a;
                pl.b = -pl.b;
                pl.c = -pl.c;
                pl.d = -pl.d;
            }
            domain_planes.push_back(pl);
        }
        (void)domain_clip.clip_radius;  // reserved; global planes ignore local R
        (void)domain_clip.min_area_frac;
    }

    std::vector<RawCell> raw_cells(sites.size());
    {
        VBW::ConvexCell cell;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            std::size_t n_clips = 0;
            if (!build_cell(cell, domain, sites[i], sites, i, domain_planes,
                            n_clips)) {
                raw_cells[i].empty = true;
                ++out.stats.n_empty_cells;
                continue;
            }
            out.stats.n_domain_plane_clips += n_clips;
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

            // Boundary face (AABB domain or solid surface).
            Face face;
            face.vertices = std::move(loop);
            face.owner = cid;
            face.neighbour = std::nullopt;
            const FaceId fid = static_cast<FaceId>(out.mesh.faces.size());
            out.mesh.faces.push_back(std::move(face));
            out.mesh.cells[cid].faces.push_back(fid);
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
                                            std::span<const CvtSite> sites,
                                            const DomainClipParams& domain_clip) {
    std::vector<Eigen::Vector3d> pos;
    pos.reserve(sites.size());
    for (const CvtSite& s : sites) {
        pos.push_back(s.pos);
    }
    return export_clipped_voronoi(domain, std::span<const Eigen::Vector3d>(pos),
                                  domain_clip);
}

}  // namespace polymesh::mesh
