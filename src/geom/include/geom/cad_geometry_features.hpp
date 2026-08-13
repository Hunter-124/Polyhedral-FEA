// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Exact-BRep geometry descriptors for the learned mesh advisor's
// out-of-distribution detector (ADR-0027).
//
// These are deliberately NOT mesh proxies. `pipeline::CaseFeatures` measures
// curvature, thinness and face counts from the tessellation, which is why
// `curved_frac` saturates to ~1.0 for any real triangulation: its formula
// `(ntri-12)/ntri` cannot distinguish a plate from a sphere. The descriptors
// here read the BRep itself, so a cylinder is a cylinder and its radius is a
// number rather than a discrete-curvature estimate.
//
// They are consumed ONLY by the OOD Mahalanobis distance, never by the network:
// the shipped ONNX contract is 43 inputs and these 15 are not among them. On a
// six-family corpus a 1-NN classifier recovers the family from these alone at
// 32/32, which makes them family identifiers rather than transferable physics --
// exactly wrong as model inputs and exactly right for "is this part unlike
// anything I was trained on".
//
// The reference implementation is `scripts/advisor/geometry_features.py`, which
// produced the training-side table these are compared against. This file mirrors
// it operation for operation, including the degenerate-case conventions, because
// a Mahalanobis distance against a mean/precision fitted in Python is only
// meaningful if the C++ produces the same numbers.

#include "geom/cad_model.hpp"

namespace polymesh::geom {

/// The 15 exact-BRep descriptors, in the order
/// `bench/advisor/ood.json:feature_columns` declares them.
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

    double curved_area_frac = 0.0;    ///< (cylinder + other) / total area
    double cyl_area_frac = 0.0;       ///< cylindrical / total area
    double plane_area_frac = 0.0;     ///< planar / total area
    double other_area_frac = 0.0;     ///< sphere + cone + torus + freeform
    double min_curv_radius_rel = 0.0; ///< smallest analytic radius / diagonal
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
