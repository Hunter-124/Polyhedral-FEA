// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Sign-aware cell validity + scale-free shape quality for every fill path.
//
// The mesher gates used to ask `std::abs(volume) <= 1e-14 * h^3`. That single
// expression hides two independent defects:
//
//   * `std::abs` makes the test sign-blind, so a *fully inverted* cell passes
//     as valid. Inverted pyramids therefore shipped to the solver and the VTU
//     (the FE assembly quietly re-flipped the negative halves, so nothing ever
//     complained) — 1712/44536 pyramids on sphere @ h=0.008.
//   * `1e-14 * h^3` is ~13 orders of magnitude below a healthy cell volume
//     (h=0.008 → eps≈5e-21 vs. a healthy tet at ~8.5e-8), so it is a
//     machine-degeneracy test, not a shape floor. Slivers of quality 1e-17
//     were accepted and the snap line-search never unsnapped them.
//
// Everything here is signed (negative ⇒ inverted) and every `*_shape_quality`
// is normalized so that 1.0 is the ideal cell of that kind — one dimensionless
// floor (`kCellShapeFloor`) is therefore meaningful across the whole zoo.

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace polymesh::mesh::validity {

/// Minimum normalized shape quality a cell must keep through boundary snapping
/// and smoothing. Well below every nominal lattice cell (tet fans ≈ 0.1-0.2,
/// lattice pyramids/prisms/hexes ≈ 0.8-1.0) but far above the sliver band the
/// machine-epsilon gates used to accept.
inline constexpr double kCellShapeFloor = 0.02;

/// Signed volume of tet (a,b,c,d); positive for a right-handed ordering.
inline double tet_signed_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                                const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

inline double max_edge(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                       const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(), (b - c).norm(),
                     (b - d).norm(), (c - d).norm()});
}

/// 6·√2·V / L_max³ — 1.0 for a regular tet, 0 for a flat one, negative when
/// inverted. Same metric the hybrid fan-tet gate already used, made signed.
inline double tet_shape_quality(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                                const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    const double emax = max_edge(a, b, c, d);
    if (!(emax > 0.0)) {
        return 0.0;
    }
    return 8.485281374238570 * tet_signed_volume(a, b, c, d) / (emax * emax * emax);
}

/// A quad base admits two tet splits: diagonal 0-2 → (0,1,2,4)+(0,2,3,4),
/// diagonal 1-3 → (1,2,3,4)+(1,3,0,4). `pyramid_best_split_volume`
/// diagnoses whether either split is geometrically usable. The conformity-safe
/// gate and FE assembly below instead use `pyramid_split_diagonal`, whose
/// base-only choice is identical on both sides of a shared quad.
inline double pyramid_split_volume_diag02(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                          const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                          const Eigen::Vector3d& p4) {
    return std::min(tet_signed_volume(p0, p1, p2, p4), tet_signed_volume(p0, p2, p3, p4));
}

inline double pyramid_split_volume_diag13(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                          const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                          const Eigen::Vector3d& p4) {
    return std::min(tet_signed_volume(p1, p2, p3, p4), tet_signed_volume(p1, p3, p0, p4));
}

/// Max over both base diagonals of the min half-volume — negative only when
/// the pyramid is inverted under BOTH splits (truly broken).
inline double pyramid_best_split_volume(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                        const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                        const Eigen::Vector3d& p4) {
    return std::max(pyramid_split_volume_diag02(p0, p1, p2, p3, p4),
                    pyramid_split_volume_diag13(p0, p1, p2, p3, p4));
}

/// Base diagonal the FE assembly splits along: 0 for 0-2, 1 for 1-3.
///
/// Two pyramids sharing a quad face MUST split it along the same diagonal or
/// the implied face triangulations mismatch and the assembled field goes
/// non-conforming (measured: constant-strain patch test degrades from <1e-12
/// to ~2e-5 when the choice is made per-cell from apex-dependent volumes).
/// The choice therefore depends ONLY on the four base nodes: the Newell mean
/// normal n̄ of the quad is compared against each triangulation's two triangle
/// normals, and the diagonal whose worst-aligned triangle is better aligned
/// wins. The metric is invariant under winding reversal (n̄ and every triangle
/// normal flip together), so both cells across a shared face always agree.
/// For a planar quad this reduces to the larger-min-triangle-area rule, which
/// is exactly the larger-min-half-volume rule (the apex height is common).
inline int pyramid_split_diagonal(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                  const Eigen::Vector3d& p2, const Eigen::Vector3d& p3) {
    const Eigen::Vector3d n012 = (p1 - p0).cross(p2 - p0);
    const Eigen::Vector3d n023 = (p2 - p0).cross(p3 - p0);
    const Eigen::Vector3d n123 = (p2 - p1).cross(p3 - p1);
    const Eigen::Vector3d n130 = (p3 - p1).cross(p0 - p1);
    const Eigen::Vector3d nbar = n012 + n023; // Newell mean (either triangulation sums to it)
    const double m02 = std::min(n012.dot(nbar), n023.dot(nbar));
    const double m13 = std::min(n123.dot(nbar), n130.dot(nbar));
    return m13 > m02 ? 1 : 0;
}

/// Min signed half-volume of the split the FE assembly actually integrates.
/// Negative ⇒ that half is inverted.
inline double pyramid_min_split_volume(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                       const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                       const Eigen::Vector3d& p4) {
    return pyramid_split_diagonal(p0, p1, p2, p3) == 1
               ? pyramid_split_volume_diag13(p0, p1, p2, p3, p4)
               : pyramid_split_volume_diag02(p0, p1, p2, p3, p4);
}

/// Base-corner triples of the pyramid (base quad 0..3 CCW, apex 4). Same rows
/// `fea::cell_quality` uses: at base corner i the right-handed edge triple is
/// (next, prev, apex). The apex itself is a collapsed point of the reference
/// map, so it has no Jacobian.
inline constexpr std::array<std::array<int, 3>, 4> kPyramidBaseCorners{{
    {{1, 3, 4}},
    {{2, 0, 4}},
    {{3, 1, 4}},
    {{0, 2, 4}},
}};

/// 1/√2 — the base-corner scaled Jacobian a regular (all-edges-equal) pyramid
/// attains, so the normalized measure below is 1.0 for that cell.
inline constexpr double kPyramidCornerIdeal = 0.7071067811865476;

/// Min base-corner scaled Jacobian, normalized so a regular pyramid scores 1.0
/// (the lattice hex→pyramid expand cell scores 0.816). Negative ⇒ a base corner
/// has folded: the isoparametric map turns inside out there even when both
/// assembly split tets still have positive volume.
inline double pyramid_min_corner_scaled_jacobian(const Eigen::Vector3d& p0,
                                                 const Eigen::Vector3d& p1,
                                                 const Eigen::Vector3d& p2,
                                                 const Eigen::Vector3d& p3,
                                                 const Eigen::Vector3d& p4) {
    const std::array<Eigen::Vector3d, 5> p{{p0, p1, p2, p3, p4}};
    double lo = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < kPyramidBaseCorners.size(); ++i) {
        const auto& t = kPyramidBaseCorners[i];
        const Eigen::Vector3d& o = p[i];
        const Eigen::Vector3d e1 = p[static_cast<std::size_t>(t[0])] - o;
        const Eigen::Vector3d e2 = p[static_cast<std::size_t>(t[1])] - o;
        const Eigen::Vector3d e3 = p[static_cast<std::size_t>(t[2])] - o;
        const double den = e1.norm() * e2.norm() * e3.norm();
        if (!(den > 0.0)) {
            return 0.0;
        }
        lo = std::min(lo, e1.dot(e2.cross(e3)) / den / kPyramidCornerIdeal);
    }
    return std::clamp(lo, -1.0, 1.0);
}

/// Volume / mean-edge³ the all-edges-equal pyramid attains — the normalizer of
/// the collapse term below.
inline constexpr double kPyramidVolumeIdeal = 0.23570226039551587;

/// Signed volume / mean-edge³, normalized so a regular pyramid scores 1.0. The
/// volume is the divergence-theorem sum over the five outward faces, the warped
/// base fanned from its own centroid — the same value `fea::cell_quality`
/// integrates, so no split diagonal biases it.
inline double pyramid_volume_collapse(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                      const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                      const Eigen::Vector3d& p4) {
    const std::array<Eigen::Vector3d, 5> p{{p0, p1, p2, p3, p4}};
    static constexpr std::array<std::array<int, 2>, 8> kEdges{{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}, {{0, 4}}, {{1, 4}}, {{2, 4}}, {{3, 4}},
    }};
    double edge_sum = 0.0;
    for (const auto& e : kEdges) {
        edge_sum += (p[static_cast<std::size_t>(e[1])] - p[static_cast<std::size_t>(e[0])]).norm();
    }
    const double mean_edge = edge_sum / static_cast<double>(kEdges.size());
    if (!(mean_edge > 0.0)) {
        return 0.0;
    }
    struct Face {
        int n;
        std::array<int, 4> v;
    };
    static constexpr std::array<Face, 5> kFaces{{
        {4, {{0, 3, 2, 1}}},
        {3, {{0, 1, 4, 0}}},
        {3, {{1, 2, 4, 0}}},
        {3, {{2, 3, 4, 0}}},
        {3, {{3, 0, 4, 0}}},
    }};
    const Eigen::Vector3d& origin = p[0];
    double volume = 0.0;
    for (const auto& f : kFaces) {
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int k = 0; k < f.n; ++k) {
            centroid += p[static_cast<std::size_t>(f.v[static_cast<std::size_t>(k)])];
        }
        centroid = centroid / static_cast<double>(f.n) - origin;
        for (int k = 0; k < f.n; ++k) {
            const Eigen::Vector3d a =
                p[static_cast<std::size_t>(f.v[static_cast<std::size_t>(k)])] - origin;
            const Eigen::Vector3d b =
                p[static_cast<std::size_t>(f.v[static_cast<std::size_t>((k + 1) % f.n)])] - origin;
            volume += a.dot(b.cross(centroid)) / 6.0;
        }
    }
    return volume / (mean_edge * mean_edge * mean_edge) / kPyramidVolumeIdeal;
}

/// Normalized quality of the two split tets the FE assembly actually integrates:
/// min quality of the pair, scaled by the lattice nominal (square base, apex over
/// the centre at half the base width — the hex→pyramid expand cell — whose split
/// tets score exactly 0.25), over the SAME diagonal the assembly splits
/// (`pyramid_split_diagonal`), so the gate and the integrator always agree.
///
/// This is the snapping/smoothing gate. It deliberately does NOT include the
/// volume-collapse or base-corner terms: adding either makes the gate stricter
/// than the mesh can satisfy while still reaching the wall, and the snap then
/// buys cell shape by retreating boundary nodes — measured 2026-08-15, folding
/// the collapse term in here took the hybrid sphere's M1max from 1.7e-16 to
/// 0.037 at h=0.15*extent. A corner fold is instead cured at conversion, for
/// free, by `pyramid_corner_folded`.
inline double pyramid_split_shape_quality(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                          const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                          const Eigen::Vector3d& p4) {
    const double q02 =
        std::min(tet_shape_quality(p0, p1, p2, p4), tet_shape_quality(p0, p2, p3, p4));
    const double q13 =
        std::min(tet_shape_quality(p1, p2, p3, p4), tet_shape_quality(p1, p3, p0, p4));
    return 4.0 * (pyramid_split_diagonal(p0, p1, p2, p3) == 1 ? q13 : q02);
}

/// Full normalized pyramid shape — the representation measure above AND the
/// base-corner scaled Jacobian, i.e. the same worst-of measure
/// `fea::cell_quality` reports for a kPyramid5.
///
/// The two differ on exactly one defect: a base quad that boundary snapping
/// warped until one of its corners folded. Measured 2026-08-15 on
/// icecream_cone h=0.008 hybrid, 960 of 24286 shipped pyramids scored < 0 under
/// `fea::cell_quality` (every one corner-driven) while the representation
/// measure rated all of them >= 0.0608 — so no offender collector ever saw
/// them and 4% of the product mesh went out with a folded isoparametric map.
///
/// Unsnapping the wall is the wrong cure (measured: icecream_cone h=0.008 exact
/// BRep p99/h 0.019 → 0.107 when this measure gates the snap). A corner-folded
/// pyramid whose split halves are healthy is the union of those two healthy
/// tets, which is also exactly the stiffness the assembly builds from it — so
/// `mesh_from_mixed_cells` ships it AS those two tets: same geometry, same
/// stiffness, no folded cell. This measure is therefore the honest report and
/// the decomposition trigger, never a reason to move a boundary node.
inline double pyramid_shape_quality(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                    const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                    const Eigen::Vector3d& p4) {
    return std::min({pyramid_split_shape_quality(p0, p1, p2, p3, p4),
                     pyramid_min_corner_scaled_jacobian(p0, p1, p2, p3, p4),
                     pyramid_volume_collapse(p0, p1, p2, p3, p4)});
}

/// True when the pyramid's base-corner Jacobian has folded below the shape floor
/// while both assembly split tets still have positive volume — the cell the
/// conversion must ship as two tets instead of one pyramid. The second condition
/// is validity, not quality: the split is worth taking whenever the two tets are
/// right-handed, because a positive-volume tet of any aspect beats a cell whose
/// map is inside out.
inline bool pyramid_corner_folded(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                  const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                  const Eigen::Vector3d& p4,
                                  double shape_floor = kCellShapeFloor) {
    return pyramid_min_corner_scaled_jacobian(p0, p1, p2, p3, p4) < shape_floor &&
           pyramid_min_split_volume(p0, p1, p2, p3, p4) > 0.0;
}

inline constexpr std::array<std::array<double, 3>, 8> kHexCornerSigns{{
    {{-1, -1, -1}},
    {{1, -1, -1}},
    {{1, 1, -1}},
    {{-1, 1, -1}},
    {{-1, -1, 1}},
    {{1, -1, 1}},
    {{1, 1, 1}},
    {{-1, 1, 1}},
}};

/// 2×2×2 Gauss points of the trilinear hex — the points the FE assembly
/// integrates at, so these are exactly the detJ values that can make the
/// solver report a non-positive Jacobian.
inline constexpr double kGauss2 = 0.5773502691896257;
inline constexpr std::array<std::array<double, 3>, 8> kHexGauss2x2x2{{
    {{-kGauss2, -kGauss2, -kGauss2}},
    {{kGauss2, -kGauss2, -kGauss2}},
    {{-kGauss2, kGauss2, -kGauss2}},
    {{kGauss2, kGauss2, -kGauss2}},
    {{-kGauss2, -kGauss2, kGauss2}},
    {{kGauss2, -kGauss2, kGauss2}},
    {{-kGauss2, kGauss2, kGauss2}},
    {{kGauss2, kGauss2, kGauss2}},
}};

inline double hex8_jacobian_det(const std::array<Eigen::Vector3d, 8>& x,
                                const Eigen::Vector3d& xi) {
    Eigen::Matrix3d jac = Eigen::Matrix3d::Zero();
    for (int a = 0; a < 8; ++a) {
        const auto& s = kHexCornerSigns[static_cast<std::size_t>(a)];
        const double dxi = 0.125 * s[0] * (1.0 + s[1] * xi[1]) * (1.0 + s[2] * xi[2]);
        const double deta = 0.125 * s[1] * (1.0 + s[0] * xi[0]) * (1.0 + s[2] * xi[2]);
        const double dzeta = 0.125 * s[2] * (1.0 + s[0] * xi[0]) * (1.0 + s[1] * xi[1]);
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

/// Min detJ over the centre and the eight 2×2×2 Gauss points. ≤ 0 ⇒ the hex
/// would produce a non-positive Jacobian during assembly.
inline double hex8_min_jacobian(const std::array<Eigen::Vector3d, 8>& x) {
    double lo = hex8_jacobian_det(x, Eigen::Vector3d::Zero());
    for (const auto& gp : kHexGauss2x2x2) {
        lo = std::min(lo, hex8_jacobian_det(x, Eigen::Vector3d(gp[0], gp[1], gp[2])));
    }
    return lo;
}

/// Corner triples of the trilinear hex (VTK/Verdict ordering).
inline constexpr std::array<std::array<int, 4>, 8> kHexCornerTriples{{
    {{0, 1, 3, 4}},
    {{1, 2, 0, 5}},
    {{2, 3, 1, 6}},
    {{3, 0, 2, 7}},
    {{4, 7, 5, 0}},
    {{5, 4, 6, 1}},
    {{6, 5, 7, 2}},
    {{7, 6, 4, 3}},
}};

/// Min corner scaled Jacobian AND signed volume / mean-edge³. Both terms are
/// normalized so a cube is 1.0; taking their minimum catches complementary
/// defects: corner skew/folding and dimensional collapse/stretch. In
/// particular, the corner term alone calls a 1×1×1e-4 pancake perfect.
inline double hex8_shape_quality(const std::array<Eigen::Vector3d, 8>& x) {
    double corner = 1.0;
    for (const auto& t : kHexCornerTriples) {
        const Eigen::Vector3d& o = x[static_cast<std::size_t>(t[0])];
        const Eigen::Vector3d e1 = x[static_cast<std::size_t>(t[1])] - o;
        const Eigen::Vector3d e2 = x[static_cast<std::size_t>(t[2])] - o;
        const Eigen::Vector3d e3 = x[static_cast<std::size_t>(t[3])] - o;
        const double den = e1.norm() * e2.norm() * e3.norm();
        if (!(den > 0.0)) {
            return 0.0;
        }
        corner = std::min(corner, e1.dot(e2.cross(e3)) / den);
    }

    // Same topological edges and outward face-centroid fan used by
    // fea::cell_quality's volume_collapse_term.
    static constexpr std::array<std::array<int, 2>, 12> kEdges{{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}, {{4, 5}}, {{5, 6}},
        {{6, 7}}, {{7, 4}}, {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
    }};
    static constexpr std::array<std::array<int, 4>, 6> kFaces{{
        {{0, 3, 2, 1}}, {{4, 5, 6, 7}}, {{0, 1, 5, 4}},
        {{1, 2, 6, 5}}, {{2, 3, 7, 6}}, {{3, 0, 4, 7}},
    }};
    double edge_sum = 0.0;
    for (const auto& e : kEdges) {
        edge_sum +=
            (x[static_cast<std::size_t>(e[1])] - x[static_cast<std::size_t>(e[0])]).norm();
    }
    const double mean_edge = edge_sum / static_cast<double>(kEdges.size());
    if (!(mean_edge > 0.0)) {
        return 0.0;
    }
    const Eigen::Vector3d& origin = x[0];
    double volume = 0.0;
    for (const auto& face : kFaces) {
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (const int i : face) {
            centroid += x[static_cast<std::size_t>(i)];
        }
        centroid = centroid / 4.0 - origin;
        for (std::size_t k = 0; k < face.size(); ++k) {
            const Eigen::Vector3d a = x[static_cast<std::size_t>(face[k])] - origin;
            const Eigen::Vector3d b =
                x[static_cast<std::size_t>(face[(k + 1) % face.size()])] - origin;
            volume += a.dot(b.cross(centroid)) / 6.0;
        }
    }
    const double volume_collapse =
        volume / (mean_edge * mean_edge * mean_edge); // cube ideal = 1
    return std::clamp(std::min(corner, volume_collapse), -1.0, 1.0);
}

/// Corner triples of the linear prism (base 0-1-2, top 3-4-5, node k+3 above
/// node k). All six dets are positive iff the prism is right-handed under the
/// same convention as `prism_signed_volume`.
inline constexpr std::array<std::array<int, 4>, 6> kPrismCornerTriples{{
    {{0, 1, 2, 3}},
    {{1, 2, 0, 4}},
    {{2, 0, 1, 5}},
    {{3, 5, 4, 0}},
    {{4, 3, 5, 1}},
    {{5, 4, 3, 2}},
}};

/// Min per-corner detJ of the prism. The summed prism volume stays positive
/// while a single corner has already folded, so this is the value the assembly
/// Gauss loop actually sees.
inline double prism_min_corner_jacobian(const std::array<Eigen::Vector3d, 6>& p) {
    double lo = std::numeric_limits<double>::infinity();
    for (const auto& t : kPrismCornerTriples) {
        const Eigen::Vector3d& o = p[static_cast<std::size_t>(t[0])];
        const Eigen::Vector3d e1 = p[static_cast<std::size_t>(t[1])] - o;
        const Eigen::Vector3d e2 = p[static_cast<std::size_t>(t[2])] - o;
        const Eigen::Vector3d e3 = p[static_cast<std::size_t>(t[3])] - o;
        lo = std::min(lo, e1.dot(e2.cross(e3)));
    }
    return lo;
}

/// Min corner scaled Jacobian normalized so an equilateral right prism is 1.0.
inline double prism_shape_quality(const std::array<Eigen::Vector3d, 6>& p) {
    double lo = 1.0;
    for (const auto& t : kPrismCornerTriples) {
        const Eigen::Vector3d& o = p[static_cast<std::size_t>(t[0])];
        const Eigen::Vector3d e1 = p[static_cast<std::size_t>(t[1])] - o;
        const Eigen::Vector3d e2 = p[static_cast<std::size_t>(t[2])] - o;
        const Eigen::Vector3d e3 = p[static_cast<std::size_t>(t[3])] - o;
        const double den = e1.norm() * e2.norm() * e3.norm();
        if (!(den > 0.0)) {
            return 0.0;
        }
        lo = std::min(lo, e1.dot(e2.cross(e3)) / den);
    }
    // sin(60°) — the best a triangular cross-section can do.
    return lo / 0.8660254037844386;
}

} // namespace polymesh::mesh::validity
