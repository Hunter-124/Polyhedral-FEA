// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Core>

#include <cstdint>
#include <set>
#include <vector>

using polymesh::geom::TriSurface;
using polymesh::mesh::BoundaryProjectionContext;
using polymesh::mesh::BoundarySupport;
using polymesh::mesh::BoundarySupportKind;
using polymesh::mesh::BoundaryTarget;

namespace {

TriSurface unit_triangle() {
    return TriSurface{
        .vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
        .triangles = {{0, 1, 2}},
    };
}

} // namespace

TEST_CASE("boundary projection keeps a classified exact owner immutable") {
    const TriSurface surface = unit_triangle();
    std::vector<BoundarySupport> provenance(1);
    provenance[0] = {BoundarySupportKind::kCadEdge, 7};
    BoundaryProjectionContext context;
    context.provenance = &provenance;
    context.target = [](const Eigen::Vector3d&,
                        BoundarySupport& owner) -> std::optional<BoundaryTarget> {
        owner = {BoundarySupportKind::kCadFace, 3};
        return BoundaryTarget{Eigen::Vector3d(0.2, 0.2, 0.0), 0.1};
    };

    const auto target = polymesh::mesh::boundary_projection_target(
        surface, Eigen::Vector3d(0.2, 0.2, 0.1), 0, &context);
    CHECK_FALSE(target.has_value());
    CHECK(provenance[0].kind == BoundarySupportKind::kCadEdge);
    CHECK(provenance[0].id == 7);
}

TEST_CASE("exact projection failure never falls back across an owned CAD edge") {
    const TriSurface surface = unit_triangle();
    std::vector<Eigen::Vector3d> nodes{{0.25, 0.25, 0.1}};
    const std::vector<std::uint32_t> boundary_nodes{0};
    std::vector<BoundarySupport> provenance(1);
    provenance[0] = {BoundarySupportKind::kCadEdge, 4};
    BoundaryProjectionContext context;
    context.provenance = &provenance;
    context.target = [](const Eigen::Vector3d&,
                        BoundarySupport&) -> std::optional<BoundaryTarget> {
        return std::nullopt;
    };
    const Eigen::Vector3d before = nodes.front();

    const auto stats = polymesh::mesh::snap_boundary_nodes(
        surface, nodes, boundary_nodes, 1.0, [](std::set<std::uint32_t>&) {}, 1.0, 2, {}, {},
        {}, false, &context);

    CHECK(stats.n_moved == 0);
    CHECK((nodes.front() - before).norm() == 0.0);
    CHECK(provenance[0].kind == BoundarySupportKind::kCadEdge);
    CHECK(provenance[0].id == 4);
}
