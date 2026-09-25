// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_export.hpp"

#include "cvt_geometry.hpp"
#include "cvt_rvd.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace {

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
using detail::bisector_keep_site;
using detail::extract_raw_cell;
using detail::QuantHash;
using detail::QuantKey;
using detail::RawCell;
using detail::RawFace;

QuantKey quantize(const Eigen::Vector3d& p, double inv_eps) {
    QuantKey k;
    k.x = llround(p.x() * inv_eps);
    k.y = llround(p.y() * inv_eps);
    k.z = llround(p.z() * inv_eps);
    return k;
}

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

    // Global solid halfspaces (same planes for every site) so adjacent cells
    // share a consistent domain; domain faces carry vglobal = -2. Sites on the
    // wrong side of any plane are dropped (not selectively clipped) so every
    // surviving cell uses the same plane set.
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

// Canonical undirected pair for site adjacency.
std::pair<std::size_t, std::size_t> site_pair(std::size_t a, std::size_t b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

/// PolyMesh admission for export_rvd_tet_clipped: compact the claimed mesh,
/// return it to the world frame and fill the final counts.
void admit_rvd_mesh(ClippedVoronoiExport& out, const Eigen::Vector3d& coordinate_origin) {
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
}

#endif // GEOGRAM

} // namespace

ClippedVoronoiExport export_clipped_voronoi(const ClipBox& domain,
                                            std::span<const Eigen::Vector3d> sites,
                                            const DomainClipParams& domain_clip) {
    ClippedVoronoiExport out;
    out.stats.n_sites = sites.size();
    out.site_to_cell.assign(sites.size(), static_cast<std::size_t>(-1));

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    (void)domain;
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
        // sites lie on the keep side, i.e. the convex envelope of the site
        // cloud. Non-supporting planes (hole walls) are skipped: a full
        // halfspace of a holed solid would cut material on the opposite side.
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

ClippedVoronoiExport export_rvd_tet_clipped(const ClipBox& domain,
                                            std::span<const Eigen::Vector3d> sites,
                                            std::span<const DomainTet> tets,
                                            double tet_search_radius) {
    ClippedVoronoiExport out;
    out.stats.n_sites = sites.size();
    // site_to_cell: first piece for that site (or npos).
    out.site_to_cell.assign(sites.size(), static_cast<std::size_t>(-1));

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    (void)domain;
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

    const detail::RvdTetCells cells =
        detail::build_rvd_tet_cells(local_domain, sites, tets, diag, out.stats);
    detail::claim_rvd_faces(cells, local_domain.min, diag, out);
    admit_rvd_mesh(out, coordinate_origin);
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
