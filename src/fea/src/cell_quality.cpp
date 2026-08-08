// SPDX-License-Identifier: BSD-3-Clause
#include "fea/cell_quality.hpp"

#include "mesh/quality.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace polymesh::fea {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Three neighbour corners per corner, ordered right-handed so a well-oriented
/// cell yields a positive determinant. Row index = the corner itself.
using CornerEdges = std::array<std::size_t, 3>;

/// Hex node order (nodal_mesh.hpp): bottom 0..3 CCW, top 4..7 CCW.
constexpr std::array<CornerEdges, 8> kHexCorners{{{1, 3, 4},
                                                  {2, 0, 5},
                                                  {3, 1, 6},
                                                  {0, 2, 7},
                                                  {7, 5, 0},
                                                  {4, 6, 1},
                                                  {5, 7, 2},
                                                  {6, 4, 3}}};

/// Prism: bottom tri 0,1,2 then top tri 3,4,5. Bottom corners use
/// (next, prev, top), top corners (prev, next, bottom) to stay right-handed.
constexpr std::array<CornerEdges, 6> kPrismCorners{
    {{1, 2, 3}, {2, 0, 4}, {0, 1, 5}, {5, 4, 0}, {3, 5, 1}, {4, 3, 2}}};

/// Pyramid: base quad 0..3 CCW, apex 4. Only the base corners have a Jacobian —
/// the apex is a collapsed point of the reference map.
constexpr std::array<CornerEdges, 4> kPyramidCorners{
    {{1, 3, 4}, {2, 0, 4}, {3, 1, 4}, {0, 2, 4}}};

/// sin(60°): the corner value a regular (equilateral-base) prism attains.
constexpr double kPrismIdeal = 0.8660254037844386;
/// 1/√2: the base-corner value a regular pyramid (all edges equal) attains.
constexpr double kPyramidIdeal = 0.7071067811865476;

/// Minimum corner scaled Jacobian, normalized so the regular cell scores 1.
/// 0 when a corner has a zero-length edge (measured as fully degenerate).
double min_corner_scaled_jacobian(const NodalMesh& mesh, const NodalElement& element,
                                  std::span<const CornerEdges> corners, double ideal) {
    double q = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Eigen::Vector3d& p = mesh.nodes[element.nodes[i]];
        const Eigen::Vector3d e1 = mesh.nodes[element.nodes[corners[i][0]]] - p;
        const Eigen::Vector3d e2 = mesh.nodes[element.nodes[corners[i][1]]] - p;
        const Eigen::Vector3d e3 = mesh.nodes[element.nodes[corners[i][2]]] - p;
        const double denom = e1.norm() * e2.norm() * e3.norm();
        if (!(denom > 0.0)) {
            return 0.0;
        }
        q = std::min(q, e1.dot(e2.cross(e3)) / denom / ideal);
    }
    return std::clamp(q, -1.0, 1.0);
}

/// Topological edges of the straight-sided cells (same corner order as above),
/// for the mean edge length the volume term below is normalized by.
using Edge = std::array<std::size_t, 2>;
constexpr std::array<Edge, 12> kHexEdges{{{0, 1},
                                          {1, 2},
                                          {2, 3},
                                          {3, 0},
                                          {4, 5},
                                          {5, 6},
                                          {6, 7},
                                          {7, 4},
                                          {0, 4},
                                          {1, 5},
                                          {2, 6},
                                          {3, 7}}};
constexpr std::array<Edge, 9> kPrismEdges{
    {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5}, {5, 3}, {0, 3}, {1, 4}, {2, 5}}};
constexpr std::array<Edge, 8> kPyramidEdges{
    {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 4}, {2, 4}, {3, 4}}};

/// One outward-oriented face loop (CCW seen from outside the cell), 3 or 4
/// corners; the unused 4th slot of a triangle is ignored.
struct FaceLoop {
    std::size_t n;
    std::array<std::size_t, 4> v;
};
constexpr std::array<FaceLoop, 6> kHexFaces{{{4, {{0, 3, 2, 1}}},
                                             {4, {{4, 5, 6, 7}}},
                                             {4, {{0, 1, 5, 4}}},
                                             {4, {{1, 2, 6, 5}}},
                                             {4, {{2, 3, 7, 6}}},
                                             {4, {{3, 0, 4, 7}}}}};
constexpr std::array<FaceLoop, 5> kPrismFaces{{{3, {{0, 2, 1, 0}}},
                                               {3, {{3, 4, 5, 0}}},
                                               {4, {{0, 1, 4, 3}}},
                                               {4, {{1, 2, 5, 4}}},
                                               {4, {{2, 0, 3, 5}}}}};
constexpr std::array<FaceLoop, 5> kPyramidFaces{{{4, {{0, 3, 2, 1}}},
                                                 {3, {{0, 1, 4, 0}}},
                                                 {3, {{1, 2, 4, 0}}},
                                                 {3, {{2, 3, 4, 0}}},
                                                 {3, {{3, 0, 4, 0}}}}};

/// Volume / mean-edge³ the regular cell of each type attains: the cube 1, the
/// equilateral prism √3/4, the all-edges-equal pyramid 1/(3√2).
constexpr double kHexVolIdeal = 1.0;
constexpr double kPrismVolIdeal = 0.4330127018922193;
constexpr double kPyramidVolIdeal = 0.23570226039551587;

/// Signed volume of a straight-sided cell by the divergence theorem over its
/// outward faces, each face fanned from its own centroid so a warped quad
/// contributes the volume its four triangles bound (no bias from picking one
/// diagonal). Positive for a right-handed cell, negative when inverted.
double cell_signed_volume(const NodalMesh& mesh, const NodalElement& element,
                          std::span<const FaceLoop> faces) {
    const Eigen::Vector3d& o = mesh.nodes[element.nodes[0]];
    double volume = 0.0;
    for (const auto& f : faces) {
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (std::size_t k = 0; k < f.n; ++k) {
            centroid += mesh.nodes[element.nodes[f.v[k]]];
        }
        centroid /= static_cast<double>(f.n);
        const Eigen::Vector3d c = centroid - o;
        for (std::size_t k = 0; k < f.n; ++k) {
            const Eigen::Vector3d a = mesh.nodes[element.nodes[f.v[k]]] - o;
            const Eigen::Vector3d b = mesh.nodes[element.nodes[f.v[(k + 1) % f.n]]] - o;
            volume += a.dot(b.cross(c)) / 6.0;
        }
    }
    return volume;
}

/// Volume-collapse term: signed volume per mean-edge-length cubed, normalized so
/// the regular cell of the type scores 1. This is the half of cell shape the
/// corner scaled Jacobian structurally cannot see — every corner of a 1×1×1e-4
/// pancake hex is a perfect right angle, so the corner measure alone calls that
/// cell flawless (exactly 1.0) while the map it stands for is conditioned 1e-4.
/// → 0 as the cell flattens or stretches, negative when the cell is inverted.
double volume_collapse_term(const NodalMesh& mesh, const NodalElement& element,
                            std::span<const Edge> edges, std::span<const FaceLoop> faces,
                            double ideal) {
    double sum = 0.0;
    for (const auto& e : edges) {
        sum += (mesh.nodes[element.nodes[e[1]]] - mesh.nodes[element.nodes[e[0]]]).norm();
    }
    const double mean_edge = sum / static_cast<double>(edges.size());
    if (!(mean_edge > 0.0)) {
        return 0.0; // every edge collapsed: nothing left of the cell.
    }
    return cell_signed_volume(mesh, element, faces) / (mean_edge * mean_edge * mean_edge) /
           ideal;
}

/// Corner shape *and* volume collapse, both normalized to 1 for the regular
/// cell: the worst of the two, so a cell has to be sound in its angles and in
/// its thickness to score well. Neither term subsumes the other — skew shows up
/// only in the corners, flatness/stretch only in the volume.
double straight_cell_quality(const NodalMesh& mesh, const NodalElement& element,
                             std::span<const CornerEdges> corners, double corner_ideal,
                             std::span<const Edge> edges, std::span<const FaceLoop> faces,
                             double vol_ideal) {
    const double corner = min_corner_scaled_jacobian(mesh, element, corners, corner_ideal);
    const double vol = volume_collapse_term(mesh, element, edges, faces, vol_ideal);
    return std::clamp(std::min(corner, vol), -1.0, 1.0);
}

/// Boundary-shape quality of a polyhedral (VEM) cell: worst face corner over all
/// faces, or 0 when the cell has non-positive signed volume (inverted / flat).
double poly_cell_quality(const NodalMesh& mesh, const NodalElement& element) {
    double q = std::numeric_limits<double>::infinity();
    double volume = 0.0;
    bool measured = false;
    for (const auto& face : element.faces) {
        const std::size_t n = face.size();
        if (n < 3) {
            continue;
        }
        // Signed volume by the divergence theorem over the fan of each face;
        // outward-oriented faces (VEM contract) give a positive total.
        const Eigen::Vector3d& a = mesh.nodes[element.nodes[face[0]]];
        for (std::size_t k = 1; k + 1 < n; ++k) {
            const Eigen::Vector3d& b = mesh.nodes[element.nodes[face[k]]];
            const Eigen::Vector3d& c = mesh.nodes[element.nodes[face[k + 1]]];
            volume += a.dot(b.cross(c)) / 6.0;
        }
        for (std::size_t k = 0; k < n; ++k) {
            const double fq = mesh::polygon_corner_quality(
                mesh.nodes[element.nodes[face[(k + n - 1) % n]]],
                mesh.nodes[element.nodes[face[k]]],
                mesh.nodes[element.nodes[face[(k + 1) % n]]], n);
            if (std::isfinite(fq)) {
                q = std::min(q, fq);
                measured = true;
            }
        }
    }
    if (!measured) {
        return kNaN;
    }
    if (!(volume > 0.0)) {
        return 0.0;
    }
    return q;
}

} // namespace

double cell_quality(const NodalMesh& mesh, const NodalElement& element) {
    const auto n_nodes = element.nodes.size();
    switch (element.type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        if (n_nodes < 4) {
            return kNaN;
        }
        return mesh::tet4_aspect_quality(
            mesh.nodes[element.nodes[0]], mesh.nodes[element.nodes[1]],
            mesh.nodes[element.nodes[2]], mesh.nodes[element.nodes[3]]);
    case ElementType::kHex8:
    case ElementType::kHex20:
        if (n_nodes < 8) {
            return kNaN;
        }
        return straight_cell_quality(mesh, element, kHexCorners, 1.0, kHexEdges, kHexFaces,
                                     kHexVolIdeal);
    case ElementType::kPrism6:
        if (n_nodes < 6) {
            return kNaN;
        }
        return straight_cell_quality(mesh, element, kPrismCorners, kPrismIdeal, kPrismEdges,
                                     kPrismFaces, kPrismVolIdeal);
    case ElementType::kPyramid5:
        if (n_nodes < 5) {
            return kNaN;
        }
        return straight_cell_quality(mesh, element, kPyramidCorners, kPyramidIdeal,
                                     kPyramidEdges, kPyramidFaces, kPyramidVolIdeal);
    case ElementType::kPolyVem:
        if (element.faces.empty() || n_nodes < 4) {
            return kNaN;
        }
        return poly_cell_quality(mesh, element);
    }
    return kNaN;
}

std::vector<double> cell_quality(const NodalMesh& mesh) {
    std::vector<double> out;
    out.reserve(mesh.elements.size());
    for (const auto& element : mesh.elements) {
        out.push_back(cell_quality(mesh, element));
    }
    return out;
}

CellQualityStats summarize_cell_quality(const NodalMesh& mesh) {
    CellQualityStats s;
    double min_q = std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (const auto& element : mesh.elements) {
        const double q = cell_quality(mesh, element);
        if (!std::isfinite(q)) {
            ++s.n_unmeasured;
            continue;
        }
        min_q = std::min(min_q, q);
        sum += q;
        ++s.n_measured;
    }
    if (s.n_measured == 0) {
        return s; // min/mean stay NaN — nothing was measured.
    }
    s.min = min_q;
    s.mean = sum / static_cast<double>(s.n_measured);
    return s;
}

} // namespace polymesh::fea
