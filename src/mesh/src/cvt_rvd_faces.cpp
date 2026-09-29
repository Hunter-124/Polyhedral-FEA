// SPDX-License-Identifier: BSD-3-Clause

// Face welding, exact face-claim validation and coplanar fragment coalescing
// for export_rvd_tet_clipped.

#include "cvt_rvd.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

namespace polymesh::mesh::detail {
namespace {

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

/// Canonical welded-vertex set for one polygon. RVD pieces that meet across a
/// tet face or a Voronoi bisector must expose the same polygon after the global
/// vertex weld, so fragments pair by this exact topological identity (never by
/// a centroid/area heuristic, which can miss interfaces or join coplanar ones).
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

} // namespace

void claim_rvd_faces(const RvdTetCells& cells, const Eigen::Vector3d& weld_origin, double diag,
                     ClippedVoronoiExport& out) {
    // Weld vertices, then pair fragments by their exact canonical vertex set.
    const double eps = 1e-9 * diag;
    const double inv_eps = 1.0 / eps;
    std::unordered_map<QuantKey, std::vector<VertexId>, QuantHash> weld;
    const double eps_squared = eps * eps;
    auto weld_point = [&](const Eigen::Vector3d& p) -> VertexId {
        QuantKey bucket;
        if (!quantize_relative(p, weld_origin, inv_eps, bucket)) {
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
    prepared.reserve(cells.pieces.size());
    for (const RvdPiece& piece : cells.pieces) {
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
                    if (piece.tet >= cells.tet_face_provenance.size() ||
                        rf.tet_local_face >= 4) {
                        provenance = PreparedFaceProvenance::kInvalidScaffold;
                    } else {
                        switch (cells.tet_face_provenance[piece.tet][rf.tet_local_face]) {
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
}

} // namespace polymesh::mesh::detail

#endif // POLYMESH_WITH_GEOGRAM
