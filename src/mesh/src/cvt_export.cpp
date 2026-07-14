// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_export.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        // Supporting halfspaces only: keep a plane iff (after orient) *all*
        // sites lie on the keep side. That is the convex envelope of the site
        // cloud from surface triangles — safe for plate_hole (hole walls are
        // non-supporting and are skipped). Full halfspaces of a holed solid
        // incorrectly cut opposite material.
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
                if (pl.a * s.x() + pl.b * s.y() + pl.c * s.z() + pl.d >= -1e-10) {
                    ++n_in;
                }
            }
            if (n_in * 2 < n_sites) {
                // Flip: triangle winding disagreed with site cloud.
                pl.a = -pl.a;
                pl.b = -pl.b;
                pl.c = -pl.c;
                pl.d = -pl.d;
                n_in = n_sites - n_in;
            }
            // Require nearly all sites inside (supporting). Small tolerance
            // for surface-adjacent free sites after soft inset.
            if (n_in + std::max<std::size_t>(1, n_sites / 50) < n_sites) {
                continue;
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

namespace {

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

/// Four halfspaces of a tet that keep the tet interior (and `hint` if possible).
bool tet_keep_planes(const DomainTet& tet, const Eigen::Vector3d& hint,
                     ClipPlane out[4]) {
    // Faces: (1,2,3), (0,3,2), (0,1,3), (0,2,1) with positive tet volume.
    const Eigen::Vector3d* v[4] = {&tet.v0, &tet.v1, &tet.v2, &tet.v3};
    static constexpr int kFace[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
    for (int f = 0; f < 4; ++f) {
        if (!plane_keep_hint(*v[kFace[f][0]], *v[kFace[f][1]], *v[kFace[f][2]],
                             hint, 1e-30, out[f])) {
            // Fall back: orient using tet centroid.
            if (!plane_keep_hint(*v[kFace[f][0]], *v[kFace[f][1]], *v[kFace[f][2]],
                                 tet.centroid, 1e-30, out[f])) {
                return false;
            }
        }
    }
    return true;
}

/// Build the (bisector-only) restricted Voronoi cell of `pos[self]` using a
/// spatial grid + security radius, and record the neighbour site indices that
/// were clipped against plus the cell radius `Ri` (max vertex distance from the
/// site). The neighbour set is a superset of the true Voronoi neighbours, which
/// is all we need: extra bisectors that miss V_i ∩ tet never cut it.
bool voronoi_neighbours(VBW::ConvexCell& cell, const ClipBox& box,
                        std::span<const Eigen::Vector3d> pos, std::size_t self,
                        const SiteGrid& grid, std::vector<std::uint32_t>& nbr,
                        std::vector<std::uint32_t>& ring_buf, double& Ri) {
    nbr.clear();
    Ri = 0.0;
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());
    const Eigen::Vector3d& s = pos[self];
    const VBW::vec3 sc = VBW::make_vec3(s.x(), s.y(), s.z());
    const double g = grid.cell_edge();
    const int kmax = grid.max_ring();
    double R2 = 0.0;
    for (int k = 0; k <= kmax; ++k) {
        ring_buf.clear();
        grid.ring(s, k, ring_buf);
        for (std::uint32_t j : ring_buf) {
            if (static_cast<std::size_t>(j) == self) {
                continue;
            }
            if ((s - pos[j]).squaredNorm() < 1e-30) {
                continue;
            }
            const ClipPlane pl = bisector_keep_site(s, pos[j]);
            cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d));
            nbr.push_back(j);
            if (cell.empty()) {
                return false;
            }
        }
        if (k >= 1) {
            R2 = cell.squared_radius(sc);
            const double dmin = static_cast<double>(k) * g;
            if (4.0 * R2 <= dmin * dmin) {
                break;
            }
        }
    }
    Ri = std::sqrt(std::max(R2 > 0.0 ? R2 : cell.squared_radius(sc), 0.0));
    return !cell.empty();
}

/// Clip V_i ∩ tet using a precomputed neighbour list. Tet planes are clipped
/// first (cheap, tags domain faces), then only the neighbour bisectors — never
/// all N sites. Bisectors carry their site global index for face pairing.
bool build_cell_tet_nbr(VBW::ConvexCell& cell, const ClipBox& box,
                        const Eigen::Vector3d& site,
                        std::span<const Eigen::Vector3d> pos,
                        std::span<const std::uint32_t> nbr, std::size_t self,
                        const ClipPlane tet_planes[4], std::size_t& n_clips) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());
    cell.create_vglobal();
    constexpr VBW::global_index_t kTetVGlobal = static_cast<VBW::global_index_t>(-2);
    for (int f = 0; f < 4; ++f) {
        const ClipPlane& pl = tet_planes[f];
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d), kTetVGlobal);
        ++n_clips;
        if (cell.empty()) {
            return false;
        }
    }
    for (std::uint32_t j : nbr) {
        if (static_cast<std::size_t>(j) == self) {
            continue;
        }
        if ((site - pos[j]).squaredNorm() < 1e-30) {
            continue;
        }
        const ClipPlane pl = bisector_keep_site(site, pos[j]);
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d),
                           static_cast<VBW::global_index_t>(j));
        if (cell.empty()) {
            return false;
        }
    }
    return !cell.empty();
}

/// Geometric face key for pairing coplanar interfaces across pieces.
struct GeoFaceKey {
    long long cx = 0, cy = 0, cz = 0;
    bool operator==(const GeoFaceKey& o) const {
        return cx == o.cx && cy == o.cy && cz == o.cz;
    }
};
struct GeoFaceHash {
    std::size_t operator()(const GeoFaceKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(k.cx) * 0x9e3779b97f4a7c15ULL;
        h ^= static_cast<std::size_t>(k.cy) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.cz) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

GeoFaceKey face_centroid_key(const std::vector<Eigen::Vector3d>& loop, double inv_eps) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (const auto& p : loop) {
        c += p;
    }
    c /= static_cast<double>(std::max<std::size_t>(1, loop.size()));
    GeoFaceKey k;
    k.cx = llround(c.x() * inv_eps);
    k.cy = llround(c.y() * inv_eps);
    k.cz = llround(c.z() * inv_eps);
    return k;
}

#endif  // GEOGRAM

}  // namespace

ClippedVoronoiExport export_rvd_tet_clipped(const ClipBox& domain,
                                            std::span<const Eigen::Vector3d> sites,
                                            std::span<const DomainTet> tets,
                                            double tet_search_radius) {
    ClippedVoronoiExport out;
    out.stats.n_sites = sites.size();
    // site_to_cell: first piece for that site (or npos).
    out.site_to_cell.assign(sites.size(), static_cast<std::size_t>(-1));

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    (void)tets;
    (void)tet_search_radius;
    out.stats.geogram_ok = false;
    return out;
#else
    if (!geogram_available() || sites.empty() || tets.empty()) {
        out.stats.geogram_ok = geogram_available();
        return out;
    }
    out.stats.geogram_ok = true;
    out.stats.domain_clip_used = true;
    geogram_ensure_initialized();

    const double diag = std::max((domain.max - domain.min).norm(), 1e-30);
    (void)tet_search_radius;  // superseded by the per-site security-radius bound

    // Precompute tet planes once oriented to tet centroid; `reach` = max vertex
    // distance from the centroid (tet bounding radius) for the near-cell test.
    struct TetReady {
        DomainTet tet;
        ClipPlane planes[4];
        double reach = 0.0;
        bool ok = false;
    };
    std::vector<TetReady> ready;
    ready.reserve(tets.size());
    for (const DomainTet& t : tets) {
        TetReady tr;
        tr.tet = t;
        if (tr.tet.centroid.squaredNorm() < 1e-30 &&
            (tr.tet.v0 - tr.tet.v1).squaredNorm() > 0.0) {
            tr.tet.centroid = 0.25 * (tr.tet.v0 + tr.tet.v1 + tr.tet.v2 + tr.tet.v3);
        }
        tr.reach = std::sqrt(std::max({(tr.tet.v0 - tr.tet.centroid).squaredNorm(),
                                       (tr.tet.v1 - tr.tet.centroid).squaredNorm(),
                                       (tr.tet.v2 - tr.tet.centroid).squaredNorm(),
                                       (tr.tet.v3 - tr.tet.centroid).squaredNorm()}));
        tr.ok = tet_keep_planes(tr.tet, tr.tet.centroid, tr.planes);
        ready.push_back(tr);
    }

    // Pieces: (site, tet, raw_cell). `tet` gives a deterministic sort key so the
    // parallel emit order matches a serial run.
    struct Piece {
        std::size_t site = 0;
        std::size_t tet = 0;
        RawCell cell;
    };
    std::vector<Piece> pieces;
    pieces.reserve(sites.size() * 4);

    // Neighbour grid over sites: each cell clips only its spatial neighbours
    // (security radius), never all N sites. Bucket edge ≈ mean site spacing.
    const Eigen::Vector3d ext = (domain.max - domain.min).cwiseMax(1e-30);
    const double box_vol = ext.x() * ext.y() * ext.z();
    double g_edge = std::cbrt(box_vol / static_cast<double>(std::max<std::size_t>(1, sites.size())));
    if (!(g_edge > 0.0)) {
        g_edge = 0.25 * diag;
    }
    g_edge = std::max(g_edge, 1e-9 * diag);
    SiteGrid grid;
    grid.build(sites, g_edge);

    std::size_t n_empty = 0;
    std::size_t n_clips_total = 0;
    double sum_vol = 0.0;
    const auto n = static_cast<std::ptrdiff_t>(sites.size());

#pragma omp parallel
    {
        VBW::ConvexCell cell;
        VBW::ConvexCell ncell;
        std::vector<std::uint32_t> ring_buf;
        std::vector<std::uint32_t> nbr;
        std::vector<Piece> loc_pieces;
        std::size_t loc_empty = 0;
        std::size_t loc_clips = 0;
        double loc_vol = 0.0;
#pragma omp for schedule(dynamic, 32)
        for (std::ptrdiff_t ii = 0; ii < n; ++ii) {
            const auto i = static_cast<std::size_t>(ii);
            double Ri = 0.0;
            if (!voronoi_neighbours(ncell, domain, sites, i, grid, nbr, ring_buf, Ri)) {
                ++loc_empty;
                continue;
            }
            bool any = false;
            for (std::size_t ti = 0; ti < ready.size(); ++ti) {
                const TetReady& tr = ready[ti];
                if (!tr.ok) {
                    continue;
                }
                // V_i is bounded by radius Ri; a tet can only meet it if its
                // nearest point is within Ri (centroid distance − tet reach).
                if ((tr.tet.centroid - sites[i]).norm() > Ri + tr.reach) {
                    continue;
                }
                std::size_t n_clips = 0;
                if (!build_cell_tet_nbr(cell, domain, sites[i], sites, nbr, i,
                                        tr.planes, n_clips)) {
                    continue;
                }
                loc_clips += n_clips;
                RawCell raw = extract_raw_cell(cell);
                if (raw.empty || raw.faces.size() < 4) {
                    continue;
                }
                loc_vol += raw.volume;
                loc_pieces.push_back(Piece{i, ti, std::move(raw)});
                any = true;
            }
            if (!any) {
                ++loc_empty;
            }
        }
#pragma omp critical
        {
            for (auto& p : loc_pieces) {
                pieces.push_back(std::move(p));
            }
            n_empty += loc_empty;
            n_clips_total += loc_clips;
            sum_vol += loc_vol;
        }
    }

    // Deterministic emit order (parallel scheduling is nondeterministic).
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        return a.site != b.site ? a.site < b.site : a.tet < b.tet;
    });
    out.stats.n_empty_cells += n_empty;
    out.stats.n_domain_plane_clips += n_clips_total;
    out.stats.sum_cell_volume += sum_vol;

    // Weld + emit pieces as cells; pair coplanar faces by centroid key.
    const double eps = 1e-9 * diag;
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

    std::unordered_map<GeoFaceKey, FaceId, GeoFaceHash> geo_faces;
    // Slightly coarser key for face pairing than vertex weld.
    const double inv_face = 1.0 / (5e-9 * diag + 1e-16);

    for (Piece& piece : pieces) {
        if (piece.cell.empty || piece.cell.faces.size() < 4) {
            continue;
        }
        const CellId cid = static_cast<CellId>(out.mesh.cells.size());
        out.mesh.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {}});
        if (out.site_to_cell[piece.site] == static_cast<std::size_t>(-1)) {
            out.site_to_cell[piece.site] = cid;
        }
        ++out.stats.n_cells;

        for (const RawFace& rf : piece.cell.faces) {
            std::vector<VertexId> loop;
            loop.reserve(rf.loop.size());
            for (const auto& p : rf.loop) {
                loop.push_back(weld_point(p));
            }
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

            // Prefer site-bisector pairing when both sites have pieces.
            if (rf.neighbour_site != static_cast<std::size_t>(-1) &&
                rf.neighbour_site < sites.size()) {
                // Fall through to geometric pairing — multi-piece makes
                // site-pair keys non-unique across tets.
            }

            const GeoFaceKey gk = face_centroid_key(rf.loop, inv_face);
            if (auto it = geo_faces.find(gk); it != geo_faces.end()) {
                Face& f = out.mesh.faces[it->second];
                if (!f.neighbour && f.owner != cid) {
                    f.neighbour = cid;
                    out.mesh.cells[cid].faces.push_back(it->second);
                    continue;
                }
            }
            Face face;
            face.vertices = std::move(loop);
            face.owner = cid;
            face.neighbour = std::nullopt;
            const FaceId fid = static_cast<FaceId>(out.mesh.faces.size());
            out.mesh.faces.push_back(std::move(face));
            out.mesh.cells[cid].faces.push_back(fid);
            geo_faces.emplace(gk, fid);
        }
    }

    // Second-pass: pair remaining free faces that share a centroid (multi-piece
    // RVD interfaces the first geometric map missed).
    {
        const double inv = 1.0 / (1e-8 * diag + 1e-16);
        struct Item {
            FaceId fid = 0;
            CellId owner = 0;
            Eigen::Vector3d c{0, 0, 0};
            Eigen::Vector3d n{0, 0, 0};
            double area = 0.0;
        };
        std::vector<Item> free_f;
        free_f.reserve(out.mesh.faces.size());
        for (std::size_t fi = 0; fi < out.mesh.faces.size(); ++fi) {
            Face& f = out.mesh.faces[fi];
            if (f.neighbour || f.vertices.size() < 3) {
                continue;
            }
            Item it;
            it.fid = static_cast<FaceId>(fi);
            it.owner = f.owner;
            for (auto v : f.vertices) {
                it.c += out.mesh.vertices[v];
            }
            it.c /= static_cast<double>(f.vertices.size());
            for (std::size_t i = 0; i < f.vertices.size(); ++i) {
                const auto& p = out.mesh.vertices[f.vertices[i]];
                const auto& q =
                    out.mesh.vertices[f.vertices[(i + 1) % f.vertices.size()]];
                it.n.x() += (p.y() - q.y()) * (p.z() + q.z());
                it.n.y() += (p.z() - q.z()) * (p.x() + q.x());
                it.n.z() += (p.x() - q.x()) * (p.y() + q.y());
            }
            it.area = 0.5 * it.n.norm();
            if (it.area > 1e-30) {
                it.n /= (2.0 * it.area);
            }
            free_f.push_back(it);
        }
        std::unordered_map<GeoFaceKey, std::vector<std::size_t>, GeoFaceHash> buckets;
        for (std::size_t i = 0; i < free_f.size(); ++i) {
            GeoFaceKey k;
            k.cx = llround(free_f[i].c.x() * inv);
            k.cy = llround(free_f[i].c.y() * inv);
            k.cz = llround(free_f[i].c.z() * inv);
            buckets[k].push_back(i);
        }
        std::vector<char> used(free_f.size(), 0);
        for (auto& kv : buckets) {
            auto& idxs = kv.second;
            for (std::size_t a = 0; a < idxs.size(); ++a) {
                if (used[idxs[a]]) {
                    continue;
                }
                for (std::size_t b = a + 1; b < idxs.size(); ++b) {
                    if (used[idxs[b]]) {
                        continue;
                    }
                    const Item& A = free_f[idxs[a]];
                    const Item& B = free_f[idxs[b]];
                    if (A.owner == B.owner) {
                        continue;
                    }
                    if (A.n.dot(B.n) > -0.5) {
                        continue;
                    }
                    const double arel =
                        std::abs(A.area - B.area) /
                        std::max({A.area, B.area, 1e-30});
                    if (arel > 0.35) {
                        continue;
                    }
                    Face& fa = out.mesh.faces[A.fid];
                    Face& fb = out.mesh.faces[B.fid];
                    fa.neighbour = B.owner;
                    for (auto& fid : out.mesh.cells[B.owner].faces) {
                        if (fid == B.fid) {
                            fid = A.fid;
                            break;
                        }
                    }
                    fb.neighbour = A.owner;
                    fb.vertices.clear();
                    used[idxs[a]] = 1;
                    used[idxs[b]] = 1;
                    break;
                }
            }
        }
    }

    out.stats.n_vertices = out.mesh.vertices.size();
    out.stats.n_faces = out.mesh.faces.size();
    for (const Face& f : out.mesh.faces) {
        if (f.vertices.empty()) {
            continue;
        }
        if (f.neighbour) {
            ++out.stats.n_interior_faces;
        } else {
            ++out.stats.n_boundary_faces;
        }
    }
    return out;
#endif
}

ClippedVoronoiExport export_rvd_tet_clipped(const ClipBox& domain,
                                            std::span<const CvtSite> sites,
                                            std::span<const DomainTet> tets,
                                            double tet_search_radius) {
    std::vector<Eigen::Vector3d> pos;
    pos.reserve(sites.size());
    for (const CvtSite& s : sites) {
        pos.push_back(s.pos);
    }
    return export_rvd_tet_clipped(domain, std::span<const Eigen::Vector3d>(pos), tets,
                                  tet_search_radius);
}

}  // namespace polymesh::mesh
