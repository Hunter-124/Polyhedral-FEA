// SPDX-License-Identifier: BSD-3-Clause
#include "fea/cell_quality.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "mesh/quality.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace polymesh::fea {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Three neighbour corners per corner, ordered right-handed so a well-oriented
/// cell yields a positive determinant. Row index = the corner itself.
using CornerEdges = std::array<std::size_t, 3>;

/// Hex node order (nodal_mesh.hpp): bottom 0..3 CCW, top 4..7 CCW.
constexpr std::array<CornerEdges, 8> kHexCorners{
    {{1, 3, 4}, {2, 0, 5}, {3, 1, 6}, {0, 2, 7}, {7, 5, 0}, {4, 6, 1}, {5, 7, 2}, {6, 4, 3}}};

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
        // RVD face coalescing can leave exact straight-through vertices where
        // several tet-clipped fragments met.  They do not change the polygon's
        // geometry, so exclude them from the corner-angle score.  Degenerate
        // zero-length edges remain measurable as bad quality.
        std::vector<std::size_t> corners;
        corners.reserve(n);
        bool has_zero_length_edge = false;
        for (std::size_t k = 0; k < n; ++k) {
            const Eigen::Vector3d& previous = mesh.nodes[element.nodes[face[(k + n - 1) % n]]];
            const Eigen::Vector3d& current = mesh.nodes[element.nodes[face[k]]];
            const Eigen::Vector3d& next = mesh.nodes[element.nodes[face[(k + 1) % n]]];
            const Eigen::Vector3d incoming = current - previous;
            const Eigen::Vector3d outgoing = next - current;
            const double scale = incoming.norm() * outgoing.norm();
            has_zero_length_edge = has_zero_length_edge || !(scale > 0.0);
            const bool straight_through = scale > 0.0 && incoming.dot(outgoing) > 0.0 &&
                                          incoming.cross(outgoing).norm() <= 1e-12 * scale;
            if (!straight_through) {
                corners.push_back(k);
            }
        }
        if (has_zero_length_edge) {
            q = 0.0;
            measured = true;
            continue;
        }
        if (corners.size() < 3) {
            // Fewer than three geometric corners is a collapsed/backtracking
            // face, not an unmeasurable decoration on an otherwise good cell.
            q = 0.0;
            measured = true;
            continue;
        }
        for (std::size_t j = 0; j < corners.size(); ++j) {
            const std::size_t previous = corners[(j + corners.size() - 1) % corners.size()];
            const std::size_t current = corners[j];
            const std::size_t next = corners[(j + 1) % corners.size()];
            const double fq = mesh::polygon_corner_quality(
                mesh.nodes[element.nodes[face[previous]]],
                mesh.nodes[element.nodes[face[current]]],
                mesh.nodes[element.nodes[face[next]]], corners.size());
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

namespace {

struct VolumeRulePoint {
    double weight = 0.0;
    Eigen::Matrix<double, Eigen::Dynamic, 3> dn;
};

std::vector<VolumeRulePoint> make_volume_rule(ElementType type) {
    std::vector<QuadraturePoint> quadrature;
    if (type == ElementType::kTet10) {
        // A quadratic tetrahedral map has a cubic det(J).
        quadrature = tet_rule(3);
    } else if (type == ElementType::kHex20) {
        // One order above the stiffness rule makes the curved-volume measure
        // insensitive to the integration shortcut used by the solver.
        quadrature = hex_rule(4);
    } else {
        quadrature = default_rule(type);
    }
    std::vector<VolumeRulePoint> out;
    out.reserve(quadrature.size());
    for (const auto& qp : quadrature) {
        out.push_back({qp.weight, eval_shape(type, qp.xi).dn});
    }
    return out;
}

const std::vector<VolumeRulePoint>& volume_rule(ElementType type) {
    static const auto rules = [] {
        std::array<std::vector<VolumeRulePoint>, 6> value;
        for (std::size_t i = 0; i < value.size(); ++i) {
            value[i] = make_volume_rule(static_cast<ElementType>(i));
        }
        return value;
    }();
    return rules[static_cast<std::size_t>(type)];
}

} // namespace

double element_volume(const NodalMesh& mesh, const NodalElement& element) {
    if (element.type == ElementType::kPolyVem) {
        double signed_volume = 0.0;
        for (const auto& face : element.faces) {
            if (face.size() < 3) {
                continue;
            }
            const Eigen::Vector3d& a = mesh.nodes[element.nodes[face[0]]];
            for (std::size_t k = 1; k + 1 < face.size(); ++k) {
                const Eigen::Vector3d& b = mesh.nodes[element.nodes[face[k]]];
                const Eigen::Vector3d& c = mesh.nodes[element.nodes[face[k + 1]]];
                signed_volume += a.dot(b.cross(c)) / 6.0;
            }
        }
        return std::abs(signed_volume);
    }
    double total = 0.0;
    for (const auto& qp : volume_rule(element.type)) {
        Eigen::Matrix3d jacobian = Eigen::Matrix3d::Zero();
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            jacobian.noalias() += qp.dn.row(static_cast<Eigen::Index>(a)).transpose() *
                                  mesh.nodes[element.nodes[a]].transpose();
        }
        total += qp.weight * std::abs(jacobian.determinant());
    }
    return total;
}

double mesh_volume(const NodalMesh& mesh) {
    double total = 0.0;
    for (const auto& element : mesh.elements) {
        total += element_volume(mesh, element);
    }
    return total;
}

} // namespace polymesh::fea
