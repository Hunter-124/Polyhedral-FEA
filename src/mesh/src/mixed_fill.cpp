// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/mixed_fill.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace polymesh::mesh {
namespace {

constexpr std::array<std::array<int, 4>, 6> kHexFaces{{
    {{0, 3, 2, 1}},
    {{4, 5, 6, 7}},
    {{0, 1, 5, 4}},
    {{2, 3, 7, 6}},
    {{0, 4, 7, 3}},
    {{1, 2, 6, 5}},
}};

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

constexpr std::array<std::array<int, 3>, 6> kFaceNbr{{
    {{0, 0, -1}},
    {{0, 0, 1}},
    {{0, -1, 0}},
    {{0, 1, 0}},
    {{-1, 0, 0}},
    {{1, 0, 0}},
}};

double tet_vol(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
               const Eigen::Vector3d& d) {
    return validity::tet_signed_volume(a, b, c, d);
}

// Boundary-snap rejection tests. Two independent reasons a cell is rejected:
// it is INVERTED (signed measure ≤ vol_eps — the old `std::abs(v) <= vol_eps`
// spelling silently accepted every fully flipped cell) or it is a SLIVER
// (normalized shape below `shape_floor`; vol_eps alone is ~1e-14·h³, thirteen
// orders below a healthy cell, so it never caught one).
// `shape_floor <= 0` disables the shape test.
bool hex_bad(const std::array<std::uint32_t, 8>& hx, const std::vector<Eigen::Vector3d>& nodes,
             double shape_floor) {
    std::array<Eigen::Vector3d, 8> x{};
    for (int i = 0; i < 8; ++i) {
        x[static_cast<std::size_t>(i)] = nodes[hx[static_cast<std::size_t>(i)]];
    }
    if (validity::hex8_min_jacobian(x) <= 0.0) {
        return true;
    }
    return shape_floor > 0.0 && validity::hex8_shape_quality(x) < shape_floor;
}

bool tet_bad(const std::array<std::uint32_t, 4>& n, const std::vector<Eigen::Vector3d>& nodes,
             double vol_eps, double shape_floor) {
    const Eigen::Vector3d& a = nodes[n[0]];
    const Eigen::Vector3d& b = nodes[n[1]];
    const Eigen::Vector3d& c = nodes[n[2]];
    const Eigen::Vector3d& d = nodes[n[3]];
    if (validity::tet_signed_volume(a, b, c, d) <= vol_eps) {
        return true;
    }
    return shape_floor > 0.0 && validity::tet_shape_quality(a, b, c, d) < shape_floor;
}

bool pyramid_bad(const std::array<std::uint32_t, 5>& n,
                 const std::vector<Eigen::Vector3d>& nodes, double vol_eps,
                 double shape_floor) {
    const Eigen::Vector3d& p0 = nodes[n[0]];
    const Eigen::Vector3d& p1 = nodes[n[1]];
    const Eigen::Vector3d& p2 = nodes[n[2]];
    const Eigen::Vector3d& p3 = nodes[n[3]];
    const Eigen::Vector3d& p4 = nodes[n[4]];
    if (validity::pyramid_min_split_volume(p0, p1, p2, p3, p4) <= vol_eps) {
        return true;
    }
    return shape_floor > 0.0 &&
           validity::pyramid_split_shape_quality(p0, p1, p2, p3, p4) < shape_floor;
}

void orient_pyramid_winding(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes);
void normalize_pyramid_diagonal(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes);

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

/// Search the interior natural-coordinate lattice for the least-displaced fan
/// apex that clears the requested cell-quality floor. If the floor cannot be
/// reached, retain the best improving candidate.
template <typename WorstQualityFn>
bool search_fan_apex(const std::array<Eigen::Vector3d, 8>& lattice, const Eigen::Vector3d& ctr,
                     WorstQualityFn&& worst, double q_reference, double shape_floor,
                     Eigen::Vector3d& best) {
    best = ctr;
    double best_q = q_reference;
    double best_d2 = std::numeric_limits<double>::max();
    constexpr std::array<double, 7> kAxis{{-0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75}};
    for (const double zeta : kAxis) {
        for (const double eta : kAxis) {
            for (const double xi : kAxis) {
                Eigen::Vector3d a = Eigen::Vector3d::Zero();
                for (std::size_t t = 0; t < 8; ++t) {
                    const auto& s = validity::kHexCornerSigns[t];
                    a += 0.125 * (1.0 + s[0] * xi) * (1.0 + s[1] * eta) * (1.0 + s[2] * zeta) *
                         lattice[t];
                }
                const double q = worst(a);
                const double d2 = (a - ctr).squaredNorm();
                if (best_q >= shape_floor) {
                    if (q >= shape_floor && d2 < best_d2) {
                        best_q = q;
                        best_d2 = d2;
                        best = a;
                    }
                } else if (q >= shape_floor || q > best_q) {
                    best_q = q;
                    best_d2 = d2;
                    best = a;
                }
            }
        }
    }
    return best_q > q_reference;
}

void emit_cell_pyramids(MixedFillOutput& out, const std::array<std::uint32_t, 8>& c,
                        std::uint32_t apex) {
    for (const auto& face : kHexFaces) {
        emit_pyramid(
            out, c[static_cast<std::size_t>(face[0])], c[static_cast<std::size_t>(face[1])],
            c[static_cast<std::size_t>(face[2])], c[static_cast<std::size_t>(face[3])], apex);
    }
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
    if (tet_vol(out.nodes[a], out.nodes[b], out.nodes[c], out.nodes[d]) < 0.0) {
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
        // Child quad: corner → mid → face-center → prev mid is wrong; use
        // corner–mid–face_center–mid_prev. For edge q the previous mid is edge q-1.
        const int qp = (q + 3) % 4;
        const auto& p = lc[static_cast<std::size_t>(qp)];
        const int pmx = (a[0] + p[0]) / 2;
        const int pmy = (a[1] + p[1]) / 2;
        const int pmz = (a[2] + p[2]) / 2;
        const auto npm = fn(2 * i + pmx, 2 * j + pmy, 2 * k + pmz);
        (void)nb;
        emit_pyramid(out, na, nm, fc, npm, apex);
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

/// Build one unsplit polyhedron for a 2:1 transition coarse cell (ADR-0019).
/// Faces match neighbors: single quad vs bulk, 4 child quads vs fine, n-gon with
/// hanging mids on mixed edges. No centroid apex / fan slivers.
template <typename FineNodeFn, typename EdgeSplitFn, typename InbFn, typename FineNbrFn>
void emit_transition_poly(MixedFillOutput& out, FineNodeFn&& fn, EdgeSplitFn&& edge_split,
                          InbFn&& inb, FineNbrFn&& fine_nbr, int i, int j, int k) {
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
        const bool free_face = !inb(ni, nj, nk);
        const bool adj_fine = !free_face && fine_nbr(ni, nj, nk);

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
            if (edge_split(ea, eb, ec, static_cast<int>(axis))) {
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

MixedFillOutput
mixed_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                   const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                   std::span<const geom::SharpEdge> features, double feature_band,
                   std::span<const Eigen::Vector3d> curvature_seeds, double seed_band,
                   bool snap_boundary, double curvature_turn_deg, bool native_poly_transitions,
                   const std::function<void()>& cancel_check, const SizeFieldFn& size_field,
                   bool local_surface_classification) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("mixed_fill_surface: h must be positive");
    }
    if (skin_layers < 1) {
        skin_layers = 1;
    }
    if (!(feature_band > 0.0) || features.empty()) {
        feature_band = 0.0;
    }
    if (!(seed_band > 0.0) || curvature_seeds.empty()) {
        seed_band = 0.0;
    }
    if (!(curvature_turn_deg > 0.0)) {
        curvature_turn_deg = 0.0;
    }
    const auto poll_cancel = [&] {
        if (cancel_check) {
            cancel_check();
        }
    };
    poll_cancel();

    // Budget for 2:1 fine subcells (up to 8× in refined bands).
    constexpr long kHybridMaxCoarse = static_cast<long>(kHybridMaxElems);
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kHybridMaxCoarse, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    // The optional h/2 classifier is sampling-only. Mixed parents are folded
    // into the existing one-level local refinement below; the coarse lattice
    // itself never advances globally.
    auto classification = classify_cells_feature_aware(
        surface, bbox_min, bbox_max, h_use, kHybridMaxCoarse,
        /*relative_volume_tolerance=*/0.01, local_surface_classification ? 1 : 0, size_field);
    const CartesianGrid& grid = classification.grid;
    const auto& inside = classification.inside;
    poll_cancel();
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double h_cell = grid.max_edge();
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[idx(i, j, k)];
    };

    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
    int max_dist = 0;
    for (int k = 0; k < nz; ++k) {
        poll_cancel();
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (const auto& o : kFaceNbr) {
                    if (!inb(i + o[0], j + o[1], k + o[2])) {
                        boundary = true;
                        break;
                    }
                }
                if (boundary) {
                    dist[idx(i, j, k)] = 0;
                    q.push({i, j, k});
                }
            }
        }
    }
    while (!q.empty()) {
        if ((q.size() & 1023U) == 0U) {
            poll_cancel();
        }
        const auto c = q.front();
        q.pop();
        const int d0 = dist[idx(c[0], c[1], c[2])];
        max_dist = std::max(max_dist, d0);
        for (const auto& o : kFaceNbr) {
            const int ni = c[0] + o[0], nj = c[1] + o[1], nk = c[2] + o[2];
            if (!inb(ni, nj, nk)) {
                continue;
            }
            auto& dn = dist[idx(ni, nj, nk)];
            if (dn < 0 || dn > d0 + 1) {
                dn = d0 + 1;
                q.push({ni, nj, nk});
            }
        }
    }

    MixedFillOutput out;
    out.h = h_cell;
    out.h_fine = h_cell;
    out.skin_layers = skin_layers;
    out.native_poly_transitions = native_poly_transitions;
    out.classification_refinement_levels = classification.refinement_levels;
    out.classification_volume_error = classification.relative_volume_error;

    // Free-surface hop skin only when no geo drivers (unit boxes). With
    // feature/seed/curvature, refine those bands to h/2 instead of flooding
    // the exterior.
    const int skin_cap = std::max(1, (max_dist + 1) / 2);
    const bool have_geo = (feature_band > 0.0) || (seed_band > 0.0) ||
                          (curvature_turn_deg > 0.0) || static_cast<bool>(size_field);
    const int skin_use = have_geo ? 0 : std::min(skin_layers, skin_cap);

    std::vector<char> is_fine(inside.size(), 0);
    std::vector<char> is_feature_skin(inside.size(), 0);
    std::vector<char> is_seed_skin(inside.size(), 0);

    for (int k = 0; k < nz; ++k) {
        poll_cancel();
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const int d = dist[idx(i, j, k)];
                if (skin_use > 0 && d >= 0 && d < skin_use) {
                    is_fine[idx(i, j, k)] = 1; // plain mode: skin as fine pyramids at h
                }
            }
        }
    }
    double field_h_min = std::numeric_limits<double>::infinity();
    double field_h_max = 0.0;
    std::size_t n_field_budget_clamped = 0;
    if (size_field) {
        const double h_floor = (h_budget > 0.0) ? h_budget : h_cell;
        for (int k = 0; k < nz; ++k) {
            poll_cancel();
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const auto id = idx(i, j, k);
                    if (!inside[id]) {
                        continue;
                    }
                    const Eigen::Vector3d centroid =
                        grid.origin +
                        Eigen::Vector3d{(static_cast<double>(i) + 0.5) * grid.cell[0],
                                        (static_cast<double>(j) + 0.5) * grid.cell[1],
                                        (static_cast<double>(k) + 0.5) * grid.cell[2]};
                    double requested = size_field(centroid);
                    if (!(requested > 0.0) || !std::isfinite(requested)) {
                        requested = h_floor;
                        ++n_field_budget_clamped;
                    } else {
                        field_h_min = std::min(field_h_min, requested);
                        field_h_max = std::max(field_h_max, requested);
                        if (requested < h_floor) {
                            ++n_field_budget_clamped;
                        }
                    }
                    const double h_target = std::max(requested, h_floor);
                    const int level = std::clamp(
                        static_cast<int>(std::lround(std::log2(h_cell / h_target))), 0, 1);
                    if (level >= 1) {
                        is_fine[id] = 1;
                    }
                }
            }
        }
        // TODO(size-field): 4x4x4 needs the 2:1 closure generalised.
    }
    // Feature/seed → fine (h/2 via 2×2×2). stamp writes into is_fine.
    stamp_feature_cells(is_fine, &is_feature_skin, nx, ny, nz, grid, surface, features,
                        feature_band);
    stamp_seed_cells(is_fine, &is_seed_skin, nx, ny, nz, grid, curvature_seeds, seed_band);
    // Per-cell turning-angle criterion (angle-adaptive; hybrid has one fine
    // level, so L2 output is unused here).
    if (curvature_turn_deg > 0.0) {
        stamp_curvature_cells(is_fine, nullptr, &is_seed_skin, nx, ny, nz, grid, surface,
                              curvature_turn_deg * 3.14159265358979323846 / 180.0);
    }

    // Fine child sampling detects sub-cell topology without a global lattice
    // advance. Mixed parents are already surface cells; one face-neighbour
    // solid shell is promoted so the 2:1 interface closes in the interior.
    if (!classification.child_inside_mask.empty()) {
        std::vector<char> promote(inside.size(), 0);
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const auto id = idx(i, j, k);
                    const auto mask = classification.child_inside_mask[id];
                    if (!inside[id] || mask == 0 || mask == std::uint8_t{0xff}) {
                        continue;
                    }
                    is_fine[id] = 1;
                    is_feature_skin[id] = 1;
                    for (const auto& o : kFaceNbr) {
                        const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                        if (inb(ni, nj, nk)) {
                            promote[idx(ni, nj, nk)] = 1;
                        }
                    }
                }
            }
        }
        for (std::size_t c = 0; c < promote.size(); ++c) {
            if (promote[c]) {
                is_fine[c] = 1;
            }
        }
    }
    // Outside → not fine.
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            is_fine[c] = 0;
            is_feature_skin[c] = 0;
            is_seed_skin[c] = 0;
            continue;
        }
        if (is_feature_skin[c] || is_seed_skin[c]) {
            ++out.n_feature_skin_cells;
        }
    }

    // 2:1 interface (v4, conforming): a hanging mid-node exists on a coarse
    // lattice edge iff ANY cell incident to that edge is fine (fine cells own
    // the mid of every one of their edges). Every non-fine cell touching such
    // an edge — not just face-neighbors of fine cells — must emit facets that
    // include those mids, else the mesh cracks along cell edges (the v3 bug:
    // unpaired (c0,c1,apex) vs (c0,m,apex)+(m,c1,apex) side triangles).
    std::vector<char> is_transition(inside.size(), 0);
    const bool size_adaptive = have_geo;
    const auto cell_fine = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[idx(i, j, k)] && is_fine[idx(i, j, k)];
    };
    // Coarse lattice edge starting at node (a,b,c) along `axis`.
    const auto edge_split = [&](int a, int b, int c, int axis) {
        for (int u = -1; u <= 0; ++u) {
            for (int v = -1; v <= 0; ++v) {
                int ci = a, cj = b, ck = c;
                if (axis == 0) {
                    cj += u;
                    ck += v;
                } else if (axis == 1) {
                    ci += u;
                    ck += v;
                } else {
                    ci += u;
                    cj += v;
                }
                if (cell_fine(ci, cj, ck)) {
                    return true;
                }
            }
        }
        return false;
    };
    if (size_adaptive) {
        auto is_free_surface = [&](int i, int j, int k) {
            for (const auto& o : kFaceNbr) {
                if (!inb(i + o[0], j + o[1], k + o[2])) {
                    return true;
                }
            }
            return false;
        };
        long n_interior = 0;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            n_interior += (inside[c] != 0);
        }
        // Recompute the 2:1 interface against the current fine set. A hanging
        // mid-node exists on a coarse lattice edge iff ANY cell incident to that
        // edge is fine, so every non-fine cell touching such an edge — not just
        // the face-neighbors of fine cells — is a transition cell.
        const auto mark_transitions = [&] {
            std::fill(is_transition.begin(), is_transition.end(), 0);
            for (int k = 0; k < nz; ++k) {
                poll_cancel();
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = idx(i, j, k);
                        if (!inside[id] || is_fine[id]) {
                            continue;
                        }
                        bool fan = false;
                        for (const auto& o : kFaceNbr) {
                            if (cell_fine(i + o[0], j + o[1], k + o[2])) {
                                fan = true;
                                break;
                            }
                        }
                        // Edge-adjacent fine cells also hang mids on this cell's edges.
                        for (int eb = 0; !fan && eb < 2; ++eb) {
                            for (int ec = 0; !fan && ec < 2; ++ec) {
                                fan = edge_split(i, j + eb, k + ec, 0) ||
                                      edge_split(i + eb, j, k + ec, 1) ||
                                      edge_split(i + eb, j + ec, k, 2);
                            }
                        }
                        is_transition[id] = fan ? 1 : 0;
                    }
                }
            }
        };
        // Free-surface gap-close only (2 hops). Spatial seeds already cover the
        // hole ring; a long free-surface BFS floods flat box faces and kills
        // bulk/fine contrast on the exterior.
        constexpr int kFsGapHops = 2;
        for (int pass = 0; pass < kFsGapHops; ++pass) {
            std::vector<char> promote(inside.size(), 0);
            for (int k = 0; k < nz; ++k) {
                poll_cancel();
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = idx(i, j, k);
                        if (!inside[id] || is_fine[id] || !is_free_surface(i, j, k)) {
                            continue;
                        }
                        for (const auto& o : kFaceNbr) {
                            const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                            if (inb(ni, nj, nk) && is_fine[idx(ni, nj, nk)]) {
                                promote[id] = 1;
                                break;
                            }
                        }
                    }
                }
            }
            for (std::size_t c = 0; c < promote.size(); ++c) {
                if (promote[c]) {
                    is_fine[c] = 1;
                }
            }
        }
        // Push the 2:1 interface one cell inside the wall by promoting
        // free-surface transition cells to fine. This is not cosmetic: a
        // transition cell touching the free surface has its own nodes moved by
        // the caller's boundary snap and tangential smoothing, which squashes
        // its apex fan (the "rings" seen mid-bore) and — on the native-poly path
        // — bends its facets out of plane, so the VEM/FE constant-strain patch
        // degrades from machine zero to ~1.5e-5 (measured 2026-08-08 on the unit
        // box). An interior transition cell keeps exact lattice facets. Run to a
        // fixed point: promotion hangs new mids (monotone, is_fine only grows).
        // Every exit leaves `is_transition` consistent with `is_fine`.
        mark_transitions();
        // Monotone with at least one promotion per round, so `n_interior` rounds
        // is the exact termination bound — the old fixed 64 could truncate the
        // fixed point on a long thin wall and leave transition fans sitting on
        // the surface for the snap to squash.
        for (long guard = 0; guard <= n_interior; ++guard) {
            poll_cancel();
            bool changed = false;
            for (int k = 0; k < nz; ++k) {
                poll_cancel();
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = idx(i, j, k);
                        if (is_transition[id] && is_free_surface(i, j, k)) {
                            is_fine[id] = 1;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed) {
                break;
            }
            mark_transitions();
        }
        out.h_fine = 0.5 * h_cell;

        // Fine-level affordability (ADR-0015). A fine coarse cell becomes 2×2×2
        // hexes and the product-FE expansion (ADR-0013) turns each of those into
        // 6 pyramid5 — 48 elements against 6 for a bulk hex — so a fine set that
        // has stopped being local is ruinous: the turning-angle criterion alone
        // stamps 74% of the cylinder and 80% of the plate at the campaign h, and
        // on a wall two cells thick every cell is a free-surface cell, so the
        // promotions above run until the whole slab is fine (plate_hole -h 0.005:
        // 2544 of 2544 cells, 118448 elements, 12.3× the predicted count).
        //
        // When the graded lattice busts the element budget but a *uniform* h/2
        // lattice fits inside it, the uniform lattice wins on every axis: 8 hexes
        // per coarse cell instead of 48, hex8 accuracy instead of split-pyramid,
        // finer everywhere, and — legitimately, because no coarse cell survives —
        // no 2:1 interface and no transition cells. That is a real answer to a
        // sizing field asking for a smaller h, not a suppression of the 2:1
        // machinery: below the budget the graded lattice and its transition cells
        // are kept untouched. Measured 2026-08-08: plate_hole 118448 → 20352
        // (12.3× → 2.1× predicted), cylinder -h 0.01 63994 → 12800 (5.3× → 1.1×),
        // while cylinder_prism -h 0.12·extent (est. 23k) and the sphere/cantilever
        // graded lattices (uniform h/2 would need 74k/375k cells, over budget)
        // keep their transitions.
        //
        // Per-transition fan cost is measured, not guessed: 18 elements on the
        // cylinder and the sphere, 26 on plate_hole (fan size grows with the
        // number of fine neighbours), so 24 is a representative upper-middle.
        constexpr long kFanElemsPerTransition = 24;
        const long max_hybrid_elems = static_cast<long>(kHybridMaxElems);
        long n_fine_cells = 0, n_trans_cells = 0;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            n_fine_cells += (is_fine[c] != 0);
            n_trans_cells += (is_transition[c] != 0);
        }
        const long n_lattice_hex =
            8 * n_fine_cells + (n_interior - n_fine_cells - n_trans_cells);
        const long est_graded =
            native_poly_transitions
                ? n_lattice_hex + n_trans_cells
                : 6 * n_lattice_hex + kFanElemsPerTransition * n_trans_cells;
        if (est_graded > max_hybrid_elems && 8 * n_interior <= max_hybrid_elems) {
            std::fill(is_transition.begin(), is_transition.end(), 0);
            for (std::size_t c = 0; c < is_fine.size(); ++c) {
                is_fine[c] = (inside[c] != 0) ? 1 : 0;
            }
            out.n_feature_skin_cells = 0;
        }
    }
    out.field_h_min = std::isfinite(field_h_min) ? field_h_min : 0.0;
    out.field_h_max = field_h_max;
    out.n_field_budget_clamped = n_field_budget_clamped;
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (is_fine[c]) {
            ++out.n_level1_cells;
        } else {
            ++out.n_level0_cells;
        }
    }

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
               coarse_inside[idx(i, j, k)];
    };
    // -> bool, not deduced: `inside` is a std::vector<bool>, whose element access
    // returns a proxy reference, and libc++ rejects the deduced mismatch.
    const auto fine_child_inside = [&](int I, int J, int K) -> bool {
        if (I < 0 || I >= 2 * nx || J < 0 || J >= 2 * ny || K < 0 || K >= 2 * nz) {
            return false;
        }
        const int i = I / 2, j = J / 2, k = K / 2;
        if (classification.child_inside_mask.empty()) {
            return inside[idx(i, j, k)];
        }
        const int bit = (I % 2) + 2 * (J % 2) + 4 * (K % 2);
        return (classification.child_inside_mask[idx(i, j, k)] &
                static_cast<std::uint8_t>(1U << bit)) != 0;
    };

    // Every apex-fan cell group emitted below (2:1 closure fan, plain-mode skin
    // fan): the shared apex node, the lattice cell it sits in, and the cell
    // range it owns. The shell-apex pass at the end of this function re-places
    // those apexes when the boundary snap would crush their fan.
    struct FanSpan {
        std::uint32_t apex;
        std::array<std::uint32_t, 8> corners;
        std::size_t first;
        std::size_t end;
    };
    std::vector<FanSpan> fan_spans;

    for (int k = 0; k < nz; ++k) {
        poll_cancel();
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto id = idx(i, j, k);
                if (!inside[id]) {
                    continue;
                }

                if (size_adaptive && is_fine[id]) {
                    // Emit every live h/2 child and derive its free faces from the
                    // same child mask. The former parent-face-only pass omitted
                    // live/void interfaces inside a mixed parent; those nodes
                    // never entered boundary snapping and caused the apparent
                    // curved-fidelity regression on spheres and cones.
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
                        emit_transition_poly(
                            out, node_fine, edge_split, inb,
                            [&](int ni, int nj, int nk) {
                                return is_fine[idx(ni, nj, nk)] != 0;
                            },
                            i, j, k);
                        continue;
                    }
                    // Conforming 2:1 closure (v4): apex fan over each face polygon.
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
                        if (inb(ni, nj, nk) && is_fine[idx(ni, nj, nk)]) {
                            // Fine face-neighbor: 4 quarter-quad pyramids (mid +
                            // face-center nodes shared with the fine sub-hexes).
                            emit_subdivided_face_pyramids(out, node_fine, i, j, k,
                                                          static_cast<int>(f), apex);
                            continue;
                        }
                        const bool free_face = !inb(ni, nj, nk);
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
                            if (edge_split(ea, eb, ec, static_cast<int>(axis))) {
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
                            // (np > 4 ⇒ at least one mid exists). A corner
                            // anchor sees the two halves of its own split edge
                            // collinearly and emits a zero-volume tet (the v4
                            // M6=0 defect); a mid never lies on another split
                            // edge's line, so every fan facet has real area.
                            // Mid-ness is intrinsic to the shared face, so both
                            // cells pick the same anchor — no cracks.
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
                    if (inb(i + o[0], j + o[1], k + o[2])) {
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

    if (out.cells.empty()) {
        throw ValidityError("mixed_fill_surface: no interior cells");
    }

    // Shell-cell apex placement (product-FE path). The caller expands every
    // surviving hex into 6 pyramid5 around its centroid and snaps the boundary
    // onto the STL only afterwards. On a curved wall a stair cell can carry 7
    // of its 8 corners in boundary quads; the snap lands them all on the same
    // surface patch, the centroid — which was barely inside to begin with —
    // ends up level with or past three of the six bases, the snap's validity
    // predicate rejects those pyramids, and its line-search buys their validity
    // by retreating the wall. That retreat *is* the boundary residual the
    // curved scorecard measures: sphere m1_max 0.209 h at h = 0.15 extent, 24
    // offending cells, no node ever fully unsnapped (so it never showed up as
    // an unsnap count). The same happens to the 2:1 closure fans below, whose
    // apex is likewise the cell centre — those ship inverted boundary tets.
    //
    // Fix it here, where the surface is still in hand: predict where the snap
    // will put each boundary corner (closest point, same travel cap) and, for
    // the cells a centroid apex cannot survive, place the apex where the
    // predicted cell is healthy — expanding the hex now, or just moving the
    // fan's existing apex node. Faces stay the same quads, the caller's expand
    // passes pyramids through, and a fan apex belongs to no other cell, so
    // conformity and the element count are both unchanged. Measured
    // 2026-08-08: sphere m1_max 0.0313 → 1.7e-16 (score 0.850 → 0.893),
    // cylinder_prism 0.00752 → 0.00661, plate 9.6e-12 unchanged, inverted
    // cells in the -h default hybrid meshes 66 → 17 (sphere) and 9 → 0
    // (icecream_cone), element counts identical everywhere.
    //
    // Cells whose predicted snap keeps the centroid healthy are left alone on
    // purpose: biasing them unconditionally measurably hurts (cylinder_prism
    // 0.063 h → 0.162 h, plate 1e-12 → 0.084 h), because on a flat
    // axis-aligned wall the corners do not move at all and an off-centre apex
    // only thins the inner pyramids.
    //
    // Gated on the lattice already being mixed: a pure-hex lattice is kept as
    // hex8 by the caller (ADR-0013), and introducing the first pyramid here
    // would expand the whole mesh 6× for one cell.
    const bool product_expand_follows = !native_poly_transitions && out.n_hex > 0 &&
                                        (out.n_pyramid > 0 || out.n_tet > 0 || out.n_poly > 0);
    if (product_expand_follows && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        // Predicted post-snap site of every boundary node. Mirror
        // snap_boundary_nodes exactly: true crease/rim nodes prefer the
        // detected feature when it is as close as the surface target, then the
        // total travel is capped at 1.25h.
        const double travel_cap = 1.25 * h_cell;
        const double edge_prefer_r = 0.55 * h_cell;
        std::map<std::uint32_t, Eigen::Vector3d> predicted;
        for (const auto g : bnode_set) {
            const Eigen::Vector3d& p = out.nodes[g];
            const auto cp = closest_on_surface(surface, p);
            Eigen::Vector3d target = cp.point;
            if (!features.empty()) {
                const auto cf = geom::closest_on_features(p, surface, features);
                if (std::isfinite(cf.distance) && cf.distance > 1e-15 &&
                    cf.distance <= edge_prefer_r &&
                    cf.distance <= cp.distance + 0.08 * h_cell) {
                    target = cf.point;
                }
            }
            const Eigen::Vector3d d = target - p;
            const double len = d.norm();
            predicted.emplace(g, len > travel_cap ? Eigen::Vector3d(p + d * (travel_cap / len))
                                                  : target);
        }
        const double shape_floor = validity::kCellShapeFloor;
        // Post-snap site of a node: its prediction when the snap will move it,
        // its lattice site otherwise.
        const auto site = [&](std::uint32_t g) -> const Eigen::Vector3d& {
            const auto it = predicted.find(g);
            return it == predicted.end() ? out.nodes[g] : it->second;
        };
        // Best apex for a fan, searched over the natural coordinates of the
        // lattice cell it lives in. Only |ξ|,|η|,|ζ| ≤ 0.75 are offered, so the
        // apex stays inside the cell: every base of the fan lies in one of the
        // cell's face planes, so the winding emit_pyramid / emit_tet chose still
        // holds and the *unsnapped* mesh is valid however the prediction turns
        // out. Among the candidates that clear the floor the SMALLEST
        // displacement from the centroid wins rather than the outright best
        // score: the prediction is a single closest-point shot while the real
        // snap is eight capped passes plus a line-search, so the least-committal
        // apex that survives generalises better than the predicted optimum
        // (measured on the sphere: 0.068 h for max-score, 1.7e-16 for this).
        // The sample set is itself measured, not derived — the prediction is
        // only approximate, so ±0.75 in steps of 0.25 was picked as the best of
        // ±0.6/0.3 (sphere 0.068 h) and ±0.8/0.2 (0.0104 h) on the three curved
        // scorecards; all three keep cylinder_prism and plate at their bounds.
        const std::size_t n_emitted = out.cells.size();
        std::vector<char> replaced(n_emitted, 0);
        std::size_t n_replaced = 0;
        for (std::size_t ci = 0; ci < n_emitted; ++ci) {
            if ((ci & 255U) == 0U) {
                poll_cancel();
            }
            if (out.cells[ci].kind != MixedCellKind::kHex8) {
                continue;
            }
            const std::array<std::uint32_t, 8> cn = out.cells[ci].nodes;
            std::array<Eigen::Vector3d, 8> lattice{};
            std::array<Eigen::Vector3d, 8> snapped{};
            Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
            int n_core = 0;
            for (std::size_t t = 0; t < 8; ++t) {
                lattice[t] = out.nodes[cn[t]];
                ctr += lattice[t];
                const auto it = predicted.find(cn[t]);
                if (it == predicted.end()) {
                    snapped[t] = lattice[t];
                    ++n_core;
                } else {
                    snapped[t] = it->second;
                }
            }
            if (n_core == 0 || n_core == 8) {
                continue; // untouched cell, or nothing left to anchor an apex to
            }
            ctr /= 8.0;
            // Worst pyramid of the predicted post-snap cell for a given apex.
            // Base winding follows emit_pyramid (apex on the positive side of
            // the lattice face) so the split-tet volumes keep their sign.
            const auto worst_split = [&](const Eigen::Vector3d& a) {
                double q = std::numeric_limits<double>::max();
                for (const auto& face : kHexFaces) {
                    const auto f0 = static_cast<std::size_t>(face[0]);
                    const auto f1 = static_cast<std::size_t>(face[1]);
                    const auto f2 = static_cast<std::size_t>(face[2]);
                    const auto f3 = static_cast<std::size_t>(face[3]);
                    const Eigen::Vector3d nrm =
                        (lattice[f1] - lattice[f0]).cross(lattice[f2] - lattice[f0]);
                    const bool ccw = nrm.dot(a - lattice[f0]) > 0.0;
                    q = std::min(q, validity::pyramid_split_shape_quality(
                                        snapped[f0], snapped[ccw ? f1 : f3], snapped[f2],
                                        snapped[ccw ? f3 : f1], a));
                }
                return q;
            };
            const double q_centroid = worst_split(ctr);
            if (q_centroid >= shape_floor) {
                continue; // the caller's centroid apex survives its own snap
            }
            Eigen::Vector3d best;
            if (!search_fan_apex(lattice, ctr, worst_split, q_centroid, shape_floor, best)) {
                continue; // no apex does better; leave the cell to the caller
            }
            const auto apex = static_cast<std::uint32_t>(out.nodes.size());
            out.nodes.push_back(best);
            emit_cell_pyramids(out, cn, apex);
            out.movable_fans.push_back({apex, cn});
            replaced[ci] = 1;
            ++n_replaced;
            --out.n_hex;
        }
        // Same treatment for the apex fans emitted above (2:1 closure, plain
        // skin). Their apex is the coarse cell centre and their bases include
        // free faces, so a snapped free face walks straight through it and the
        // product mesh ships inverted boundary tets — scene.cpp's tet offender
        // rule only tests the sign and its comment already concedes it would
        // rather peel them than unsnap the wall. Re-placing the apex costs
        // nothing: it is an interior node of the fan, shared by no other cell.
        for (const auto& fan : fan_spans) {
            poll_cancel();
            std::array<Eigen::Vector3d, 8> lattice{};
            Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
            for (std::size_t t = 0; t < 8; ++t) {
                lattice[t] = out.nodes[fan.corners[t]];
                ctr += lattice[t];
            }
            ctr /= 8.0;
            bool touched = false;
            for (std::size_t ci = fan.first; ci < fan.end && !touched; ++ci) {
                const auto& cell = out.cells[ci];
                for (std::uint8_t m = 0; m + 1 < cell.n_nodes; ++m) {
                    if (predicted.count(cell.nodes[m]) != 0) {
                        touched = true;
                        break;
                    }
                }
            }
            if (!touched) {
                continue;
            }
            // The apex is the last node of every fan cell (emit_pyramid /
            // emit_tet), so only the bases come from the predicted sites.
            const auto worst_fan = [&](const Eigen::Vector3d& a) {
                double q = std::numeric_limits<double>::max();
                for (std::size_t ci = fan.first; ci < fan.end; ++ci) {
                    const auto& cell = out.cells[ci];
                    if (cell.kind == MixedCellKind::kPyramid5) {
                        q = std::min(q, validity::pyramid_split_shape_quality(
                                            site(cell.nodes[0]), site(cell.nodes[1]),
                                            site(cell.nodes[2]), site(cell.nodes[3]), a));
                    } else if (cell.kind == MixedCellKind::kTet4) {
                        q = std::min(q, validity::tet_shape_quality(site(cell.nodes[0]),
                                                                    site(cell.nodes[1]),
                                                                    site(cell.nodes[2]), a));
                    }
                }
                return q;
            };
            const double q_centroid = worst_fan(ctr);
            if (q_centroid >= shape_floor) {
                continue;
            }
            Eigen::Vector3d best;
            if (search_fan_apex(lattice, ctr, worst_fan, q_centroid, shape_floor, best)) {
                out.nodes[fan.apex] = best;
            }
        }
        if (n_replaced > 0) {
            std::vector<MixedCell> kept;
            kept.reserve(out.cells.size() - n_replaced);
            for (std::size_t ci = 0; ci < out.cells.size(); ++ci) {
                if (ci < n_emitted && replaced[ci]) {
                    continue;
                }
                kept.push_back(std::move(out.cells[ci]));
            }
            out.cells = std::move(kept);
        }
    }

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        poll_cancel();
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const double vol_eps = 1e-14 * h_cell * h_cell * h_cell;
        // Sliver floor: vol_eps is a machine-degeneracy test, so on its own the
        // snap was free to flatten skin cells to ~1e-17 quality and the unsnap
        // line-search never fired. Nominal lattice cells score ~0.8-1.0 here.
        const double shape_floor = validity::kCellShapeFloor;
        // Use bulk h as move/search budget (fine nodes still reproject within it).
        out.boundary_max_distance =
            snap_boundary_nodes(
                surface, out.nodes, bnodes, h_cell,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : out.cells) {
                        bool bad = false;
                        if (cell.kind == MixedCellKind::kTet4) {
                            bad = tet_bad(
                                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]},
                                out.nodes, vol_eps, shape_floor);
                        } else if (cell.kind == MixedCellKind::kPyramid5) {
                            bad = pyramid_bad({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                               cell.nodes[3], cell.nodes[4]},
                                              out.nodes, vol_eps, shape_floor);
                        } else if (cell.kind == MixedCellKind::kPolyVem) {
                            std::vector<Eigen::Vector3d> coords;
                            coords.reserve(cell.poly_nodes.size());
                            for (const auto g : cell.poly_nodes) {
                                coords.push_back(out.nodes[g]);
                            }
                            bad = closed_poly_volume(coords, cell.poly_faces) <= vol_eps;
                        } else {
                            bad = hex_bad({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                           cell.nodes[3], cell.nodes[4], cell.nodes[5],
                                           cell.nodes[6], cell.nodes[7]},
                                          out.nodes, shape_floor);
                        }
                        if (!bad) {
                            continue;
                        }
                        if (cell.kind == MixedCellKind::kPolyVem) {
                            offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                        } else {
                            for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                                offenders.insert(cell.nodes[m]);
                            }
                        }
                    }
                },
                /*max_move_frac=*/1.25, /*passes=*/8, features)
                .max_residual;
        // The closest-point prediction above is intentionally conservative,
        // but the real multi-pass projection plus per-node rollback can finish
        // at a different site. Re-evaluate every private transition/skin fan
        // against the ACTUAL snapped bases and move only its interior apex.
        // This cannot change boundary conformity or the measured residual.
        if (!fan_spans.empty()) {
            std::map<std::uint32_t, std::vector<const MixedCell*>> live_fan_cells;
            for (const auto& fan : fan_spans) {
                live_fan_cells.try_emplace(fan.apex);
            }
            for (const auto& cell : out.cells) {
                if (cell.kind != MixedCellKind::kPyramid5 &&
                    cell.kind != MixedCellKind::kTet4) {
                    continue;
                }
                const auto apex = cell.nodes[static_cast<std::size_t>(cell.n_nodes - 1)];
                const auto it = live_fan_cells.find(apex);
                if (it != live_fan_cells.end()) {
                    it->second.push_back(&cell);
                }
            }
            for (const auto& fan : fan_spans) {
                poll_cancel();
                const auto fit = live_fan_cells.find(fan.apex);
                if (fit == live_fan_cells.end() || fit->second.empty()) {
                    continue;
                }
                std::array<Eigen::Vector3d, 8> lattice{};
                Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
                for (std::size_t t = 0; t < 8; ++t) {
                    lattice[t] = out.nodes[fan.corners[t]];
                    ctr += lattice[t];
                }
                ctr /= 8.0;
                const auto worst_actual = [&](const Eigen::Vector3d& a) {
                    double q = std::numeric_limits<double>::max();
                    for (const auto* cell : fit->second) {
                        if (cell->kind == MixedCellKind::kPyramid5) {
                            q = std::min(q, validity::pyramid_split_shape_quality(
                                                out.nodes[cell->nodes[0]],
                                                out.nodes[cell->nodes[1]],
                                                out.nodes[cell->nodes[2]],
                                                out.nodes[cell->nodes[3]], a));
                        } else {
                            q = std::min(
                                q, validity::tet_shape_quality(out.nodes[cell->nodes[0]],
                                                               out.nodes[cell->nodes[1]],
                                                               out.nodes[cell->nodes[2]], a));
                        }
                    }
                    return q;
                };
                const double q_current = worst_actual(out.nodes[fan.apex]);
                if (q_current >= shape_floor) {
                    continue;
                }
                Eigen::Vector3d best;
                if (search_fan_apex(lattice, ctr, worst_actual, q_current, shape_floor,
                                    best)) {
                    out.nodes[fan.apex] = best;
                }
            }
        }
    }
    // Snapping can change which geometric base diagonal is preferred; rotate
    // again so any direct MixedFillOutput consumer sees the chosen split in
    // VTK/PyVista's fixed local 0-2 slot.
    for (auto& cell : out.cells) {
        normalize_pyramid_diagonal(cell, out.nodes);
    }
    return out;
}

std::size_t repair_mixed_fan_apices(MixedFillOutput& fill, double shape_floor) {
    if (fill.movable_fans.empty()) {
        return 0;
    }
    std::map<std::uint32_t, std::vector<MixedCell*>> live_fan_cells;
    for (const auto& fan : fill.movable_fans) {
        live_fan_cells.try_emplace(fan.apex);
    }
    for (auto& cell : fill.cells) {
        if (cell.kind != MixedCellKind::kPyramid5 && cell.kind != MixedCellKind::kTet4) {
            continue;
        }
        const auto apex = cell.nodes[static_cast<std::size_t>(cell.n_nodes - 1)];
        const auto it = live_fan_cells.find(apex);
        if (it != live_fan_cells.end()) {
            it->second.push_back(&cell);
        }
    }

    std::size_t n_moved = 0;
    for (const auto& fan : fill.movable_fans) {
        const auto fit = live_fan_cells.find(fan.apex);
        if (fit == live_fan_cells.end() || fit->second.empty()) {
            continue;
        }
        std::array<Eigen::Vector3d, 8> lattice{};
        Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
        for (std::size_t t = 0; t < 8; ++t) {
            lattice[t] = fill.nodes[fan.corners[t]];
            ctr += lattice[t];
        }
        ctr /= 8.0;
        const auto worst_actual = [&](const Eigen::Vector3d& a) {
            double q = std::numeric_limits<double>::max();
            for (const auto* cell : fit->second) {
                if (cell->kind == MixedCellKind::kPyramid5) {
                    q = std::min(q, validity::pyramid_split_shape_quality(
                                        fill.nodes[cell->nodes[0]], fill.nodes[cell->nodes[1]],
                                        fill.nodes[cell->nodes[2]], fill.nodes[cell->nodes[3]],
                                        a));
                } else {
                    q = std::min(q, validity::tet_shape_quality(
                                        fill.nodes[cell->nodes[0]], fill.nodes[cell->nodes[1]],
                                        fill.nodes[cell->nodes[2]], a));
                }
            }
            return q;
        };
        const double q_current = worst_actual(fill.nodes[fan.apex]);
        if (q_current >= shape_floor) {
            continue;
        }
        Eigen::Vector3d best;
        if (search_fan_apex(lattice, ctr, worst_actual, q_current, shape_floor, best)) {
            fill.nodes[fan.apex] = best;
            ++n_moved;
        }
    }
    for (auto& cell : fill.cells) {
        normalize_pyramid_diagonal(cell, fill.nodes);
    }
    return n_moved;
}

MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill) {
    MixedFillOutput out;
    out.h = fill.h;
    out.h_fine = fill.h_fine;
    out.boundary_quads = fill.boundary_quads;
    out.local_child_boundary_quads = fill.local_child_boundary_quads;
    out.movable_fans = fill.movable_fans;
    out.boundary_max_distance = fill.boundary_max_distance;
    out.skin_layers = fill.skin_layers;
    out.n_feature_skin_cells = fill.n_feature_skin_cells;
    out.n_fine_cells = fill.n_fine_cells;
    out.n_transition_cells = fill.n_transition_cells;
    out.n_level0_cells = fill.n_level0_cells;
    out.n_level1_cells = fill.n_level1_cells;
    out.classification_refinement_levels = fill.classification_refinement_levels;
    out.classification_volume_error = fill.classification_volume_error;
    out.field_h_min = fill.field_h_min;
    out.field_h_max = fill.field_h_max;
    out.n_field_budget_clamped = fill.n_field_budget_clamped;
    out.native_poly_transitions = fill.native_poly_transitions;
    out.nodes = fill.nodes;
    out.n_hex = 0;
    out.n_pyramid = 0;
    out.n_tet = 0;
    out.n_poly = 0;
    out.cells.reserve(fill.cells.size() + 5 * fill.n_hex);

    for (const auto& cell : fill.cells) {
        if (cell.kind == MixedCellKind::kTet4) {
            out.cells.push_back(cell);
            ++out.n_tet;
            continue;
        }
        if (cell.kind == MixedCellKind::kPyramid5) {
            MixedCell pyr = cell;
            normalize_pyramid_diagonal(pyr, out.nodes);
            out.cells.push_back(std::move(pyr));
            ++out.n_pyramid;
            continue;
        }
        if (cell.kind == MixedCellKind::kPolyVem) {
            out.cells.push_back(cell);
            ++out.n_poly;
            continue;
        }
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (int i = 0; i < 8; ++i) {
            center += out.nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        center /= 8.0;
        const auto apex = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(center);
        out.movable_fans.push_back({apex, cell.nodes});
        for (const auto& face : kHexFaces) {
            MixedCell pyr;
            pyr.kind = MixedCellKind::kPyramid5;
            pyr.n_nodes = 5;
            pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
            pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[1])];
            pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
            pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[3])];
            pyr.nodes[4] = apex;
            orient_pyramid_winding(pyr, out.nodes);
            normalize_pyramid_diagonal(pyr, out.nodes);
            out.cells.push_back(pyr);
            ++out.n_pyramid;
        }
    }
    return out;
}

} // namespace polymesh::mesh
