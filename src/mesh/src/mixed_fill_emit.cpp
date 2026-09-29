// SPDX-License-Identifier: BSD-3-Clause
// mixed_fill_surface cell emission: bulk hex, h/2 children, 2:1 transition
// fans or native polyhedra, plain-mode skin fans, and boundary quads.
#include "mixed_fill_internal.hpp"

#include "mesh/cell_validity.hpp"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace polymesh::mesh::detail::mixed {
namespace {

// Local (di,dj,dk) of the 8 hex corners in unit cell {0,1}^3.
constexpr std::array<std::array<int, 3>, 8> kHexCornerLocal{{
    {{0, 0, 0}},
    {{1, 0, 0}},
    {{1, 1, 0}},
    {{0, 1, 0}},
    {{0, 0, 1}},
    {{1, 0, 1}},
    {{1, 1, 1}},
    {{0, 1, 1}},
}};

void emit_pyramid(MixedFillOutput& out, std::uint32_t n0, std::uint32_t n1, std::uint32_t n2,
                  std::uint32_t n3, std::uint32_t apex) {
    MixedCell pyr;
    pyr.kind = MixedCellKind::kPyramid5;
    pyr.n_nodes = 5;
    pyr.nodes[0] = n0;
    pyr.nodes[1] = n1;
    pyr.nodes[2] = n2;
    pyr.nodes[3] = n3;
    pyr.nodes[4] = apex;
    orient_pyramid_winding(pyr, out.nodes);
    normalize_pyramid_diagonal(pyr, out.nodes);
    out.cells.push_back(pyr);
    ++out.n_pyramid;
}

void emit_hex(MixedFillOutput& out, const std::array<std::uint32_t, 8>& c) {
    MixedCell hx;
    hx.kind = MixedCellKind::kHex8;
    hx.n_nodes = 8;
    hx.nodes = c;
    out.cells.push_back(hx);
    ++out.n_hex;
}

void emit_tet(MixedFillOutput& out, std::uint32_t a, std::uint32_t b, std::uint32_t c,
              std::uint32_t d) {
    MixedCell t;
    t.kind = MixedCellKind::kTet4;
    t.n_nodes = 4;
    if (validity::tet_signed_volume(out.nodes[a], out.nodes[b], out.nodes[c], out.nodes[d]) <
        0.0) {
        std::swap(b, c);
    }
    t.nodes[0] = a;
    t.nodes[1] = b;
    t.nodes[2] = c;
    t.nodes[3] = d;
    out.cells.push_back(t);
    ++out.n_tet;
}

/// Emit 4 child quads for a hex face (local corner indices 0..7) using mid-edge
/// + face-center nodes already present in the fine index map via `fn`.
template <typename FineNodeFn>
void emit_subdivided_face_pyramids(MixedFillOutput& out, FineNodeFn&& fn, int i, int j, int k,
                                   int face, std::uint32_t apex) {
    // Local unit coords of face corners (0 or 2 in fine steps of a coarse cell).
    const auto& fl = kHexFaces[static_cast<std::size_t>(face)];
    std::array<std::array<int, 3>, 4> lc{};
    for (int q = 0; q < 4; ++q) {
        const auto& corner =
            kHexCornerLocal[static_cast<std::size_t>(fl[static_cast<std::size_t>(q)])];
        lc[static_cast<std::size_t>(q)] = {{2 * corner[0], 2 * corner[1], 2 * corner[2]}};
    }
    // Face center in local fine coords (0..2).
    const int fcx = (lc[0][0] + lc[1][0] + lc[2][0] + lc[3][0]) / 4;
    const int fcy = (lc[0][1] + lc[1][1] + lc[2][1] + lc[3][1]) / 4;
    const int fcz = (lc[0][2] + lc[1][2] + lc[2][2] + lc[3][2]) / 4;
    const auto fc = fn(2 * i + fcx, 2 * j + fcy, 2 * k + fcz);
    for (int q = 0; q < 4; ++q) {
        const int qn = (q + 1) % 4;
        const auto& a = lc[static_cast<std::size_t>(q)];
        const auto& b = lc[static_cast<std::size_t>(qn)];
        const int mx = (a[0] + b[0]) / 2;
        const int my = (a[1] + b[1]) / 2;
        const int mz = (a[2] + b[2]) / 2;
        const auto na = fn(2 * i + a[0], 2 * j + a[1], 2 * k + a[2]);
        const auto nm = fn(2 * i + mx, 2 * j + my, 2 * k + mz);
        const auto nb = fn(2 * i + b[0], 2 * j + b[1], 2 * k + b[2]);
        // Child quad: corner, mid of edge q, face centre, mid of edge q-1.
        const int qp = (q + 3) % 4;
        const auto& p = lc[static_cast<std::size_t>(qp)];
        const int pmx = (a[0] + p[0]) / 2;
        const int pmy = (a[1] + p[1]) / 2;
        const int pmz = (a[2] + p[2]) / 2;
        const auto npm = fn(2 * i + pmx, 2 * j + pmy, 2 * k + pmz);
        (void)nb; // unused, but its fn() call fixes node-id order
        emit_pyramid(out, na, nm, fc, npm, apex);
    }
}

/// Build one unsplit polyhedron for a 2:1 transition coarse cell (ADR-0019).
/// Faces match neighbors: single quad vs bulk, 4 child quads vs fine, n-gon with
/// hanging mids on mixed edges. No centroid apex / fan slivers.
template <typename FineNodeFn>
void emit_transition_poly(MixedFillOutput& out, FineNodeFn&& fn, const MixedLattice& lat,
                          int i, int j, int k) {
    struct Builder {
        std::vector<std::uint32_t> nodes;
        std::map<std::uint32_t, std::uint32_t> local_of;
        std::vector<std::vector<std::uint32_t>> faces;

        std::uint32_t add(std::uint32_t g) {
            const auto it = local_of.find(g);
            if (it != local_of.end()) {
                return it->second;
            }
            const auto L = static_cast<std::uint32_t>(nodes.size());
            nodes.push_back(g);
            local_of.emplace(g, L);
            return L;
        }

        void add_face_global(const std::vector<std::uint32_t>& gids) {
            std::vector<std::uint32_t> face;
            face.reserve(gids.size());
            for (const auto g : gids) {
                face.push_back(add(g));
            }
            faces.push_back(std::move(face));
        }
    } b;

    for (std::size_t f = 0; f < 6; ++f) {
        const auto& o = kFaceNbr[f];
        const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
        const bool free_face = !lat.inb(ni, nj, nk);
        const bool adj_fine = !free_face && lat.is_fine[lat.idx(ni, nj, nk)] != 0;

        const auto& fl = kHexFaces[f];
        std::array<std::array<int, 3>, 4> fcoord{};
        for (int q = 0; q < 4; ++q) {
            const auto& corner =
                kHexCornerLocal[static_cast<std::size_t>(fl[static_cast<std::size_t>(q)])];
            fcoord[static_cast<std::size_t>(q)] = {
                {2 * (i + corner[0]), 2 * (j + corner[1]), 2 * (k + corner[2])}};
        }

        if (adj_fine) {
            // 4 child quads sharing mid-edge + face-center with fine sub-hexes.
            const int fcx = (fcoord[0][0] + fcoord[1][0] + fcoord[2][0] + fcoord[3][0]) / 4;
            const int fcy = (fcoord[0][1] + fcoord[1][1] + fcoord[2][1] + fcoord[3][1]) / 4;
            const int fcz = (fcoord[0][2] + fcoord[1][2] + fcoord[2][2] + fcoord[3][2]) / 4;
            const auto fc = fn(fcx, fcy, fcz);
            for (int q = 0; q < 4; ++q) {
                const auto& A = fcoord[static_cast<std::size_t>(q)];
                const auto& B = fcoord[static_cast<std::size_t>((q + 1) % 4)];
                const auto& P = fcoord[static_cast<std::size_t>((q + 3) % 4)];
                const int mx = (A[0] + B[0]) / 2, my = (A[1] + B[1]) / 2,
                          mz = (A[2] + B[2]) / 2;
                const int pmx = (A[0] + P[0]) / 2, pmy = (A[1] + P[1]) / 2,
                          pmz = (A[2] + P[2]) / 2;
                const auto na = fn(A[0], A[1], A[2]);
                const auto nm = fn(mx, my, mz);
                const auto npm = fn(pmx, pmy, pmz);
                b.add_face_global({na, nm, fc, npm});
            }
            continue;
        }

        // Coarse / free / transition-neighbor face: corners + hanging mids.
        std::vector<std::uint32_t> poly;
        poly.reserve(8);
        for (int q = 0; q < 4; ++q) {
            const auto& A = fcoord[static_cast<std::size_t>(q)];
            const auto& B = fcoord[static_cast<std::size_t>((q + 1) % 4)];
            poly.push_back(fn(A[0], A[1], A[2]));
            std::size_t axis = 0;
            for (std::size_t d = 0; d < 3; ++d) {
                if (A[d] != B[d]) {
                    axis = d;
                }
            }
            int ea = A[0] / 2, eb = A[1] / 2, ec = A[2] / 2;
            const int sa = std::min(A[axis], B[axis]) / 2;
            if (axis == 0) {
                ea = sa;
            } else if (axis == 1) {
                eb = sa;
            } else {
                ec = sa;
            }
            if (lat.edge_split(ea, eb, ec, static_cast<int>(axis))) {
                poly.push_back(fn((A[0] + B[0]) / 2, (A[1] + B[1]) / 2, (A[2] + B[2]) / 2));
            }
        }
        if (free_face) {
            if (poly.size() == 4) {
                out.boundary_quads.push_back({{poly[0], poly[1], poly[2], poly[3]}});
            } else {
                // Fan tris for boundary bookkeeping (tri encoded as q2==q3).
                const std::uint32_t a0 = poly[0];
                for (std::size_t t = 1; t + 1 < poly.size(); ++t) {
                    out.boundary_quads.push_back({{a0, poly[t], poly[t + 1], poly[t + 1]}});
                }
            }
        }
        b.add_face_global(poly);
    }

    // Orient faces outward: Newell normal must point away from cell centroid.
    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
    for (const auto g : b.nodes) {
        ctr += out.nodes[g];
    }
    ctr /= static_cast<double>(b.nodes.size());
    for (auto& face : b.faces) {
        if (face.size() < 3) {
            continue;
        }
        Eigen::Vector3d n = Eigen::Vector3d::Zero();
        Eigen::Vector3d fcent = Eigen::Vector3d::Zero();
        for (std::size_t t = 0; t < face.size(); ++t) {
            const auto& a = out.nodes[b.nodes[face[t]]];
            const auto& bb = out.nodes[b.nodes[face[(t + 1) % face.size()]]];
            n[0] += (a[1] - bb[1]) * (a[2] + bb[2]);
            n[1] += (a[2] - bb[2]) * (a[0] + bb[0]);
            n[2] += (a[0] - bb[0]) * (a[1] + bb[1]);
            fcent += a;
        }
        fcent /= static_cast<double>(face.size());
        n *= 0.5;
        if (n.dot(fcent - ctr) < 0.0) {
            std::reverse(face.begin(), face.end());
        }
    }

    MixedCell cell;
    cell.kind = MixedCellKind::kPolyVem;
    cell.n_nodes = 0;
    cell.poly_nodes = std::move(b.nodes);
    cell.poly_faces = std::move(b.faces);
    if (cell.poly_nodes.size() < 4 || cell.poly_faces.size() < 4) {
        return;
    }
    std::vector<Eigen::Vector3d> coords;
    coords.reserve(cell.poly_nodes.size());
    for (const auto g : cell.poly_nodes) {
        coords.push_back(out.nodes[g]);
    }
    if (closed_poly_volume(coords, cell.poly_faces) <= 0.0) {
        return;
    }
    out.cells.push_back(std::move(cell));
    ++out.n_poly;
}

} // namespace

/// Choose the better cyclic winding once, when a pyramid is emitted. A
/// first-triangle normal is insufficient for a warped quad, so compare the
/// minimum signed half-volume of the conformity-selected split in both
/// directions. Post-snap normalization must NOT repeat this operation: changing
/// winding after deformation would hide an inversion rather than repair it.
void orient_pyramid_winding(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes) {
    const auto split_min = [&](std::uint32_t n0, std::uint32_t n1, std::uint32_t n2,
                               std::uint32_t n3) {
        return validity::pyramid_min_split_volume(nodes[n0], nodes[n1], nodes[n2], nodes[n3],
                                                  nodes[pyr.nodes[4]]);
    };
    const double forward = split_min(pyr.nodes[0], pyr.nodes[1], pyr.nodes[2], pyr.nodes[3]);
    const double reversed = split_min(pyr.nodes[0], pyr.nodes[3], pyr.nodes[2], pyr.nodes[1]);
    if (reversed > forward) {
        std::swap(pyr.nodes[1], pyr.nodes[3]);
    }
}

/// Rotate a cyclic pyramid base so the conformity-safe selected geometric
/// diagonal is local 0-2 for VTK/PyVista, validity, and FE assembly.
void normalize_pyramid_diagonal(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes) {
    if (pyr.kind != MixedCellKind::kPyramid5 || pyr.n_nodes != 5 ||
        validity::pyramid_split_diagonal(nodes[pyr.nodes[0]], nodes[pyr.nodes[1]],
                                         nodes[pyr.nodes[2]], nodes[pyr.nodes[3]]) == 0) {
        return;
    }
    const auto n0 = pyr.nodes[0];
    pyr.nodes[0] = pyr.nodes[1];
    pyr.nodes[1] = pyr.nodes[2];
    pyr.nodes[2] = pyr.nodes[3];
    pyr.nodes[3] = n0;
}

void emit_cell_pyramids(MixedFillOutput& out, const std::array<std::uint32_t, 8>& c,
                        std::uint32_t apex) {
    for (const auto& face : kHexFaces) {
        emit_pyramid(
            out, c[static_cast<std::size_t>(face[0])], c[static_cast<std::size_t>(face[1])],
            c[static_cast<std::size_t>(face[2])], c[static_cast<std::size_t>(face[3])], apex);
    }
}

/// Closed polyhedron volume via face fans (Newell / divergence). Face loops are
/// local indices into `coords`. Positive when faces are outward-oriented.
double closed_poly_volume(const std::vector<Eigen::Vector3d>& coords,
                          const std::vector<std::vector<std::uint32_t>>& faces) {
    double vol = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        const Eigen::Vector3d& o = coords[face[0]];
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            const Eigen::Vector3d& a = coords[face[i]];
            const Eigen::Vector3d& b = coords[face[i + 1]];
            vol += o.dot(a.cross(b));
        }
    }
    return vol / 6.0;
}

std::vector<FanSpan> emit_mixed_cells(const MixedLattice& lat, MixedFillOutput& out,
                                      bool native_poly_transitions,
                                      const std::function<void()>& cancel_check) {
    const auto& classification = lat.classification;
    const CartesianGrid& grid = classification.grid;
    const auto& inside = classification.inside;
    const int nx = lat.nx, ny = lat.ny, nz = lat.nz;
    const bool size_adaptive = lat.size_adaptive;
    const auto& is_fine = lat.is_fine;
    const auto& is_transition = lat.is_transition;

    // Fine-index node map: I∈[0,2nx], J∈[0,2ny], K∈[0,2nz].
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_fine = [&](int I, int J, int K) -> std::uint32_t {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{I, J, K}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.push_back(Eigen::Vector3d{
                grid.origin[0] + 0.5 * static_cast<double>(I) * grid.cell[0],
                grid.origin[1] + 0.5 * static_cast<double>(J) * grid.cell[1],
                grid.origin[2] + 0.5 * static_cast<double>(K) * grid.cell[2],
            });
        }
        return it->second;
    };

    auto coarse_corners = [&](int i, int j, int k) -> std::array<std::uint32_t, 8> {
        return {{
            node_fine(2 * i, 2 * j, 2 * k),
            node_fine(2 * i + 2, 2 * j, 2 * k),
            node_fine(2 * i + 2, 2 * j + 2, 2 * k),
            node_fine(2 * i, 2 * j + 2, 2 * k),
            node_fine(2 * i, 2 * j, 2 * k + 2),
            node_fine(2 * i + 2, 2 * j, 2 * k + 2),
            node_fine(2 * i + 2, 2 * j + 2, 2 * k + 2),
            node_fine(2 * i, 2 * j + 2, 2 * k + 2),
        }};
    };

    auto fine_sub_corners = [&](int i, int j, int k, int a, int b,
                                int c) -> std::array<std::uint32_t, 8> {
        const int I = 2 * i + a, J = 2 * j + b, K = 2 * k + c;
        return {{
            node_fine(I, J, K),
            node_fine(I + 1, J, K),
            node_fine(I + 1, J + 1, K),
            node_fine(I, J + 1, K),
            node_fine(I, J, K + 1),
            node_fine(I + 1, J, K + 1),
            node_fine(I + 1, J + 1, K + 1),
            node_fine(I, J + 1, K + 1),
        }};
    };

    const auto& coarse_inside = classification.coarse_inside.empty()
                                    ? classification.inside
                                    : classification.coarse_inside;
    const auto coarse_was_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               coarse_inside[lat.idx(i, j, k)];
    };
    // -> bool, not deduced: `inside` is a std::vector<bool>, whose element access
    // returns a proxy reference, and libc++ rejects the deduced mismatch.
    const auto fine_child_inside = [&](int I, int J, int K) -> bool {
        if (I < 0 || I >= 2 * nx || J < 0 || J >= 2 * ny || K < 0 || K >= 2 * nz) {
            return false;
        }
        const int i = I / 2, j = J / 2, k = K / 2;
        if (classification.child_inside_mask.empty()) {
            return inside[lat.idx(i, j, k)];
        }
        const int bit = (I % 2) + 2 * (J % 2) + 4 * (K % 2);
        return (classification.child_inside_mask[lat.idx(i, j, k)] &
                static_cast<std::uint8_t>(1U << bit)) != 0;
    };

    std::vector<FanSpan> fan_spans;

    for (int k = 0; k < nz; ++k) {
        poll_cancel(cancel_check);
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto id = lat.idx(i, j, k);
                if (!inside[id]) {
                    continue;
                }

                if (size_adaptive && is_fine[id]) {
                    // Emit every live h/2 child; its free faces come from the same
                    // child mask, so live/void interfaces inside a mixed parent
                    // become boundary quads and enter the snap.
                    ++out.n_fine_cells;
                    const auto child_mask = classification.child_inside_mask.empty()
                                                ? std::uint8_t{0xff}
                                                : classification.child_inside_mask[id];
                    for (int c = 0; c < 2; ++c) {
                        for (int b = 0; b < 2; ++b) {
                            for (int a = 0; a < 2; ++a) {
                                const int child_bit = a + 2 * b + 4 * c;
                                if ((child_mask &
                                     static_cast<std::uint8_t>(1U << child_bit)) == 0) {
                                    continue;
                                }
                                const auto child = fine_sub_corners(i, j, k, a, b, c);
                                emit_hex(out, child);
                                const int I = 2 * i + a;
                                const int J = 2 * j + b;
                                const int K = 2 * k + c;
                                for (std::size_t f = 0; f < kFaceNbr.size(); ++f) {
                                    const auto& o = kFaceNbr[f];
                                    if (fine_child_inside(I + o[0], J + o[1], K + o[2])) {
                                        continue;
                                    }
                                    const auto& face = kHexFaces[f];
                                    const std::array<std::uint32_t, 4> quad{{
                                        child[static_cast<std::size_t>(face[0])],
                                        child[static_cast<std::size_t>(face[1])],
                                        child[static_cast<std::size_t>(face[2])],
                                        child[static_cast<std::size_t>(face[3])],
                                    }};

                                    int pi = i, pj = j, pk = k;
                                    if (I + o[0] < 2 * i) {
                                        --pi;
                                    } else if (I + o[0] >= 2 * i + 2) {
                                        ++pi;
                                    }
                                    if (J + o[1] < 2 * j) {
                                        --pj;
                                    } else if (J + o[1] >= 2 * j + 2) {
                                        ++pj;
                                    }
                                    if (K + o[2] < 2 * k) {
                                        --pk;
                                    } else if (K + o[2] >= 2 * k + 2) {
                                        ++pk;
                                    }
                                    const bool existed_on_coarse_grid =
                                        coarse_was_inside(i, j, k) !=
                                        coarse_was_inside(pi, pj, pk);
                                    out.boundary_quads.push_back(quad);
                                    if (!existed_on_coarse_grid) {
                                        out.local_child_boundary_quads.push_back(quad);
                                    }
                                }
                            }
                        }
                    }
                    continue;
                }

                if (size_adaptive && is_transition[id]) {
                    ++out.n_transition_cells;
                    if (native_poly_transitions) {
                        // ADR-0019: one unsplit polyhedron per transition cell → VEM.
                        emit_transition_poly(out, node_fine, lat, i, j, k);
                        continue;
                    }
                    // Conforming 2:1 closure: apex fan over each face polygon.
                    // Face polygon = 4 corners + the hanging mid of every split
                    // edge. Both cells sharing a face build the same polygon and
                    // the same canonical fan, so every facet pairs — no cracks.
                    const auto c = coarse_corners(i, j, k);
                    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
                    for (int t = 0; t < 8; ++t) {
                        ctr += out.nodes[c[static_cast<std::size_t>(t)]];
                    }
                    ctr /= 8.0;
                    const auto apex = static_cast<std::uint32_t>(out.nodes.size());
                    out.nodes.push_back(ctr);
                    const std::size_t fan_first = out.cells.size();

                    for (std::size_t f = 0; f < 6; ++f) {
                        const auto& o = kFaceNbr[f];
                        const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                        if (lat.inb(ni, nj, nk) && is_fine[lat.idx(ni, nj, nk)]) {
                            // Fine face-neighbor: 4 quarter-quad pyramids (mid +
                            // face-center nodes shared with the fine sub-hexes).
                            emit_subdivided_face_pyramids(out, node_fine, i, j, k,
                                                          static_cast<int>(f), apex);
                            continue;
                        }
                        const bool free_face = !lat.inb(ni, nj, nk);
                        const auto& fl = kHexFaces[f];
                        std::array<std::array<int, 3>, 4> fcoord{};
                        for (int q = 0; q < 4; ++q) {
                            const auto& corner = kHexCornerLocal[static_cast<std::size_t>(
                                fl[static_cast<std::size_t>(q)])];
                            fcoord[static_cast<std::size_t>(q)] = {{2 * (i + corner[0]),
                                                                    2 * (j + corner[1]),
                                                                    2 * (k + corner[2])}};
                        }
                        std::array<std::uint32_t, 8> poly{};
                        std::array<char, 8> poly_is_mid{};
                        int np = 0;
                        for (int q = 0; q < 4; ++q) {
                            const auto& A = fcoord[static_cast<std::size_t>(q)];
                            const auto& B = fcoord[static_cast<std::size_t>((q + 1) % 4)];
                            poly[static_cast<std::size_t>(np++)] = node_fine(A[0], A[1], A[2]);
                            std::size_t axis = 0;
                            for (std::size_t d = 0; d < 3; ++d) {
                                if (A[d] != B[d]) {
                                    axis = d;
                                }
                            }
                            int ea = A[0] / 2, eb = A[1] / 2, ec = A[2] / 2;
                            const int sa = std::min(A[axis], B[axis]) / 2;
                            if (axis == 0) {
                                ea = sa;
                            } else if (axis == 1) {
                                eb = sa;
                            } else {
                                ec = sa;
                            }
                            if (lat.edge_split(ea, eb, ec, static_cast<int>(axis))) {
                                poly_is_mid[static_cast<std::size_t>(np)] = 1;
                                poly[static_cast<std::size_t>(np++)] = node_fine(
                                    (A[0] + B[0]) / 2, (A[1] + B[1]) / 2, (A[2] + B[2]) / 2);
                            }
                        }
                        if (np == 4) {
                            emit_pyramid(out, poly[0], poly[1], poly[2], poly[3], apex);
                            if (free_face) {
                                out.boundary_quads.push_back(
                                    {{poly[0], poly[1], poly[2], poly[3]}});
                            }
                        } else {
                            // Canonical fan from the min-node-id *mid* vertex
                            // (np > 4 ⇒ a mid exists). A corner anchor sees the two
                            // halves of its own split edge collinearly and emits a
                            // zero-volume tet; a mid never lies on another split
                            // edge's line. Mid-ness is intrinsic to the shared face,
                            // so both cells pick the same anchor — no cracks.
                            int ai = -1;
                            for (int q = 0; q < np; ++q) {
                                if (!poly_is_mid[static_cast<std::size_t>(q)]) {
                                    continue;
                                }
                                if (ai < 0 || poly[static_cast<std::size_t>(q)] <
                                                  poly[static_cast<std::size_t>(ai)]) {
                                    ai = q;
                                }
                            }
                            if (ai < 0) {
                                ai = 0; // unreachable: np > 4 has a mid
                            }
                            const std::uint32_t anchor = poly[static_cast<std::size_t>(ai)];
                            for (int q = 0; q < np; ++q) {
                                const std::uint32_t u = poly[static_cast<std::size_t>(q)];
                                const std::uint32_t v =
                                    poly[static_cast<std::size_t>((q + 1) % np)];
                                if (u == anchor || v == anchor) {
                                    continue;
                                }
                                emit_tet(out, anchor, u, v, apex);
                                if (free_face) {
                                    out.boundary_quads.push_back({{anchor, u, v, v}});
                                }
                            }
                        }
                    }
                    fan_spans.push_back({apex, c, fan_first, out.cells.size()});
                    out.movable_fans.push_back({apex, c});
                    continue;
                }

                // Bulk hex (or plain-mode skin as pyramids at h).
                // Native-poly mode keeps free-surface skin as hex FE (no fan).
                const auto c = coarse_corners(i, j, k);
                if (!size_adaptive && is_fine[id] && !native_poly_transitions) {
                    // Plain hybrid: free-surface skin pyramids at bulk h.
                    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
                    for (int t = 0; t < 8; ++t) {
                        ctr += out.nodes[c[static_cast<std::size_t>(t)]];
                    }
                    ctr /= 8.0;
                    const auto apex = static_cast<std::uint32_t>(out.nodes.size());
                    out.nodes.push_back(ctr);
                    const std::size_t fan_first = out.cells.size();
                    emit_cell_pyramids(out, c, apex);
                    fan_spans.push_back({apex, c, fan_first, out.cells.size()});
                    out.movable_fans.push_back({apex, c});
                } else {
                    emit_hex(out, c);
                }
                for (std::size_t f = 0; f < 6; ++f) {
                    const auto& o = kFaceNbr[f];
                    if (lat.inb(i + o[0], j + o[1], k + o[2])) {
                        continue;
                    }
                    const auto& face = kHexFaces[f];
                    out.boundary_quads.push_back({{c[static_cast<std::size_t>(face[0])],
                                                   c[static_cast<std::size_t>(face[1])],
                                                   c[static_cast<std::size_t>(face[2])],
                                                   c[static_cast<std::size_t>(face[3])]}});
                }
            }
        }
    }
    return fan_spans;
}

} // namespace polymesh::mesh::detail::mixed
