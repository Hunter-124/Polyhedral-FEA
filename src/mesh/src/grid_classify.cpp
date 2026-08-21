// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/grid_classify.hpp"

#include "mesh/poly_mesh.hpp"
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <vector>

#if defined(POLYMESH_WITH_OPENMP)
#include <omp.h>
#endif

namespace polymesh::mesh {
namespace {

void validate_h_bbox(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                     double h, const char* where) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError(std::format("{}: h must be positive and finite", where));
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0 || !extent.allFinite()) {
        throw ValidityError(std::format("{}: empty or non-finite bounding box", where));
    }
}

int cells_for_extent(double extent, double h) {
    // ceil(extent/h) with a tiny slack so exact divisions stay exact (e.g. 1/0.1).
    return std::max(1, static_cast<int>(std::ceil(extent / h - 1e-14)));
}

/// Merge nearly-equal ray hits so a shared edge / face diagonal between two
/// coplanar triangles contributes one crossing, not two (parity flip → void).
void dedupe_crossings(std::vector<double>& crossings, double zeps) {
    if (crossings.empty()) {
        return;
    }
    std::sort(crossings.begin(), crossings.end());
    std::size_t w = 1;
    for (std::size_t r = 1; r < crossings.size(); ++r) {
        if (crossings[r] - crossings[w - 1] > zeps) {
            crossings[w++] = crossings[r];
        }
    }
    crossings.resize(w);
}

std::vector<bool> classify_impl(const geom::TriSurface& surface, const CartesianGrid& grid,
                                int ray_axis) {
    const int a0 = (ray_axis + 1) % 3;
    const int a1 = (ray_axis + 2) % 3;
    // Lattice is always (nx,ny,nz) in xyz. Plane axes a0/a1, ray along ray_axis.
    // IMPORTANT: use uint8_t (not vector<bool>) for the parallel fill — vector<bool>
    // packs bits so concurrent writes to different indices still race on the same word.
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(grid.cell_count()), 0);

    const double char_len = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1.0});
    const double zeps = 1e-12 * char_len + 1e-9 * char_len;
    const double area_eps = 1e-30 * char_len * char_len;

    const int n_axis[3] = {grid.nx, grid.ny, grid.nz};
    const int ni = n_axis[a0];
    const int nj = n_axis[a1];
    const int nk = n_axis[ray_axis];

    // Parallel over plane rows: each (i,j) owns a disjoint set of cells along the ray.
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(dynamic, 4)
#endif
    for (int j = 0; j < nj; ++j) {
        for (int i = 0; i < ni; ++i) {
            const double c0 = grid.origin[a0] + (static_cast<double>(i) + 0.5) * grid.cell[a0];
            const double c1 = grid.origin[a1] + (static_cast<double>(j) + 0.5) * grid.cell[a1];

            std::vector<double> crossings;
            crossings.reserve(surface.triangles.size() / 8 + 4);
            for (const auto& tri : surface.triangles) {
                const Eigen::Vector3d& A = surface.vertices[tri[0]];
                const Eigen::Vector3d& B = surface.vertices[tri[1]];
                const Eigen::Vector3d& C = surface.vertices[tri[2]];
                const double d1 =
                    (B[a0] - A[a0]) * (c1 - A[a1]) - (B[a1] - A[a1]) * (c0 - A[a0]);
                const double d2 =
                    (C[a0] - B[a0]) * (c1 - B[a1]) - (C[a1] - B[a1]) * (c0 - B[a0]);
                const double d3 =
                    (A[a0] - C[a0]) * (c1 - C[a1]) - (A[a1] - C[a1]) * (c0 - C[a0]);
                const bool has_neg = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
                const bool has_pos = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
                if (has_neg && has_pos) {
                    continue;
                }
                const double area = d1 + d2 + d3;
                if (std::abs(area) <= area_eps) {
                    continue;
                }
                crossings.push_back((d2 * A[ray_axis] + d3 * B[ray_axis] + d1 * C[ray_axis]) /
                                    area);
            }
            dedupe_crossings(crossings, zeps);

            for (int k = 0; k < nk; ++k) {
                const double cs = grid.origin[ray_axis] +
                                  (static_cast<double>(k) + 0.5) * grid.cell[ray_axis];
                const auto below = std::upper_bound(crossings.begin(), crossings.end(), cs) -
                                   crossings.begin();
                if (below % 2 != 1) {
                    continue;
                }
                // Map (i,j,k) on (a0,a1,ray) back to (ix,iy,iz).
                int ix = 0, iy = 0, iz = 0;
                int* comps[3] = {&ix, &iy, &iz};
                *comps[a0] = i;
                *comps[a1] = j;
                *comps[ray_axis] = k;
                inside[grid.index(ix, iy, iz)] = 1;
            }
        }
    }
    // API keeps vector<bool>; conversion is serial and cheap vs the classify.
    return std::vector<bool>(inside.begin(), inside.end());
}

} // namespace

double min_h_for_cell_budget(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             long max_cells, int subdivision) {
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0 || !extent.allFinite() || max_cells < 1) {
        return 0.0;
    }
    const int sub = std::max(1, subdivision);
    // Isotropic: (sub * L_i / h) product ≤ max_cells ⇒ h ≥ sub * cbrt(∏ L / max).
    const double vol = std::max(extent[0] * extent[1] * extent[2], 1e-300);
    const double h_iso =
        static_cast<double>(sub) * std::cbrt(vol / static_cast<double>(max_cells));
    // Axis-wise lower bound: each n_i ≥ 1, but also n_i ≈ sub*L_i/h; if one axis
    // is very short, isotropic still works; pad 5% for ceil/even rounding.
    return h_iso * 1.05;
}

namespace {

void set_cell_from_n(CartesianGrid& g, const Eigen::Vector3d& extent) {
    g.cell[0] = extent[0] / static_cast<double>(std::max(1, g.nx));
    g.cell[1] = extent[1] / static_cast<double>(std::max(1, g.ny));
    g.cell[2] = extent[2] / static_cast<double>(std::max(1, g.nz));
}

/// Shrink n so nx*ny*nz ≤ max_cells while keeping n ≥ min_n and optional even.
void fit_cell_budget(CartesianGrid& g, const Eigen::Vector3d& extent, long max_cells,
                     int min_n, bool even) {
    if (max_cells < 1) {
        max_cells = 1;
    }
    min_n = std::max(1, min_n);
    if (even && (min_n % 2)) {
        ++min_n;
    }
    // Snap toward coarser (never finer): odd → n-1 when even required.
    auto snap = [&](int n) {
        n = std::max(min_n, n);
        if (even && (n % 2)) {
            n = std::max(min_n, n - 1);
        }
        return n;
    };

    for (int attempt = 0; attempt < 64 && g.cell_count() > max_cells; ++attempt) {
        const double ratio =
            static_cast<double>(max_cells) / static_cast<double>(std::max(1L, g.cell_count()));
        const double scale = std::cbrt(ratio) * 0.999;
        auto shrink_axis = [&](int n) {
            int n2 = snap(static_cast<int>(std::floor(static_cast<double>(n) * scale)));
            if (n2 >= n && n > min_n) {
                n2 = snap(n - (even ? 2 : 1));
            }
            return n2;
        };
        const int nx0 = g.nx, ny0 = g.ny, nz0 = g.nz;
        g.nx = shrink_axis(g.nx);
        g.ny = shrink_axis(g.ny);
        g.nz = shrink_axis(g.nz);
        if (g.nx == nx0 && g.ny == ny0 && g.nz == nz0) {
            break; // at min — fall through to proportional reassignment
        }
        set_cell_from_n(g, extent);
    }

    if (g.cell_count() > max_cells) {
        // Last resort: distribute max_cells ∝ extent, honouring min_n / even.
        const double ex = std::max(extent[0], 1e-300);
        const double ey = std::max(extent[1], 1e-300);
        const double ez = std::max(extent[2], 1e-300);
        const double s = std::cbrt(static_cast<double>(max_cells) / (ex * ey * ez));
        g.nx = snap(static_cast<int>(std::floor(ex * s)));
        g.ny = snap(static_cast<int>(std::floor(ey * s)));
        g.nz = snap(static_cast<int>(std::floor(ez * s)));
        while (g.cell_count() > max_cells) {
            if (g.nx >= g.ny && g.nx >= g.nz && g.nx > min_n) {
                g.nx = snap(g.nx - (even ? 2 : 1));
            } else if (g.ny >= g.nz && g.ny > min_n) {
                g.ny = snap(g.ny - (even ? 2 : 1));
            } else if (g.nz > min_n) {
                g.nz = snap(g.nz - (even ? 2 : 1));
            } else {
                break;
            }
        }
        set_cell_from_n(g, extent);
    }

    if (g.cell_count() > max_cells) {
        // Pathological: min_n³ > max_cells — emit densest legal min grid.
        g.nx = min_n;
        g.ny = min_n;
        g.nz = min_n;
        set_cell_from_n(g, extent);
    }
}

} // namespace

CartesianGrid make_bbox_grid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             double h, long max_cells) {
    validate_h_bbox(bbox_min, bbox_max, h, "make_bbox_grid");
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    CartesianGrid g;
    g.origin = bbox_min;
    g.nx = cells_for_extent(extent[0], h);
    g.ny = cells_for_extent(extent[1], h);
    g.nz = cells_for_extent(extent[2], h);
    set_cell_from_n(g, extent);
    if (g.cell_count() > max_cells) {
        fit_cell_budget(g, extent, max_cells, /*min_n=*/1, /*even=*/false);
    }
    return g;
}

CartesianGrid make_bbox_grid_even(const Eigen::Vector3d& bbox_min,
                                  const Eigen::Vector3d& bbox_max, double h, int min_cells,
                                  long max_cells) {
    validate_h_bbox(bbox_min, bbox_max, h, "make_bbox_grid_even");
    if (min_cells < 2) {
        min_cells = 2;
    }
    if (min_cells % 2) {
        ++min_cells;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    CartesianGrid g;
    g.origin = bbox_min;
    auto even_n = [&](double ext) {
        int n = std::max(min_cells, cells_for_extent(ext, h));
        if (n % 2) {
            ++n;
        }
        return n;
    };
    g.nx = even_n(extent[0]);
    g.ny = even_n(extent[1]);
    g.nz = even_n(extent[2]);
    set_cell_from_n(g, extent);
    if (g.cell_count() > max_cells) {
        fit_cell_budget(g, extent, max_cells, min_cells, /*even=*/true);
    }
    return g;
}

std::vector<bool> classify_cells_inside(const geom::TriSurface& surface,
                                        const CartesianGrid& grid) {
    return classify_impl(surface, grid, /*ray_axis=*/2);
}

FeatureAwareClassification
classify_cells_feature_aware(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                             const Eigen::Vector3d& bbox_max, double h, long max_cells,
                             double relative_volume_tolerance, int max_refinement_levels,
                             const std::function<double(const Eigen::Vector3d&)>& size_field,
                             bool even_cells) {
    validate_h_bbox(bbox_min, bbox_max, h, "classify_cells_feature_aware");
    if (max_cells < 1) {
        throw ValidityError("classify_cells_feature_aware: max_cells must be positive");
    }
    if (!(relative_volume_tolerance > 0.0) || !std::isfinite(relative_volume_tolerance)) {
        throw ValidityError(
            "classify_cells_feature_aware: relative volume tolerance must be positive");
    }
    (void)size_field;

    // Closed-surface volume in divergence form about bbox_min. Translating the
    // origin keeps the triple products small for parts far from world zero.
    double signed_surface_volume = 0.0;
    for (const auto& tri : surface.triangles) {
        if (tri[0] >= surface.vertices.size() || tri[1] >= surface.vertices.size() ||
            tri[2] >= surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d a = surface.vertices[tri[0]] - bbox_min;
        const Eigen::Vector3d b = surface.vertices[tri[1]] - bbox_min;
        const Eigen::Vector3d c = surface.vertices[tri[2]] - bbox_min;
        signed_surface_volume += a.dot(b.cross(c)) / 6.0;
    }
    const double surface_volume = std::abs(signed_surface_volume);
    const auto relative_error = [&](double classified_volume) {
        return surface_volume > 0.0 && std::isfinite(surface_volume)
                   ? std::abs(classified_volume - surface_volume) / surface_volume
                   : 0.0;
    };

    FeatureAwareClassification out;
    // Even cell counts (opt-in) put every bbox mid-plane on a lattice plane, as
    // the alternating 5-tet split in mesh/lattice_split.hpp requires.
    out.grid = even_cells
                   ? make_bbox_grid_even(bbox_min, bbox_max, h, /*min_cells=*/2, max_cells)
                   : make_bbox_grid(bbox_min, bbox_max, h, max_cells);
    out.inside = classify_cells_inside(surface, out.grid);
    out.surface_volume = surface_volume;
    out.classified_volume =
        static_cast<double>(std::count(out.inside.begin(), out.inside.end(), true)) *
        out.grid.cell.prod();
    out.relative_volume_error = relative_error(out.classified_volume);
    if (max_refinement_levels <= 0) {
        return out;
    }
    out.coarse_inside = out.inside;

    // The h/2 lattice is a sampler, not the delivered lattice. Its exact 2×
    // indexing lets callers subdivide only parents whose children mix solid
    // and void, without making every bulk cell pay the global 8× cost.
    CartesianGrid fine;
    fine.origin = out.grid.origin;
    fine.cell = 0.5 * out.grid.cell;
    fine.nx = 2 * out.grid.nx;
    fine.ny = 2 * out.grid.ny;
    fine.nz = 2 * out.grid.nz;
    const auto fine_inside = classify_cells_inside(surface, fine);

    out.child_inside_mask.assign(out.inside.size(), std::uint8_t{0});
    std::size_t n_inside_children = 0;
    for (int k = 0; k < out.grid.nz; ++k) {
        for (int j = 0; j < out.grid.ny; ++j) {
            for (int i = 0; i < out.grid.nx; ++i) {
                std::uint8_t mask = 0;
                for (int d = 0; d < 2; ++d) {
                    for (int b = 0; b < 2; ++b) {
                        for (int a = 0; a < 2; ++a) {
                            if (!fine_inside[fine.index(2 * i + a, 2 * j + b, 2 * k + d)]) {
                                continue;
                            }
                            const int bit = a + 2 * b + 4 * d;
                            mask |= static_cast<std::uint8_t>(1U << bit);
                            ++n_inside_children;
                        }
                    }
                }
                const auto id = out.grid.index(i, j, k);
                out.child_inside_mask[id] = mask;
                out.inside[id] = mask != 0;
                if (mask != 0 && mask != std::uint8_t{0xff}) {
                    ++out.n_mixed_cells;
                }
            }
        }
    }
    out.refinement_levels = out.n_mixed_cells > 0 ? 1 : 0;
    out.classified_volume = static_cast<double>(n_inside_children) * fine.cell.prod();
    out.relative_volume_error = relative_error(out.classified_volume);
    return out;
}

CanonicalCellMap canonical_cell_map(const CartesianGrid& grid, const MirrorFrame& frame) {
    CanonicalCellMap map;
    if (!frame.any() || grid.cell_count() < 1) {
        return map;
    }
    const Eigen::Vector3d extent{grid.cell[0] * static_cast<double>(grid.nx),
                                 grid.cell[1] * static_cast<double>(grid.ny),
                                 grid.cell[2] * static_cast<double>(grid.nz)};
    const double tol = 1e-9 * extent.norm();
    const int n[3] = {grid.nx, grid.ny, grid.nz};
    bool any_axis = false;
    for (int a = 0; a < 3; ++a) {
        const double mid = grid.origin[a] + 0.5 * extent[a];
        map.axis[static_cast<std::size_t>(a)] =
            frame.plane[static_cast<std::size_t>(a)] && std::abs(mid - frame.center[a]) <= tol;
        any_axis = any_axis || map.axis[static_cast<std::size_t>(a)];
    }
    if (!any_axis) {
        return map;
    }
    const auto count = static_cast<std::size_t>(grid.cell_count());
    map.canonical.resize(count);
    map.flipped.assign(count, std::uint8_t{0});
    for (int k = 0; k < grid.nz; ++k) {
        for (int j = 0; j < grid.ny; ++j) {
            for (int i = 0; i < grid.nx; ++i) {
                const int ijk[3] = {i, j, k};
                int canonical[3] = {i, j, k};
                std::uint8_t flipped = 0;
                for (int a = 0; a < 3; ++a) {
                    if (!map.axis[static_cast<std::size_t>(a)]) {
                        continue;
                    }
                    const int mirror = n[a] - 1 - ijk[a];
                    if (mirror < ijk[a]) {
                        canonical[a] = mirror;
                        flipped |= static_cast<std::uint8_t>(1U << a);
                    }
                }
                const auto id = grid.index(i, j, k);
                map.canonical[id] = static_cast<std::uint32_t>(
                    grid.index(canonical[0], canonical[1], canonical[2]));
                map.flipped[id] = flipped;
            }
        }
    }
    return map;
}

void symmetrise_classification(FeatureAwareClassification& classification,
                               const CanonicalCellMap& map) {
    if (!map.active() || map.canonical.size() != classification.inside.size()) {
        return;
    }
    const auto count = classification.inside.size();
    const auto inside_before = classification.inside;
    for (std::size_t c = 0; c < count; ++c) {
        classification.inside[c] = inside_before[map.canonical[c]];
    }
    if (classification.coarse_inside.size() == count) {
        const auto coarse_before = classification.coarse_inside;
        for (std::size_t c = 0; c < count; ++c) {
            classification.coarse_inside[c] = coarse_before[map.canonical[c]];
        }
    }
    if (classification.child_inside_mask.size() != count) {
        return;
    }
    // Child bit b = a + 2b + 4d is the child's own octant inside the parent, so
    // reflecting parent axis `a` swaps the two children along `a`: bit index XOR
    // (1 << a). The flipped mask is already in that encoding.
    const auto mask_before = classification.child_inside_mask;
    std::size_t n_mixed = 0;
    std::size_t n_inside_children = 0;
    for (std::size_t c = 0; c < count; ++c) {
        const std::uint8_t source = mask_before[map.canonical[c]];
        const std::uint8_t flip = map.flipped[c];
        std::uint8_t mask = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((source & static_cast<std::uint8_t>(1U << (bit ^ flip))) != 0) {
                mask |= static_cast<std::uint8_t>(1U << bit);
            }
        }
        classification.child_inside_mask[c] = mask;
        classification.inside[c] = mask != 0;
        if (mask != 0 && mask != std::uint8_t{0xff}) {
            ++n_mixed;
        }
        for (int bit = 0; bit < 8; ++bit) {
            if ((mask & static_cast<std::uint8_t>(1U << bit)) != 0) {
                ++n_inside_children;
            }
        }
    }
    classification.n_mixed_cells = n_mixed;
    classification.refinement_levels = n_mixed > 0 ? 1 : 0;
    const double child_volume = 0.125 * classification.grid.cell.prod();
    classification.classified_volume = static_cast<double>(n_inside_children) * child_volume;
    classification.relative_volume_error =
        classification.surface_volume > 0.0 && std::isfinite(classification.surface_volume)
            ? std::abs(classification.classified_volume - classification.surface_volume) /
                  classification.surface_volume
            : 0.0;
}

std::vector<bool> classify_cells_inside_axis(const geom::TriSurface& surface,
                                             const CartesianGrid& grid, int ray_axis) {
    if (ray_axis < 0 || ray_axis > 2) {
        throw ValidityError("classify_cells_inside_axis: ray_axis must be 0, 1, or 2");
    }
    return classify_impl(surface, grid, ray_axis);
}

} // namespace polymesh::mesh
