// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/graded_sizing.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/quality.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"
#include "support/box_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

namespace {

using polymesh::mesh::GradedTetFillOutput;
using polymesh::mesh::MixedFillOutput;
using polymesh::mesh::SizeFieldFn;

bool same_node_bytes(const std::vector<Eigen::Vector3d>& a,
                     const std::vector<Eigen::Vector3d>& b) {
    return a.size() == b.size() &&
           (a.empty() ||
            std::memcmp(a.data(), b.data(), a.size() * sizeof(Eigen::Vector3d)) == 0);
}

double tet_mean_edge(const GradedTetFillOutput& fill, std::size_t ti) {
    const auto& t = fill.mesh.tets[ti];
    double sum = 0.0;
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            sum += (fill.mesh.nodes[t[static_cast<std::size_t>(a)]] -
                    fill.mesh.nodes[t[static_cast<std::size_t>(b)]])
                       .norm();
        }
    }
    return sum / 6.0;
}

double max_adjacent_tet_edge_ratio(const GradedTetFillOutput& fill) {
    static constexpr int kFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    std::map<std::array<std::uint32_t, 3>, std::size_t> owner;
    double max_ratio = 1.0;
    for (std::size_t ti = 0; ti < fill.mesh.tets.size(); ++ti) {
        for (const auto& f : kFaces) {
            std::array<std::uint32_t, 3> key{
                fill.mesh.tets[ti][static_cast<std::size_t>(f[0])],
                fill.mesh.tets[ti][static_cast<std::size_t>(f[1])],
                fill.mesh.tets[ti][static_cast<std::size_t>(f[2])]};
            std::sort(key.begin(), key.end());
            const auto [it, inserted] = owner.try_emplace(key, ti);
            if (!inserted) {
                const double a = tet_mean_edge(fill, ti);
                const double b = tet_mean_edge(fill, it->second);
                max_ratio = std::max(max_ratio, std::max(a, b) / std::min(a, b));
            }
        }
    }
    return max_ratio;
}

std::array<std::size_t, 2> half_tet_counts(const GradedTetFillOutput& fill) {
    std::array<std::size_t, 2> counts{};
    for (const auto& tet : fill.mesh.tets) {
        const Eigen::Vector3d centroid =
            0.25 * (fill.mesh.nodes[tet[0]] + fill.mesh.nodes[tet[1]] +
                    fill.mesh.nodes[tet[2]] + fill.mesh.nodes[tet[3]]);
        ++counts[centroid.x() < 0.5 ? 0 : 1];
    }
    return counts;
}

} // namespace

TEST_CASE("empty size fields preserve graded and hybrid fill bytes", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const SizeFieldFn empty;

    const auto graded_default = polymesh::mesh::graded_tet_fill_surface(
        model.surface, model.bbox_min, model.bbox_max, 0.34, 1);
    const auto graded_empty = polymesh::mesh::graded_tet_fill_surface(
        model.surface, model.bbox_min, model.bbox_max, 0.34, 1, {}, 0.0, {}, 0.0, 0.0, nullptr,
        empty);
    REQUIRE(graded_empty.mesh.nodes.size() == graded_default.mesh.nodes.size());
    REQUIRE(graded_empty.mesh.tets.size() == graded_default.mesh.tets.size());
    REQUIRE(same_node_bytes(graded_empty.mesh.nodes, graded_default.mesh.nodes));

    const auto hybrid_default = polymesh::mesh::mixed_fill_surface(
        model.surface, model.bbox_min, model.bbox_max, 0.25, 1);
    const auto hybrid_empty =
        polymesh::mesh::mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, 0.25,
                                           1, {}, 0.0, {}, 0.0, true, 0.0, false, {}, empty);
    REQUIRE(hybrid_empty.nodes.size() == hybrid_default.nodes.size());
    REQUIRE(hybrid_empty.cells.size() == hybrid_default.cells.size());
    REQUIRE(same_node_bytes(hybrid_empty.nodes, hybrid_default.nodes));
}

TEST_CASE("linear size field gives monotone graded density", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    std::array<std::size_t, 3> totals{};
    std::array<std::size_t, 3> l1{};
    std::array<std::size_t, 3> l2{};
    std::array<std::size_t, 2> base_halves{};
    constexpr std::array<double, 3> scales{{1.0, 0.75, 0.5}};
    for (std::size_t run = 0; run < scales.size(); ++run) {
        const double scale = scales[run];
        const SizeFieldFn field = [scale](const Eigen::Vector3d& x) {
            return 0.35 * scale * (0.25 + 0.75 * x.x());
        };
        const auto fill = polymesh::mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, 0.25, 1, {}, 0.0, {}, 0.0, 0.0,
            nullptr, field);
        totals[run] = fill.mesh.tets.size();
        l1[run] = fill.n_level1_cells;
        l2[run] = fill.n_level2_cells;
        if (run == 0) {
            base_halves = half_tet_counts(fill);
        }
    }
    // Level counts come along because a mesh whose element total ignores the
    // extra marked cells is the signature of a lattice split whose longest-edge
    // bisection has lost its cell-local terminal edge (see mesh/lattice_split.hpp).
    INFO("linear field tets scale 1.0/0.75/0.5 = "
         << totals[0] << "/" << totals[1] << "/" << totals[2] << " L1 cells " << l1[0] << "/"
         << l1[1] << "/" << l1[2] << " L2 cells " << l2[0] << "/" << l2[1] << "/" << l2[2]);
    REQUIRE(base_halves[0] > base_halves[1]);
    REQUIRE(l1[0] < l1[1]);
    REQUIRE(totals[0] < totals[1]);
    REQUIRE(totals[1] < totals[2]);
}

TEST_CASE("linear field selects spatially fine hybrid hexes", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const SizeFieldFn field = [](const Eigen::Vector3d& x) {
        return 0.175 * (0.25 + 0.75 * x.x());
    };
    const auto fill = polymesh::mesh::mixed_fill_surface(model.surface, model.bbox_min,
                                                         model.bbox_max, 0.125, 1, {}, 0.0, {},
                                                         0.0, false, 0.0, false, {}, field);
    std::array<std::size_t, 2> fine_hexes{};
    for (const auto& cell : fill.cells) {
        if (cell.kind != polymesh::mesh::MixedCellKind::kHex8) {
            continue;
        }
        Eigen::Vector3d lo = fill.nodes[cell.nodes[0]];
        Eigen::Vector3d hi = lo;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (std::size_t i = 0; i < 8; ++i) {
            const auto& p = fill.nodes[cell.nodes[i]];
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
            centroid += p;
        }
        centroid /= 8.0;
        if ((hi - lo).maxCoeff() < 0.75 * fill.h) {
            ++fine_hexes[centroid.x() < 0.5 ? 0 : 1];
        }
    }
    REQUIRE(fill.n_level0_cells > 0);
    REQUIRE(fill.n_level1_cells > 0);
    REQUIRE(fine_hexes[0] > fine_hexes[1]);
}

TEST_CASE("continuous field transition is no sharper than seed ball", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const Eigen::Vector3d source{0.125, 0.125, 0.125};
    const std::vector<Eigen::Vector3d> seeds{source};
    const auto seeded = polymesh::mesh::graded_tet_fill_surface(
        model.surface, model.bbox_min, model.bbox_max, 0.25, 1, {}, 0.0, seeds, 0.42);

    const std::vector<polymesh::adapt::SizeSource> sources{{source, 0.0625}};
    const auto field = polymesh::adapt::size_field_from_sources(sources, 0.0625, 0.25, 0.5);
    const auto graded = polymesh::mesh::graded_tet_fill_surface(
        model.surface, model.bbox_min, model.bbox_max, 0.25, 1, {}, 0.0, {}, 0.0, 0.0, nullptr,
        field);

    REQUIRE(graded.n_level2_cells > 0);
    REQUIRE(seeded.n_level2_cells > 0);
    REQUIRE(max_adjacent_tet_edge_ratio(graded) <=
            max_adjacent_tet_edge_ratio(seeded) + 1e-12);
}

TEST_CASE("field-driven graded tet stays conforming and positive", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const SizeFieldFn field = [](const Eigen::Vector3d& x) {
        return 0.30 * (0.25 + 0.75 * x.x());
    };
    const auto fill = polymesh::mesh::graded_tet_fill_surface(model.surface, model.bbox_min,
                                                              model.bbox_max, 0.30, 1, {}, 0.0,
                                                              {}, 0.0, 0.0, nullptr, field);

    const auto conformity = polymesh::mesh::tet4_face_conformity(
        fill.mesh.nodes, fill.mesh.tets, model.bbox_min, model.bbox_max, 0.2 * fill.h_coarse);
    REQUIRE(conformity.n_hanging_faces == 0);
    REQUIRE(conformity.n_nonconforming == 0);
    REQUIRE(conformity.is_conforming);
    for (const auto& tet : fill.mesh.tets) {
        REQUIRE(polymesh::mesh::tet_signed_volume(
                    fill.mesh.nodes[tet[0]], fill.mesh.nodes[tet[1]], fill.mesh.nodes[tet[2]],
                    fill.mesh.nodes[tet[3]]) > 0.0);
    }
}

TEST_CASE("size field budget floor clamps without throwing", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const double tiny = (model.bbox_max - model.bbox_min).norm() / 10000.0;
    const SizeFieldFn field = [tiny](const Eigen::Vector3d&) { return tiny; };
    const auto out =
        polymesh::pipeline::volume_mesh(model, 0.20, polymesh::pipeline::VolumeMesher::kHybrid,
                                        1, false, {}, 0.0, 0.0, 0, 0, 0, {}, field);

    REQUIRE_FALSE(out.mesh.elements.empty());
    REQUIRE(out.mesh.elements.size() <= polymesh::mesh::kHybridMaxElems);
    REQUIRE(out.mesher_note.find("clamped at budget floor") != std::string::npos);
}

TEST_CASE("field-driven fill is byte deterministic", "[sizefield][mesher]") {
    const auto model = polymesh::testsupport::box_model(1.0, 1.0, 1.0);
    const SizeFieldFn field = [](const Eigen::Vector3d& x) {
        return 0.30 * (0.25 + 0.75 * x.x());
    };
    const auto a = polymesh::mesh::graded_tet_fill_surface(model.surface, model.bbox_min,
                                                           model.bbox_max, 0.30, 1, {}, 0.0,
                                                           {}, 0.0, 0.0, nullptr, field);
    const auto b = polymesh::mesh::graded_tet_fill_surface(model.surface, model.bbox_min,
                                                           model.bbox_max, 0.30, 1, {}, 0.0,
                                                           {}, 0.0, 0.0, nullptr, field);

    REQUIRE(a.mesh.tets == b.mesh.tets);
    REQUIRE(same_node_bytes(a.mesh.nodes, b.mesh.nodes));
}
