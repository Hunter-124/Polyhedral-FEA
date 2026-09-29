# SPDX-License-Identifier: BSD-3-Clause
"""The advisor feature schema, declared once as named column families.

Every column list the dataset builder, the loader and the geometry extractor
agree on lives here. The families are concatenated into the layouts each
consumer needs; the ORDER of those concatenations is a contract, not a style
choice:

* ``FEATURE_COLUMNS`` is the per-row ``features`` object testlab writes from
  ``pipeline::CaseFeatures``, in that struct's order.
* :func:`input_columns` is the network input vector. The C++ side resolves
  inputs by name through ``normalization.json:input_columns``, but the ONNX
  graph is positional, so a family is only ever APPENDED to a layout -- an
  insertion would silently reindex a shipped model. Which inputs a deployed
  model takes is decided by its exported ``normalization.json``, never by these
  lists; they decide what the NEXT training run produces.

Adding a family (for instance an explicit loading-condition block) means
declaring it here, appending it to the layouts that carry it, and retraining
and re-exporting so ``normalization.json`` records the new columns.
"""

from __future__ import annotations

#: Rows written by the advisor-aware testlab path carry the full feature and
#: action vectors. Legacy campaign rows leave the features and most action
#: columns empty, so they are excluded from the training table.
ADVISOR_ROW_SCHEMAS = frozenset({"advisor-row-v3", "advisor-row-v4"})

# --------------------------------------------------------------------------- #
# Per-row CaseFeatures (``row["features"]``), in ``pipeline::CaseFeatures``
# order. Geometric values are bbox-normalized.
# --------------------------------------------------------------------------- #

#: Tessellation-derived part geometry measured by the engine on the mesh input.
MESH_GEOMETRY_COLUMNS: list[str] = [
    "bbox_dx", "bbox_dy", "bbox_dz", "diag", "volume", "surface_area",
    "sa_over_v23", "n_faces", "n_sharp_edges", "sharp_edge_len_total",
    "curved_frac", "kappa_max_h", "kappa_mean_h", "thin_min_over_diag",
    "thin_p10_over_diag", "min_feature_h",
]

#: Fixed / loaded region descriptors: which faces the case selects, how much of
#: the surface they cover, the load direction and how it relates to the part.
BC_REGION_COLUMNS: list[str] = [
    "n_fix_faces", "n_load_faces", "fix_area_frac", "load_area_frac",
    "load_dir_x", "load_dir_y", "load_dir_z", "fix_load_dist_over_diag",
    "load_axis_alignment",
]

#: Material inputs carried in the row's feature vector.
MATERIAL_COLUMNS: list[str] = ["poisson"]

#: Proximity / crease / singularity block of the portable-cost retrain. These
#: are properties of the SOLID: ``geometry_features.py`` emits the same ten per
#: part, and the loader takes the per-row value (see
#: ``dataset._load_geometry_features``).
PROXIMITY_GEOMETRY_COLUMNS: list[str] = [
    "geo_n_inner_loops", "geo_hole_spacing_min_rel", "geo_hole_spacing_p10_rel",
    "geo_feat_pair_dist_min_rel", "geo_feat_pair_dist_p10_rel",
    "geo_feat_pair_dist_mean_rel", "geo_dihedral_p10", "geo_dihedral_p50",
    "geo_dihedral_p90", "geo_singular_lambda_min",
]

#: How the fixed and loaded regions sit relative to the part's features, and
#: how multi-axial the load is. Properties of a LOAD CASE rather than of a
#: solid, so they only ever come per row from ``pipeline::extract_case_features``.
BC_INTERACTION_COLUMNS: list[str] = [
    "load_to_feature_dist_min_rel", "fix_to_feature_dist_min_rel",
    "case_load_multiaxiality",
]

#: The full per-row feature vector.
FEATURE_COLUMNS: list[str] = (
    MESH_GEOMETRY_COLUMNS + BC_REGION_COLUMNS + MATERIAL_COLUMNS
    + PROXIMITY_GEOMETRY_COLUMNS + BC_INTERACTION_COLUMNS
)

# --------------------------------------------------------------------------- #
# Per-part exact-BRep descriptors (``geometry_features.py`` ->
# ``bench/advisor/geometry_features.csv``), joined by geometry name.
# --------------------------------------------------------------------------- #

#: Surface-type area split, curvature radii, topology counts and bulk shape.
BREP_DESCRIPTOR_COLUMNS: list[str] = [
    "geo_curved_area_frac", "geo_cyl_area_frac", "geo_plane_area_frac",
    "geo_other_area_frac", "geo_min_curv_radius_rel", "geo_log_curv_radius_mean",
    "geo_log_curv_radius_std", "geo_n_faces", "geo_n_edges",
    "geo_face_area_cv", "geo_aspect_max", "geo_aspect_mid", "geo_volume_frac",
    "geo_area_over_v23", "geo_min_face_size_rel",
]

#: Column order of ``geometry_features.csv`` (after ``part``).
GEOMETRY_TABLE_COLUMNS: list[str] = BREP_DESCRIPTOR_COLUMNS + PROXIMITY_GEOMETRY_COLUMNS

# --------------------------------------------------------------------------- #
# Case context, derived by the dataset builder from the part's ``*.case.json``.
# --------------------------------------------------------------------------- #

CASE_COLUMNS: list[str] = [
    "case_poisson", "case_n_fix_regions", "case_n_load_regions", "case_load_dir_x",
    "case_load_dir_y", "case_load_dir_z", "case_traction_magnitude",
]

# --------------------------------------------------------------------------- #
# Actions
# --------------------------------------------------------------------------- #

#: Every swept action recorded in the dataset table (``row["action"]``).
ACTION_COLUMNS: list[str] = [
    "h", "h_rel", "mesher", "element_tendency", "skin_layers", "feature_refine",
    "bc_grading", "adapt_passes", "eta_target", "p_elevate", "adapt_leb_waves",
    "cost_only", "order",
]

#: The numeric actions the network takes as inputs.
#:
#: ``p_elevate`` is deliberately absent. It is not merely unvaried in the
#: corpus, it is redundant: ``apps/cli/commands_solve.cpp`` computes
#: ``p_elevate = decision.p_elevate || decision.order >= 2``, so it actuates
#: exactly what ``order >= 2`` already actuates. Advertising it as a separate
#: policy dimension claimed a control the engine does not have.
CONTINUOUS_ACTION_COLUMNS: list[str] = [
    "h_rel", "eta_target", "adapt_passes", "element_tendency",
    "skin_layers", "feature_refine", "bc_grading", "adapt_leb_waves",
]

#: Categorical actions, encoded as vocabulary indices and embedded in-graph.
CATEGORICAL_INDEX_COLUMNS: list[str] = ["order_idx", "mesher_idx"]

# --------------------------------------------------------------------------- #
# Derived scale-law inputs (computed per row by ``dataset.derived_features``)
# --------------------------------------------------------------------------- #

#: Element count obeys ``n ~ volume / h^3`` and the cost heads are log10
#: targets, so the relationship the cost heads need is LINEAR in these four
#: and in nothing the raw columns offer: ``h`` itself is not an input at all
#: (only the dimensionless ``h_rel``), and ``volume`` spans several decades
#: across the corpus, which standardisation compresses into a spike.
#:
#: Measured on the clean regenerated dataset with a family-held-out split, the
#: net without these features predicted DOF to a validation MAE of 0.70 in
#: log10 -- a factor of five -- while a LightGBM baseline on the same split and
#: the same columns reached 0.059, because trees can recover a ratio by
#: splitting where a standardised MLP cannot.
DERIVED_FEATURE_COLUMNS: list[str] = [
    "log10_volume", "log10_diag", "log10_h", "log10_cells",
]

# --------------------------------------------------------------------------- #
# Layouts
# --------------------------------------------------------------------------- #

#: Row identity columns leading the dataset table.
IDENTITY_COLUMNS: list[str] = ["schema", "campaign", "cfg_id", "part", "tier"]


def input_families(geometry_columns: list[str]) -> list[tuple[str, list[str]]]:
    """The network input vector as ``(family, columns)`` in positional order.

    ``geometry_columns`` are the per-part descriptors actually joined from
    ``geometry_features.csv``: the loader drops any it cannot find, and any a
    row already carries, so the block is resolved at load time.
    """
    return [
        ("features", list(FEATURE_COLUMNS)),
        ("geometry", list(geometry_columns)),
        ("case", list(CASE_COLUMNS)),
        ("continuous_action", list(CONTINUOUS_ACTION_COLUMNS)),
        ("derived", list(DERIVED_FEATURE_COLUMNS)),
        ("categorical", list(CATEGORICAL_INDEX_COLUMNS)),
    ]


def input_columns(geometry_columns: list[str]) -> list[str]:
    """The flat network input vector, the concatenation of :func:`input_families`."""
    return [name for _, columns in input_families(geometry_columns) for name in columns]
