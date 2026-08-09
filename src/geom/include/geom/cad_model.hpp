// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// First-class CAD BRep model (ADR-0020). Product meshing should use CadModel
// as the geometry source; TriSurface tessellation is derived for viz / legacy
// hybrid fill / compare only.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>
#include <cstddef>
#include <cstdint>

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace polymesh::geom {

/// Live BRep-backed part geometry. When built without POLYMESH_WITH_OCC,
/// load_* throw GeomError("OpenCASCADE not enabled").
///
/// Units: metres (SI), matching the rest of geom.
class CadModel {
  public:
    CadModel() = default;

    /// Load a STEP (.step / .stp) file into a retained BRep.
    [[nodiscard]] static CadModel load_step(const std::filesystem::path& path);

    /// Load a native OpenCASCADE BREP (.brep / .brp) file.
    [[nodiscard]] static CadModel load_brep(const std::filesystem::path& path);

    /// True when a non-null BRep is held.
    [[nodiscard]] bool empty() const noexcept;

    /// True when this build has OCC and a shape is loaded.
    [[nodiscard]] bool has_brep() const noexcept;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    [[nodiscard]] Eigen::Vector3d bbox_min() const noexcept { return bbox_min_; }
    [[nodiscard]] Eigen::Vector3d bbox_max() const noexcept { return bbox_max_; }
    [[nodiscard]] double bbox_diagonal() const noexcept;

    /// Derived surface triangulation for viewport / legacy hybrid bridge /
    /// compare. Does **not** replace the BRep. `deflection` ≤ 0 selects an
    /// automatic linear sag (fraction of the bbox diagonal). `angular_deflection`
    /// ≤ 0 selects the default (~0.2 rad) cap on facet-normal turn across a
    /// curved face — the knob that controls pipe/fillet smoothness.
    [[nodiscard]] TriSurface tessellate(double deflection = 0.0,
                                        double angular_deflection = 0.0) const;

    /// Alias used by hybrid_zoo until it is fully BRep-native (ADR-0020).
    [[nodiscard]] TriSurface boundary_surface_for_legacy_fill(double deflection = 0.0) const {
        return tessellate(deflection);
    }

    /// Opaque OCC shape accessor for mesher code compiled with OCC.
    /// Returns nullptr when empty or without OCC.
    [[nodiscard]] const void* shape_handle() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::string name_;
    Eigen::Vector3d bbox_min_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d bbox_max_ = Eigen::Vector3d::Zero();

    void compute_bbox();
};
/// Stable topological owner of an exact BRep projection. IDs are zero-based
/// and follow the same TopExp::MapShapes order used by CadTopology. Degenerate
/// OCC edges are omitted from the edge-id sequence, also matching CadEdge::id.
enum class CadSupportKind : std::uint8_t {
    kUnknown = 0,
    kVertex,
    kEdge,
    kFace,
};

inline constexpr std::uint32_t kInvalidCadSupportId =
    std::numeric_limits<std::uint32_t>::max();

/// Closest-point projection of a query onto the live BRep surface (ADR-0024 Q2a).
struct ProjectResult {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    /// Unit normal at the projected point, oriented outward-ish (face
    /// orientation with REVERSED flipped). Zero if unavailable.
    Eigen::Vector3d normal = Eigen::Vector3d::Zero();
    /// Primary exact support returned by the extrema solve.
    CadSupportKind support_kind = CadSupportKind::kUnknown;
    std::uint32_t support_id = kInvalidCadSupportId;
    /// Stable trimmed face used by the projection, even when the primary
    /// support is one of that face's boundary edges or vertices.
    std::uint32_t face_id = kInvalidCadSupportId;
    /// Euclidean distance |p − point| (metres).
    double distance = 0.0;
};

/// Read-only evidence about the retained BRep. Counts are unique topological
/// entities, volume is in m³, and surface area is in m².
///
/// An empty model or a build without OpenCASCADE returns the deterministic
/// default (`available == false`) rather than throwing.
struct BRepInspection {
    bool available = false;
    bool valid = false;
    bool closed = false;
    std::size_t solid_count = 0;
    std::size_t shell_count = 0;
    std::size_t closed_shell_count = 0;
    std::size_t face_count = 0;
    std::size_t edge_count = 0;
    std::size_t vertex_count = 0;
    double volume = 0.0;
    double surface_area = 0.0;
};

/// Inspect validity, watertight-shell evidence, topology, and exact BRep
/// properties without exposing OpenCASCADE types.
[[nodiscard]] BRepInspection inspect_brep(const CadModel& model);

/// Result of deterministic exact trimmed-face sampling. Counters make the hard
/// attempt and storage ceilings observable to diagnostics and regressions.
struct BRepSurfaceSamples {
    std::vector<Eigen::Vector3d> points;
    std::size_t face_count = 0;
    std::size_t uv_attempt_count = 0;
    std::size_t fallback_vertex_count = 0;
};

/// Deterministically sample exact points on the trimmed faces of the retained
/// BRep without constructing an OCC triangulation. `points` never exceeds
/// `max_samples`, and `uv_attempt_count` never exceeds 9 * `max_samples`.
/// Every face receives at least one sample (an exact finite face vertex is the
/// fallback for pathologically thin trims).
///
/// Empty/no-OCC models return a default result. A non-empty model throws
/// GeomError when `max_samples` is zero or smaller than the BRep face count.
[[nodiscard]] BRepSurfaceSamples sample_brep_surface(const CadModel& model,
                                                     std::size_t max_samples);

/// Project `p` onto the BRep surface of `model`.
///
/// OCC path: BRepExtrema_DistShapeShape on faces (trimmed), with
/// GeomAPI_ProjectPointOnSurf for UV + surface normal. Returns nullopt when
/// the model is empty, projection fails, or this build has no OCC (stub).
/// Does not throw for the no-OCC / empty cases so STL-only callers can ignore.
[[nodiscard]] std::optional<ProjectResult> project_point_on_surface(const CadModel& model,
                                                                    const Eigen::Vector3d& p);

/// Project onto one stable trimmed face. Unlike an infinite-surface projector,
/// this never returns a point outside the face's wires.
[[nodiscard]] std::optional<ProjectResult>
project_point_on_face(const CadModel& model, std::uint32_t face_id, const Eigen::Vector3d& p);

/// Project onto one stable nondegenerate CadEdge::id.
[[nodiscard]] std::optional<ProjectResult>
project_point_on_edge(const CadModel& model, std::uint32_t edge_id, const Eigen::Vector3d& p);

/// Return the exact position of one stable CAD vertex.
[[nodiscard]] std::optional<ProjectResult> project_point_on_vertex(const CadModel& model,
                                                                   std::uint32_t vertex_id,
                                                                   const Eigen::Vector3d& p);

/// Convenience: load STEP or BREP by extension; otherwise GeomError.
[[nodiscard]] CadModel load_cad(const std::filesystem::path& path);

} // namespace polymesh::geom
