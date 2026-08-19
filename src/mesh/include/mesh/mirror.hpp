// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Reflection symmetry of the meshed geometry, and the query gate that makes
// every geometry-derived mesher decision reflection-equivariant (ADR-0036 §7).
//
// The lattice tiling itself mirrors (mesh/lattice_split.hpp) and the
// accept/reject passes order themselves on a mirror-invariant key
// (mesh/surface_project.hpp `MirrorKeyFrame`), yet a mirror-symmetric part still
// came out with a visibly asymmetric element pattern: measured on sphere.step at
// h = 8 mm, only 75.4 / 95.9 / 83.1% of tets had a mirror image about the three
// bbox mid-planes. The reason is upstream of ordering. Every decision the mesher
// makes about *where the material is* — inside/outside parity, the child mask,
// curvature and feature stamps, jut detection, the snap target — is read off the
// OCC tessellation, and that tessellation is not mirror-symmetric: at the
// product's own settings (5e-4·diag deflection, 0.2 rad) the fraction of
// tessellation vertices with an exact mirror partner is
//
//   sphere      x  0.00%   y 99.69%   z  1.33%
//   plate_hole  x  5.97%   y  100%    z  100%
//   cylinder    x  100%    y  100%    z  100%
//
// A seam placed on one side of a plane, or a facet row that starts half a facet
// further along, gives a cell and its mirror image genuinely different inputs,
// and no amount of tie-breaking can recover a symmetry the inputs never had.
//
// So the geometry is queried through a fold instead. When the *exact* geometry is
// verified mirror-symmetric about a bbox mid-plane, a query point is reflected
// into the low-side octant first, the answer is computed there, and point-valued
// answers are reflected back. A point and its mirror image then fold to the same
// canonical point, so they receive the same answer by construction — the decision
// is equivariant no matter how lopsided the tessellation is. Because the fold is
// only ever installed after the reflected geometry is *measured* to lie on the
// exact BRep, the folded answer is the same answer: fidelity is unchanged.

#include "geom/cad_model.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <vector>

namespace polymesh::mesh {

/// Verified mirror planes of the meshed geometry, as axis-normal planes through
/// `center`. A default-constructed frame has no planes and gates nothing.
struct MirrorFrame {
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    /// `plane[a]` — reflecting axis `a` about `center[a]` maps the geometry onto
    /// itself.
    std::array<bool, 3> plane{{false, false, false}};
    /// Largest distance from a reflected exact geometry sample back to the
    /// geometry, over the accepted planes, as a fraction of the bbox diagonal.
    /// Reported so the fold's geometric cost is observable rather than assumed.
    double max_residual_over_diag = 0.0;
    /// How close to a plane a node must be to count as sitting ON it, in metres.
    /// Set to 1e-9 of the bbox diagonal at detection.
    double plane_tolerance = 0.0;

    [[nodiscard]] bool any() const { return plane[0] || plane[1] || plane[2]; }

    [[nodiscard]] int count() const {
        return (plane[0] ? 1 : 0) + (plane[1] ? 1 : 0) + (plane[2] ? 1 : 0);
    }

    /// Canonical representative of `p`'s reflection orbit: the low-side octant
    /// of every accepted plane. Written as `c - |p - c|` so a point and its
    /// mirror image fold to the same value up to rounding of the reflection
    /// itself, which is the tightest either can be — mirrored coordinates agree
    /// to a few ulp, never exactly (ADR-0036).
    [[nodiscard]] Eigen::Vector3d fold(const Eigen::Vector3d& p) const {
        Eigen::Vector3d q = p;
        for (int a = 0; a < 3; ++a) {
            if (plane[static_cast<std::size_t>(a)]) {
                q[a] = center[a] - std::abs(p[a] - center[a]);
            }
        }
        return q;
    }

    /// Does `p` sit on the mirror plane of axis `a`?
    [[nodiscard]] bool on_plane(const Eigen::Vector3d& p, int a) const {
        return plane[static_cast<std::size_t>(a)] &&
               std::abs(p[a] - center[a]) <= plane_tolerance;
    }

    /// Force every coordinate of `target` whose axis plane `at` sits on back onto
    /// that plane.
    ///
    /// A node ON a mirror plane is its own reflection, so an orbit lock imposes
    /// nothing on it — yet any motion with a component normal to the plane breaks
    /// the symmetry by itself. Measured on plate_hole at h = 6 mm with every other
    /// stage exact, exactly two nodes ended up off the x plane (at 0.0006 of the
    /// extent) and cost 8 tets their mirror image; on icecream_cone the same
    /// mechanism moved rim nodes off the plane they sat on.
    [[nodiscard]] Eigen::Vector3d clamp_to_planes(const Eigen::Vector3d& target,
                                                  const Eigen::Vector3d& at) const {
        Eigen::Vector3d r = target;
        for (int a = 0; a < 3; ++a) {
            if (on_plane(at, a)) {
                r[a] = center[a];
            }
        }
        return r;
    }

    /// Reflect a point computed in the folded frame back into the octant `like`
    /// came from, then hold it on any plane `like` sits on. Use for anything
    /// positional: projection targets, closest points, pin sites.
    [[nodiscard]] Eigen::Vector3d unfold(const Eigen::Vector3d& q,
                                         const Eigen::Vector3d& like) const {
        Eigen::Vector3d r = q;
        for (int a = 0; a < 3; ++a) {
            if (plane[static_cast<std::size_t>(a)] && like[a] > center[a]) {
                r[a] = 2.0 * center[a] - q[a];
            }
        }
        return clamp_to_planes(r, like);
    }

    /// Reflect a direction computed in the folded frame back into `like`'s
    /// octant. Free vectors carry no origin, so only the sign flips.
    [[nodiscard]] Eigen::Vector3d unfold_direction(const Eigen::Vector3d& v,
                                                   const Eigen::Vector3d& like) const {
        Eigen::Vector3d r = v;
        for (int a = 0; a < 3; ++a) {
            if (plane[static_cast<std::size_t>(a)] && like[a] > center[a]) {
                r[a] = -v[a];
            }
        }
        return r;
    }
};

/// `frame.fold(p)` with a null-safe frame pointer, for call sites that thread an
/// optional frame.
[[nodiscard]] inline Eigen::Vector3d mirror_fold(const MirrorFrame* frame,
                                                 const Eigen::Vector3d& p) {
    return frame != nullptr ? frame->fold(p) : p;
}

/// `frame.unfold(q, like)` with a null-safe frame pointer.
[[nodiscard]] inline Eigen::Vector3d mirror_unfold(const MirrorFrame* frame,
                                                   const Eigen::Vector3d& q,
                                                   const Eigen::Vector3d& like) {
    return frame != nullptr ? frame->unfold(q, like) : q;
}

/// Mirror planes of an **exact BRep** about the mid-planes of the given bbox.
///
/// The test is dense sampling of the exact trimmed faces
/// (`geom::sample_brep_surface`), reflected and projected back onto the BRep
/// (`geom::project_point_on_surface`). A plane is accepted only when every
/// reflected sample lands on the solid within `tol_frac` of the bbox diagonal.
///
/// Sampling the faces, rather than comparing topology, is deliberate: a
/// mirror-symmetric solid need not have mirror-symmetric topology. A sphere's
/// seam edge lies wholly on one side of the x = 0 plane, so a topology match
/// rejects the sphere's x symmetry, which is real.
///
/// The tolerance does double duty. A reflected sample's residual is ~2δ where δ
/// is the offset between the bbox mid-plane and the geometry's true plane, so
/// accepting at 1e-7 of the diagonal also certifies that the plane the *lattice*
/// mirrors about is the plane the *geometry* mirrors about to within 5e-8 of the
/// diagonal — four orders below the 1e-4 boundary-fidelity bar, and an order
/// below the 1e-6 tolerance at which mirrored nodes are counted as partners.
///
/// Returns an empty frame for STL-only/OCC-disabled builds.
[[nodiscard]] MirrorFrame detect_mirror_frame(const geom::CadModel& cad,
                                              const Eigen::Vector3d& bbox_min,
                                              const Eigen::Vector3d& bbox_max,
                                              double tol_frac = 1e-7);


/// Reflection-orbit lookup over a mesh node array.
///
/// Folding the geometry queries makes every *input* symmetric, which is
/// necessary and not sufficient: a pass that mutates the mesh sequentially — the
/// sliver-cap collapse is the measured case — can still take a decision on one
/// side and find it illegal on the other, because the first decision changed the
/// state the second is judged against. Measured on cylinder.step at h = 8 mm with
/// every query folded, the collapse round entered at exactly 100/100/100%
/// mirrored tets and left at 99.7/98.8/99.4%: 170 of 1880 collapses had no mirror
/// image, all of them on the curved wall, none of them near a mid-plane.
///
/// The cure is to decide for a whole orbit at once, which needs the orbit: this
/// maps a node to the node sitting at its reflected position. A missing partner
/// answers `npos`, and the caller then refuses the decision rather than applying
/// half of it.
class MirrorNodeOrbit {
  public:
    static constexpr std::uint32_t npos = 0xffffffffu;

    /// Empty (inactive) when `frame` has no plane. `tol` is the position match
    /// tolerance in metres; mirrored node coordinates agree to a few ulp, so any
    /// tolerance far below the node spacing works.
    MirrorNodeOrbit(const MirrorFrame& frame, const std::vector<Eigen::Vector3d>& nodes,
                    double tol);

    [[nodiscard]] bool active() const { return active_; }

    /// Number of non-identity reflection subsets: 2^(planes) − 1.
    [[nodiscard]] unsigned reflection_count() const { return reflections_; }

    /// `p` reflected by the axes selected in `mask` (bit `a` reflects the a-th
    /// *plane axis*, in ascending axis order).
    [[nodiscard]] Eigen::Vector3d reflect(const Eigen::Vector3d& p, unsigned mask) const;

    /// Node at `nodes[node]` reflected by `mask`, or `npos` when no node sits
    /// there.
    [[nodiscard]] std::uint32_t reflected(std::uint32_t node, unsigned mask) const;

    /// The node at the canonical (low-side) point of `node`'s orbit, together
    /// with the mask that maps that canonical point back to `node`'s own octant.
    /// `{npos, 0}` when the canonical position carries no node.
    [[nodiscard]] std::pair<std::uint32_t, unsigned> canonical(std::uint32_t node) const;

  private:
    struct Cell {
        long long x, y, z;
        bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct CellHash {
        std::size_t operator()(const Cell& c) const noexcept;
    };

    [[nodiscard]] Cell cell_of(const Eigen::Vector3d& p) const;

    const std::vector<Eigen::Vector3d>* nodes_ = nullptr;
    MirrorFrame frame_;
    std::array<int, 3> axes_{{-1, -1, -1}}; // plane axes, ascending
    int n_axes_ = 0;
    unsigned reflections_ = 0;
    bool active_ = false;
    double tol_ = 0.0;
    double inv_quantum_ = 0.0;
    std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> buckets_;
};

/// Mirror planes of a triangle surface about its own bbox mid-planes.
///
/// Used when there is no BRep at all (STL input, OCC-disabled build). Here the
/// tessellation *is* the geometry, so the test is exact and combinatorial: every
/// vertex must have a mirror partner within `tol_frac` of the diagonal, and the
/// reflected triangle set must be the triangle set. Nothing weaker is safe —
/// a fold justified by "close enough" tessellation would mirror away real
/// asymmetric detail.
[[nodiscard]] MirrorFrame detect_mirror_frame(const geom::TriSurface& surface,
                                             double tol_frac = 1e-9);

} // namespace polymesh::mesh
