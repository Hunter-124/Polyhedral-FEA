// SPDX-License-Identifier: BSD-3-Clause

// Restricted-Voronoi ∩ tet cell construction for export_rvd_tet_clipped, and
// the raw ConvexCell face extraction shared with export_clipped_voronoi.

#include "cvt_rvd.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

namespace polymesh::mesh::detail {
namespace {

static constexpr int kTetLocalFaces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};

constexpr VBW::global_index_t tet_face_tag(std::size_t local_face) {
    return static_cast<VBW::global_index_t>(-2 - static_cast<int>(local_face));
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

} // namespace

RawCell extract_raw_cell(VBW::ConvexCell& cell, bool tet_scaffold_tags) {
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

RvdTetCells build_rvd_tet_cells(const ClipBox& local_domain,
                                std::span<const Eigen::Vector3d> sites,
                                std::span<const DomainTet> tets, double diag,
                                ClippedVoronoiExportStats& stats) {
    RvdTetCells result;
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
            ++stats.n_unpaired_scaffold_faces;
            ++stats.n_invalid_face_claims;
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
    result.tet_face_provenance.reserve(tets.size());
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
        result.tet_face_provenance.push_back(tr.face_provenance);
        ready.push_back(tr);
    }

    // Pieces: (site, tet, raw_cell). `tet` gives a deterministic sort key so the
    // parallel emit order matches a serial run.
    std::vector<RvdPiece> pieces;
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
        std::vector<RvdPiece> loc_pieces;
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
                loc_pieces.push_back(RvdPiece{i, ti, std::move(raw)});
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
    std::sort(pieces.begin(), pieces.end(), [](const RvdPiece& a, const RvdPiece& b) {
        return a.site != b.site ? a.site < b.site : a.tet < b.tet;
    });
    // Floating-point reductions are order-sensitive. Sum in the same sorted
    // (site,tet) order used for emission so 1-thread and N-thread exports have
    // bit-identical aggregate statistics.
    for (const RvdPiece& piece : pieces) {
        sum_vol += piece.cell.volume;
    }
    stats.n_empty_cells += n_empty;
    stats.n_domain_plane_clips += n_clips_total;
    stats.sum_cell_volume += sum_vol;
    result.pieces = std::move(pieces);
    return result;
}

} // namespace polymesh::mesh::detail

#endif // POLYMESH_WITH_GEOGRAM
