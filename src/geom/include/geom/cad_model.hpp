// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// First-class CAD BRep model (ADR-0020). Product meshing should use CadModel
// as the geometry source; TriSurface tessellation is derived for viz / legacy
// hybrid fill / compare only.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <filesystem>
#include <memory>
#include <string>

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
    /// automatic fraction of the bbox diagonal (same policy as historical
    /// load_step).
    [[nodiscard]] TriSurface tessellate(double deflection = 0.0) const;

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

/// Convenience: load STEP or BREP by extension; otherwise GeomError.
[[nodiscard]] CadModel load_cad(const std::filesystem::path& path);

} // namespace polymesh::geom
