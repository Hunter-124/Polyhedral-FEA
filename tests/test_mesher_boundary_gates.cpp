// SPDX-License-Identifier: BSD-3-Clause
// Boundary-conformance gates across the CAD meshers (ADR-0035): boundary
// nodes on the exact BRep, sharp BRep edges reproduced by mesh feature
// segments, the hybrid curved-boundary survey, and the shipped exterior gate.

#include "fea/boundary_faces.hpp"
#include "fea/element_validity.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";
constexpr char kPlateHole[] = "tests/fixtures/parts/plate_hole.step";
constexpr char kCantilever[] = "tests/fixtures/parts/cantilever.step";
constexpr char kIcecreamCone[] = "tests/fixtures/parts/icecream_cone.step";
constexpr char kPipe[] = "tests/fixtures/parts/pipe.step";
constexpr char kSmokeBar[] = "tests/fixtures/parts/smoke_bar.step";
constexpr char kCylinder[] = "tests/fixtures/parts/cylinder.step";

std::vector<std::uint32_t> boundary_nodes(const polymesh::fea::NodalMesh& mesh) {
    std::set<std::uint32_t> nodes;
    for (const auto& face : polymesh::fea::extract_boundary_faces(mesh)) {
        nodes.insert(face.begin(), face.end());
    }
    return {nodes.begin(), nodes.end()};
}
using BoundaryQuad = std::array<std::uint32_t, 4>;

std::array<std::uint32_t, 4> face_key(BoundaryQuad face) {
    std::sort(face.begin(), face.end());
    return face;
}

std::vector<std::uint32_t> nodes_on_faces(std::span<const BoundaryQuad> faces) {
    std::set<std::uint32_t> nodes;
    for (const auto& face : faces) {
        nodes.insert(face.begin(), face.end());
    }
    return {nodes.begin(), nodes.end()};
}

std::vector<std::uint32_t>
nodes_on_original_boundary(const polymesh::pipeline::VolumeMeshOutput& volume) {
    std::set<std::array<std::uint32_t, 4>> local_faces;
    std::set<std::uint32_t> local_nodes;
    for (const auto& face : volume.local_child_boundary_quads) {
        local_faces.insert(face_key(face));
        local_nodes.insert(face.begin(), face.end());
    }
    std::set<std::uint32_t> nodes;
    for (const auto& face : volume.boundary_quads) {
        if (local_faces.contains(face_key(face))) {
            continue;
        }
        for (const auto node : face) {
            if (!local_nodes.contains(node)) {
                nodes.insert(node);
            }
        }
    }
    return {nodes.begin(), nodes.end()};
}

polymesh::mesh::SampleDistribution
exact_residuals_over_h(const polymesh::geom::CadModel& cad,
                       const std::vector<Eigen::Vector3d>& points,
                       const std::vector<std::uint32_t>& indices, double h) {
    std::vector<double> residuals;
    residuals.reserve(indices.size());
    for (const auto node : indices) {
        if (node >= points.size()) {
            continue;
        }
        if (const auto exact = polymesh::geom::project_point_on_surface(cad, points[node])) {
            residuals.push_back(exact->distance / h);
        }
    }
    return polymesh::mesh::summarize_samples(residuals);
}

} // namespace

// The local h/2 classifier creates live/void child faces (on sphere and
// icecream these are outer curved interfaces inside mixed parents, not bores);
// every such face must go through the same snap/projection path as the
// original boundary. Original surfaces keep the 0.10 (p99) / 0.25 (max) rails;
// the harder local subset is capped independently at 0.06.
TEST_CASE("brep_fidelity: hybrid curved boundary survey stays bounded", "[cad][fidelity]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }

    struct SurveyPart {
        const char* name;
        const char* path;
    };
    const SurveyPart parts[] = {
        {"cantilever", kCantilever},
        {"cylinder", kCylinder},
        {"icecream_cone", kIcecreamCone},
        {"pipe", kPipe},
        {"plate_hole", kPlateHole},
        {"smoke_bar", kSmokeBar},
        {"sphere", kSphere},
    };
    for (const auto& part : parts) {
        if (!std::filesystem::exists(part.path)) {
            SKIP(std::string(part.path) + " missing");
        }
        const auto model = polymesh::pipeline::Model::load(part.path);
        REQUIRE(model.cad);
        for (const double h_rel : {0.20, 0.12, 0.08}) {
            if (h_rel == 0.08 && std::string_view(part.name) != "plate_hole" &&
                std::string_view(part.name) != "cylinder") {
                continue;
            }
            const double h = h_rel * (model.bbox_max - model.bbox_min).norm();
            std::optional<polymesh::pipeline::VolumeMeshOutput> maybe_vol;
            try {
                maybe_vol = polymesh::pipeline::volume_mesh(
                    model, h, polymesh::pipeline::VolumeMesher::kHybrid,
                    /*skin_layers=*/2, /*feature_refine=*/true);
            } catch (const polymesh::pipeline::GeometryVolumeLimitError& e) {
                CAPTURE(part.name, h_rel, e.what());
                CHECK_FALSE(e.solved_stage);
                CHECK(e.assessment.available);
                const bool expected_guard =
                    std::string(e.what()).find("feature unresolved") != std::string::npos ||
                    e.assessment.relative_error > polymesh::pipeline::kGeometryVolumeHardLimit;
                CHECK(expected_guard);
                continue;
            }
            const auto& vol = *maybe_vol;
            const auto nodes = boundary_nodes(vol.mesh);
            const auto original_nodes = nodes_on_original_boundary(vol);
            const auto local_nodes = nodes_on_faces(vol.local_child_boundary_quads);
            REQUIRE_FALSE(nodes.empty());
            REQUIRE_FALSE(original_nodes.empty());
            const auto residual = exact_residuals_over_h(*model.cad, vol.mesh.nodes, nodes, h);
            const auto original_residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, original_nodes, h);
            const auto local_residual =
                exact_residuals_over_h(*model.cad, vol.mesh.nodes, local_nodes, h);
            REQUIRE(residual.count == nodes.size());
            REQUIRE(original_residual.count == original_nodes.size());
            REQUIRE(local_residual.count == local_nodes.size());
            CHECK(local_residual.max <= 0.06);
            CHECK(local_residual.p99 <= 0.06);
            CAPTURE(part.name, h_rel, residual.max, residual.p99, original_residual.max,
                    original_residual.p99, local_residual.max, local_residual.p99,
                    vol.mesher_note);
            CHECK(original_residual.max <= 0.25);
            CHECK(original_residual.p99 <= 0.10);
            CHECK(residual.max <= 0.25);
            CHECK(residual.p99 <= 0.10);
        }
    }
}

// ADR-0035: boundary nodes sit ON the exact BRep (not the tessellation, and at
// a sharp edge on the edge curve rather than a face). This measures the one
// thing a mesher fully controls: where it puts a node. Facet centroids and edge
// midpoints are excluded — a straight facet spanning a curve carries the chord
// sag h²κ/8, a discretisation property, not a placement error.
TEST_CASE("boundary nodes land on the exact BRep for every CAD mesher",
          "[cad][fidelity][feature_pin]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* name;
        const char* path;
        polymesh::pipeline::VolumeMesher mesher;
        double node_p99_over_h; // ceiling on the 99th percentile
        double node_max_over_h; // ceiling on the worst single node
    };
    // Ceilings are the measured post-fix values with headroom. The graded and
    // varyhedron paths place every node on the BRep to machine precision, so
    // their ceilings are 1e-12 — a real regression cannot hide under that.
    const Case cases[] = {
        {"sphere/graded", kSphere, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12, 1e-12},
        {"sphere/varyhedron", kSphere, polymesh::pipeline::VolumeMesher::kVaryhedron, 1e-12,
         1e-12},
        {"cylinder/graded", kCylinder, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12,
         0.02},
        {"cylinder/varyhedron", kCylinder, polymesh::pipeline::VolumeMesher::kVaryhedron,
         1e-12, 1e-12},
        {"plate_hole/graded", kPlateHole, polymesh::pipeline::VolumeMesher::kGradedTet, 1e-12,
         0.002},
        {"plate_hole/varyhedron", kPlateHole, polymesh::pipeline::VolumeMesher::kVaryhedron,
         1e-12, 0.002},
        {"icecream_cone/graded", kIcecreamCone, polymesh::pipeline::VolumeMesher::kGradedTet,
         1e-12, 0.25},
        {"icecream_cone/varyhedron", kIcecreamCone,
         polymesh::pipeline::VolumeMesher::kVaryhedron, 1e-12, 0.25},
    };
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        const auto vol = polymesh::pipeline::volume_mesh(model, kH, c.mesher);
        REQUIRE_FALSE(vol.mesh.nodes.empty());
        const auto nodes = boundary_nodes(vol.mesh);
        REQUIRE_FALSE(nodes.empty());
        const auto residual = exact_residuals_over_h(*model.cad, vol.mesh.nodes, nodes, kH);
        CAPTURE(c.name, residual.p99, residual.max, vol.mesher_note);
        CHECK(residual.p99 <= c.node_p99_over_h);
        CHECK(residual.max <= c.node_max_over_h);
    }
}

// The pinning pass is what reproduces a sharp CAD edge. Its direction of
// interest is CAD -> mesh: for every sampled point of a sharp BRep edge, how
// far is the nearest mesh feature segment? A mesh that chamfers a 90 deg edge
// fails here even when every node is on some face of the solid.
TEST_CASE("sharp BRep edges are reproduced by mesh feature segments",
          "[cad][fidelity][feature_pin]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* name;
        const char* path;
        double edge_p99_over_h;
    };
    const Case cases[] = {
        {"plate_hole", kPlateHole, 0.09},
        {"cylinder", kCylinder, 0.20},
        {"icecream_cone", kIcecreamCone, 0.10},
    };
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        // feature_refine=true is what the product CLI runs; without the
        // feature band the rim is meshed at bulk h and the reproduction of a
        // small circular edge is bounded by the lattice, not by the pin.
        const auto vol = polymesh::pipeline::volume_mesh(
            model, kH, polymesh::pipeline::VolumeMesher::kGradedTet, /*skin_layers=*/2,
            /*feature_refine=*/true);
        const auto quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        const std::vector<polymesh::mesh::FreeFace> faces(quads.begin(), quads.end());
        const auto segments =
            polymesh::mesh::mesh_dihedral_feature_segments(vol.mesh.nodes, faces);
        const auto fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
            *model.cad, vol.mesh.nodes, faces, segments, kH, 0.0);
        REQUIRE(fidelity.available);
        const auto& reverse = fidelity.sharp_brep_edge_samples_to_mesh_feature_segments;
        CAPTURE(c.name, reverse.over_h.p99, reverse.over_h.max, vol.mesher_note);
        if (reverse.metres.count == 0) {
            continue; // no sharp edges on this part
        }
        CHECK(reverse.over_h.p99 <= c.edge_p99_over_h);
    }
}

// The exterior conformity gate (ADR-0035). Two claims, both about the mesh that
// actually ships rather than about any mesher's own intermediate boundary set:
// every emitted element is integrable by the assembly's own rule, and the true
// element exterior — not the lattice skin the snap ran on — is on the BRep.
TEST_CASE("brep_fidelity: the shipped exterior conforms and every cell is integrable",
          "[cad][fidelity][exterior_gate]") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* path;
        polymesh::pipeline::VolumeMesher mesher;
        const char* name;
        double node_p99_over_h; // ceiling on the shipped boundary NODES
    };
    // Ceilings are the measured numbers plus headroom, per part×mesher, because
    // what the lattice can reach differs: the conforming meshers land on the
    // BRep to machine precision, while the uniform Cartesian tet fill is bounded
    // by its own cell-shape floor (ADR-0035 §5) and only improves.
    const std::array<Case, 6> cases{{
        {kSphere, polymesh::pipeline::VolumeMesher::kGradedTet, "sphere/graded", 1e-12},
        {kSphere, polymesh::pipeline::VolumeMesher::kVaryhedron, "sphere/varyhedron", 1e-12},
        {kSphere, polymesh::pipeline::VolumeMesher::kHybrid, "sphere/hybrid", 0.01},
        {kIcecreamCone, polymesh::pipeline::VolumeMesher::kHybrid, "cone/hybrid", 1e-12},
        {kIcecreamCone, polymesh::pipeline::VolumeMesher::kGradedTet, "cone/graded", 1e-12},
        {kPlateHole, polymesh::pipeline::VolumeMesher::kVaryhedron, "plate_hole/varyhedron",
         1e-12},
    }};
    constexpr double kH = 0.008;
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            SKIP(std::string("missing fixture: ") + c.path);
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        const auto vol = polymesh::pipeline::volume_mesh(model, kH, c.mesher,
                                                         /*skin_layers=*/2,
                                                         /*feature_refine=*/true);
        // Integrability of what ships. This is the claim `fea::cell_quality`
        // cannot make: a cell can clear the shape floor and still have a
        // non-positive Jacobian at a quadrature point.
        std::size_t nonintegrable = 0;
        for (const auto& element : vol.mesh.elements) {
            if (!polymesh::fea::element_jacobians_positive(vol.mesh, element)) {
                ++nonintegrable;
            }
        }
        CAPTURE(c.name, vol.mesher_note);
        CHECK(nonintegrable == 0);

        const auto quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        REQUIRE_FALSE(quads.empty());
        std::set<std::uint32_t> exterior;
        for (const auto& quad : quads) {
            exterior.insert(quad.begin(), quad.end());
        }
        std::vector<double> residuals;
        residuals.reserve(exterior.size());
        for (const auto node : exterior) {
            const auto exact =
                polymesh::geom::project_point_on_surface(*model.cad, vol.mesh.nodes[node]);
            if (exact) {
                residuals.push_back(exact->distance / kH);
            }
        }
        REQUIRE_FALSE(residuals.empty());
        std::sort(residuals.begin(), residuals.end());
        const double p99 = residuals[static_cast<std::size_t>(
            0.99 * static_cast<double>(residuals.size() - 1))];
        CAPTURE(p99, residuals.back(), residuals.size());
        CHECK(p99 <= c.node_p99_over_h);
    }
}
