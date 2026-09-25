// SPDX-License-Identifier: BSD-3-Clause
// Feature columns: the `pipeline::CaseFeatures` -> dataset-column mapping and
// the per-action columns (action dials, categorical indices, scale-law inputs).
#include "advisor_internal.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace polymesh::advisor {

void Advisor::Impl::apply_action(FeatureColumns& columns,
                                 const AdvisorDecision& action) const {
    // An out-of-vocabulary category is encoded as `size()`, the reserved
    // unknown-embedding slot the trainer allocates (`n_order_slots =
    // len(order_choices) + 1`). Returning 0 would silently score the row as if
    // it had asked for the FIRST category, which is a valid, wrong answer.
    const auto index_of = [](const auto& choices, const auto& value) -> double {
        const auto it = std::find(choices.begin(), choices.end(), value);
        return it == choices.end() ? static_cast<double>(choices.size())
                                   : static_cast<double>(std::distance(choices.begin(), it));
    };
    columns["h_rel"] = action.h_rel;
    columns["eta_target"] = action.eta_target;
    columns["adapt_passes"] = static_cast<double>(action.adapt_passes);
    columns["p_elevate"] = action.p_elevate ? 1.0 : 0.0;
    columns["order"] = static_cast<double>(action.order);
    columns["order_idx"] = index_of(order_choices, action.order);
    columns["mesher_idx"] = index_of(mesher_choices, action.mesher);

    // Scale-law inputs, mirroring `scripts/advisor/dataset.py:derived_features`.
    // They depend on the ACTION as well as the part (h = h_rel * diag), so they
    // are recomputed per candidate here rather than once in `to_columns`.
    // Leaving them out is not neutral: `encode` would fill all four from the
    // training median and score every part as if it were of average size.
    const auto lg = [](double value) {
        return value > 0.0 && std::isfinite(value) ? std::log10(value)
                                                   : std::numeric_limits<double>::quiet_NaN();
    };
    const auto column_of = [&columns](const char* name) {
        const auto it = columns.find(name);
        return it == columns.end() ? std::numeric_limits<double>::quiet_NaN() : it->second;
    };
    const double log_volume = lg(column_of("volume"));
    const double log_diag = lg(column_of("diag"));
    const double log_h = log_diag + lg(action.h_rel);
    columns["log10_volume"] = log_volume;
    columns["log10_diag"] = log_diag;
    columns["log10_h"] = log_h;
    columns["log10_cells"] = log_volume - 3.0 * log_h;
}

FeatureColumns to_columns(const pipeline::CaseFeatures& f) {
    FeatureColumns columns = {
        {"bbox_dx", f.bbox_dx},
        {"bbox_dy", f.bbox_dy},
        {"bbox_dz", f.bbox_dz},
        {"diag", f.diag},
        {"volume", f.volume},
        {"surface_area", f.surface_area},
        {"sa_over_v23", f.sa_over_v23},
        {"n_faces", static_cast<double>(f.n_faces)},
        {"n_sharp_edges", static_cast<double>(f.n_sharp_edges)},
        {"sharp_edge_len_total", f.sharp_edge_len_total},
        {"curved_frac", f.curved_frac},
        {"kappa_max_h", f.kappa_max_h},
        {"kappa_mean_h", f.kappa_mean_h},
        {"thin_min_over_diag", f.thin_min_over_diag},
        {"thin_p10_over_diag", f.thin_p10_over_diag},
        {"min_feature_h", f.min_feature_h},
        {"n_fix_faces", static_cast<double>(f.n_fix_faces)},
        {"n_load_faces", static_cast<double>(f.n_load_faces)},
        {"fix_area_frac", f.fix_area_frac},
        {"load_area_frac", f.load_area_frac},
        {"load_dir_x", f.load_dir_x},
        {"load_dir_y", f.load_dir_y},
        {"load_dir_z", f.load_dir_z},
        {"fix_load_dist_over_diag", f.fix_load_dist_over_diag},
        {"load_axis_alignment", f.load_axis_alignment},
        {"poisson", f.poisson},
        // The case_* columns duplicate what the feature extractor already knows;
        // the ones it cannot know (region counts, traction magnitude) are left
        // out on purpose so they are imputed rather than invented.
        {"case_poisson", f.poisson},
        {"case_load_dir_x", f.load_dir_x},
        {"case_load_dir_y", f.load_dir_y},
        {"case_load_dir_z", f.load_dir_z},
    };

    // Exact-BRep descriptors. Whether the network consumes them is decided by
    // normalization.json:input_columns (encode() imputes any absent one); the
    // out-of-distribution test reads the ood.json:feature_columns names from
    // this map in raw units under its own center/scale.
    //
    // Inserted only when the feature extractor actually measured them from a
    // BRep. When it did not -- no OpenCASCADE, or a model carrying no CAD -- the
    // keys stay ABSENT rather than zero, and mahalanobis() then refuses to score
    // rather than testing fabricated values. Zero is a legal measured value for
    // several of these descriptors (a fully planar part has
    // geo_curved_area_frac == 0), so a zero-filled block would be
    // indistinguishable from a real measurement of a simple part.
    if (f.geo_available) {
        columns.emplace("geo_curved_area_frac", f.geo_curved_area_frac);
        columns.emplace("geo_cyl_area_frac", f.geo_cyl_area_frac);
        columns.emplace("geo_plane_area_frac", f.geo_plane_area_frac);
        columns.emplace("geo_other_area_frac", f.geo_other_area_frac);
        columns.emplace("geo_min_curv_radius_rel", f.geo_min_curv_radius_rel);
        columns.emplace("geo_log_curv_radius_mean", f.geo_log_curv_radius_mean);
        columns.emplace("geo_log_curv_radius_std", f.geo_log_curv_radius_std);
        columns.emplace("geo_n_faces", f.geo_n_faces);
        columns.emplace("geo_n_edges", f.geo_n_edges);
        columns.emplace("geo_face_area_cv", f.geo_face_area_cv);
        columns.emplace("geo_aspect_max", f.geo_aspect_max);
        columns.emplace("geo_aspect_mid", f.geo_aspect_mid);
        columns.emplace("geo_volume_frac", f.geo_volume_frac);
        columns.emplace("geo_area_over_v23", f.geo_area_over_v23);
        columns.emplace("geo_min_face_size_rel", f.geo_min_face_size_rel);
        columns.emplace("geo_n_inner_loops", f.geo_n_inner_loops);
        columns.emplace("geo_hole_spacing_min_rel", f.geo_hole_spacing_min_rel);
        columns.emplace("geo_hole_spacing_p10_rel", f.geo_hole_spacing_p10_rel);
        columns.emplace("geo_feat_pair_dist_min_rel", f.geo_feat_pair_dist_min_rel);
        columns.emplace("geo_feat_pair_dist_p10_rel", f.geo_feat_pair_dist_p10_rel);
        columns.emplace("geo_feat_pair_dist_mean_rel", f.geo_feat_pair_dist_mean_rel);
        columns.emplace("geo_dihedral_p10", f.geo_dihedral_p10);
        columns.emplace("geo_dihedral_p50", f.geo_dihedral_p50);
        columns.emplace("geo_dihedral_p90", f.geo_dihedral_p90);
        columns.emplace("geo_singular_lambda_min", f.geo_singular_lambda_min);
        columns.emplace("load_to_feature_dist_min_rel",
                        f.load_to_feature_dist_min_rel);
        columns.emplace("fix_to_feature_dist_min_rel",
                        f.fix_to_feature_dist_min_rel);
        columns.emplace("case_load_multiaxiality", f.case_load_multiaxiality);
    }
    return columns;
}

} // namespace polymesh::advisor
