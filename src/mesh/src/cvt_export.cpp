// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_export.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
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
static constexpr int kTetLocalFaces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};

constexpr VBW::global_index_t tet_face_tag(std::size_t local_face) {
    return static_cast<VBW::global_index_t>(-2 - static_cast<int>(local_face));
}

struct QuantKey {
    long long x = 0, y = 0, z = 0;
    bool operator==(const QuantKey& o) const { return x == o.x && y == o.y && z == o.z; }
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
bool quantize_relative(const Eigen::Vector3d& p, const Eigen::Vector3d& origin, double inv_eps,
                       QuantKey& key) {
    // A power-of-two bound is represented exactly even when long double is
    // binary64, and leaves ample headroom for the ±1 neighbour walk.
    constexpr long long kMaxBucket = (1LL << 60);
    constexpr long long kMinBucket = -kMaxBucket;
    long long coordinates[3]{};
    for (Eigen::Index axis = 0; axis < 3; ++axis) {
        const long double delta =
            static_cast<long double>(p[axis]) - static_cast<long double>(origin[axis]);
        const long double scaled = delta * static_cast<long double>(inv_eps);
        const long double rounded = std::round(scaled);
        if (!std::isfinite(rounded) || rounded < static_cast<long double>(kMinBucket) ||
            rounded > static_cast<long double>(kMaxBucket)) {
            return false;
        }
        coordinates[axis] = static_cast<long long>(rounded);
    }
    key = QuantKey{coordinates[0], coordinates[1], coordinates[2]};
    return true;
}

ClipPlane bisector_keep_site(const Eigen::Vector3d& site, const Eigen::Vector3d& other) {
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
                     const Eigen::Vector3d& c, const Eigen::Vector3d& hint, double min_area,
                     ClipPlane& out) {
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

enum class RawFaceProvenance {
    kDomainBoundary,
    kTetScaffold,
    kBisector,
};

/// Face extracted from one cell before welding / pairing.
struct RawFace {
    std::vector<Eigen::Vector3d> loop; // ordered polygon
    RawFaceProvenance provenance = RawFaceProvenance::kDomainBoundary;
    /// Set only for a bisector face; preserves the opposite site identity.
    std::size_t neighbour_site = static_cast<std::size_t>(-1);
    /// Set only for a tagged tetra scaffold face.
    std::size_t tet_local_face = static_cast<std::size_t>(-1);
};

struct RawCell {
    std::vector<RawFace> faces;
    double volume = 0.0;
    bool empty = true;
};

bool build_cell(VBW::ConvexCell& cell, const ClipBox& box, const Eigen::Vector3d& site,
                std::span<const Eigen::Vector3d> all_sites, std::size_t self,
                std::span<const ClipPlane> domain_planes, std::size_t& n_domain_clips) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    // Use create_vglobal so bisector planes carry neighbour site ids.
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(), box.max.y(),
                       box.max.z());
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
        constexpr VBW::global_index_t kDomainVGlobal = static_cast<VBW::global_index_t>(-2);
        for (const ClipPlane& pl : domain_planes) {
            const double sd = pl.a * site.x() + pl.b * site.y() + pl.c * site.z() + pl.d;
            if (sd < -1e-10) {
                return false; // site outside solid halfspaces
            }
            cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d), kDomainVGlobal);
            ++n_domain_clips;
            if (cell.empty()) {
                return false;
            }
        }
    }
    return !cell.empty();
}

RawCell extract_raw_cell(VBW::ConvexCell& cell, bool tet_scaffold_tags = false) {
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
            bool is_tet_face = false;
            if (tet_scaffold_tags) {
                for (std::size_t local_face = 0; local_face < 4; ++local_face) {
                    if (g == tet_face_tag(local_face)) {
                        face.provenance = RawFaceProvenance::kTetScaffold;
                        face.tet_local_face = local_face;
                        is_tet_face = true;
                        break;
                    }
                }
            }
            // -1 = box / unset; -2 = generic domain surface. All remaining
            // values are exact neighbour-site ids from Voronoi bisectors.
            if (!is_tet_face && g != static_cast<VBW::global_index_t>(-1) &&
                g != static_cast<VBW::global_index_t>(-2)) {
                face.provenance = RawFaceProvenance::kBisector;
                face.neighbour_site = static_cast<std::size_t>(g);
            }
        }

        cell.for_each_Voronoi_vertex(v, [&](VBW::index_t t) {
            const VBW::vec3 p = cell.triangle_point(static_cast<VBW::ushort>(t));
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

#endif // GEOGRAM

} // namespace

std::vector<ClipPlane> domain_planes_from_surface(const geom::TriSurface& surface,
                                                  const Eigen::Vector3d& interior_hint,
                                                  double min_area) {
    std::vector<ClipPlane> planes;
    planes.reserve(surface.triangles.size());
    for (const auto& tri : surface.triangles) {
        if (tri[0] >= surface.vertices.size() || tri[1] >= surface.vertices.size() ||
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

ClippedVoronoiExport export_clipped_voronoi(const ClipBox& domain,
                                            std::span<const Eigen::Vector3d> sites,
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
        (void)domain_clip.clip_radius; // reserved; global planes ignore local R
        (void)domain_clip.min_area_frac;
    }

    std::vector<RawCell> raw_cells(sites.size());
    {
        VBW::ConvexCell cell;
        for (std::size_t i = 0; i < sites.size(); ++i) {
            std::size_t n_clips = 0;
            if (!build_cell(cell, domain, sites[i], sites, i, domain_planes, n_clips)) {
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
        out.mesh.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {}});
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
                face.neighbour = std::nullopt; // filled when neighbour arrives
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
    return export_clipped_voronoi(domain, std::span<const Eigen::Vector3d>(pos), domain_clip);
}

namespace {

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

/// Four halfspaces of a tet that keep the tet interior (and `hint` if possible).
bool tet_keep_planes(const DomainTet& tet, const Eigen::Vector3d& hint, ClipPlane out[4]) {
    // Fixed local face order, shared with scaffold multiplicity/tagging.
    const Eigen::Vector3d* v[4] = {&tet.v0, &tet.v1, &tet.v2, &tet.v3};
    for (int f = 0; f < 4; ++f) {
        if (!plane_keep_hint(*v[kTetLocalFaces[f][0]], *v[kTetLocalFaces[f][1]],
                             *v[kTetLocalFaces[f][2]], hint, 1e-30, out[f])) {
            // Fall back: orient using tet centroid.
            if (!plane_keep_hint(*v[kTetLocalFaces[f][0]], *v[kTetLocalFaces[f][1]],
                                 *v[kTetLocalFaces[f][2]], tet.centroid, 1e-30, out[f])) {
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
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(), box.max.y(),
                       box.max.z());
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
bool build_cell_tet_nbr(VBW::ConvexCell& cell, const ClipBox& box, const Eigen::Vector3d& site,
                        std::span<const Eigen::Vector3d> pos,
                        std::span<const std::uint32_t> nbr, std::size_t self,
                        const ClipPlane tet_planes[4], std::size_t& n_clips) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(), box.max.y(),
                       box.max.z());
    cell.create_vglobal();
    for (std::size_t f = 0; f < 4; ++f) {
        const ClipPlane& pl = tet_planes[f];
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d), tet_face_tag(f));
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

enum class ScaffoldFaceProvenance {
    kDomainBoundary,
    kInternal,
    kNonManifold,
};

struct ScaffoldFaceKey {
    std::array<std::array<double, 3>, 3> vertices{};

    bool operator<(const ScaffoldFaceKey& other) const { return vertices < other.vertices; }
};

ScaffoldFaceKey scaffold_face_key(const DomainTet& tet, std::size_t local_face) {
    const Eigen::Vector3d* vertices[4] = {&tet.v0, &tet.v1, &tet.v2, &tet.v3};
    ScaffoldFaceKey key;
    for (std::size_t corner = 0; corner < 3; ++corner) {
        const Eigen::Vector3d& p = *vertices[kTetLocalFaces[local_face][corner]];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            // Numeric exactness is intended; canonicalize signed zero so
            // equivalent scaffold coordinates share one key.
            key.vertices[corner][axis] = p[static_cast<Eigen::Index>(axis)] == 0.0
                                             ? 0.0
                                             : p[static_cast<Eigen::Index>(axis)];
        }
    }
    std::sort(key.vertices.begin(), key.vertices.end());
    return key;
}

/// Canonical welded-vertex set for one polygon. RVD pieces that meet across a
/// tet face or a Voronoi bisector must expose the same polygon after the global
/// vertex weld.  Pairing by this exact topological identity avoids the old
/// centroid/area heuristic, which could both miss real interfaces and join
/// unrelated coplanar fragments.
struct VertexFaceKey {
    std::vector<VertexId> vertices;
    bool operator==(const VertexFaceKey& o) const { return vertices == o.vertices; }
};

struct VertexFaceHash {
    std::size_t operator()(const VertexFaceKey& key) const noexcept {
        std::size_t h = key.vertices.size();
        for (const VertexId v : key.vertices) {
            h ^= static_cast<std::size_t>(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

VertexFaceKey canonical_face_key(std::span<const VertexId> loop) {
    VertexFaceKey key;
    key.vertices.assign(loop.begin(), loop.end());
    std::sort(key.vertices.begin(), key.vertices.end());
    key.vertices.erase(std::unique(key.vertices.begin(), key.vertices.end()),
                       key.vertices.end());
    return key;
}

bool loops_have_opposite_winding(std::span<const VertexId> a, std::span<const VertexId> b) {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    const std::size_t n = a.size();
    for (std::size_t start = 0; start < n; ++start) {
        if (b[start] != a[0]) {
            continue;
        }
        bool opposite = true;
        for (std::size_t i = 1; i < n; ++i) {
            if (a[i] != b[(start + n - i) % n]) {
                opposite = false;
                break;
            }
        }
        if (opposite) {
            return true;
        }
    }
    return false;
}

struct RvdEdgeKey {
    VertexId a = 0;
    VertexId b = 0;
    bool operator<(const RvdEdgeKey& o) const { return a != o.a ? a < o.a : b < o.b; }
};

RvdEdgeKey rvd_edge(VertexId a, VertexId b) {
    return a < b ? RvdEdgeKey{a, b} : RvdEdgeKey{b, a};
}

Eigen::Vector3d polygon_area_vector(const PolyMesh& mesh, std::span<const VertexId> loop) {
    Eigen::Vector3d area = Eigen::Vector3d::Zero();
    if (loop.size() < 3) {
        return area;
    }
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const VertexId a = loop[i];
        const VertexId b = loop[(i + 1) % loop.size()];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size()) {
            return Eigen::Vector3d::Zero();
        }
        area += mesh.vertices[a].cross(mesh.vertices[b]);
    }
    return area;
}

bool merge_face_component(PolyMesh& mesh, std::span<const FaceId> component) {
    if (component.size() < 2) {
        return false;
    }
    std::map<RvdEdgeKey, int> edge_counts;
    Eigen::Vector3d reference_area = Eigen::Vector3d::Zero();
    for (const FaceId fid : component) {
        if (fid >= mesh.faces.size() || mesh.faces[fid].vertices.size() < 3) {
            return false;
        }
        const auto& loop = mesh.faces[fid].vertices;
        reference_area += polygon_area_vector(mesh, loop);
        for (std::size_t i = 0; i < loop.size(); ++i) {
            ++edge_counts[rvd_edge(loop[i], loop[(i + 1) % loop.size()])];
        }
    }

    std::map<VertexId, std::vector<VertexId>> boundary;
    std::size_t n_boundary_edges = 0;
    for (const auto& [edge, count] : edge_counts) {
        if (count == 2) {
            continue;
        }
        if (count != 1) {
            return false;
        }
        boundary[edge.a].push_back(edge.b);
        boundary[edge.b].push_back(edge.a);
        ++n_boundary_edges;
    }
    if (boundary.size() < 3 || n_boundary_edges != boundary.size()) {
        return false;
    }
    for (const auto& [vertex, neighbours] : boundary) {
        (void)vertex;
        if (neighbours.size() != 2) {
            return false;
        }
    }

    const VertexId start = boundary.begin()->first;
    std::vector<VertexId> merged;
    merged.reserve(boundary.size());
    VertexId previous = std::numeric_limits<VertexId>::max();
    VertexId current = start;
    for (std::size_t step = 0; step < boundary.size(); ++step) {
        merged.push_back(current);
        const auto& neighbours = boundary[current];
        VertexId next = neighbours[0];
        if (next == previous) {
            next = neighbours[1];
        } else if (previous == std::numeric_limits<VertexId>::max()) {
            next = std::min(neighbours[0], neighbours[1]);
        }
        previous = current;
        current = next;
        if (current == start && step + 1 != boundary.size()) {
            return false;
        }
    }
    if (current != start || merged.size() != boundary.size()) {
        return false;
    }
    if (polygon_area_vector(mesh, merged).dot(reference_area) < 0.0) {
        std::reverse(merged.begin(), merged.end());
    }

    const FaceId keep = component.front();
    const CellId owner = mesh.faces[keep].owner;
    const std::optional<CellId> neighbour = mesh.faces[keep].neighbour;
    mesh.faces[keep].vertices = std::move(merged);
    for (std::size_t i = 1; i < component.size(); ++i) {
        const FaceId remove = component[i];
        if (remove >= mesh.faces.size()) {
            return false;
        }
        auto erase_face = [&](CellId cell_id) {
            if (cell_id >= mesh.cells.size()) {
                return;
            }
            auto& ids = mesh.cells[cell_id].faces;
            ids.erase(std::remove(ids.begin(), ids.end(), remove), ids.end());
        };
        erase_face(owner);
        if (neighbour) {
            erase_face(*neighbour);
        }
        mesh.faces[remove].vertices.clear();
    }
    return true;
}

void coalesce_rvd_interior_faces(PolyMesh& mesh, ClippedVoronoiExportStats& stats) {
    std::map<std::pair<CellId, CellId>, std::vector<FaceId>> groups;
    for (std::size_t fi = 0; fi < mesh.faces.size(); ++fi) {
        const Face& face = mesh.faces[fi];
        if (!face.neighbour || face.vertices.size() < 3) {
            continue;
        }
        const CellId a = std::min(face.owner, *face.neighbour);
        const CellId b = std::max(face.owner, *face.neighbour);
        groups[{a, b}].push_back(static_cast<FaceId>(fi));
    }

    for (const auto& [cell_pair, faces] : groups) {
        (void)cell_pair;
        if (faces.size() < 2) {
            continue;
        }
        std::vector<std::size_t> parent(faces.size());
        for (std::size_t i = 0; i < parent.size(); ++i) {
            parent[i] = i;
        }
        const auto root = [&](std::size_t i) {
            while (parent[i] != i) {
                i = parent[i];
            }
            return i;
        };
        auto unite = [&](std::size_t a, std::size_t b) {
            a = root(a);
            b = root(b);
            if (a != b) {
                parent[b] = a;
            }
        };
        std::map<RvdEdgeKey, std::size_t> first_face;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const auto& loop = mesh.faces[faces[i]].vertices;
            for (std::size_t e = 0; e < loop.size(); ++e) {
                const RvdEdgeKey edge = rvd_edge(loop[e], loop[(e + 1) % loop.size()]);
                if (const auto it = first_face.find(edge); it != first_face.end()) {
                    unite(i, it->second);
                } else {
                    first_face.emplace(edge, i);
                }
            }
        }
        std::map<std::size_t, std::vector<FaceId>> components;
        for (std::size_t i = 0; i < faces.size(); ++i) {
            components[root(i)].push_back(faces[i]);
        }
        for (const auto& [component_id, component] : components) {
            (void)component_id;
            if (merge_face_component(mesh, component)) {
                ++stats.n_coalesced_faces;
                stats.n_coalesced_face_fragments += component.size() - 1;
            }
        }
    }
}

#endif // GEOGRAM

} // namespace

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
    // Geogram's clipping predicates and the weld grid operate in a stable
    // domain-local frame. This avoids catastrophic cancellation and integer
    // bucket overflow for small parts translated far from the world origin.
    const Eigen::Vector3d coordinate_origin = domain.min;
    ClipBox local_domain;
    local_domain.min = Eigen::Vector3d::Zero();
    local_domain.max = domain.max - coordinate_origin;
    std::vector<Eigen::Vector3d> local_sites;
    local_sites.reserve(sites.size());
    for (const Eigen::Vector3d& site : sites) {
        local_sites.push_back(site - coordinate_origin);
    }
    std::vector<DomainTet> local_tets;
    local_tets.reserve(tets.size());
    for (DomainTet tet : tets) {
        if (tet.centroid.squaredNorm() < 1e-30 && (tet.v0 - tet.v1).squaredNorm() > 0.0) {
            tet.centroid = 0.25 * (tet.v0 + tet.v1 + tet.v2 + tet.v3);
        }
        tet.v0 -= coordinate_origin;
        tet.v1 -= coordinate_origin;
        tet.v2 -= coordinate_origin;
        tet.v3 -= coordinate_origin;
        tet.centroid -= coordinate_origin;
        local_tets.push_back(tet);
    }
    sites = local_sites;
    tets = local_tets;

    const double diag = std::max((local_domain.max - local_domain.min).norm(), 1e-30);
    (void)tet_search_radius; // superseded by the per-site security-radius bound
    // Classify the tetrahedral scaffold from exact shared input coordinates.
    // The local face order is fixed by kTetLocalFaces: one occurrence is the
    // domain skin, two form an internal cut, and any larger multiplicity is a
    // non-manifold scaffold that is rejected before clipping.
    std::map<ScaffoldFaceKey, std::size_t> scaffold_multiplicity;
    for (const DomainTet& tet : tets) {
        for (std::size_t local_face = 0; local_face < 4; ++local_face) {
            ++scaffold_multiplicity[scaffold_face_key(tet, local_face)];
        }
    }
    for (const auto& [face, multiplicity] : scaffold_multiplicity) {
        (void)face;
        if (multiplicity > 2) {
            ++out.stats.n_unpaired_scaffold_faces;
            ++out.stats.n_invalid_face_claims;
        }
    }

    // Precompute tet planes once oriented to tet centroid; `reach` = max vertex
    // distance from the centroid (tet bounding radius) for the near-cell test.
    struct TetReady {
        DomainTet tet;
        ClipPlane planes[4];
        std::array<ScaffoldFaceProvenance, 4> face_provenance{};
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
        for (std::size_t local_face = 0; local_face < 4; ++local_face) {
            const std::size_t multiplicity =
                scaffold_multiplicity.at(scaffold_face_key(tr.tet, local_face));
            if (multiplicity == 1) {
                tr.face_provenance[local_face] = ScaffoldFaceProvenance::kDomainBoundary;
            } else if (multiplicity == 2) {
                tr.face_provenance[local_face] = ScaffoldFaceProvenance::kInternal;
            } else {
                tr.face_provenance[local_face] = ScaffoldFaceProvenance::kNonManifold;
                tr.ok = false;
            }
        }
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
    const Eigen::Vector3d ext = (local_domain.max - local_domain.min).cwiseMax(1e-30);
    const double box_vol = ext.x() * ext.y() * ext.z();
    double g_edge =
        std::cbrt(box_vol / static_cast<double>(std::max<std::size_t>(1, sites.size())));
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
#pragma omp for schedule(dynamic, 32)
        for (std::ptrdiff_t ii = 0; ii < n; ++ii) {
            const auto i = static_cast<std::size_t>(ii);
            double Ri = 0.0;
            if (!voronoi_neighbours(ncell, local_domain, sites, i, grid, nbr, ring_buf, Ri)) {
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
                if (!build_cell_tet_nbr(cell, local_domain, sites[i], sites, nbr, i, tr.planes,
                                        n_clips)) {
                    continue;
                }
                loc_clips += n_clips;
                RawCell raw = extract_raw_cell(cell, true);
                if (raw.empty || raw.faces.size() < 4) {
                    continue;
                }
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
        }
    }

    // Deterministic emit order (parallel scheduling is nondeterministic).
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        return a.site != b.site ? a.site < b.site : a.tet < b.tet;
    });
    // Floating-point reductions are order-sensitive. Sum in the same sorted
    // (site,tet) order used for emission so 1-thread and N-thread exports have
    // bit-identical aggregate statistics.
    for (const Piece& piece : pieces) {
        sum_vol += piece.cell.volume;
    }
    out.stats.n_empty_cells += n_empty;
    out.stats.n_domain_plane_clips += n_clips_total;
    out.stats.sum_cell_volume += sum_vol;

    // Weld vertices, then pair fragments by their exact canonical vertex set.
    const double eps = 1e-9 * diag;
    const double inv_eps = 1.0 / eps;
    std::unordered_map<QuantKey, std::vector<VertexId>, QuantHash> weld;
    const double eps_squared = eps * eps;
    auto weld_point = [&](const Eigen::Vector3d& p) -> VertexId {
        QuantKey bucket;
        if (!quantize_relative(p, local_domain.min, inv_eps, bucket)) {
            // Invalid/non-finite geometry cannot safely participate in a
            // topology weld. Preserve it as a unique vertex and hard-fail
            // admission through the existing invalid-claim gate.
            ++out.stats.n_invalid_face_claims;
            const auto id = static_cast<VertexId>(out.mesh.vertices.size());
            out.mesh.vertices.push_back(p);
            return id;
        }
        VertexId best = std::numeric_limits<VertexId>::max();
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                for (long long dz = -1; dz <= 1; ++dz) {
                    const QuantKey neighbour{bucket.x + dx, bucket.y + dy, bucket.z + dz};
                    const auto it = weld.find(neighbour);
                    if (it == weld.end()) {
                        continue;
                    }
                    for (const VertexId id : it->second) {
                        if (id < best &&
                            (out.mesh.vertices[id] - p).squaredNorm() <= eps_squared) {
                            best = id;
                        }
                    }
                }
            }
        }
        if (best != std::numeric_limits<VertexId>::max()) {
            return best;
        }
        const auto id = static_cast<VertexId>(out.mesh.vertices.size());
        out.mesh.vertices.push_back(p);
        weld[bucket].push_back(id);
        return id;
    };

    constexpr std::size_t kNoSite = static_cast<std::size_t>(-1);
    enum class PreparedFaceProvenance {
        kDomainBoundary,
        kInternalScaffold,
        kInvalidScaffold,
        kBisector,
    };
    struct PreparedFace {
        std::vector<VertexId> loop;
        std::size_t expected_other_site = kNoSite;
        PreparedFaceProvenance provenance = PreparedFaceProvenance::kDomainBoundary;
    };
    struct PreparedPiece {
        std::size_t site = 0;
        std::size_t tet = 0;
        std::vector<PreparedFace> faces;
    };
    std::vector<PreparedPiece> prepared;
    prepared.reserve(pieces.size());
    for (const Piece& piece : pieces) {
        if (piece.cell.empty || piece.cell.faces.size() < 4) {
            continue;
        }
        PreparedPiece pp;
        pp.site = piece.site;
        pp.tet = piece.tet;
        pp.faces.reserve(piece.cell.faces.size());
        for (const RawFace& rf : piece.cell.faces) {
            std::vector<VertexId> loop;
            loop.reserve(rf.loop.size());
            for (const Eigen::Vector3d& point : rf.loop) {
                loop.push_back(weld_point(point));
            }
            std::vector<VertexId> dedup;
            for (const VertexId vertex : loop) {
                if (dedup.empty() || dedup.back() != vertex) {
                    dedup.push_back(vertex);
                }
            }
            if (dedup.size() >= 2 && dedup.front() == dedup.back()) {
                dedup.pop_back();
            }
            if (dedup.size() >= 3) {
                PreparedFaceProvenance provenance = PreparedFaceProvenance::kDomainBoundary;
                std::size_t expected_other_site = kNoSite;
                if (rf.provenance == RawFaceProvenance::kBisector) {
                    provenance = PreparedFaceProvenance::kBisector;
                    expected_other_site = rf.neighbour_site;
                } else if (rf.provenance == RawFaceProvenance::kTetScaffold) {
                    if (piece.tet >= ready.size() || rf.tet_local_face >= 4) {
                        provenance = PreparedFaceProvenance::kInvalidScaffold;
                    } else {
                        switch (ready[piece.tet].face_provenance[rf.tet_local_face]) {
                        case ScaffoldFaceProvenance::kDomainBoundary:
                            provenance = PreparedFaceProvenance::kDomainBoundary;
                            break;
                        case ScaffoldFaceProvenance::kInternal:
                            provenance = PreparedFaceProvenance::kInternalScaffold;
                            break;
                        case ScaffoldFaceProvenance::kNonManifold:
                            provenance = PreparedFaceProvenance::kInvalidScaffold;
                            break;
                        }
                    }
                }
                pp.faces.push_back(
                    PreparedFace{std::move(dedup), expected_other_site, provenance});
            }
        }
        if (pp.faces.size() >= 4) {
            prepared.push_back(std::move(pp));
        }
    }

    // A restricted Voronoi region can have disconnected components in a
    // non-convex solid. Union only pieces joined by an exact same-site tet-cut
    // face; never collapse disjoint shells into one VEM cell.
    std::vector<std::size_t> parent(prepared.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }
    const auto root = [&](std::size_t i) {
        while (parent[i] != i) {
            i = parent[i];
        }
        return i;
    };
    auto unite = [&](std::size_t a, std::size_t b) {
        a = root(a);
        b = root(b);
        if (a != b) {
            parent[b] = a;
        }
    };
    std::map<std::pair<std::size_t, std::vector<VertexId>>, std::size_t> first_same_site_face;
    for (std::size_t i = 0; i < prepared.size(); ++i) {
        for (const PreparedFace& face : prepared[i].faces) {
            if (face.provenance != PreparedFaceProvenance::kInternalScaffold) {
                continue;
            }
            const VertexFaceKey key = canonical_face_key(face.loop);
            const auto map_key = std::make_pair(prepared[i].site, key.vertices);
            if (const auto it = first_same_site_face.find(map_key);
                it != first_same_site_face.end()) {
                unite(i, it->second);
            } else {
                first_same_site_face.emplace(map_key, i);
            }
        }
    }

    std::vector<CellId> piece_cell(prepared.size());
    std::map<std::size_t, CellId> component_cell;
    for (std::size_t i = 0; i < prepared.size(); ++i) {
        const std::size_t component = root(i);
        auto [it, fresh] = component_cell.try_emplace(component);
        if (fresh) {
            const CellId cid = static_cast<CellId>(out.mesh.cells.size());
            it->second = cid;
            out.mesh.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {}});
            ++out.stats.n_cells;
            if (out.site_to_cell[prepared[i].site] == static_cast<std::size_t>(-1)) {
                out.site_to_cell[prepared[i].site] = cid;
            } else {
                ++out.stats.n_split_site_components;
            }
        }
        piece_cell[i] = it->second;
    }

    struct ExactFaceClaim {
        FaceId face = 0;
        std::size_t site = kNoSite;
        std::size_t expected_other_site = kNoSite;
        PreparedFaceProvenance provenance = PreparedFaceProvenance::kDomainBoundary;
        std::size_t count = 0;
        bool has_scaffold_claim = false;
        bool scaffold_cancelled = false;
        bool has_bisector_claim = false;
        bool bisector_paired = false;
    };
    std::unordered_map<VertexFaceKey, ExactFaceClaim, VertexFaceHash> face_claims;

    const auto is_scaffold = [](PreparedFaceProvenance provenance) {
        return provenance == PreparedFaceProvenance::kInternalScaffold ||
               provenance == PreparedFaceProvenance::kInvalidScaffold;
    };

    for (std::size_t piece_index = 0; piece_index < prepared.size(); ++piece_index) {
        const PreparedPiece& piece = prepared[piece_index];
        const CellId cid = piece_cell[piece_index];
        for (const PreparedFace& rf : piece.faces) {
            const VertexFaceKey face_key = canonical_face_key(rf.loop);
            if (auto it = face_claims.find(face_key); it != face_claims.end()) {
                ExactFaceClaim& claim = it->second;
                ++claim.count;
                claim.has_scaffold_claim =
                    claim.has_scaffold_claim || is_scaffold(rf.provenance);
                claim.has_bisector_claim = claim.has_bisector_claim ||
                                           rf.provenance == PreparedFaceProvenance::kBisector;
                if (claim.count != 2) {
                    claim.scaffold_cancelled = false;
                    claim.bisector_paired = false;
                    ++out.stats.n_invalid_face_claims;
                    continue;
                }

                Face& first = out.mesh.faces[claim.face];
                const bool opposite = loops_have_opposite_winding(first.vertices, rf.loop);
                if (claim.provenance == PreparedFaceProvenance::kInternalScaffold &&
                    rf.provenance == PreparedFaceProvenance::kInternalScaffold) {
                    const bool same_site = claim.site == piece.site;
                    if (!same_site || first.owner != cid || !opposite) {
                        ++out.stats.n_invalid_face_claims;
                        continue;
                    }
                    auto& cell_faces = out.mesh.cells[cid].faces;
                    cell_faces.erase(
                        std::remove(cell_faces.begin(), cell_faces.end(), claim.face),
                        cell_faces.end());
                    first.vertices.clear();
                    claim.scaffold_cancelled = true;
                    continue;
                }

                if (claim.provenance == PreparedFaceProvenance::kBisector &&
                    rf.provenance == PreparedFaceProvenance::kBisector) {
                    const bool site_pair_ok = claim.site != piece.site &&
                                              claim.expected_other_site == piece.site &&
                                              rf.expected_other_site == claim.site;
                    if (!site_pair_ok || !opposite) {
                        ++out.stats.n_invalid_face_claims;
                        continue;
                    }
                    first.neighbour = cid;
                    out.mesh.cells[cid].faces.push_back(claim.face);
                    claim.bisector_paired = true;
                    continue;
                }

                // Domain faces are exterior-only, and provenance categories
                // may never pair with one another.
                ++out.stats.n_invalid_face_claims;
                continue;
            }

            Face face;
            face.vertices = rf.loop;
            face.owner = cid;
            face.neighbour = std::nullopt;
            const FaceId fid = static_cast<FaceId>(out.mesh.faces.size());
            out.mesh.faces.push_back(std::move(face));
            out.mesh.cells[cid].faces.push_back(fid);
            face_claims.emplace(
                face_key,
                ExactFaceClaim{fid, piece.site, rf.expected_other_site, rf.provenance, 1,
                               is_scaffold(rf.provenance), false,
                               rf.provenance == PreparedFaceProvenance::kBisector, false});
        }
    }

    for (const auto& [key, claim] : face_claims) {
        (void)key;
        if (claim.has_scaffold_claim && !claim.scaffold_cancelled) {
            ++out.stats.n_unpaired_scaffold_faces;
            // Never expose an internal scaffold cut as domain skin.
            if (claim.face < out.mesh.faces.size()) {
                out.mesh.faces[claim.face].vertices.clear();
                out.mesh.faces[claim.face].neighbour.reset();
            }
        }
        if (claim.has_bisector_claim && !claim.bisector_paired) {
            ++out.stats.n_unpaired_bisector_faces;
        }
    }

    coalesce_rvd_interior_faces(out.mesh, out.stats);

    // Cancellation leaves tombstone faces and intersection-only vertices.
    // Compact both tables before exposing the mesh: downstream PolyMesh
    // validation rejects empty faces, and VEM assigns a displacement DOF to
    // every exported vertex.
    {
        constexpr FaceId kInvalidFace = std::numeric_limits<FaceId>::max();
        std::vector<FaceId> face_remap(out.mesh.faces.size(), kInvalidFace);
        std::vector<Face> compact_faces;
        compact_faces.reserve(out.mesh.faces.size());
        for (std::size_t fi = 0; fi < out.mesh.faces.size(); ++fi) {
            if (out.mesh.faces[fi].vertices.size() < 3) {
                continue;
            }
            face_remap[fi] = static_cast<FaceId>(compact_faces.size());
            compact_faces.push_back(std::move(out.mesh.faces[fi]));
        }
        for (Cell& cell : out.mesh.cells) {
            std::vector<FaceId> compact_ids;
            compact_ids.reserve(cell.faces.size());
            for (const FaceId old_id : cell.faces) {
                if (old_id >= face_remap.size() || face_remap[old_id] == kInvalidFace) {
                    continue;
                }
                const FaceId new_id = face_remap[old_id];
                if (std::find(compact_ids.begin(), compact_ids.end(), new_id) ==
                    compact_ids.end()) {
                    compact_ids.push_back(new_id);
                }
            }
            cell.faces = std::move(compact_ids);
        }
        out.mesh.faces = std::move(compact_faces);

        constexpr VertexId kInvalidVertex = std::numeric_limits<VertexId>::max();
        std::vector<VertexId> vertex_remap(out.mesh.vertices.size(), kInvalidVertex);
        std::vector<Eigen::Vector3d> compact_vertices;
        compact_vertices.reserve(out.mesh.vertices.size());
        for (Face& face : out.mesh.faces) {
            for (VertexId& old_id : face.vertices) {
                if (old_id >= vertex_remap.size()) {
                    continue;
                }
                if (vertex_remap[old_id] == kInvalidVertex) {
                    vertex_remap[old_id] = static_cast<VertexId>(compact_vertices.size());
                    compact_vertices.push_back(out.mesh.vertices[old_id]);
                }
                old_id = vertex_remap[old_id];
            }
        }
        out.mesh.vertices = std::move(compact_vertices);
    }
    for (Eigen::Vector3d& vertex : out.mesh.vertices) {
        vertex += coordinate_origin;
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

} // namespace polymesh::mesh
