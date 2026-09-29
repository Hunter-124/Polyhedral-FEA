// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Exact-BRep geometry descriptors for the learned mesh advisor (ADR-0027).
//
// These read the BRep, not a mesh proxy: `pipeline::CaseFeatures` measures
// curvature, thinness and face counts from the tessellation (its `curved_frac`
// saturates near 1.0 for any real triangulation), whereas here a cylinder is a
// cylinder and its radius is a number.
//
// Which descriptors the advisor consumes is decided by the shipped artifacts,
// not by this header: `normalization.json:input_columns` (network inputs) and
// `ood.json:feature_columns` (OOD Mahalanobis distance) name the `geo_*`
// columns the deployed model uses.
//
// Reference implementation: `scripts/advisor/geometry_features.py`. This file
// mirrors it operation for operation, including the degenerate-case
// conventions, because the training-side statistics were fitted on its output.

#include "geom/cad_model.hpp"

namespace polymesh::geom {

/// Exact-BRep descriptors. Artifact column name is `geo_` + field name; field
/// order matches the descriptor block of `bench/advisor/ood.json:feature_columns`.
///
/// Units: all dimensionless. Lengths are normalised by the bounding-box
/// diagonal, areas by the total surface area, so the whole block is scale-free
/// and a part scaled by 10x lands in the same place.
struct GeometryDescriptors {
    /// False when built without OpenCASCADE, or when the model carries no
    /// BRep. Every field is then left at its default and the caller must not
    /// treat the values as measured -- an imputed descriptor block would make
    /// the OOD distance meaningless rather than merely inaccurate.
    bool available = false;

    double curved_area_frac = 0.0;     ///< (cylinder + other) / total area
    double cyl_area_frac = 0.0;        ///< cylindrical / total area
    double plane_area_frac = 0.0;      ///< planar / total area
    double other_area_frac = 0.0;      ///< sphere + cone + torus + freeform
    double min_curv_radius_rel = 0.0;  ///< smallest analytic radius / diagonal
    double log_curv_radius_mean = 0.0; ///< area-weighted mean of log10(r/diag)
    double log_curv_radius_std = 0.0;  ///< area-weighted sd of log10(r/diag)
    double n_faces = 0.0;
    double n_edges = 0.0;
    double face_area_cv = 0.0;      ///< sd/mean of face areas (population sd)
    double aspect_max = 0.0;        ///< longest bbox extent / shortest
    double aspect_mid = 0.0;        ///< middle bbox extent / shortest
    double volume_frac = 0.0;       ///< solid volume / bbox volume
    double area_over_v23 = 0.0;     ///< total area / volume^(2/3)
    double min_face_size_rel = 0.0; ///< min sqrt(face area) / diagonal
};

/// Compute the descriptors from a live BRep.
///
/// Never throws: on a degenerate bounding box, an empty model, or any OCC
/// failure it returns `available == false` rather than a partial block, because
/// a half-computed descriptor vector would silently move the OOD distance.
[[nodiscard]] GeometryDescriptors compute_geometry_descriptors(const CadModel& model);

} // namespace polymesh::geom
