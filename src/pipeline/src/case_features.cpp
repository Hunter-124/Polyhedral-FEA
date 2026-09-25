// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "geom/cad_geometry_features.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {

// --- proximity / load / singularity descriptors ------------------------------
//
// Every geometric quantity below is read from the exact BRep: face areas, CAD
// vertex positions, `geom::CadEdge::dihedral_rad`, closed-form circles through
// exact curve samples, and outward normals from `geom::project_point_on_face`.
// None of it touches the tessellation, and that is a requirement rather than a
// preference: `scripts/advisor/geometry_features.py` recomputes the same ten
// `geo_*` columns from the same STEP through the OCP bindings and has no
// tessellation, so a tessellation-derived column would differ between the two
// sides by the meshing deflection instead of by roundoff. Nothing here is
// fitted, and nothing is thresholded except where the threshold is a definition.

/// Faces below this fraction of the total BRep area are "salient features":
/// hole walls, boss walls, rib flanks, fillet strips. Mirrored as
/// `SALIENT_FACE_AREA_FRACTION` in `scripts/advisor/geometry_features.py`.
constexpr double kSalientFaceAreaFraction = 0.05;

/// Curve / surface sampling density asked of `geom::extract_topology`, matching
/// the exact-CAD selection path in apps/testlab. Three distinct samples is all
/// a circle needs, and four interior stations guarantee them.
constexpr int kFeatureTopologySamples = 4;

/// Relative length tolerance for "the same axis line", "the same radius" and
/// "this sample lies on that circle". The several BRep faces of one bore come
/// from one analytic surface, so they agree to roundoff; 1e-7 of the diagonal
/// is orders of magnitude tighter than any real hole spacing and orders looser
/// than the roundoff.
constexpr double kGeometryRelTol = 1e-7;

/// Relative tolerance for "this query point already lies on that face", loose
/// enough for the extrema solver's own convergence.
constexpr double kOnSurfaceRelTol = 1e-6;

/// Dot-product tolerance for "parallel axes" on unit vectors.
constexpr double kParallelTol = 1e-9;

/// Creases examined for reentrancy. The Williams exponent falls monotonically
/// with the opening angle, so the smallest exponent belongs to the sharpest
/// reentrant crease: the scan walks creases in ascending dihedral order and
/// stops at the first reentrant one. The cap only bounds the pathological
/// all-convex part, and the Python mirror uses the same number.
constexpr std::size_t kMaxCreaseProbes = 256;

/// Ascending-sorted quantile, floor convention (index = floor(q·(n−1)), no
/// interpolation) — the convention `thin_p10_over_diag` already uses below, and
/// the one the Python mirror repeats. `numpy.percentile` interpolates and is
/// therefore deliberately not used on either side.
double quantile_floor(const std::vector<double>& ascending, double q) {
    if (ascending.empty()) {
        return 0.0;
    }
    const auto index =
        static_cast<std::size_t>(q * static_cast<double>(ascending.size() - 1));
    return ascending[std::min(index, ascending.size() - 1)];
}

struct ExactCircle {
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    Eigen::Vector3d axis = Eigen::Vector3d::UnitZ();
    double radius = 0.0;
};

/// Circumcircle of three points in closed form: centre = p0 + a·u + b·v with
/// u = p1−p0, v = p2−p0, and (a, b) from the two equal-distance conditions.
/// Collinear input has no circle — which is exactly how the straight seam edge
/// of a closed cylinder gets rejected.
std::optional<ExactCircle> circle_through(const Eigen::Vector3d& p0,
                                          const Eigen::Vector3d& p1,
                                          const Eigen::Vector3d& p2, double scale) {
    const Eigen::Vector3d u = p1 - p0;
    const Eigen::Vector3d v = p2 - p0;
    const double uu = u.squaredNorm();
    const double vv = v.squaredNorm();
    const double uv = u.dot(v);
    const double det = uu * vv - uv * uv; // |u×v|², zero exactly when collinear
    const double floor_det =
        kGeometryRelTol * kGeometryRelTol * scale * scale * scale * scale;
    if (!(det > floor_det) || !std::isfinite(det)) {
        return std::nullopt;
    }
    const double a = vv * (uu - uv) / (2.0 * det);
    const double b = uu * (vv - uv) / (2.0 * det);
    const Eigen::Vector3d w = a * u + b * v;
    const Eigen::Vector3d normal = u.cross(v);
    ExactCircle circle;
    circle.center = p0 + w;
    circle.radius = w.norm();
    if (!(circle.radius > 0.0) || !std::isfinite(circle.radius) ||
        !(normal.norm() > 0.0)) {
        return std::nullopt;
    }
    circle.axis = normal.normalized();
    return circle;
}

/// The exact circle one BRep edge lies on, or nothing when the edge is not a
/// circular arc. Three probe samples fix the circle and every remaining sample
/// verifies it, so a spline, an ellipse, or a seam line is rejected rather than
/// approximated.
std::optional<ExactCircle> edge_circle(const geom::CadEdge& edge, double scale) {
    const std::size_t n = edge.samples.size();
    if (n < 3 || !(scale > 0.0)) {
        return std::nullopt;
    }
    const auto circle =
        circle_through(edge.samples[0], edge.samples[n / 3], edge.samples[(2 * n) / 3], scale);
    if (!circle) {
        return std::nullopt;
    }
    const double tol = kGeometryRelTol * scale;
    for (const Eigen::Vector3d& sample : edge.samples) {
        const Eigen::Vector3d delta = sample - circle->center;
        if (std::abs(delta.dot(circle->axis)) > tol) {
            return std::nullopt; // out of the circle's plane
        }
        if (std::abs(delta.norm() - circle->radius) > tol) {
            return std::nullopt; // not equidistant from the centre
        }
    }
    return circle;
}

/// One cylindrical bore: the axis line it is drilled along, and its radius.
struct Bore {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    Eigen::Vector3d axis = Eigen::Vector3d::UnitZ();
    double radius = 0.0;
};

/// Distance between two axis LINES: the perpendicular distance when the axes
/// are parallel, the skew-line distance otherwise. Antiparallel axes are the
/// same line, and the cross-product test says so.
double axis_distance(const Bore& a, const Bore& b) {
    const Eigen::Vector3d delta = b.point - a.point;
    const Eigen::Vector3d cross = a.axis.cross(b.axis);
    const double cross_norm = cross.norm();
    if (cross_norm <= kParallelTol) {
        return (delta - delta.dot(a.axis) * a.axis).norm();
    }
    return std::abs(delta.dot(cross / cross_norm));
}

/// Cylindrical bores of the part: cylindrical faces carrying a circular rim
/// whose outward normal points at the axis, i.e. where the material is OUTSIDE
/// the cylinder. A boss or a shaft has its material inside its own cylinder and
/// is deliberately not a bore — the two mean opposite things for a mesh and for
/// a stress field. Faces belonging to one bore (a hole split into
/// half-cylinders) collapse into one entry by axis line and radius, so the count
/// is holes rather than faces.
std::vector<Bore> detect_bores(const geom::CadModel& cad, const geom::CadTopology& topo,
                               double diag) {
    std::vector<Bore> bores;
    if (!(diag > 0.0)) {
        return bores;
    }
    for (const geom::CadFace& face : topo.faces) {
        if (face.kind != geom::CadSurfaceKind::kCylinder || face.samples.empty()) {
            continue;
        }
        std::optional<ExactCircle> rim;
        for (const std::uint32_t edge_id : face.edge_ids) {
            if (edge_id >= topo.edges.size()) {
                continue;
            }
            rim = edge_circle(topo.edges[edge_id], diag);
            if (rim) {
                break;
            }
        }
        if (!rim) {
            continue; // a fillet strip or a trimmed patch, not a drilled bore
        }
        const Eigen::Vector3d probe = face.samples.front();
        const auto hit = geom::project_point_on_face(cad, face.id, probe);
        if (!hit || hit->distance > kOnSurfaceRelTol * diag || !(hit->normal.norm() > 0.5)) {
            continue;
        }
        const Eigen::Vector3d offset = probe - rim->center;
        const Eigen::Vector3d radial_vec = offset - offset.dot(rim->axis) * rim->axis;
        const double radial_norm = radial_vec.norm();
        if (!(radial_norm > kGeometryRelTol * diag)) {
            continue;
        }
        if (hit->normal.normalized().dot(radial_vec / radial_norm) >= 0.0) {
            continue; // outward normal leads away from the axis: boss or shaft
        }
        Bore bore;
        bore.point = rim->center;
        bore.axis = rim->axis;
        bore.radius = rim->radius;
        const bool duplicate = std::any_of(bores.begin(), bores.end(), [&](const Bore& other) {
            return other.axis.cross(bore.axis).norm() <= kParallelTol &&
                   axis_distance(other, bore) <= kGeometryRelTol * diag &&
                   std::abs(other.radius - bore.radius) <= kGeometryRelTol * diag;
        });
        if (!duplicate) {
            bores.push_back(bore);
        }
    }
    return bores;
}

/// Area-weighted centroids of the edge-connected clusters of salient faces.
///
/// A face's position is the mean of its distinct exact CAD vertices rather than
/// its area centroid: the vertex set is an exact BRep property that this code
/// and the Python mirror can each read without having to agree on a surface
/// integration rule. Adjacency is a shared BRep edge, so the two half-cylinders
/// of one hole are one feature while two separate holes are two.
std::vector<Eigen::Vector3d> salient_feature_centroids(const geom::CadTopology& topo) {
    std::vector<Eigen::Vector3d> centroids;
    const std::size_t n_faces = topo.faces.size();
    if (n_faces == 0) {
        return centroids;
    }
    double total_area = 0.0;
    for (const geom::CadFace& face : topo.faces) {
        total_area += std::abs(face.area);
    }
    if (!(total_area > 0.0)) {
        return centroids;
    }
    const double area_limit = kSalientFaceAreaFraction * total_area;

    std::vector<std::size_t> salient;
    std::vector<Eigen::Vector3d> position(n_faces, Eigen::Vector3d::Zero());
    for (std::size_t i = 0; i < n_faces; ++i) {
        const geom::CadFace& face = topo.faces[i];
        if (!(std::abs(face.area) < area_limit)) {
            continue;
        }
        std::set<std::uint32_t> vertex_ids;
        for (const std::uint32_t edge_id : face.edge_ids) {
            if (edge_id >= topo.edges.size()) {
                continue;
            }
            vertex_ids.insert(topo.edges[edge_id].v0);
            vertex_ids.insert(topo.edges[edge_id].v1);
        }
        Eigen::Vector3d sum = Eigen::Vector3d::Zero();
        std::size_t count = 0;
        for (const std::uint32_t vertex_id : vertex_ids) {
            if (vertex_id >= topo.vertices.size()) {
                continue;
            }
            sum += topo.vertices[vertex_id].position;
            ++count;
        }
        if (count == 0) {
            continue; // a fully periodic face has no vertices to stand on
        }
        position[i] = sum / static_cast<double>(count);
        salient.push_back(i);
    }
    if (salient.empty()) {
        return centroids;
    }

    std::vector<std::size_t> parent(n_faces);
    for (std::size_t i = 0; i < n_faces; ++i) {
        parent[i] = i;
    }
    const auto root = [&parent](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    std::unordered_map<std::uint32_t, std::size_t> edge_owner;
    for (const std::size_t face_index : salient) {
        for (const std::uint32_t edge_id : topo.faces[face_index].edge_ids) {
            const auto [it, inserted] = edge_owner.emplace(edge_id, face_index);
            if (!inserted) {
                const std::size_t a = root(it->second);
                const std::size_t b = root(face_index);
                if (a != b) {
                    parent[b] = a;
                }
            }
        }
    }

    // `try_emplace` with an explicit zero, not `operator[]`: a default-built
    // Eigen::Vector3d holds uninitialized memory, so accumulating into a
    // subscript-created entry would sum onto garbage.
    std::map<std::size_t, std::pair<Eigen::Vector3d, double>> clusters;
    for (const std::size_t face_index : salient) {
        const double weight = std::abs(topo.faces[face_index].area);
        const std::size_t key = root(face_index);
        auto found = clusters.find(key);
        if (found == clusters.end()) {
            found = clusters
                        .emplace(key, std::pair<Eigen::Vector3d, double>{
                                          Eigen::Vector3d::Zero(), 0.0})
                        .first;
        }
        found->second.first += weight * position[face_index];
        found->second.second += weight;
    }
    centroids.reserve(clusters.size());
    for (const auto& cluster : clusters) {
        const auto& entry = cluster.second;
        if (entry.second > 0.0) {
            centroids.push_back(entry.first / entry.second);
        }
    }
    return centroids;
}

/// Ascending pairwise distances between points. Fewer than two points means no
/// pair exists at all, which the caller reports as the "infinitely far"
/// sentinel rather than as zero.
std::vector<double> pairwise_distances(const std::vector<Eigen::Vector3d>& points) {
    std::vector<double> distances;
    if (points.size() < 2) {
        return distances;
    }
    distances.reserve(points.size() * (points.size() - 1) / 2);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            const double d = (points[j] - points[i]).norm();
            if (std::isfinite(d)) {
                distances.push_back(d);
            }
        }
    }
    std::sort(distances.begin(), distances.end());
    return distances;
}

/// Smallest Williams eigenvalue in (0,1) for a traction-free wedge of material
/// opening angle `omega` (radians); stress behaves as r^(λ−1), so λ < 1 is a
/// singularity and λ = 1 is a bounded corner.
///
/// The two in-plane modes of a homogeneous isotropic wedge with both radial
/// faces traction free satisfy
///     sin(λ·ω) + λ·sin(ω) = 0   (symmetric)
///     sin(λ·ω) − λ·sin(ω) = 0   (antisymmetric)
/// and neither equation contains Poisson's ratio: for free–free faces the
/// in-plane exponent is ν-independent (Williams 1952; Sinclair, Appl. Mech.
/// Rev. 57(4), 2004). ν enters the clamped-face and bonded-interface members of
/// the same family, which a geometry column cannot know about, so asserting a ν
/// dependence here would be a fabricated fit rather than physics.
///
/// Reference values this reproduces: ω = π → 1 (no corner), ω = 3π/2 → 0.5445
/// (the textbook 90° reentrant corner), ω = 2π → 0.5 (a crack).
///
/// The root finder is deliberately dumb and bounded: a fixed 4096-interval scan
/// of (0,1] for the first sign change, then a fixed 100 bisections. No seed, no
/// convergence test, and no iteration count that depends on the input, so the
/// value is reproducible bit for bit in double precision.
double williams_lambda(double omega) {
    constexpr int kScanIntervals = 4096;
    constexpr int kBisections = 100;
    if (!std::isfinite(omega) || omega <= std::numbers::pi ||
        omega > 2.0 * std::numbers::pi) {
        return 1.0; // outside the reentrant domain (π, 2π]: nothing singular
    }
    double smallest = 1.0;
    for (const double sign : {1.0, -1.0}) {
        const auto residual = [omega, sign](double lambda) {
            return std::sin(lambda * omega) + sign * lambda * std::sin(omega);
        };
        double lo = 1.0 / static_cast<double>(kScanIntervals);
        double f_lo = residual(lo);
        for (int k = 2; k <= kScanIntervals; ++k) {
            const double hi = static_cast<double>(k) / static_cast<double>(kScanIntervals);
            const double f_hi = residual(hi);
            if (f_lo == 0.0) {
                smallest = std::min(smallest, lo);
                break;
            }
            if (f_lo * f_hi < 0.0) {
                double a = lo;
                double b = hi;
                double f_a = f_lo;
                for (int step = 0; step < kBisections; ++step) {
                    const double mid = 0.5 * (a + b);
                    const double f_mid = residual(mid);
                    if ((f_a < 0.0) == (f_mid < 0.0)) {
                        a = mid;
                        f_a = f_mid;
                    } else {
                        b = mid;
                    }
                }
                smallest = std::min(smallest, 0.5 * (a + b));
                break;
            }
            lo = hi;
            f_lo = f_hi;
        }
    }
    return smallest;
}

/// Material opening angle at one sharp BRep crease, radians.
///
/// The magnitude is the exact `geom::CadEdge::dihedral_rad`. That convention is
/// unsigned — a 90° convex edge and a 90° reentrant one both report π/2 — so the
/// sign has to come from geometry: for each of the two adjacent faces, step a
/// short distance off the crease INTO that face (the side whose projection
/// distance stays at zero) and read the outward normal at the crease. The crease
/// is reentrant exactly when each face rises on the outward side of the other,
/// and the material angle is then 2π − dihedral. Returns nothing when either
/// normal or either in-face side is undecidable, so an unmeasurable crease is
/// skipped instead of guessed.
std::optional<double> crease_opening_angle(const geom::CadModel& cad,
                                           const geom::CadEdge& edge, std::uint32_t face_a,
                                           std::uint32_t face_b, double diag) {
    const std::size_t n = edge.samples.size();
    if (n < 3 || !(diag > 0.0) || !(edge.dihedral_rad > 0.0) ||
        !std::isfinite(edge.dihedral_rad)) {
        return std::nullopt;
    }
    const std::size_t mid = n / 2;
    const Eigen::Vector3d point = edge.samples[mid];
    const Eigen::Vector3d along =
        edge.samples[std::min(mid + 1, n - 1)] - edge.samples[mid - 1];
    if (!(along.norm() > 0.0)) {
        return std::nullopt;
    }
    const Eigen::Vector3d tangent = along.normalized();
    const double step = 1e-3 * diag;

    const std::array<std::uint32_t, 2> face_ids{face_a, face_b};
    std::array<Eigen::Vector3d, 2> normals{Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero()};
    std::array<Eigen::Vector3d, 2> inward{Eigen::Vector3d::Zero(), Eigen::Vector3d::Zero()};
    for (std::size_t side = 0; side < 2; ++side) {
        const auto at_crease = geom::project_point_on_face(cad, face_ids[side], point);
        if (!at_crease || at_crease->distance > kOnSurfaceRelTol * diag ||
            !(at_crease->normal.norm() > 0.5)) {
            return std::nullopt;
        }
        normals[side] = at_crease->normal.normalized();
        const Eigen::Vector3d across = normals[side].cross(tangent);
        if (!(across.norm() > 0.0)) {
            return std::nullopt;
        }
        const Eigen::Vector3d direction = across.normalized();
        double best = std::numeric_limits<double>::infinity();
        for (const double orientation : {1.0, -1.0}) {
            const Eigen::Vector3d candidate = point + (orientation * step) * direction;
            const auto probe = geom::project_point_on_face(cad, face_ids[side], candidate);
            if (probe && probe->distance < best) {
                best = probe->distance;
                inward[side] = orientation * direction;
            }
        }
        // A step into the face stays on it (a curved face misses by O(step²·κ));
        // a step off it is a whole step away. Anything between the two is not a
        // decision, and a guessed side would flip the material angle by 2π−2d.
        if (!(best < 0.25 * step)) {
            return std::nullopt;
        }
    }
    const double bend = normals[0].dot(inward[1]) + normals[1].dot(inward[0]);
    return bend > 0.0 ? 2.0 * std::numbers::pi - edge.dihedral_rad : edge.dihedral_rad;
}

/// The two distinct faces on either side of every BRep edge, by edge id.
std::unordered_map<std::uint32_t, std::array<std::uint32_t, 2>>
edge_face_pairs(const geom::CadTopology& topo) {
    std::unordered_map<std::uint32_t, std::array<std::uint32_t, 2>> pairs;
    std::unordered_map<std::uint32_t, std::size_t> counts;
    for (const geom::CadFace& face : topo.faces) {
        std::set<std::uint32_t> distinct(face.edge_ids.begin(), face.edge_ids.end());
        for (const std::uint32_t edge_id : distinct) {
            std::size_t& count = counts[edge_id];
            if (count < 2) {
                pairs[edge_id][count] = face.id;
            }
            ++count;
        }
    }
    for (const auto& [edge_id, count] : counts) {
        if (count != 2) {
            pairs.erase(edge_id);
        }
    }
    return pairs;
}

/// The ten part-level proximity / crease / singularity values, and the salient
/// feature centroids the two boundary-condition columns need. Every default is
/// the documented "nothing to measure" sentinel.
struct ProximityFeatures {
    double n_inner_loops = 0.0;
    double hole_spacing_min_rel = 1.0;
    double hole_spacing_p10_rel = 1.0;
    double feat_pair_dist_min_rel = 1.0;
    double feat_pair_dist_p10_rel = 1.0;
    double feat_pair_dist_mean_rel = 1.0;
    double dihedral_p10 = std::numbers::pi;
    double dihedral_p50 = std::numbers::pi;
    double dihedral_p90 = std::numbers::pi;
    double singular_lambda_min = 1.0;
    std::vector<Eigen::Vector3d> feature_centroids;
};

/// Measure the block from one BRep. Never throws: an OCC failure leaves the
/// sentinels, because "nothing measured" and "zero distance" must not be the
/// same number. The topology is extracted once and shared by all four
/// measurements.
ProximityFeatures proximity_features(const geom::CadModel& cad, double diag) {
    ProximityFeatures out;
    const double inv_diag = diag > 0.0 && std::isfinite(diag) ? 1.0 / diag : 0.0;
    if (!(inv_diag > 0.0)) {
        return out;
    }
    try {
        const geom::CadTopology topo = geom::extract_topology(cad, kFeatureTopologySamples);

        const std::vector<Bore> bores = detect_bores(cad, topo, diag);
        out.n_inner_loops = static_cast<double>(bores.size());
        if (bores.size() >= 2) {
            std::vector<double> spacing;
            spacing.reserve(bores.size() * (bores.size() - 1) / 2);
            for (std::size_t i = 0; i + 1 < bores.size(); ++i) {
                for (std::size_t j = i + 1; j < bores.size(); ++j) {
                    spacing.push_back(axis_distance(bores[i], bores[j]) * inv_diag);
                }
            }
            std::sort(spacing.begin(), spacing.end());
            out.hole_spacing_min_rel = spacing.front();
            out.hole_spacing_p10_rel = quantile_floor(spacing, 0.1);
        }

        out.feature_centroids = salient_feature_centroids(topo);
        const std::vector<double> pairs = pairwise_distances(out.feature_centroids);
        if (!pairs.empty()) {
            double sum = 0.0;
            for (const double distance : pairs) {
                sum += distance;
            }
            out.feat_pair_dist_min_rel = pairs.front() * inv_diag;
            out.feat_pair_dist_p10_rel = quantile_floor(pairs, 0.1) * inv_diag;
            out.feat_pair_dist_mean_rel = sum / static_cast<double>(pairs.size()) * inv_diag;
        }

        // Creases: every sharp BRep edge that actually has two faces and
        // therefore a measured dihedral. Seams and open-shell boundaries are
        // classified sharp with dihedral 0 by `geom::extract_topology`, and a 0
        // there means "not measured" rather than "knife edge", so they are
        // excluded instead of counted as the sharpest creases in the part.
        std::vector<std::pair<double, std::uint32_t>> creases;
        creases.reserve(topo.edges.size());
        for (const geom::CadEdge& edge : topo.edges) {
            if (edge.feature == geom::CadEdgeFeature::kSharp && edge.dihedral_rad > 0.0 &&
                std::isfinite(edge.dihedral_rad)) {
                creases.emplace_back(edge.dihedral_rad, edge.id);
            }
        }
        std::sort(creases.begin(), creases.end());
        if (!creases.empty()) {
            std::vector<double> angles;
            angles.reserve(creases.size());
            for (const auto& crease : creases) {
                angles.push_back(crease.first);
            }
            out.dihedral_p10 = quantile_floor(angles, 0.1);
            out.dihedral_p50 = quantile_floor(angles, 0.5);
            out.dihedral_p90 = quantile_floor(angles, 0.9);
        }

        // The Williams exponent falls monotonically with the opening angle, and
        // a reentrant crease's opening angle is 2π − dihedral, so the sharpest
        // reentrant crease carries the smallest exponent: walk creases
        // sharpest-first and stop at the first reentrant one rather than probing
        // every edge of the part.
        const auto neighbours = edge_face_pairs(topo);
        const std::size_t probes = std::min(creases.size(), kMaxCreaseProbes);
        for (std::size_t k = 0; k < probes; ++k) {
            const std::uint32_t edge_id = creases[k].second;
            const auto faces = neighbours.find(edge_id);
            if (faces == neighbours.end() || edge_id >= topo.edges.size()) {
                continue;
            }
            const auto opening = crease_opening_angle(cad, topo.edges[edge_id],
                                                      faces->second[0], faces->second[1], diag);
            if (!opening || !(*opening > std::numbers::pi)) {
                continue;
            }
            out.singular_lambda_min = williams_lambda(*opening);
            break;
        }
    } catch (...) {
        return ProximityFeatures{};
    }
    return out;
}

} // namespace

CaseFeatures extract_case_features(
    const Model& model, std::span<const RefineRegion> fix_regions,
    std::span<const RefineRegion> load_regions, const Eigen::Vector3d& load_dir,
    double poisson, std::span<const Eigen::Vector3d> load_region_tractions) {
    CaseFeatures out;
    // Salient-feature cluster centroids (world coordinates), measured from the
    // BRep below and consumed after the boundary-condition regions have been
    // measured from the tessellation.
    std::vector<Eigen::Vector3d> feature_centroids;
    const Eigen::Vector3d ext = (model.bbox_max - model.bbox_min).cwiseMax(0.0);
    const double bbox_diag = ext.norm();
    const double inv_diag =
        bbox_diag > 0.0 && std::isfinite(bbox_diag) ? 1.0 / bbox_diag : 0.0;
    out.bbox_dx = ext.x() * inv_diag;
    out.bbox_dy = ext.y() * inv_diag;
    out.bbox_dz = ext.z() * inv_diag;
    out.diag = inv_diag > 0.0 ? 1.0 : 0.0;
    out.n_faces = model.surface.triangles.size();
    out.poisson = std::isfinite(poisson) ? poisson : 0.0;

    // Exact-BRep descriptors for the advisor's OOD test. Same
    // retained-cad-then-reload-source_path pattern as resolve_mesh_size above,
    // guarded so a surface-only or no-OCC build simply reports them unavailable
    // instead of contributing zeros to a Mahalanobis distance.
    if (geom::occ_enabled()) {
        try {
            std::optional<geom::CadModel> cad_owned;
            const geom::CadModel* cad_ptr = nullptr;
            if (model.cad && !model.cad->empty()) {
                cad_ptr = &(*model.cad);
            } else if (!model.source_path.empty()) {
                cad_owned = geom::load_cad(model.source_path);
                if (cad_owned && !cad_owned->empty()) {
                    cad_ptr = &(*cad_owned);
                }
            }
            if (cad_ptr != nullptr) {
                const geom::GeometryDescriptors geo =
                    geom::compute_geometry_descriptors(*cad_ptr);
                if (geo.available) {
                    out.geo_available = true;
                    out.geo_curved_area_frac = geo.curved_area_frac;
                    out.geo_cyl_area_frac = geo.cyl_area_frac;
                    out.geo_plane_area_frac = geo.plane_area_frac;
                    out.geo_other_area_frac = geo.other_area_frac;
                    out.geo_min_curv_radius_rel = geo.min_curv_radius_rel;
                    out.geo_log_curv_radius_mean = geo.log_curv_radius_mean;
                    out.geo_log_curv_radius_std = geo.log_curv_radius_std;
                    out.geo_n_faces = geo.n_faces;
                    out.geo_n_edges = geo.n_edges;
                    out.geo_face_area_cv = geo.face_area_cv;
                    out.geo_aspect_max = geo.aspect_max;
                    out.geo_aspect_mid = geo.aspect_mid;
                    out.geo_volume_frac = geo.volume_frac;
                    out.geo_area_over_v23 = geo.area_over_v23;
                    out.geo_min_face_size_rel = geo.min_face_size_rel;
                }
                // Proximity, crease and regularity descriptors from the same
                // BRep, guarded separately from the descriptor block above: a
                // topology failure must not retract `geo_available`, which
                // decides whether the advisor is allowed to run its OOD test at
                // all. These columns failing is a missing feature; that flag
                // failing is a refusal.
                ProximityFeatures proximity = proximity_features(*cad_ptr, bbox_diag);
                out.geo_n_inner_loops = proximity.n_inner_loops;
                out.geo_hole_spacing_min_rel = proximity.hole_spacing_min_rel;
                out.geo_hole_spacing_p10_rel = proximity.hole_spacing_p10_rel;
                out.geo_feat_pair_dist_min_rel = proximity.feat_pair_dist_min_rel;
                out.geo_feat_pair_dist_p10_rel = proximity.feat_pair_dist_p10_rel;
                out.geo_feat_pair_dist_mean_rel = proximity.feat_pair_dist_mean_rel;
                out.geo_dihedral_p10 = proximity.dihedral_p10;
                out.geo_dihedral_p50 = proximity.dihedral_p50;
                out.geo_dihedral_p90 = proximity.dihedral_p90;
                out.geo_singular_lambda_min = proximity.singular_lambda_min;
                feature_centroids = std::move(proximity.feature_centroids);
            }
        } catch (...) {
            out.geo_available = false;
        }
    }

    double surface_area_m2 = 0.0;
    double signed_volume_m3 = 0.0;
    struct RegionMeasure {
        std::size_t n_faces = 0;
        double area = 0.0;
        Eigen::Vector3d area_centroid = Eigen::Vector3d::Zero();
    };
    RegionMeasure fix;
    RegionMeasure load;
    // Per-load-region selected area, for ranking the load resultants. The
    // aggregate `load` measure stays the union of the regions, unchanged.
    std::vector<double> load_region_area(load_regions.size(), 0.0);
    const auto contains = [](const RefineRegion& region, const Eigen::Vector3d& point) {
        return (region.lo.array() <= region.hi.array()).all() &&
               (point.array() >= region.lo.array()).all() &&
               (point.array() <= region.hi.array()).all();
    };
    const auto selected = [&](std::span<const RefineRegion> regions,
                              const Eigen::Vector3d& point) {
        return std::any_of(regions.begin(), regions.end(), [&](const RefineRegion& region) {
            return contains(region, point);
        });
    };
    for (const auto& tri : model.surface.triangles) {
        if (tri[0] >= model.surface.vertices.size() ||
            tri[1] >= model.surface.vertices.size() ||
            tri[2] >= model.surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
        const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
        const double area = 0.5 * (b - a).cross(c - a).norm();
        if (!(area > 0.0) || !std::isfinite(area)) {
            continue;
        }
        const Eigen::Vector3d centroid = (a + b + c) / 3.0;
        surface_area_m2 += area;
        signed_volume_m3 += a.dot(b.cross(c)) / 6.0;
        if (selected(fix_regions, centroid)) {
            ++fix.n_faces;
            fix.area += area;
            fix.area_centroid += area * centroid;
        }
        bool in_load = false;
        for (std::size_t r = 0; r < load_regions.size(); ++r) {
            if (contains(load_regions[r], centroid)) {
                in_load = true;
                load_region_area[r] += area;
            }
        }
        if (in_load) {
            ++load.n_faces;
            load.area += area;
            load.area_centroid += area * centroid;
        }
    }
    const double volume_m3 = std::abs(signed_volume_m3);
    out.surface_area = surface_area_m2 * inv_diag * inv_diag;
    out.volume = volume_m3 * inv_diag * inv_diag * inv_diag;
    if (out.volume > 0.0) {
        out.sa_over_v23 = out.surface_area / std::pow(out.volume, 2.0 / 3.0);
    }
    out.n_fix_faces = fix.n_faces;
    out.n_load_faces = load.n_faces;
    if (surface_area_m2 > 0.0) {
        out.fix_area_frac = fix.area / surface_area_m2;
        out.load_area_frac = load.area / surface_area_m2;
    }

    try {
        const auto sharp = geom::detect_sharp_edges(model.surface, 30.0);
        out.n_sharp_edges = sharp.size();
        double min_sharp = std::numeric_limits<double>::infinity();
        for (const auto& edge : sharp) {
            if (edge.v0 >= model.surface.vertices.size() ||
                edge.v1 >= model.surface.vertices.size()) {
                continue;
            }
            const double length =
                (model.surface.vertices[edge.v1] - model.surface.vertices[edge.v0]).norm();
            if (length > 0.0 && std::isfinite(length)) {
                out.sharp_edge_len_total += length * inv_diag;
                min_sharp = std::min(min_sharp, length);
            }
        }
        if (std::isfinite(min_sharp)) {
            out.min_feature_h = min_sharp * inv_diag;
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    try {
        const auto curvature = geom::estimate_vertex_curvature(model.surface);
        double sum = 0.0;
        std::size_t n = 0;
        std::size_t curved = 0;
        for (const double kappa : curvature.kappa) {
            if (!(kappa >= 0.0) || !std::isfinite(kappa)) {
                continue;
            }
            const double scaled = kappa * bbox_diag;
            out.kappa_max_h = std::max(out.kappa_max_h, scaled);
            sum += scaled;
            ++n;
            if (scaled > 1e-8) {
                ++curved;
            }
        }
        if (n > 0) {
            out.kappa_mean_h = sum / static_cast<double>(n);
            out.curved_frac = static_cast<double>(curved) / static_cast<double>(n);
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    try {
        const auto thickness = geom::estimate_local_thickness(model.surface);
        std::vector<double> finite;
        finite.reserve(thickness.thickness.size());
        for (const double value : thickness.thickness) {
            if (geom::has_finite_thickness(value) && value >= 0.0) {
                finite.push_back(value * inv_diag);
            }
        }
        if (!finite.empty()) {
            std::sort(finite.begin(), finite.end());
            out.thin_min_over_diag = finite.front();
            const std::size_t p10 =
                static_cast<std::size_t>(0.1 * static_cast<double>(finite.size() - 1));
            out.thin_p10_over_diag = finite[p10];
        }
    } catch (...) {
        // Malformed or empty surfaces retain the finite zero defaults.
    }

    Eigen::Vector3d direction = Eigen::Vector3d::Zero();
    if (load_dir.allFinite() && load_dir.norm() > 0.0) {
        direction = load_dir.normalized();
    }
    out.load_dir_x = direction.x();
    out.load_dir_y = direction.y();
    out.load_dir_z = direction.z();
    if (fix.area > 0.0 && load.area > 0.0) {
        const Eigen::Vector3d delta =
            load.area_centroid / load.area - fix.area_centroid / fix.area;
        out.fix_load_dist_over_diag = delta.norm() * inv_diag;
        if (delta.norm() > 0.0 && direction.norm() > 0.0) {
            out.load_axis_alignment = std::abs(direction.dot(delta.normalized()));
        }
    }

    // Distance from each boundary-condition region to the nearest salient
    // feature: "is the stress raiser under the load" is the interaction the v7
    // vector had no column for. No features, or no selected region, leaves the
    // one-full-diagonal sentinel.
    if (!feature_centroids.empty()) {
        const auto nearest_feature = [&](const Eigen::Vector3d& point) {
            double best = std::numeric_limits<double>::infinity();
            for (const Eigen::Vector3d& centroid : feature_centroids) {
                best = std::min(best, (centroid - point).norm());
            }
            return best;
        };
        if (load.area > 0.0) {
            const double distance = nearest_feature(load.area_centroid / load.area);
            if (std::isfinite(distance)) {
                out.load_to_feature_dist_min_rel = distance * inv_diag;
            }
        }
        if (fix.area > 0.0) {
            const double distance = nearest_feature(fix.area_centroid / fix.area);
            if (std::isfinite(distance)) {
                out.fix_to_feature_dist_min_rel = distance * inv_diag;
            }
        }
    }

    // Load multiaxiality: the angle between the AXES of the two largest load
    // resultants. A region's resultant is its selected area times its traction
    // vector, which is the true resultant of the uniform traction the case
    // files specify rather than a proxy. The axis angle (|cos| before acos)
    // puts it in [0, π/2]: 0 means one load axis however many patches there
    // are, π/2 means two orthogonal ones. Regions whose traction is missing or
    // degenerate fall back to the summed case direction, so a caller with only
    // that direction measures 0 rather than noise.
    if (load_region_area.size() >= 2) {
        std::vector<std::pair<double, Eigen::Vector3d>> resultants;
        resultants.reserve(load_region_area.size());
        for (std::size_t r = 0; r < load_region_area.size(); ++r) {
            if (!(load_region_area[r] > 0.0)) {
                continue;
            }
            Eigen::Vector3d traction = r < load_region_tractions.size()
                                           ? load_region_tractions[r]
                                           : Eigen::Vector3d::Zero();
            if (!traction.allFinite() || !(traction.norm() > 0.0)) {
                traction = direction;
            }
            if (!(traction.norm() > 0.0)) {
                continue;
            }
            resultants.emplace_back(load_region_area[r] * traction.norm(),
                                    traction.normalized());
        }
        if (resultants.size() >= 2) {
            // Magnitude descending, then direction lexicographically: two
            // equal-magnitude patches must not let the sort's internals decide
            // which pair the angle is measured between.
            std::partial_sort(resultants.begin(), resultants.begin() + 2, resultants.end(),
                              [](const auto& lhs, const auto& rhs) {
                                  if (lhs.first != rhs.first) {
                                      return lhs.first > rhs.first;
                                  }
                                  for (int axis = 0; axis < 3; ++axis) {
                                      if (lhs.second[axis] != rhs.second[axis]) {
                                          return lhs.second[axis] < rhs.second[axis];
                                      }
                                  }
                                  return false;
                              });
            const double alignment =
                std::clamp(std::abs(resultants[0].second.dot(resultants[1].second)), 0.0, 1.0);
            out.case_load_multiaxiality = std::acos(alignment);
        }
    }
    return out;
}

} // namespace polymesh::pipeline
