// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"
#include "geom/stl.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace polymesh::mesh;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    return s;
}

} // namespace

TEST_CASE("closest_on_surface hits box corner") {
    const auto s = unit_box();
    const auto c = closest_on_surface(s, {-0.1, -0.1, -0.1});
    REQUIRE(c.distance < 0.2);
    REQUIRE(c.point.minCoeff() >= -1e-12);
}

TEST_CASE("graded tet fill emits more tets than uniform coarse") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 2);
    REQUIRE_FALSE(graded.mesh.tets.empty());
    REQUIRE(graded.n_fine_cells > 0);
    // Uniform tet at h=0.5 on unit box: roughly 8 cells * 6 tets
    auto uniform = tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, false);
    // Graded should typically produce more tets due to fine skin
    REQUIRE(graded.mesh.tets.size() >= uniform.tets.size());
    check_tet_fill_geometry(graded.mesh);
}

TEST_CASE("pipeline graded mesher builds valid nodal mesh") {
    const auto s = unit_box();
    // write minimal model path via volume_mesh needs Model - build manually
    polymesh::pipeline::Model m;
    m.surface = s;
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(s.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 0.5, polymesh::pipeline::VolumeMesher::kGradedTet, 2);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
}

TEST_CASE("feature band refines more cells than surface skin alone") {
    const auto s = unit_box();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    REQUIRE_FALSE(edges.empty());
    // Large h → few cells; feature_band covering the whole box should force
    // more fine blocks than skin-only grading.
    auto skin_only = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, {}, 0.0);
    auto with_feat = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, 1, edges, 2.0);
    REQUIRE(with_feat.n_fine_cells >= skin_only.n_fine_cells);
    REQUIRE(with_feat.n_feature_cells > 0);
    REQUIRE(with_feat.mesh.tets.size() >= skin_only.mesh.tets.size());
    check_tet_fill_geometry(with_feat.mesh);
}

TEST_CASE("feature/seed bands refine more blocks at multi-level lattice") {
    // Features/seeds densify which blocks are L1/L2; same coarse lattice.
    const auto s = unit_box();
    const auto edges = polymesh::geom::detect_sharp_edges(s, 30.0);
    auto plain = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, {}, 0.0);
    auto feat = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, edges, 0.8);
    REQUIRE(plain.subdivision == 2);
    REQUIRE(feat.subdivision == 2);
    // Same coarse spacing; features force more fine cells / tets.
    REQUIRE(std::abs(feat.h_coarse - plain.h_coarse) < 1e-12);
    REQUIRE(feat.n_feature_cells > 0);
    REQUIRE(feat.n_fine_cells >= plain.n_fine_cells);
    REQUIRE(feat.mesh.tets.size() >= plain.mesh.tets.size());
    check_tet_fill_geometry(feat.mesh);

    // Seed balls alone mark L2 blocks (no sharp edges needed).
    std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.0}, {0.5, 0.5, 1.0}};
    auto seeded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.4, 1, {}, 0.0, seeds, 0.5);
    REQUIRE(seeded.subdivision == 2);
    REQUIRE(seeded.n_seed_cells > 0);
    // L2 active → h_fine ~ h/4.
    REQUIRE(seeded.h_fine < 0.55 * seeded.h_coarse);
    check_tet_fill_geometry(seeded.mesh);
}

TEST_CASE("graded tet bulk size tracks requested h (multi-level)") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, 2);
    REQUIRE(graded.subdivision == 2);
    // h_coarse ≈ h; skin-only → L1 → h_fine ≈ h/2 (within lattice rounding).
    REQUIRE(graded.h_coarse > 0.2);
    REQUIRE(graded.h_coarse < 0.35);
    REQUIRE(graded.h_fine > 0.1);
    REQUIRE(graded.h_fine < 0.2);
    REQUIRE(graded.n_coarse_cells > 0);
    REQUIRE(graded.n_fine_cells > 0);
    check_tet_fill_geometry(graded.mesh);
}

TEST_CASE("graded multi-level has finer edges near seeds than bulk") {
    const auto s = unit_box();
    std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.5}};
    auto graded =
        graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, 1, {}, 0.0, seeds, 0.35);
    REQUIRE(graded.n_seed_cells > 0);
    REQUIRE_FALSE(graded.mesh.tets.empty());

    // Classify tets by centroid distance to seed; compare median edge lengths.
    std::vector<double> near_edges;
    std::vector<double> far_edges;
    const Eigen::Vector3d seed = seeds[0];
    for (const auto& t : graded.mesh.tets) {
        const Eigen::Vector3d c =
            0.25 * (graded.mesh.nodes[t[0]] + graded.mesh.nodes[t[1]] +
                    graded.mesh.nodes[t[2]] + graded.mesh.nodes[t[3]]);
        double elen = 0.0;
        int ne = 0;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                elen += (graded.mesh.nodes[t[static_cast<std::size_t>(a)]] -
                         graded.mesh.nodes[t[static_cast<std::size_t>(b)]])
                            .norm();
                ++ne;
            }
        }
        elen /= static_cast<double>(ne);
        if ((c - seed).norm() < 0.35) {
            near_edges.push_back(elen);
        } else if ((c - seed).norm() > 0.55) {
            far_edges.push_back(elen);
        }
    }
    REQUIRE_FALSE(near_edges.empty());
    REQUIRE_FALSE(far_edges.empty());
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double m_near = median(near_edges);
    const double m_far = median(far_edges);
    // Far bulk should be coarser than the seed band (visible size field).
    REQUIRE(m_far > 1.35 * m_near);
}

TEST_CASE("pipeline graded with feature_refine notes feature blocks") {
    polymesh::pipeline::Model m;
    m.surface = unit_box();
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(m.surface.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 0.5, polymesh::pipeline::VolumeMesher::kGradedTet, 1, true);
    REQUIRE(vol.mesher_note.find("feature") != std::string::npos);
    REQUIRE_NOTHROW(vol.mesh.check_validity());
}

TEST_CASE("graded tet tiny h auto-coarsens instead of throwing grid too fine") {
    // Previously: make_bbox_grid_even(h/2) blew past 512k cells and threw
    // "grid too fine; increase element size" — product graded path must mesh.
    const auto s = unit_box();
    REQUIRE_NOTHROW(graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 1e-4, 2));
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 1e-4, 2);
    REQUIRE_FALSE(graded.mesh.tets.empty());
    REQUIRE(graded.h_coarse > 1e-4); // raised to cell budget
    REQUIRE_NOTHROW(check_tet_fill_geometry(graded.mesh));

    polymesh::pipeline::Model m;
    m.surface = s;
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.region_count = 1;
    m.triangle_region.assign(s.triangles.size(), 0);
    auto vol = polymesh::pipeline::volume_mesh(
        m, 1e-4, polymesh::pipeline::VolumeMesher::kGradedTet, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
}

TEST_CASE("make_bbox_grid_even respects max_cells budget") {
    using polymesh::mesh::make_bbox_grid_even;
    using polymesh::mesh::kDefaultMaxGridCells;
    // Request absurdly fine even lattice on unit cube.
    auto g = make_bbox_grid_even({0, 0, 0}, {1, 1, 1}, 1e-6, 2, kDefaultMaxGridCells);
    REQUIRE(g.nx % 2 == 0);
    REQUIRE(g.ny % 2 == 0);
    REQUIRE(g.nz % 2 == 0);
    REQUIRE(g.cell_count() <= kDefaultMaxGridCells);
    REQUIRE(g.nx >= 2);
}

TEST_CASE("graded fill surface-snaps boundary (not pure staircase)") {
    const auto s = unit_box();
    auto graded = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, 2);
    REQUIRE_FALSE(graded.mesh.boundary_quads.empty());
    std::vector<std::uint32_t> bnodes;
    for (const auto& q : graded.mesh.boundary_quads) {
        bnodes.insert(bnodes.end(), q.begin(), q.end());
    }
    std::sort(bnodes.begin(), bnodes.end());
    bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
    const auto conf = surface_conformity(s, graded.mesh.nodes, bnodes);
    // After multi-pass snap, max residual should be well below one fine cell.
    REQUIRE(conf.max_distance < 0.55 * graded.h_fine);
    REQUIRE_NOTHROW(check_tet_fill_geometry(graded.mesh));
}

TEST_CASE("cylinder_prism graded+feature notes curvature seeds and snaps") {
    // Hole / curved wall fixture: feature grading should emit curv_seeds and a
    // snap residual line (geometry-variable mesh path).
    auto model = polymesh::testsupport::model_from_surface(
        polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    REQUIRE(model.surface.triangles.size() >= 4);
    // volume_mesh needs positive h (auto-h is resolve_mesh_size in the job path).
    const double h = 0.15 * (model.bbox_max - model.bbox_min).maxCoeff();
    REQUIRE(h > 0.0);
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, polymesh::pipeline::VolumeMesher::kGradedTet, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("graded") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);
    // Curved hole should register curvature seeds on a decent tessellation.
    // (Allow either curv_seeds or thin_seeds — both are geometry grading.)
    const bool geo = vol.mesher_note.find("curv_seeds") != std::string::npos ||
                     vol.mesher_note.find("thin_seeds") != std::string::npos ||
                     vol.mesher_note.find("feature") != std::string::npos;
    REQUIRE(geo);
}

namespace {

/// Fraction of tets whose reflection about the node-set mid-plane normal to
/// `axis` is also a tet of the same mesh. 1.0 means the tiling is exactly
/// mirror-symmetric; the single-orientation Kuhn lattice scored 0.0.
double mirror_tet_fraction(const std::vector<Eigen::Vector3d>& nodes,
                           const std::vector<std::array<std::uint32_t, 4>>& tets, int axis) {
    if (tets.empty()) {
        return 0.0;
    }
    Eigen::Vector3d lo = nodes.front();
    Eigen::Vector3d hi = nodes.front();
    for (const auto& p : nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const double quantum = 1e-9 * (hi - lo).norm();
    const double inv_q = quantum > 0.0 ? 1.0 / quantum : 0.0;
    const double plane = lo[axis] + hi[axis];
    using GridKey = std::array<long long, 3>;
    struct GridHash {
        std::size_t operator()(const GridKey& k) const noexcept {
            std::size_t h = static_cast<std::size_t>(k[0]) * 73856093ULL;
            h ^= static_cast<std::size_t>(k[1]) * 19349663ULL + (h << 6) + (h >> 2);
            h ^= static_cast<std::size_t>(k[2]) * 83492791ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    const auto grid_key = [&](const Eigen::Vector3d& p) {
        return GridKey{static_cast<long long>(std::llround(p.x() * inv_q)),
                       static_cast<long long>(std::llround(p.y() * inv_q)),
                       static_cast<long long>(std::llround(p.z() * inv_q))};
    };
    std::unordered_map<GridKey, std::uint32_t, GridHash> at;
    at.reserve(nodes.size() * 2);
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        at.emplace(grid_key(nodes[i]), i);
    }
    using TetKey = std::array<std::uint32_t, 4>;
    struct TetHash {
        std::size_t operator()(const TetKey& k) const noexcept {
            std::size_t h = 0;
            for (const auto v : k) {
                h ^= static_cast<std::size_t>(v) * 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            }
            return h;
        }
    };
    std::unordered_set<TetKey, TetHash> present;
    present.reserve(tets.size() * 2);
    for (const auto& t : tets) {
        TetKey k = t;
        std::sort(k.begin(), k.end());
        present.insert(k);
    }
    std::size_t mirrored = 0;
    for (const auto& t : tets) {
        TetKey k{};
        bool ok = true;
        for (int v = 0; v < 4 && ok; ++v) {
            Eigen::Vector3d p = nodes[t[static_cast<std::size_t>(v)]];
            p[axis] = plane - p[axis];
            const auto it = at.find(grid_key(p));
            if (it == at.end()) {
                ok = false;
            } else {
                k[static_cast<std::size_t>(v)] = it->second;
            }
        }
        if (!ok) {
            continue;
        }
        std::sort(k.begin(), k.end());
        mirrored += present.count(k);
    }
    return static_cast<double>(mirrored) / static_cast<double>(tets.size());
}

} // namespace

// The lattice tiling itself must be mirror-symmetric: a symmetric part meshed on
// a symmetric lattice may not come out visibly slanted. Single-orientation Kuhn
// scored exactly 0.0 here on every axis while its NODES were 100% symmetric,
// which is what made the defect invisible to node-level checks.
TEST_CASE("lattice tilings mirror about every bbox mid-plane", "[mesher][symmetry]") {
    const auto s = unit_box();
    SECTION("uniform tet fill") {
        for (const double h : {0.25, 0.2, 1.0 / 6.0}) {
            const auto fill = tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, h, false);
            REQUIRE_FALSE(fill.tets.empty());
            for (int axis = 0; axis < 3; ++axis) {
                INFO("uniform h=" << h << " axis=" << axis);
                REQUIRE(mirror_tet_fraction(fill.nodes, fill.tets, axis) == 1.0);
            }
        }
    }
    SECTION("graded fill with a symmetric size field") {
        // Radial field: finer toward the box centre, so LEB marks are symmetric
        // about all three mid-planes and only the closure can break symmetry.
        const SizeFieldFn field = [](const Eigen::Vector3d& x) {
            const double r = (x - Eigen::Vector3d::Constant(0.5)).norm();
            return 0.08 + 0.30 * r;
        };
        const auto fill = graded_tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.2, 1, {}, 0.0, {},
                                                 0.0, 0.0, nullptr, field);
        REQUIRE_FALSE(fill.mesh.tets.empty());
        for (int axis = 0; axis < 3; ++axis) {
            INFO("graded axis=" << axis << " tets=" << fill.mesh.tets.size());
            REQUIRE(mirror_tet_fraction(fill.mesh.nodes, fill.mesh.tets, axis) == 1.0);
        }
    }
}
