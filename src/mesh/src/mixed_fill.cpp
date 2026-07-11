// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/mixed_fill.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
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

constexpr std::array<std::array<double, 3>, 8> kHexCornerSigns{{
    {{-1, -1, -1}},
    {{1, -1, -1}},
    {{1, 1, -1}},
    {{-1, 1, -1}},
    {{-1, -1, 1}},
    {{1, -1, 1}},
    {{1, 1, 1}},
    {{-1, 1, 1}},
}};

double tet_vol(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
               const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

double hex8_jac_det(const std::array<Eigen::Vector3d, 8>& x, const Eigen::Vector3d& xi) {
    Eigen::Matrix3d jac = Eigen::Matrix3d::Zero();
    for (int a = 0; a < 8; ++a) {
        const double sx = kHexCornerSigns[static_cast<std::size_t>(a)][0];
        const double sy = kHexCornerSigns[static_cast<std::size_t>(a)][1];
        const double sz = kHexCornerSigns[static_cast<std::size_t>(a)][2];
        const double dxi = 0.125 * sx * (1.0 + sy * xi[1]) * (1.0 + sz * xi[2]);
        const double deta = 0.125 * sy * (1.0 + sx * xi[0]) * (1.0 + sz * xi[2]);
        const double dzeta = 0.125 * sz * (1.0 + sx * xi[0]) * (1.0 + sy * xi[1]);
        const auto& xa = x[static_cast<std::size_t>(a)];
        jac(0, 0) += dxi * xa[0];
        jac(0, 1) += dxi * xa[1];
        jac(0, 2) += dxi * xa[2];
        jac(1, 0) += deta * xa[0];
        jac(1, 1) += deta * xa[1];
        jac(1, 2) += deta * xa[2];
        jac(2, 0) += dzeta * xa[0];
        jac(2, 1) += dzeta * xa[1];
        jac(2, 2) += dzeta * xa[2];
    }
    return jac.determinant();
}

bool hex_inverted(const std::array<std::uint32_t, 8>& hx,
                  const std::vector<Eigen::Vector3d>& nodes) {
    std::array<Eigen::Vector3d, 8> x{};
    for (int i = 0; i < 8; ++i) {
        x[static_cast<std::size_t>(i)] = nodes[hx[static_cast<std::size_t>(i)]];
    }
    if (hex8_jac_det(x, Eigen::Vector3d::Zero()) <= 0.0) {
        return true;
    }
    static constexpr double g = 0.5773502691896257;
    static constexpr std::array<std::array<double, 3>, 8> gps{{
        {{-g, -g, -g}},
        {{g, -g, -g}},
        {{-g, g, -g}},
        {{g, g, -g}},
        {{-g, -g, g}},
        {{g, -g, g}},
        {{-g, g, g}},
        {{g, g, g}},
    }};
    for (const auto& gp : gps) {
        if (hex8_jac_det(x, Eigen::Vector3d(gp[0], gp[1], gp[2])) <= 0.0) {
            return true;
        }
    }
    return false;
}

bool tet_inverted(const std::array<std::uint32_t, 4>& n,
                  const std::vector<Eigen::Vector3d>& nodes, double vol_eps) {
    return tet_vol(nodes[n[0]], nodes[n[1]], nodes[n[2]], nodes[n[3]]) <= vol_eps;
}

bool pyramid_inverted(const std::array<std::uint32_t, 5>& n,
                      const std::vector<Eigen::Vector3d>& nodes, double vol_eps) {
    const double v1 = tet_vol(nodes[n[0]], nodes[n[1]], nodes[n[2]], nodes[n[4]]);
    const double v2 = tet_vol(nodes[n[0]], nodes[n[2]], nodes[n[3]], nodes[n[4]]);
    return std::abs(v1) <= vol_eps || std::abs(v2) <= vol_eps;
}

void emit_pyramid(MixedFillOutput& out, std::uint32_t n0, std::uint32_t n1, std::uint32_t n2,
                  std::uint32_t n3, std::uint32_t apex) {
    MixedCell pyr;
    pyr.kind = MixedCellKind::kPyramid5;
    pyr.n_nodes = 5;
    const Eigen::Vector3d& p0 = out.nodes[n0];
    const Eigen::Vector3d& p1 = out.nodes[n1];
    const Eigen::Vector3d& p2 = out.nodes[n2];
    const Eigen::Vector3d nrm = (p1 - p0).cross(p2 - p0);
    const bool apex_on_positive = nrm.dot(out.nodes[apex] - p0) > 0.0;
    if (apex_on_positive) {
        pyr.nodes[0] = n0;
        pyr.nodes[1] = n1;
        pyr.nodes[2] = n2;
        pyr.nodes[3] = n3;
    } else {
        pyr.nodes[0] = n0;
        pyr.nodes[1] = n3;
        pyr.nodes[2] = n2;
        pyr.nodes[3] = n1;
    }
    pyr.nodes[4] = apex;
    out.cells.push_back(pyr);
    ++out.n_pyramid;
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

} // namespace

MixedFillOutput mixed_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                                   std::span<const geom::SharpEdge> features,
                                   double feature_band,
                                   std::span<const Eigen::Vector3d> curvature_seeds,
                                   double seed_band, bool snap_boundary,
                                   double curvature_turn_deg) {
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

    // Budget for 2:1 fine subcells (up to 8× in refined bands).
    constexpr long kHybridMaxCoarse = 48 * 1024;
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kHybridMaxCoarse, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h_use);
    const auto inside = classify_cells_inside(surface, grid);
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

    // Free-surface hop skin only when no geo drivers (unit boxes). With
    // feature/seed/curvature, refine those bands to h/2 instead of flooding
    // the exterior.
    const int skin_cap = std::max(1, (max_dist + 1) / 2);
    const bool have_geo =
        (feature_band > 0.0) || (seed_band > 0.0) || (curvature_turn_deg > 0.0);
    const int skin_use = have_geo ? 0 : std::min(skin_layers, skin_cap);

    std::vector<char> is_fine(inside.size(), 0);
    std::vector<char> is_feature_skin(inside.size(), 0);
    std::vector<char> is_seed_skin(inside.size(), 0);

    for (int k = 0; k < nz; ++k) {
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
        // Free-surface gap-close only (2 hops). Spatial seeds already cover the
        // hole ring; a long free-surface BFS floods flat box faces and kills
        // bulk/fine contrast on the exterior.
        {
            constexpr int kFsGapHops = 2;
            for (int pass = 0; pass < kFsGapHops; ++pass) {
                std::vector<char> promote(inside.size(), 0);
                for (int k = 0; k < nz; ++k) {
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
        }
        // Transition cells host apex-fan tets. A fan at the free surface gets
        // squashed when the wall snap pulls its base nodes through the apex
        // plane (degenerate boundary tets, the fan "rings" seen mid-bore) —
        // promote free-surface transition cells to fine so the 2:1 interface
        // always sits one cell inside the wall. Promotion hangs new mids, so
        // iterate to a fixed point (monotone: is_fine only grows).
        for (int guard = 0; guard < 64; ++guard) {
            std::fill(is_transition.begin(), is_transition.end(), 0);
            for (int k = 0; k < nz; ++k) {
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
            bool changed = false;
            for (int k = 0; k < nz; ++k) {
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = idx(i, j, k);
                        if (is_transition[id] && is_free_surface(i, j, k)) {
                            is_fine[id] = 1;
                            is_transition[id] = 0;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed) {
                break;
            }
        }
        out.h_fine = 0.5 * h_cell;
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

    auto emit_boundary_quad_fine = [&](int I0, int J0, int K0, int I1, int J1, int K1, int I2,
                                       int J2, int K2, int I3, int J3, int K3) {
        out.boundary_quads.push_back({{node_fine(I0, J0, K0), node_fine(I1, J1, K1),
                                       node_fine(I2, J2, K2), node_fine(I3, J3, K3)}});
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto id = idx(i, j, k);
                if (!inside[id]) {
                    continue;
                }

                if (size_adaptive && is_fine[id]) {
                    // 2×2×2 fine hexes at h/2 (all hex; product expand → pyramids).
                    // Free-surface pyramids here would break face match with interior
                    // fine hexes and show as jagged exterior patches in the wireframe.
                    ++out.n_fine_cells;
                    for (int c = 0; c < 2; ++c) {
                        for (int b = 0; b < 2; ++b) {
                            for (int a = 0; a < 2; ++a) {
                                emit_hex(out, fine_sub_corners(i, j, k, a, b, c));
                            }
                        }
                    }
                    // Boundary quads at h/2 on free faces.
                    for (std::size_t f = 0; f < 6; ++f) {
                        const auto& o = kFaceNbr[f];
                        if (inb(i + o[0], j + o[1], k + o[2])) {
                            continue;
                        }
                        // 4 child quads on this coarse face.
                        const auto& fl = kHexFaces[f];
                        std::array<std::array<int, 3>, 4> lc{};
                        for (int qn = 0; qn < 4; ++qn) {
                            const auto& corner = kHexCornerLocal[static_cast<std::size_t>(
                                fl[static_cast<std::size_t>(qn)])];
                            lc[static_cast<std::size_t>(qn)] = {
                                {2 * corner[0], 2 * corner[1], 2 * corner[2]}};
                        }
                        const int fcx = (lc[0][0] + lc[1][0] + lc[2][0] + lc[3][0]) / 4;
                        const int fcy = (lc[0][1] + lc[1][1] + lc[2][1] + lc[3][1]) / 4;
                        const int fcz = (lc[0][2] + lc[1][2] + lc[2][2] + lc[3][2]) / 4;
                        for (int q = 0; q < 4; ++q) {
                            const int qn = (q + 1) % 4;
                            const int qp = (q + 3) % 4;
                            const auto& A = lc[static_cast<std::size_t>(q)];
                            const auto& B = lc[static_cast<std::size_t>(qn)];
                            const auto& P = lc[static_cast<std::size_t>(qp)];
                            const int mx = (A[0] + B[0]) / 2, my = (A[1] + B[1]) / 2,
                                      mz = (A[2] + B[2]) / 2;
                            const int pmx = (A[0] + P[0]) / 2, pmy = (A[1] + P[1]) / 2,
                                      pmz = (A[2] + P[2]) / 2;
                            emit_boundary_quad_fine(2 * i + A[0], 2 * j + A[1], 2 * k + A[2],
                                                    2 * i + mx, 2 * j + my, 2 * k + mz,
                                                    2 * i + fcx, 2 * j + fcy, 2 * k + fcz,
                                                    2 * i + pmx, 2 * j + pmy, 2 * k + pmz);
                        }
                    }
                    continue;
                }

                if (size_adaptive && is_transition[id]) {
                    // Conforming 2:1 closure (v4): apex fan over each face polygon.
                    // Face polygon = 4 corners + the hanging mid of every split
                    // edge. Both cells sharing a face build the same polygon and
                    // the same canonical fan, so every facet pairs — no cracks.
                    ++out.n_transition_cells;
                    const auto c = coarse_corners(i, j, k);
                    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
                    for (int t = 0; t < 8; ++t) {
                        ctr += out.nodes[c[static_cast<std::size_t>(t)]];
                    }
                    ctr /= 8.0;
                    const auto apex = static_cast<std::uint32_t>(out.nodes.size());
                    out.nodes.push_back(ctr);

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
                    continue;
                }

                // Bulk hex (or plain-mode skin as pyramids at h).
                const auto c = coarse_corners(i, j, k);
                if (!size_adaptive && is_fine[id]) {
                    // Plain hybrid: free-surface skin pyramids at bulk h.
                    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
                    for (int t = 0; t < 8; ++t) {
                        ctr += out.nodes[c[static_cast<std::size_t>(t)]];
                    }
                    ctr /= 8.0;
                    const auto apex = static_cast<std::uint32_t>(out.nodes.size());
                    out.nodes.push_back(ctr);
                    emit_cell_pyramids(out, c, apex);
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

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const double vol_eps = 1e-14 * h_cell * h_cell * h_cell;
        // Use bulk h as move/search budget (fine nodes still reproject within it).
        out.boundary_max_distance =
            snap_boundary_nodes(
                surface, out.nodes, bnodes, h_cell,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : out.cells) {
                        bool bad = false;
                        if (cell.kind == MixedCellKind::kTet4) {
                            bad = tet_inverted(
                                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]},
                                out.nodes, vol_eps);
                        } else if (cell.kind == MixedCellKind::kPyramid5) {
                            bad =
                                pyramid_inverted({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                                  cell.nodes[3], cell.nodes[4]},
                                                 out.nodes, vol_eps);
                        } else {
                            bad = hex_inverted({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                                cell.nodes[3], cell.nodes[4], cell.nodes[5],
                                                cell.nodes[6], cell.nodes[7]},
                                               out.nodes);
                        }
                        if (!bad) {
                            continue;
                        }
                        for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                            offenders.insert(cell.nodes[m]);
                        }
                    }
                },
                /*max_move_frac=*/1.25, /*passes=*/8, features)
                .max_residual;
    }
    return out;
}

MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill) {
    MixedFillOutput out;
    out.h = fill.h;
    out.h_fine = fill.h_fine;
    out.boundary_quads = fill.boundary_quads;
    out.boundary_max_distance = fill.boundary_max_distance;
    out.skin_layers = fill.skin_layers;
    out.n_feature_skin_cells = fill.n_feature_skin_cells;
    out.n_fine_cells = fill.n_fine_cells;
    out.n_transition_cells = fill.n_transition_cells;
    out.nodes = fill.nodes;
    out.n_hex = 0;
    out.n_pyramid = 0;
    out.n_tet = 0;
    out.cells.reserve(fill.cells.size() + 5 * fill.n_hex);

    for (const auto& cell : fill.cells) {
        if (cell.kind == MixedCellKind::kTet4) {
            out.cells.push_back(cell);
            ++out.n_tet;
            continue;
        }
        if (cell.kind == MixedCellKind::kPyramid5) {
            out.cells.push_back(cell);
            ++out.n_pyramid;
            continue;
        }
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (int i = 0; i < 8; ++i) {
            center += out.nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        center /= 8.0;
        const auto apex = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(center);
        for (const auto& face : kHexFaces) {
            MixedCell pyr;
            pyr.kind = MixedCellKind::kPyramid5;
            pyr.n_nodes = 5;
            const Eigen::Vector3d& p0 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[0])]];
            const Eigen::Vector3d& p1 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[1])]];
            const Eigen::Vector3d& p2 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[2])]];
            const Eigen::Vector3d n = (p1 - p0).cross(p2 - p0);
            const bool apex_on_positive = n.dot(center - p0) > 0.0;
            if (apex_on_positive) {
                pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
                pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[1])];
                pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
                pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[3])];
            } else {
                pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
                pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[3])];
                pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
                pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[1])];
            }
            pyr.nodes[4] = apex;
            out.cells.push_back(pyr);
            ++out.n_pyramid;
        }
    }
    return out;
}

} // namespace polymesh::mesh
